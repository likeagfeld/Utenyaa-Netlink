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
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

extern utenyaa_game_t g_Game;
extern saturn_uart16550_t g_uart;

typedef enum {
    CCD_INIT = 0,
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

/* Per-character downloaded payloads. UNET_CC_MAX × ~2.6 KB ≈ 168 KB
 * static — sized for the design ceiling but in practice most users
 * will have <10 chars. Phase D reuses this pool when loading sprites
 * into VDP1 at game start. */
#define CCD_MAX  UNET_CC_MAX
static uint8_t s_payloads[CCD_MAX][UNET_CC_PAYLOAD_BYTES];
static bool    s_payload_valid[CCD_MAX];
static int     s_payload_count;

/* Phase D — VDP1 sprite-slot registration. JO_MAX_SPRITE was bumped
 * to 130 in the makefile (was 100); base CHARS.PAK + EXP.PAK + WEAP.PAK
 * etc. consume ~103 slots — that leaves ~25 free for custom chars,
 * so we cap at 5 registered customs (5 × 5 frames = 25 slots).
 * Phase D follow-up: per-match dynamic loading of just the chars
 * actually selected (max 4 unique players) which would let us
 * support more customs without raising JO_MAX_SPRITE further. */
#define CC_MAX_REGISTERED  5
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

static void ccd_send_list_req(void)
{
    unet_send_cc_list_req();
    s_list_wait_timer = 0;
}

static void ccd_request_next(void)
{
    const unet_state_data_t* nd = unet_get_data();
    if (s_dl_idx >= (int)nd->cc_list_count) {
        s_state = CCD_DONE;
        return;
    }
    /* Reset per-char net state so the receiver writes into a clean
     * buffer for THIS index. */
    unet_cc_reset_dl_slot();
    unet_send_cc_download_req((uint8_t)s_dl_idx);
    s_dl_request_timer = 0;
}

void cc_download_init(void)
{
    int i;
    s_state = CCD_INIT;
    s_dl_idx = 0;
    s_dl_request_timer = 0;
    s_list_wait_timer = 0;
    s_a_held_prev = 0;
    s_payload_count = 0;
    for (i = 0; i < CCD_MAX; i++) s_payload_valid[i] = false;
    /* Don't clear s_sprite_id_for_custom on re-entry — sprite slots
     * registered in a prior download flow this boot should survive
     * (jo_sprite_add can't free, and re-registering would burn more
     * slots). The user's intent on re-running download is "refresh
     * the catalog"; sprite registration can't be redone safely so
     * we keep the original slots until reboot. */

    unet_cc_reset_state();
    /* Nudge into WAITING_LIST immediately. */
    ccd_send_list_req();
    s_state = CCD_WAITING_LIST;
    jo_clear_screen();
}

void cc_download_tick(void)
{
    switch (s_state) {
    case CCD_INIT:
        ccd_send_list_req();
        s_state = CCD_WAITING_LIST;
        break;

    case CCD_WAITING_LIST: {
        const unet_state_data_t* nd = unet_get_data();
        s_list_wait_timer++;
        if (nd->cc_list_done) {
            s_dl_idx = 0;
            if (nd->cc_list_count == 0) {
                s_state = CCD_DONE;   /* server has none to share */
            } else {
                s_state = CCD_DOWNLOADING;
                ccd_request_next();
            }
        } else if (s_list_wait_timer > CCD_LIST_TIMEOUT_FRAMES) {
            s_state = CCD_FAILED;
        }
        break;
    }

    case CCD_DOWNLOADING: {
        const unet_state_data_t* nd = unet_get_data();
        s_dl_request_timer++;
        if (nd->cc_dl_complete && nd->cc_dl_idx == (uint8_t)s_dl_idx) {
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
            if (s_state == CCD_DONE) ccd_register_sprites();
        } else if (s_dl_request_timer > CCD_DL_TIMEOUT_FRAMES) {
            /* Timeout — give up on this character, advance. If too
             * many fail, the user will see a reduced count at DONE. */
            s_dl_idx++;
            ccd_request_next();
            if (s_state == CCD_DONE) ccd_register_sprites();
        } else if (s_dl_request_timer > 0
                   && (s_dl_request_timer % CCD_REREQUEST_FRAMES) == 0
                   && !nd->cc_dl_active) {
            /* Re-send the download request if we got nothing back yet. */
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
    if ((s_state == CCD_DONE || s_state == CCD_FAILED)
        && a_now && !s_a_held_prev)
    {
        /* Disconnect cleanly + fresh boot back to title. Mirrors
         * the lobby Y-disconnect path (lobby.c:186) so the modem
         * + UART get torn down identically. */
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
    case CCD_WAITING_LIST:
        snprintf(status, sizeof(status), "%-30s", "Listing characters...");
        break;
    case CCD_DOWNLOADING: {
        const unet_state_data_t* nd = unet_get_data();
        int total = (int)nd->cc_list_count;
        int done = s_dl_idx;
        snprintf(status, sizeof(status), "Downloading char %d/%d",
                 done + 1, total);
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
        snprintf(status, sizeof(status), "Download complete: %2d chars", n);
        snprintf(progress, sizeof(progress), "%-30s", "Press A to return to title");
        break;
    }
    case CCD_FAILED:
        snprintf(status, sizeof(status), "%-30s", "Download FAILED");
        snprintf(progress, sizeof(progress), "%-30s", "Press A to return to title");
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
