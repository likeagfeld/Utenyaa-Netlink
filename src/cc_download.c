/**
 * cc_download.c — Phase C custom-character download state machine.
 *
 * State graph:
 *
 *   INIT
 *     │ cc_download_init()  → send CC_LIST_REQ
 *     ▼
 *   WAITING_LIST
 *     │ g_net.cc_list_done becomes true
 *     ▼
 *   DOWNLOADING (idx=0..count-1)
 *     │  send CC_DOWNLOAD_REQ(idx)  → wait for cc_dl_complete
 *     │  on complete: copy g_net.cc_dl_buf into s_payloads[idx],
 *     │               increment idx, send next request
 *     │  on idx == count: → DONE
 *     ▼
 *   DONE  (or FAILED on timeout / CRC)
 *     │ A button → disconnect + fresh boot to TITLE
 *     ▼
 *
 * Saves each character's 2648-byte payload to a static buffer pool
 * sized for UNET_CC_MAX entries. Phase D will hand these payloads
 * to a Saturn-side sprite loader. (Backup-RAM persistence is also
 * a Phase D-or-later step — for now characters live in RAM only
 * for the duration of the boot session.)
 */

#include "cc_download.h"
#include <jo/jo.h>
#include "font.h"
#include "net/utenyaa_game.h"
#include "net/utenyaa_net.h"
#include "net/utenyaa_protocol.h"
#include "net/utenyaa_main_glue.h"
#include "net/saturn_uart16550.h"
#include "net/modem.h"
#include "name_entry.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

extern utenyaa_game_t g_Game;
extern saturn_uart16550_t g_uart;

typedef enum {
    CCD_INIT = 0,
    CCD_WAITING_AUTH,    /* wait for unet_state >= UNET_STATE_LOBBY before sending CC_LIST_REQ */
    CCD_WAITING_LIST,
    CCD_DOWNLOADING,
    CCD_DONE,
    CCD_FAILED,
} ccd_state_t;

static ccd_state_t s_state;
static int  s_dl_idx;             /* current char being requested */
static int  s_dl_request_timer;   /* frames since we last sent a request */
static int  s_list_wait_timer;    /* frames since CC_LIST_REQ sent */
static int  s_a_held_prev;        /* edge-detect on A at DONE/FAILED */
static int  s_b_held_prev;        /* edge-detect on B at DONE/FAILED */

/* Phase D — VDP1 sprite-slot registration. The Phase D commit had
 * bumped JO_MAX_SPRITE 100→130 to make room for customs, but the
 * extra 30 slots × sizeof(jo_3d_quad)=32 = 960 extra bytes inside
 * the 250 KB jo_malloc pool pushed peak boot allocation
 * (LOGO.TGA stream 123,452 + decoded 122,888 + sprite_quad 4,168 =
 * 250,508) past the pool ceiling and triggered "LOGO.TGA: Out of
 * Memory" in __jo_tga_load. JO_MAX_SPRITE has been reverted to 100
 * (alpha-0.7 baseline) so LOGO.TGA fits with a 452-byte margin.
 *
 * Consequence: ccd_register_sprites attempts jo_sprite_add but the
 * 100-slot pool is already fully consumed by base CHARS/EXP/WEAP/HUD
 * PAKs — every call returns -1 and Player::Draw's fallback path
 * (charIdx % kBuiltinChars) renders downloaded customs as their
 * built-in counterpart. This is intentional for the 0.9 hotfix; a
 * proper Phase D follow-up would dynamically swap slots on match
 * start to load just the 4 chars actually picked. */
#define CC_MAX_REGISTERED  5

/* CCD_MAX = the cap on how many characters this Saturn caches in
 * the static payload pool. CRITICAL: this directly multiplies the
 * BSS footprint at CCD_MAX × UNET_CC_PAYLOAD_BYTES (2648). The
 * upstream UNET_CC_MAX = 64 (server-side ceiling) is way too large
 * for Saturn — at 64 × 2648 = ~168 KB BSS, the binary's runtime
 * footprint busted the documented "~317 KB cold-boot binary-size
 * threshold" and broke logo.tga loading on first boot of alpha-0.9
 * (operator-reported "out of memory on logo.tga" + null pointer
 * crashes downstream). Sized to CC_MAX_REGISTERED here so we only
 * ever hold what we can register into VDP1 anyway. The download
 * loop early-stops past CCD_MAX entries; if the server has more,
 * the operator just gets the first 5. */
#define CCD_MAX  CC_MAX_REGISTERED
static uint8_t s_payloads[CCD_MAX][UNET_CC_PAYLOAD_BYTES];
static bool    s_payload_valid[CCD_MAX];
static int     s_payload_count;

/* -1 means "not yet registered". A static int array initializes to
 * 0 by default, which is a valid sprite slot (sprite 0 = built-in
 * char 0 frame 0); using -1 prevents accidental aliasing before
 * the first download completes. */
static int s_sprite_id_for_custom[CC_MAX_REGISTERED] = { -1, -1, -1, -1, -1 };
static int s_registered_count;
static bool s_sprites_registered_this_session;

/* Register up to CC_MAX_REGISTERED downloaded characters as VDP1
 * sprites. Each character produces 5 sequential sprite IDs (one per
 * frame). Called once when CCD transitions to DONE — running it
 * again across boot cycles is unsafe because jo_sprite_add allocates
 * VRAM monotonically without a free path. */
static void ccd_register_sprites(void)
{
    if (s_sprites_registered_this_session) return;
    s_sprites_registered_this_session = true;

    int registered = 0;
    int i, fi;
    for (i = 0; i < s_payload_count && registered < CC_MAX_REGISTERED; i++) {
        if (!s_payload_valid[i]) continue;
        const uint8_t* payload = s_payloads[i];
        /* Header is 88 bytes; frame data starts at offset 88. Each
         * frame is 16×16×2 = 512 bytes (UNET_CC_FRAME_BYTES). */
        int first_sprite_id = -1;
        for (fi = 0; fi < UNET_CC_FRAMES; fi++) {
            jo_img img;
            img.width  = UNET_CC_W;
            img.height = UNET_CC_H;
            /* Cast away const — jo_sprite_add's jo_img has non-const
             * data ptr but doesn't actually mutate during DMA. */
            img.data   = (void*)(payload + 88 + fi * UNET_CC_FRAME_BYTES);
            int sid = jo_sprite_add(&img);
            if (fi == 0) first_sprite_id = sid;
            if (sid < 0) {
                first_sprite_id = -1;
                break;   /* JO_MAX_SPRITE exhausted; bail this char */
            }
        }
        s_sprite_id_for_custom[registered] = first_sprite_id;
        registered++;
    }
    /* Mark unused slots invalid. */
    for (; registered < CC_MAX_REGISTERED; registered++)
        s_sprite_id_for_custom[registered] = -1;
    s_registered_count = 0;
    for (i = 0; i < CC_MAX_REGISTERED; i++)
        if (s_sprite_id_for_custom[i] >= 0) s_registered_count++;
}

#define CCD_LIST_TIMEOUT_FRAMES   600   /* 10s @ 60fps */
#define CCD_DL_TIMEOUT_FRAMES     900   /* 15s per character */
#define CCD_REREQUEST_FRAMES      300   /* re-send DOWNLOAD_REQ if stalled */
/* Auth wait: server drops every opcode that arrives before the client
 * is authenticated (userver.py: `if not c.authenticated: return`).
 * We must wait for unet_state to reach LOBBY (set on WELCOME) before
 * sending CC_LIST_REQ. 600 frames = 10s; auth normally completes in
 * ~1 s but allow generous slack for slow modem ack. */
#define CCD_AUTH_TIMEOUT_FRAMES   600

static void ccd_send_list_req(void)
{
    unet_send_cc_list_req();
    s_list_wait_timer = 0;
}

static void ccd_request_next(void)
{
    const unet_state_data_t* nd = unet_get_data();
    /* Stop at server's count OR our static pool cap (CCD_MAX),
     * whichever comes first. The pool cap protects against the
     * BSS-overshoot that crashed alpha-0.9 boot when CCD_MAX was
     * 64 × 2648 bytes. */
    int hard_cap = (int)nd->cc_list_count;
    if (hard_cap > CCD_MAX) hard_cap = CCD_MAX;
    if (s_dl_idx >= hard_cap) {
        s_state = CCD_DONE;
        return;
    }
    /* Reset per-char net state so the receiver writes into a clean
     * buffer for THIS index. */
    unet_cc_reset_dl_slot();
    unet_send_cc_download_req((uint8_t)s_dl_idx);
    s_dl_request_timer = 0;
}

/* Diagnostic helper — emits a CC_DBG line to the server journal so
 * the next test session's journalctl produces a clean state-transition
 * trail without needing per-chunk noise. Format:
 *   CC_DBG <state-tag> [extra]
 * where state-tag is one of INIT/AUTH/LIST/DL_BEGIN/DL_END/DONE/FAILED. */
static void ccd_log(const char* msg)
{
    unet_send_dbg_log(msg);
}

void cc_download_init(void)
{
    int i;
    s_state = CCD_INIT;
    s_dl_idx = 0;
    s_dl_request_timer = 0;
    s_list_wait_timer = 0;
    /* Initialize s_a_held_prev = 1 (treat A as already held). The user
     * arrived at this screen by pressing A on the title-menu's
     * "Download Characters" button — that A is still held when we
     * land here on frame 1. With prev=0 the rising-edge detector
     * would fire immediately on entry (wrong on WAITING_LIST since
     * the gate ignores it) AND, more critically, would leave prev=1
     * for the rest of the screen so the operator's A-press at DONE/
     * FAILED never produces a fresh rising edge — operator-reported
     * "pressing A to return to title does nothing." Initializing
     * to 1 forces a release-then-press cycle, which the operator
     * naturally does the first time they tap A on the FAILED screen. */
    s_a_held_prev = 1;
    s_b_held_prev = 1;
    s_payload_count = 0;
    for (i = 0; i < CCD_MAX; i++) s_payload_valid[i] = false;
    /* Don't clear s_sprite_id_for_custom on re-entry — sprite slots
     * registered in a prior download flow this boot should survive
     * (jo_sprite_add can't free, and re-registering would burn more
     * slots). The user's intent on re-running download is "refresh
     * the catalog"; sprite registration can't be redone safely so
     * we keep the original slots until reboot. */

    unet_cc_reset_state();
    /* DON'T send CC_LIST_REQ yet. The server discards every opcode
     * that arrives before the client is authenticated:
     *
     *   userver.py:2359   if not c.authenticated: return
     *
     * cc_download_init runs at CONNECT_STAGE_CONNECTED (right after
     * unet_on_connected sends MSG_CONNECT) — auth has NOT yet
     * completed (WELCOME → SET_USERNAME → WELCOME-back round-trip
     * takes ~1 s). Sending CC_LIST_REQ here gets dropped silently;
     * the Saturn-side state machine then waits 10 s for a list
     * response that will never come and trips CCD_FAILED, producing
     * the operator-reported "Download Failed" with no obvious cause.
     * Wait for unet_get_state() == UNET_STATE_LOBBY (set on WELCOME
     * receipt) before sending. */
    s_state = CCD_WAITING_AUTH;
    ccd_log("CC_DBG init -> WAITING_AUTH");
    jo_clear_screen();
}

void cc_download_tick(void)
{
    switch (s_state) {
    case CCD_INIT:
        s_state = CCD_WAITING_AUTH;
        break;

    case CCD_WAITING_AUTH: {
        /* Wait for auth handshake to complete on the network side.
         * unet_get_state() returns UNET_STATE_LOBBY once the server's
         * WELCOME has been received and the client is authenticated. */
        s_list_wait_timer++;
        unet_state_t st = unet_get_state();
        if (st == UNET_STATE_LOBBY ||
            st == UNET_STATE_PLAYING)
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "CC_DBG auth-ok t=%d -> WAITING_LIST", s_list_wait_timer);
            ccd_log(buf);
            ccd_send_list_req();
            s_state = CCD_WAITING_LIST;
            s_list_wait_timer = 0;
        } else if (s_list_wait_timer > CCD_AUTH_TIMEOUT_FRAMES) {
            /* Auth never completed — surface as FAILED so operator
             * gets a recognizable error path back to title. */
            ccd_log("CC_DBG auth-timeout -> FAILED");
            s_state = CCD_FAILED;
        }
        break;
    }

    case CCD_WAITING_LIST: {
        const unet_state_data_t* nd = unet_get_data();
        s_list_wait_timer++;
        if (nd->cc_list_done) {
            char buf[64];
            snprintf(buf, sizeof(buf),
                     "CC_DBG list-done count=%u t=%d", (unsigned)nd->cc_list_count, s_list_wait_timer);
            ccd_log(buf);
            s_dl_idx = 0;
            if (nd->cc_list_count == 0) {
                ccd_log("CC_DBG no-chars -> DONE");
                s_state = CCD_DONE;   /* server has none to share */
            } else {
                s_state = CCD_DOWNLOADING;
                ccd_request_next();
            }
        } else if (s_list_wait_timer > CCD_LIST_TIMEOUT_FRAMES) {
            ccd_log("CC_DBG list-timeout -> FAILED");
            s_state = CCD_FAILED;
        }
        break;
    }

    case CCD_DOWNLOADING: {
        const unet_state_data_t* nd = unet_get_data();
        s_dl_request_timer++;
        /* Periodic progress log at 1Hz so we can see chunk delivery
         * pacing without spamming every frame. */
        if ((s_dl_request_timer % 60) == 0) {
            char buf[80];
            snprintf(buf, sizeof(buf),
                     "CC_DBG DL idx=%d t=%d rx=%u/%u active=%d",
                     s_dl_idx, s_dl_request_timer,
                     (unsigned)nd->cc_dl_received_bytes,
                     (unsigned)nd->cc_dl_total,
                     nd->cc_dl_active ? 1 : 0);
            ccd_log(buf);
        }
        if (nd->cc_dl_complete && nd->cc_dl_idx == (uint8_t)s_dl_idx) {
            char buf[80];
            snprintf(buf, sizeof(buf),
                     "CC_DBG DL complete idx=%d sz=%u",
                     s_dl_idx, (unsigned)nd->cc_dl_total);
            ccd_log(buf);
            /* Snapshot this character's payload into our pool. */
            int sz = (int)nd->cc_dl_total;
            if (sz > 0 && sz <= (int)sizeof(s_payloads[0])
                && s_dl_idx < CCD_MAX)
            {
                memcpy(s_payloads[s_dl_idx], nd->cc_dl_buf, (size_t)sz);
                s_payload_valid[s_dl_idx] = true;
                if (s_payload_count <= s_dl_idx)
                    s_payload_count = s_dl_idx + 1;
            }
            unet_cc_consume_complete();
            s_dl_idx++;
            ccd_request_next();
            /* If ccd_request_next saw end-of-list, it transitioned
             * us to CCD_DONE — register VDP1 sprites now while we're
             * still on the download screen (idle frame budget). */
            if (s_state == CCD_DONE) {
                ccd_log("CC_DBG all-DL-done -> DONE registering sprites");
                ccd_register_sprites();
            }
        } else if (s_dl_request_timer > CCD_DL_TIMEOUT_FRAMES) {
            /* Timeout — give up on this character, advance. If too
             * many fail, the user will see a reduced count at DONE. */
            char buf[64];
            snprintf(buf, sizeof(buf),
                     "CC_DBG DL timeout idx=%d rx=%u/%u",
                     s_dl_idx,
                     (unsigned)nd->cc_dl_received_bytes,
                     (unsigned)nd->cc_dl_total);
            ccd_log(buf);
            s_dl_idx++;
            ccd_request_next();
            if (s_state == CCD_DONE) ccd_register_sprites();
        } else if (s_dl_request_timer > 0
                   && (s_dl_request_timer % CCD_REREQUEST_FRAMES) == 0
                   && !nd->cc_dl_active) {
            /* Re-send the download request if we got nothing back yet. */
            ccd_log("CC_DBG re-request DL — no chunks received");
            unet_send_cc_download_req((uint8_t)s_dl_idx);
        }
        break;
    }

    case CCD_DONE:
    case CCD_FAILED:
        /* Idle; cc_download_input handles A-press exit. */
        break;
    }
}

void cc_download_input(void)
{
    bool a_now = jo_is_pad1_key_pressed(JO_KEY_A);
    bool b_now = jo_is_pad1_key_pressed(JO_KEY_B);
    if ((s_state == CCD_DONE || s_state == CCD_FAILED)
        && a_now && !s_a_held_prev)
    {
        /* A press → continue to online play. Modem + auth are
         * already alive (we just used them to download), so the
         * cleanest path is straight into name entry: the user
         * replaces the placeholder "DL" with their real name, the
         * confirm sends a rename to the server, and then the
         * normal lobby flow takes over WITHOUT re-dialing. */
        ccd_log(s_state == CCD_DONE
                ? "CC_DBG A at DONE -> NAME_ENTRY (online play)"
                : "CC_DBG A at FAILED -> NAME_ENTRY (online play)");
        jo_clear_screen();
        g_Game.downloadCharsMode = false;
        /* g_Game.isOnlineMode stays true — modem connection is
         * preserved across the transition. */
        g_Game.gameState = UGAME_STATE_NAME_ENTRY;
        /* Use the blank init variant so the placeholder "DL" we set
         * for the download dial doesn't pre-fill the entry — and
         * neither does any "DL" that may have leaked into backup
         * RAM in an earlier session if the user had ever confirmed
         * the prefilled value. Operator-reported: "after I have
         * downloaded the character and go to name entry, it is
         * prefilled with DL for the name for some reason." */
        nameEntry_init_blank();
        s_state = CCD_INIT;
    }
    else if ((s_state == CCD_DONE || s_state == CCD_FAILED)
             && b_now && !s_b_held_prev)
    {
        /* B press → disconnect cleanly + return to title (the old
         * A behaviour). */
        ccd_log(s_state == CCD_DONE
                ? "CC_DBG B at DONE -> title (disconnect)"
                : "CC_DBG B at FAILED -> title (disconnect)");
        unet_send_disconnect();
        modem_hangup(&g_uart);
        jo_clear_screen();
        g_Game.downloadCharsMode = false;
        g_Game.isOnlineMode = false;
        g_Game.gameState = UGAME_STATE_TITLE_SCREEN;
        g_Game.titleScreenChoice = 2;
        s_state = CCD_INIT;
    }
    s_a_held_prev = a_now ? 1 : 0;
    s_b_held_prev = b_now ? 1 : 0;
}

void cc_download_draw(void)
{
    if (g_Game.gameState != UGAME_STATE_DOWNLOAD_CHARS) return;

    /* Header */
    font_draw_centered("DOWNLOAD CUSTOM CHARACTERS", FONT_Y(4), 500);

    /* Status line by state. font_draw_centered pads to its own width
     * so we use fixed-length strings to avoid trail-off from prior
     * status messages with shorter text. */
    char status[40];
    char progress[40];
    progress[0] = '\0';

    switch (s_state) {
    case CCD_INIT:
        snprintf(status, sizeof(status), "%-30s", "Initializing...");
        break;
    case CCD_WAITING_AUTH:
        snprintf(status, sizeof(status), "%-30s", "Authenticating...");
        break;
    case CCD_WAITING_LIST:
        snprintf(status, sizeof(status), "%-30s", "Listing characters...");
        break;
    case CCD_DOWNLOADING: {
        const unet_state_data_t* nd = unet_get_data();
        int total = (int)nd->cc_list_count;
        int done = s_dl_idx;
        {
            char raw[40];
            snprintf(raw, sizeof(raw), "Downloading char %d/%d",
                     done + 1, total);
            /* Same 30-char pad as CCD_DONE — keep centered column
             * stable so transitions don't leak leading chars. */
            snprintf(status, sizeof(status), "%-30s", raw);
        }
        /* Progress bar — 20 cells wide. */
        int filled = (total > 0)
            ? (done * 20 / total)
            : 20;
        if (filled > 20) filled = 20;
        if (filled < 0)  filled = 0;
        char bar[24];
        bar[0] = '[';
        int i;
        for (i = 0; i < 20; i++) bar[1 + i] = (i < filled) ? '#' : '-';
        bar[21] = ']';
        bar[22] = '\0';
        snprintf(progress, sizeof(progress), "%-30s", bar);
        break;
    }
    case CCD_DONE: {
        int n = s_payload_count;
        char raw[40];
        snprintf(raw, sizeof(raw), "Download complete: %2d chars", n);
        /* Pad to 30 chars so the centered draw column stays at the
         * SAME col regardless of state. Without this, the 21-char
         * "Listing characters..." (centered at col 5) leaks "Li"
         * leading characters when the shorter "Download complete..."
         * (centered at col 7) overwrites only cols 7+ — operator-
         * reported "LiDownload Complete:..." artifact. */
        snprintf(status, sizeof(status), "%-30s", raw);
        snprintf(progress, sizeof(progress), "%-30s", "A: Online Play   B: Title");
        break;
    }
    case CCD_FAILED:
        snprintf(status, sizeof(status), "%-30s", "Download FAILED");
        snprintf(progress, sizeof(progress), "%-30s", "A: Online Play   B: Title");
        break;
    }

    font_draw_centered(status,   FONT_Y(10), 500);
    if (progress[0]) font_draw_centered(progress, FONT_Y(12), 500);
}

const unsigned char* cc_download_get_payload(int idx, int* out_size)
{
    if (idx < 0 || idx >= CCD_MAX || !s_payload_valid[idx]) {
        if (out_size) *out_size = 0;
        return 0;
    }
    if (out_size) *out_size = UNET_CC_PAYLOAD_BYTES;
    return s_payloads[idx];
}

int cc_download_count(void)
{
    return s_payload_count;
}

int cc_download_get_sprite_id(int local_idx)
{
    if (local_idx < 0 || local_idx >= CC_MAX_REGISTERED) return -1;
    return s_sprite_id_for_custom[local_idx];
}

int cc_download_registered_count(void)
{
    return s_registered_count;
}
