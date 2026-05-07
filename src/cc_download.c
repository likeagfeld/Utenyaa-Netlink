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
        } else if (s_dl_request_timer > CCD_DL_TIMEOUT_FRAMES) {
            /* Timeout — give up on this character, advance. If too
             * many fail, the user will see a reduced count at DONE. */
            s_dl_idx++;
            ccd_request_next();
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
