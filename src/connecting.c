/**
 * connecting.c - Modem connection screen for Utenyaa online play
 *
 * Frame-by-frame state machine for modem probe / init / dial.
 * Adapted from Flicky's Flock connecting.c.
 */

#include <jo/jo.h>
#include "connecting.h"
#include "lobby.h"
#include "font.h"
#include "net/utenyaa_game.h"
#include "net/utenyaa_net.h"
#include "net/saturn_uart16550.h"
#include "net/modem.h"

extern void slSynch(void);

#define CONNECT_DIAL_NUMBER   "199405"
#define CONNECT_DIAL_TIMEOUT  180000000  /* ~60s at 28.6MHz */

/* Shared UART/transport/modem flag — defined in main.cxx */
extern saturn_uart16550_t g_uart;
extern bool g_modem_detected;
extern net_transport_t g_saturn_transport;

typedef enum {
    CONNECT_STAGE_INIT = 0,
    CONNECT_STAGE_SHOW_PROBE,
    CONNECT_STAGE_PROBING,
    CONNECT_STAGE_SHOW_INIT,
    CONNECT_STAGE_MODEM_INIT,
    CONNECT_STAGE_SHOW_DIAL,
    CONNECT_STAGE_DIALING,
    CONNECT_STAGE_CONNECTED,
    CONNECT_STAGE_FAILED
} connect_stage_t;

static connect_stage_t g_connect_stage;
static const char* g_connect_msg = "";
static int g_connect_timer = 0;

void connecting_init(void)
{
    g_connect_stage = CONNECT_STAGE_INIT;
    g_connect_msg = "PREPARING...";
    g_connect_timer = 0;

    jo_audio_stop_cd();
    jo_clear_screen();

    unet_init();
    unet_set_modem_available(g_modem_detected);
    unet_set_username(g_Game.playerName[0] ? g_Game.playerName : "PLAYER");
}

void connecting_input(void)
{
    if (g_Game.gameState != UGAME_STATE_CONNECTING) return;

    if (jo_is_pad1_key_pressed(JO_KEY_B)) {
        if (!g_Game.input.pressedLT) {
            unet_send_disconnect();
            jo_clear_screen();
            g_Game.input.pressedABC = true;
            g_Game.titleScreenChoice = 2;
            g_Game.gameState = UGAME_STATE_TITLE_SCREEN;
        }
        g_Game.input.pressedLT = true;
    } else {
        g_Game.input.pressedLT = false;
    }
}

void connecting_update(void)
{
    modem_result_t result;

    if (g_Game.gameState != UGAME_STATE_CONNECTING) return;

    switch (g_connect_stage) {

    case CONNECT_STAGE_INIT:
        if (!g_modem_detected) {
            g_connect_msg = "NO NETLINK MODEM";
            unet_log("No NetLink modem detected");
            g_connect_stage = CONNECT_STAGE_FAILED;
            return;
        }
        g_connect_stage = CONNECT_STAGE_SHOW_PROBE;
        break;

    case CONNECT_STAGE_SHOW_PROBE:
        g_connect_msg = "PROBING MODEM...";
        unet_log("Probing modem...");
        g_connect_stage = CONNECT_STAGE_PROBING;
        break;

    case CONNECT_STAGE_PROBING:
        connecting_draw();
        slSynch();
        if (modem_probe(&g_uart) != MODEM_OK) {
            g_connect_msg = "NO MODEM RESPONSE";
            unet_log("No modem response");
            g_connect_stage = CONNECT_STAGE_FAILED;
            return;
        }
        unet_log("Modem detected");
        g_connect_stage = CONNECT_STAGE_SHOW_INIT;
        break;

    case CONNECT_STAGE_SHOW_INIT:
        g_connect_msg = "INITIALIZING MODEM...";
        unet_log("Initializing modem...");
        g_connect_stage = CONNECT_STAGE_MODEM_INIT;
        break;

    case CONNECT_STAGE_MODEM_INIT:
        connecting_draw();
        slSynch();
        if (modem_init(&g_uart) != MODEM_OK) {
            g_connect_msg = "MODEM INIT FAILED";
            unet_log("Modem init failed");
            g_connect_stage = CONNECT_STAGE_FAILED;
            return;
        }
        unet_log("Modem ready");
        g_connect_stage = CONNECT_STAGE_SHOW_DIAL;
        break;

    case CONNECT_STAGE_SHOW_DIAL:
        g_connect_msg = "DIALING SERVER...";
        unet_log("Dialing " CONNECT_DIAL_NUMBER "...");
        g_connect_stage = CONNECT_STAGE_DIALING;
        break;

    case CONNECT_STAGE_DIALING:
        connecting_draw();
        slSynch();
        result = modem_dial(&g_uart, CONNECT_DIAL_NUMBER, CONNECT_DIAL_TIMEOUT);
        switch (result) {
        case MODEM_CONNECT:
            g_connect_msg = "CONNECTED!";
            unet_log("Connection established!");
            modem_flush_input(&g_uart);
            g_connect_stage = CONNECT_STAGE_CONNECTED;
            break;
        case MODEM_NO_CARRIER:
            g_connect_msg = "NO CARRIER";
            unet_log("NO CARRIER - Check cable");
            g_connect_stage = CONNECT_STAGE_FAILED;
            break;
        case MODEM_BUSY:
            g_connect_msg = "LINE BUSY";
            unet_log("LINE BUSY - Try again");
            g_connect_stage = CONNECT_STAGE_FAILED;
            break;
        case MODEM_NO_DIALTONE:
            g_connect_msg = "NO DIALTONE";
            unet_log("NO DIALTONE - Check line");
            g_connect_stage = CONNECT_STAGE_FAILED;
            break;
        case MODEM_NO_ANSWER:
            g_connect_msg = "NO ANSWER";
            unet_log("NO ANSWER - Server down?");
            g_connect_stage = CONNECT_STAGE_FAILED;
            break;
        case MODEM_TIMEOUT_ERR:
            g_connect_msg = "TIMEOUT";
            unet_log("TIMEOUT - Server offline?");
            g_connect_stage = CONNECT_STAGE_FAILED;
            break;
        default:
            g_connect_msg = "UNKNOWN ERROR";
            unet_log("Dial failed");
            g_connect_stage = CONNECT_STAGE_FAILED;
            break;
        }
        break;

    case CONNECT_STAGE_CONNECTED:
        saturn_uart_reg_write(&g_uart, SATURN_UART_FCR,
            SATURN_UART_FCR_ENABLE | SATURN_UART_FCR_RXRESET);
        unet_set_transport(&g_saturn_transport);
        unet_on_connected();
        jo_clear_screen();
        g_Game.gameState = UGAME_STATE_LOBBY;
        lobby_init();
        break;

    case CONNECT_STAGE_FAILED:
        g_connect_timer++;
        if (g_connect_timer > 180) {
            jo_clear_screen();
            g_Game.gameState = UGAME_STATE_TITLE_SCREEN;
        }
        break;
    }
}

void connecting_draw(void)
{
    const unet_state_data_t* nd;
    int i;

    if (g_Game.gameState != UGAME_STATE_CONNECTING) return;

    font_draw_centered("CONNECTING", FONT_Y(8), 500);
    font_draw_centered(g_connect_msg, FONT_Y(14), 500);

    nd = unet_get_data();
    for (i = 0; i < 4; i++) {
        if (i < nd->log_count) {
            font_draw(nd->log_lines[i], FONT_X(3), FONT_Y(17 + i), 500);
        }
    }

    font_draw_centered("PRESS B TO CANCEL", FONT_Y(26), 500);
}
