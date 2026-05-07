/**
 * utenyaa_main_glue.c - C/C++ bridge for the online layer
 *
 * Defines the module-level singletons, wires the Saturn UART as a
 * net_transport, detects the modem at boot, and routes per-frame
 * screen callbacks based on g_Game.gameState.
 */

#include "utenyaa_main_glue.h"
#include "utenyaa_net.h"
#include "saturn_uart16550.h"
#include "modem.h"
#include "net_transport.h"

#include "../font.h"
#include "../connecting.h"
#include "../name_entry.h"
#include "../lobby.h"
#include "../map_pick.h"
#include "../cc_download.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/*============================================================================
 * Singletons
 *============================================================================*/

utenyaa_game_t     g_Game           = {0};
saturn_uart16550_t g_uart           = {0};
bool               g_modem_detected = false;

/*============================================================================
 * NetLink LED heartbeat (board ctrl bit 7)
 *============================================================================*/

#define NETLINK_BOARD_CTRL  (*(volatile uint8_t*)0x25885031)
#define NETLINK_BUS_STROBE  (*(volatile uint8_t*)0x2582503D)

static int g_led_counter = 0;

/*============================================================================
 * net_transport adapter over saturn_uart16550
 *============================================================================*/

static bool saturn_transport_rx_ready(void* ctx)
{
    return saturn_uart_rx_ready((saturn_uart16550_t*)ctx);
}

static uint8_t saturn_transport_rx_byte(void* ctx)
{
    saturn_uart16550_t* u = (saturn_uart16550_t*)ctx;
    return (uint8_t)saturn_uart_reg_read(u, SATURN_UART_RBR);
}

static int saturn_transport_send(void* ctx, const uint8_t* data, int len)
{
    saturn_uart16550_t* u = (saturn_uart16550_t*)ctx;
    int i;
    for (i = 0; i < len; i++) {
        if (!saturn_uart_putc(u, data[i])) return i;
    }
    return len;
}

net_transport_t g_saturn_transport = {
    saturn_transport_rx_ready,
    saturn_transport_rx_byte,
    saturn_transport_send,
    (void*)0,   /* is_connected */
    (void*)0    /* ctx — set in unet_glue_init() */
};

/*============================================================================
 * Init
 *============================================================================*/

static bool s_glue_initialized = false;
static bool s_modem_probed = false;

/* Minimal init: just set g_Game defaults so main.cxx can read them.
 * The heavy stuff (font load, net init, modem detect) is deferred to
 * unet_glue_lazy_init_online() which only fires when the user actually
 * picks Play Online from the menu. Crashing on emulators without
 * NetLink MMIO while the user is still on the title screen would be
 * terrible UX. */
void unet_glue_init(void)
{
    if (s_glue_initialized) return;
    s_glue_initialized = true;

    memset(&g_Game, 0, sizeof(g_Game));
    g_Game.gameState = UGAME_STATE_NONE;
    g_Game.myPlayerID = 0xFF;
    g_Game.myPlayerID2 = 0xFF;
    g_Game.myCharacter = 0xFF;
    g_Game.myCharacter2 = 0xFF;
    g_Game.myStageVote = 0xFF;
    g_Game.titleScreenChoice = 0;
    g_modem_detected = false;
}

/* Called from unet_glue_enter_online() ONLY when the user actually
 * chose to go online from the MainMenu. Runs the expensive/possibly-
 * unsafe steps: font load, net state init, SMPC NEON to the modem,
 * UART register probing. */
static void unet_glue_lazy_init_online(void)
{
    static const struct { uint32_t base; uint32_t stride; } addrs[] = {
        { 0x25895001, 4 },
        { 0x04895001, 4 }
    };
    int i;

    if (s_modem_probed) return;
    s_modem_probed = true;

    /* Load bitmap font */
    font_load();

    /* Init net client state machine */
    unet_init();

    /* Bind transport */
    g_saturn_transport.ctx = &g_uart;

    /* Detect modem (non-blocking, does not dial) */
    saturn_netlink_smpc_enable();
    g_modem_detected = false;
    for (i = 0; i < 2; i++) {
        g_uart.base   = addrs[i].base;
        g_uart.stride = addrs[i].stride;
        if (saturn_uart_detect(&g_uart)) {
            g_modem_detected = true;
            break;
        }
    }
    unet_set_modem_available(g_modem_detected);
}

/*============================================================================
 * Enter online flow from the MainMenu ONLINE button
 *============================================================================*/

void unet_glue_enter_online(void)
{
    /* Run heavy init exactly once, the first time the user picks online. */
    unet_glue_lazy_init_online();
    g_Game.isOnlineMode = true;
    g_Game.downloadCharsMode = false;   /* normal online play, not char-DL */
    g_Game.gameState = UGAME_STATE_NAME_ENTRY;
    nameEntry_init();
}

/*============================================================================
 * Phase C — title-screen "Download Custom Characters" entry point.
 * Same dial flow as PlayOnline, but the post-auth transition routes
 * to UGAME_STATE_DOWNLOAD_CHARS instead of UGAME_STATE_LOBBY (see the
 * downloadCharsMode check in connecting.c).
 *============================================================================*/

void unet_glue_enter_char_download(void)
{
    unet_glue_lazy_init_online();
    g_Game.isOnlineMode = true;
    g_Game.downloadCharsMode = true;
    /* Skip the name-entry screen — character download doesn't need a
     * username; the server doesn't gate CC_LIST_REQ on auth identity.
     * Use a fixed placeholder to keep the auth handshake happy. */
    g_Game.playerName[0] = 'D';
    g_Game.playerName[1] = 'L';
    g_Game.playerName[2] = '\0';
    g_Game.gameState = UGAME_STATE_CONNECTING;
    /* Reset CC state so a prior aborted run doesn't leak state. */
    unet_cc_reset_state();
    /* Run the same per-entry init the Play-Online path runs through
     * name_entry.c. Without this, the connecting state machine never
     * gets unet_init()'d, never receives the modem-availability flag
     * via unet_set_modem_available(), and never sets the username.
     * Symptom (operator-reported): the Download-Characters dial does
     * not detect a modem even though Play-Online dialing on the same
     * boot cycle works. */
    connecting_init();
}

/*============================================================================
 * Is an online screen currently active? (Lets C++ skip offline render path)
 *============================================================================*/

bool unet_glue_is_online_screen_active(void)
{
    switch (g_Game.gameState) {
    case UGAME_STATE_NAME_ENTRY:
    case UGAME_STATE_CONNECTING:
    case UGAME_STATE_LOBBY:
    case UGAME_STATE_MAP_PICK:
    case UGAME_STATE_DOWNLOAD_CHARS:
        return true;
    default:
        return false;
    }
}

/*============================================================================
 * Per-frame tick: run unet_tick + dispatch screens + LED blink
 *============================================================================*/

static void led_tick(void)
{
    bool blink = g_modem_detected && (
        g_Game.gameState == UGAME_STATE_CONNECTING ||
        g_Game.gameState == UGAME_STATE_LOBBY ||
        (g_Game.gameState == UGAME_STATE_GAMEPLAY && g_Game.isOnlineMode));

    if (blink) {
        g_led_counter++;
        if (g_led_counter >= 40) g_led_counter = 0;

        if (g_led_counter == 0) {
            uint8_t val = NETLINK_BOARD_CTRL;
            NETLINK_BUS_STROBE = 0;
            NETLINK_BOARD_CTRL = (uint8_t)(val | 0x80u);
            NETLINK_BUS_STROBE = 0;
        } else if (g_led_counter == 10) {
            uint8_t val = NETLINK_BOARD_CTRL;
            NETLINK_BUS_STROBE = 0;
            NETLINK_BOARD_CTRL = (uint8_t)(val & 0x7Fu);
            NETLINK_BUS_STROBE = 0;
        }
    } else if (g_led_counter != 0) {
        uint8_t val = NETLINK_BOARD_CTRL;
        NETLINK_BUS_STROBE = 0;
        NETLINK_BOARD_CTRL = (uint8_t)(val & 0x7Fu);
        NETLINK_BUS_STROBE = 0;
        g_led_counter = 0;
    }
}

void unet_glue_tick_frame(void)
{
    /* Skip entirely unless the user has entered the online flow. Avoids
     * any NetLink MMIO poking on the title screen and during offline play. */
    if (!s_modem_probed) return;

    /* Network state machine (runs every frame once transport is live) */
    unet_tick();

    /* Sync C++-side g_Game.myPlayerID + myPlayerID2 from the net state.
     * myPlayerID set as soon as WELCOME arrives (server-assigned pid).
     * myPlayerID2 set when LOCAL_PLAYER_ACK arrives at match-start
     * (server-allocated P2 pid). WITHOUT this second sync, the C++
     * bridge's `g_Game.myPlayerID2 != 0xFF` death-check guard never
     * passes → CLIENT_DEATH_P2 (op 0x1D) is never sent → server's
     * alive_count never drops on P2 death → match doesn't end on
     * kill. (User-reported bug, root-caused 2026-04-28.) */
    {
        const unet_state_data_t* nd = unet_get_data();
        if (nd->my_player_id != 0xFF && g_Game.myPlayerID != nd->my_player_id) {
            g_Game.myPlayerID = nd->my_player_id;
        }
        if (nd->my_player_id2 != 0xFF && g_Game.myPlayerID2 != nd->my_player_id2) {
            g_Game.myPlayerID2 = nd->my_player_id2;
        }
    }

    /* Screen dispatch by gameState */
    switch (g_Game.gameState) {
    case UGAME_STATE_NAME_ENTRY:
        nameEntry_input();
        nameEntry_draw();
        break;
    case UGAME_STATE_CONNECTING:
        connecting_input();
        connecting_update();
        connecting_draw();
        break;
    case UGAME_STATE_LOBBY:
        lobby_input();
        lobby_update();
        lobby_draw();
        break;
    case UGAME_STATE_MAP_PICK:
        map_pick_input();
        map_pick_draw();
        break;
    case UGAME_STATE_DOWNLOAD_CHARS:
        cc_download_input();
        cc_download_tick();
        cc_download_draw();
        break;
    default:
        break;
    }

    led_tick();
}
