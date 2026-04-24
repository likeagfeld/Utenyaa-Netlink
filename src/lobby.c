/**
 * lobby.c - Online lobby screen (STUB — full implementation in Phase 3b)
 *
 * Placeholder so connecting.c can link. Full lobby with character grid,
 * stage vote tiles, ready/bot controls, post-match results with clearer
 * column headers (WINS / BEST HP / KILLS / DEATHS / MATCHES), and the
 * Z-button leaderboard overlay is implemented in a follow-up pass.
 */

#include <jo/jo.h>
#include "lobby.h"
#include "font.h"
#include "net/utenyaa_game.h"
#include "net/utenyaa_net.h"

void lobby_init(void)
{
    jo_clear_screen();
    unet_clear_log();
}

void lobby_input(void)
{
    if (g_Game.gameState != UGAME_STATE_LOBBY) return;
    /* TODO: ready toggle, character select, stage vote, add/remove bot,
     *       start game, disconnect, Z-overlay leaderboard. */
}

void lobby_update(void)
{
    if (g_Game.gameState != UGAME_STATE_LOBBY) return;
    /* TODO: auto-start transition when GAME_START arrives (handled via
     *       unet_get_state() == UNET_STATE_PLAYING). */
}

void lobby_draw(void)
{
    const unet_state_data_t* nd;
    int i;

    if (g_Game.gameState != UGAME_STATE_LOBBY) return;

    nd = unet_get_data();

    font_draw_centered("UTENYAA LOBBY", FONT_Y(3), 500);

    for (i = 0; i < UNET_MAX_PLAYERS; i++) {
        if (!nd->lobby_players[i].active) continue;
        font_printf(FONT_X(4), FONT_Y(7 + i), 500,
                    "%c %-16s %s",
                    nd->lobby_players[i].id == nd->my_player_id ? '>' : ' ',
                    nd->lobby_players[i].name,
                    nd->lobby_players[i].ready ? "READY" : "     ");
    }

    font_draw_centered("STUB - FULL UI IN PHASE 3B", FONT_Y(26), 500);
}
