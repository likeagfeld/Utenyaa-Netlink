/**
 * lobby.c - Online Lobby Screen for Utenyaa
 *
 * Shows up to 4 connected players with ready states, character selections,
 * and stage vote tally. Handles lobby controls and game-start transition.
 *
 * Controls:
 *   A/C        toggle ready
 *   START      request game start (auto-readies if needed)
 *   L/R        cycle character (unique per player, skip taken)
 *   X          cycle stage vote (Island / Cross / Valley / Railway)
 *   UP/DOWN    add/remove bot
 *   B          back to title (stay connected)
 *   Y          disconnect
 *   Z (hold)   stats overlay — last-game results / online leaderboard
 *
 * Adapted from Flicky's Flock lobby.c. Column headers on the Z overlay
 * use full words per user feedback (WINS / BEST HP / KILLS / DEATHS /
 * MATCHES) — the Flicky/Disasteroids abbreviations were unclear.
 */

#include <jo/jo.h>
#include "lobby.h"
#include "font.h"
#include "net/utenyaa_game.h"
#include "net/utenyaa_net.h"
#include "net/saturn_uart16550.h"
#include "net/modem.h"

extern saturn_uart16550_t g_uart;

/*============================================================================
 * Edge-tracking state
 *============================================================================*/

static bool g_z_held = false;
static bool g_z_was_held = false;
static int  g_z_page_timer = 0;
static int  g_z_page = 0;
static int  g_z_last_drawn_page = -1;  /* tracks which Z page rendered last frame; -1 = none */
#define Z_PAGE_INTERVAL 180  /* 3s @ 60fps */

static bool g_ltrig_pressed = false;
static bool g_rtrig_pressed = false;
static bool g_x_pressed = false;

/* Disconnect confirmation prompt — set when user presses B or Y so
 * an accidental press doesn't kick them out of the lobby. */
typedef enum {
    LOBBY_CONFIRM_NONE = 0,
    LOBBY_CONFIRM_BACK_TO_TITLE,    /* B pressed */
    LOBBY_CONFIRM_DISCONNECT        /* Y pressed */
} lobby_confirm_t;
static lobby_confirm_t g_confirm = LOBBY_CONFIRM_NONE;

/*============================================================================
 * Stage names (indexed by UNET_STAGE_*)
 *============================================================================*/

static const char* const STAGE_NAMES[UNET_STAGE_COUNT] = {
    "ISLAND", "CROSS", "VALLEY", "RAILWAY"
};

/*============================================================================
 * P2 hot-plug helper (Jo Engine Port B → inputs[6] quirk matches Flicky) */
/*============================================================================*/

static int getP2Port(void)
{
    /* Utenyaa's jo_engine PerDigital struct doesn't expose mask/type
     * the way Flicky's newer one does. Use the portable availability
     * check: any controller beyond the primary (index 0) is "P2". */
    int count = 0, i;
    for (i = 0; i < JO_INPUT_MAX_DEVICE; i++) {
        if (jo_is_input_available(i)) {
            if (count == 1) return i;
            count++;
        }
    }
    return -1;
}

/*============================================================================
 * Character uniqueness helper
 *============================================================================*/

static bool character_taken(uint8_t char_id, uint8_t my_id)
{
    const unet_state_data_t* nd = unet_get_data();
    int i;
    for (i = 0; i < nd->lobby_count; i++) {
        if (!nd->lobby_players[i].active) continue;
        if (nd->lobby_players[i].id == my_id) continue;
        if (nd->lobby_players[i].character_id == char_id) return true;
    }
    return false;
}

static uint8_t next_available_character(uint8_t from, int direction)
{
    /* Clamp cycling to the actual asset count from CHARS.PAK
     * (5 distinct characters × 5 frames each), not the protocol's
     * UNET_MAX_CHARACTERS = 12 ceiling. */
    int max = unet_glue_num_characters();
    if (max < 1) max = 1;
    uint8_t c = (from < (uint8_t)max) ? from : 0;
    int tries = 0;
    for (tries = 0; tries < max; tries++) {
        if (direction > 0) c = (uint8_t)((c + 1) % max);
        else               c = (uint8_t)((c + max - 1) % max);
        if (!character_taken(c, g_Game.myPlayerID)) return c;
    }
    return from;  /* all taken (shouldn't happen with 4-max-players / 5-chars) */
}

/*============================================================================
 * Lifecycle
 *============================================================================*/

void lobby_init(void)
{
    g_z_held = false;
    g_z_was_held = false;
    g_z_page_timer = 0;
    g_z_page = 0;
    g_ltrig_pressed = false;
    g_rtrig_pressed = false;
    g_x_pressed = false;
    g_confirm = LOBBY_CONFIRM_NONE;

    unet_clear_log();
    jo_clear_screen();
    unet_request_leaderboard();
}

/*============================================================================
 * Input
 *============================================================================*/

void lobby_input(void)
{
    if (g_Game.gameState != UGAME_STATE_LOBBY) return;

    /* SIMPLIFIED LOBBY (per user: 'keep our online implementation as
     * simple as possible to troubleshoot' / 'we dont need bots' /
     * 'lets make sure that the disconnect button is mapped to Y
     * only from the lobby... thats it... nothing else should
     * disconnect or return to title screen').
     *
     * Inputs handled — exactly three, no others:
     *   A / C  → toggle ready
     *   START  → request game start
     *   Y      → disconnect (only path back to title screen) */

    /* A / C = ready toggle */
    if (jo_is_pad1_key_pressed(JO_KEY_A) || jo_is_pad1_key_pressed(JO_KEY_C)) {
        if (!g_Game.input.pressedABC) unet_send_ready();
        g_Game.input.pressedABC = true;
    } else {
        g_Game.input.pressedABC = false;
    }

    /* START = request game start */
    if (jo_is_pad1_key_pressed(JO_KEY_START)) {
        if (!g_Game.input.pressedStart) {
            const unet_state_data_t* nd = unet_get_data();
            bool server_says_ready = false;
            int k;
            for (k = 0; k < nd->lobby_count; k++) {
                if (nd->lobby_players[k].id == g_Game.myPlayerID) {
                    server_says_ready = nd->lobby_players[k].ready;
                    break;
                }
            }
            if (!server_says_ready && !unet_is_ready()) unet_send_ready();
            unet_send_start_game();
        }
        g_Game.input.pressedStart = true;
    } else {
        g_Game.input.pressedStart = false;
    }

    /* Y = disconnect — the ONLY way out of the lobby. No B, no Start,
     * no other path triggers disconnect or title return. */
    if (jo_is_pad1_key_pressed(JO_KEY_Y)) {
        if (!g_Game.input.pressedRT) {
            unet_send_disconnect();
            modem_hangup(&g_uart);
            jo_clear_screen();
            g_Game.titleScreenChoice = 2;
            g_Game.isOnlineMode = false;
            g_Game.gameState = UGAME_STATE_TITLE_SCREEN;
        }
        g_Game.input.pressedRT = true;
    } else {
        g_Game.input.pressedRT = false;
    }

    /* Z = stats overlay */
    g_z_held = jo_is_pad1_key_pressed(JO_KEY_Z) ? true : false;
}

/*============================================================================
 * Update
 *============================================================================*/

void lobby_update(void)
{
    if (g_Game.gameState != UGAME_STATE_LOBBY) return;

    /* P2 hot-plug */
    if (!g_Game.hasSecondLocal && getP2Port() >= 0) {
        int p2len = 0, i;
        g_Game.hasSecondLocal = true;
        while (g_Game.playerName[p2len] && p2len < UNET_MAX_NAME) p2len++;
        for (i = 0; i < p2len; i++) g_Game.playerName2[i] = g_Game.playerName[i];
        if (p2len < UNET_MAX_NAME) {
            g_Game.playerName2[p2len] = '2';
            g_Game.playerName2[p2len + 1] = '\0';
        } else {
            g_Game.playerName2[UNET_MAX_NAME - 1] = '2';
            g_Game.playerName2[UNET_MAX_NAME] = '\0';
        }
        g_Game.myPlayerID2 = 0xFF;
        unet_send_add_local_player(g_Game.playerName2);
    } else if (g_Game.hasSecondLocal && getP2Port() < 0) {
        g_Game.hasSecondLocal = false;
        g_Game.myPlayerID2 = 0xFF;
        g_Game.playerName2[0] = '\0';
        unet_send_remove_local_player();
    }

    /* Server transitioned us to PLAYING → switch gamestate. The C++
     * engine watches g_Game.gameState == UGAME_STATE_GAMEPLAY to spin
     * up World and run the match. */
    if (unet_get_state() == UNET_STATE_PLAYING) {
        const unet_state_data_t* nd = unet_get_data();
        g_Game.isOnlineMode = true;
        g_Game.myPlayerID = nd->my_player_id;
        jo_clear_screen();
        g_Game.gameState = UGAME_STATE_GAMEPLAY;
    }

    if (unet_get_state() == UNET_STATE_DISCONNECTED) {
        jo_clear_screen();
        g_Game.gameState = UGAME_STATE_TITLE_SCREEN;
    }
}

/*============================================================================
 * Z overlay — results / leaderboard with clearer column headers
 *============================================================================*/

static void draw_z_overlay(const unet_state_data_t* nd)
{
    int i;

    g_z_page_timer++;
    if (g_z_page_timer >= Z_PAGE_INTERVAL) {
        g_z_page_timer = 0;
        g_z_page = 1 - g_z_page;
    }

    /* Effective page = the page actually about to render (page 0 falls
     * back to leaderboard when no last-game results yet). Clear NBG0
     * once on every page flip so the two views — different column
     * widths, different X anchors, different row counts — never bleed
     * into each other. */
    int effective_page = (g_z_page == 0 && nd->has_last_results) ? 0 : 1;
    if (effective_page != g_z_last_drawn_page) {
        jo_clear_screen();
        g_z_last_drawn_page = effective_page;
    }

    if (g_z_page == 0 && nd->has_last_results) {
        /* Page 0: Last game results — full-word column headers. */
        font_draw_centered("LAST GAME RESULTS", FONT_Y(6), 500);
        font_draw_centered(nd->last_sudden_death ? "(SUDDEN DEATH)" : "",
                           FONT_Y(7), 500);
        font_draw("#  NAME             HP  KILLS  DEATHS",
                  FONT_X(1), FONT_Y(9), 500);
        for (i = 0; i < nd->game_roster_count && i < UNET_MAX_PLAYERS; i++) {
            if (!nd->game_roster[i].active) continue;
            uint8_t pid = nd->game_roster[i].id;
            bool is_winner = (pid == nd->last_winner_id);
            font_printf(FONT_X(1), FONT_Y(10 + i), 500,
                        "%c%-2d %-16s %2d  %4d  %4d",
                        is_winner ? '*' : ' ',
                        i + 1,
                        nd->game_roster[i].name,
                        nd->last_scores_hp[pid],
                        nd->last_scores_kills[pid],
                        nd->last_scores_deaths[pid]);
        }
        font_draw("* = WINNER", FONT_X(1), FONT_Y(16), 500);
    } else {
        /* Leaderboard page — clearer column headers. */
        font_draw_centered("ONLINE LEADERBOARD", FONT_Y(6), 500);
        if (nd->leaderboard_count > 0) {
            font_draw("#  NAME             WINS  BEST HP  KILLS  DEATHS  MATCHES",
                      FONT_X(-6), FONT_Y(9), 500);
            for (i = 0; i < nd->leaderboard_count && i < 10; i++) {
                font_printf(FONT_X(-6), FONT_Y(10 + i), 500,
                            "%-2d %-16s %4d  %7d  %5d  %6d  %7d",
                            i + 1,
                            nd->leaderboard[i].name,
                            nd->leaderboard[i].wins,
                            nd->leaderboard[i].best_hp,
                            nd->leaderboard[i].kills,
                            nd->leaderboard[i].deaths,
                            nd->leaderboard[i].games_played);
            }
        } else {
            font_draw_centered("NO DATA YET", FONT_Y(14), 500);
        }
    }

    font_printf(FONT_X(2), FONT_Y(22), 500,
                "Z: %s", (g_z_page == 0 && nd->has_last_results) ? "RESULTS" : "LEADERS");
}

/*============================================================================
 * Draw
 *============================================================================*/

void lobby_draw(void)
{
    const unet_state_data_t* nd;
    int i;

    if (g_Game.gameState != UGAME_STATE_LOBBY) return;
    nd = unet_get_data();

    /* DO NOT call jo_clear_screen() per frame — that wipes NBG0 cells
     * mid-display and causes the visible strobe on the "UTENYAA LOBBY"
     * title and other text rows. Instead, every text write below uses
     * a fixed-width format (e.g. "%-16s") so old characters are
     * overwritten in place. ONE jo_clear_screen() runs only on the
     * Z-toggle transition (lobby ↔ leaderboard) so the two views
     * don't overlay each other. */
    if (g_z_held != g_z_was_held) {
        jo_clear_screen();
    }

    /* Z held: replace the entire lobby render with the leaderboard
     * overlay (no header, no roster, no controls — just the board). */
    if (g_z_held) {
        draw_z_overlay(nd);
        g_z_was_held = true;
        goto skip_player_list;
    } else if (g_z_was_held) {
        /* Z just released — reset paging state for next time. */
        g_z_was_held = false;
        g_z_page_timer = 0;
        g_z_page = 0;
        g_z_last_drawn_page = -1;
    }

    font_draw_centered("UTENYAA LOBBY", FONT_Y(2), 500);
    font_printf(FONT_X(1), FONT_Y(4), 500,
                "PLAYERS: %d/%d   FIRST 4 READY PLAY ",
                nd->lobby_count, UNET_MAX_LOBBY);

    /* Persistent WINNER banner — shows after a match ends, stays
     * until the next match begins (on_game_start clears
     * has_last_results). Cleared row when no results to show
     * so it doesn't leave stale text. */
    if (nd->has_last_results &&
        nd->last_winner_id < UNET_MAX_PLAYERS &&
        nd->game_roster[nd->last_winner_id].active)
    {
        font_printf(FONT_X(1), FONT_Y(5), 500,
                    "LAST WINNER: %-20s",
                    nd->game_roster[nd->last_winner_id].name);
    }
    else
    {
        font_draw("                                      ",
                  FONT_X(1), FONT_Y(5), 500);
    }

    /* Simplified roster: name + ready status only. Character + stage
     * are auto-assigned by server in this minimal mode (per user
     * directive: 'remove voting on maps and sprite selection... have
     * it auto assigned for now... keep our online implementation as
     * simple as possible to troubleshoot'). No VDP1 sprites, no
     * "P2:" line, no vote tally — these were all writing strings
     * containing 'P2: ' / '2: ' bytes that recurred as the literal
     * value of the recurring jo_free 'Bad pointer 0x..323A20' crash. */
    font_draw("#  NAME             STATUS", FONT_X(1), FONT_Y(6), 500);
    for (i = 0; i < UNET_MAX_LOBBY; i++) {
        if (i < nd->lobby_count && nd->lobby_players[i].active) {
            const unet_lobby_player_t* lp = &nd->lobby_players[i];
            char marker = (lp->id == g_Game.myPlayerID) ? '>' : ' ';
            font_printf(FONT_X(1), FONT_Y(7 + i), 500,
                        "%c%-2d %-16s %s",
                        marker, i + 1, lp->name,
                        lp->ready ? "READY" : "     ");
        } else {
            /* Pad vacated slot with spaces so a previously-occupied
             * row doesn't show stale data after a player leaves
             * (no per-frame jo_clear_screen anymore). */
            font_draw("                            ",
                      FONT_X(1), FONT_Y(7 + i), 500);
        }
    }

    /* Waiting / start gate */
    if (nd->lobby_count < UNET_MIN_TO_START) {
        font_draw_centered("WAITING FOR 2ND PLAYER...", FONT_Y(17), 500);
    } else {
        font_draw_centered("PRESS START WHEN READY", FONT_Y(17), 500);
    }

    /* Last log line — pad to 38 chars so shorter text doesn't leak
     * residue from a prior longer line. */
    if (nd->log_count > 0) {
        font_printf(FONT_X(1), FONT_Y(19), 500, "%-38s",
                    nd->log_lines[nd->log_count - 1]);
    } else {
        font_draw("                                      ", FONT_X(1), FONT_Y(19), 500);
    }

    /* Controls hint */
    font_draw("A:READY  START:GO  Y:DISCONNECT",
              FONT_X(1), FONT_Y(25), 500);
    font_draw("HOLD Z FOR LEADERBOARD",
              FONT_X(1), FONT_Y(26), 500);

    /* Disconnect-confirmation overlay removed in simplified mode —
     * B now leaves lobby immediately (see lobby_input). */

skip_player_list:
    ;
}
