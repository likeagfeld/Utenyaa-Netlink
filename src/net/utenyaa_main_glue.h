/**
 * utenyaa_main_glue.h - Bridges C++ main.cxx and the C networking layer
 *
 * Owns the module-level singletons (g_Game, g_uart, g_modem_detected,
 * g_saturn_transport), the transport callback functions, and the
 * one-time networking init + per-frame network_tick. main.cxx calls
 * only two entry points: unet_glue_init() and unet_glue_tick_frame().
 */

#ifndef UTENYAA_MAIN_GLUE_H
#define UTENYAA_MAIN_GLUE_H

#include "utenyaa_game.h"
#include "saturn_uart16550.h"
#include "net_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Module-wide singletons — extern decls here, defined in .c */
extern utenyaa_game_t       g_Game;
extern saturn_uart16550_t   g_uart;
extern bool                 g_modem_detected;
extern net_transport_t      g_saturn_transport;

/** Call once at startup after jo_core_init(). Detects modem, inits net. */
void unet_glue_init(void);

/** Call every frame. Runs unet_tick + routes screens by g_Game.gameState. */
void unet_glue_tick_frame(void);

/** Start the online flow (called from the MainMenu ONLINE button). */
void unet_glue_enter_online(void);

/** True iff gameState indicates an online screen should render this frame
 *  (so the C++ offline renderer skips its logo/title path). */
bool unet_glue_is_online_screen_active(void);

#ifdef __cplusplus
}
#endif

#endif /* UTENYAA_MAIN_GLUE_H */
