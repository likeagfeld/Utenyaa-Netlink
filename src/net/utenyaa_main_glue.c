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

void unet_glue_init(void)
{
    static const struct { uint32_t base; uint32_t stride; } addrs[] = {
        { 0x25895001, 4 },
        { 0x04895001, 4 }
    };
    int i;

    /* Default game state — offline until user picks ONLINE from menu */
    memset(&g_Game, 0, sizeof(g_Game));
    g_Game.gameState = UGAME_STATE_NONE;
    g_Game.myPlayerID = 0xFF;
    g_Game.myPlayerID2 = 0xFF;
    g_Game.myCharacter = 0xFF;
    g_Game.myCharacter2 = 0xFF;
    g_Game.myStageVote = 0xFF;
    g_Game.titleScreenChoice = 0;

    /* Load bitmap font (FONT.TGA in cd root) */
    font_load();

    /* Init net client */
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
    g_Game.isOnlineMode = true;
    g_Game.gameState = UGAME_STATE_NAME_ENTRY;
    nameEntry_init();
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
    /* Network state machine (runs every frame once transport is live) */
    unet_tick();

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
    default:
        break;
    }

    led_tick();
}
