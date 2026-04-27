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
    uint8_t c = from;
    int tries = 0;
    for (tries = 0; tries < UNET_MAX_CHARACTERS; tries++) {
        if (direction > 0) c = (uint8_t)((c + 1) % UNET_MAX_CHARACTERS);
        else                c = (uint8_t)((c + UNET_MAX_CHARACTERS - 1) % UNET_MAX_CHARACTERS);
        if (!character_taken(c, g_Game.myPlayerID)) return c;
    }
    return from;  /* all taken (shouldn't happen with 4-max-players / 12-chars) */
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

    /* If a disconnect confirmation is pending, only A/C/B/Y/START are
     * meaningful — gate everything else so the user can't accidentally
     * change ready state, character, vote, etc. while the prompt is up. */
    if (g_confirm != LOBBY_CONFIRM_NONE) {
        const bool a = jo_is_pad1_key_pressed(JO_KEY_A) || jo_is_pad1_key_pressed(JO_KEY_C);
        const bool cancel = jo_is_pad1_key_pressed(JO_KEY_B) ||
                            jo_is_pad1_key_pressed(JO_KEY_Y) ||
                            jo_is_pad1_key_pressed(JO_KEY_START);

        if (a && !g_Game.input.pressedABC) {
            /* Confirmed — execute the action. */
            unet_send_disconnect();
            modem_hangup(&g_uart);
            jo_clear_screen();
            g_Game.input.pressedABC = true;
            g_Game.input.pressedLT = true;
            g_Game.input.pressedRT = true;
            g_Game.titleScreenChoice = 2;
            g_Game.isOnlineMode = false;
            g_Game.gameState = UGAME_STATE_TITLE_SCREEN;
            g_confirm = LOBBY_CONFIRM_NONE;
            return;
        }
        if (cancel) {
            /* Any of B/Y/START while prompt is up = cancel. */
            g_confirm = LOBBY_CONFIRM_NONE;
            g_Game.input.pressedLT = true;
            g_Game.input.pressedRT = true;
            g_Game.input.pressedStart = true;
        }
        if (a) g_Game.input.pressedABC = true; else g_Game.input.pressedABC = false;
        return;
    }

    /* A/C = ready toggle */
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

    /* B = arm "back to title" confirmation. Prompt is shown by lobby_draw;
     * second A/C confirms, B/Y/START cancels. */
    if (jo_is_pad1_key_pressed(JO_KEY_B)) {
        if (!g_Game.input.pressedLT) {
            g_confirm = LOBBY_CONFIRM_BACK_TO_TITLE;
        }
        g_Game.input.pressedLT = true;
    } else {
        g_Game.input.pressedLT = false;
    }

    /* Y = arm "disconnect" confirmation. */
    if (jo_is_pad1_key_pressed(JO_KEY_Y)) {
        if (!g_Game.input.pressedRT) {
            g_confirm = LOBBY_CONFIRM_DISCONNECT;
        }
        g_Game.input.pressedRT = true;
    } else {
        g_Game.input.pressedRT = false;
    }

    /* UP = add bot */
    if (jo_is_pad1_key_pressed(JO_KEY_UP)) {
        if (!g_Game.input.pressedUp) unet_send_bot_add();
        g_Game.input.pressedUp = true;
    } else {
        g_Game.input.pressedUp = false;
    }

    /* DOWN = remove bot */
    if (jo_is_pad1_key_pressed(JO_KEY_DOWN)) {
        if (!g_Game.input.pressedDown) unet_send_bot_remove();
        g_Game.input.pressedDown = true;
    } else {
        g_Game.input.pressedDown = false;
    }

    /* L trigger = previous character (skip taken) */
    if (jo_is_pad1_key_pressed(JO_KEY_L)) {
        if (!g_ltrig_pressed) {
            uint8_t cur = (g_Game.myCharacter == 0xFF) ? 0 : g_Game.myCharacter;
            uint8_t next = next_available_character(cur, -1);
            unet_send_character_select(next);
            g_Game.myCharacter = next;
        }
        g_ltrig_pressed = true;
    } else {
        g_ltrig_pressed = false;
    }

    /* R trigger = next character (skip taken) */
    if (jo_is_pad1_key_pressed(JO_KEY_R)) {
        if (!g_rtrig_pressed) {
            uint8_t cur = (g_Game.myCharacter == 0xFF) ? 0 : g_Game.myCharacter;
            uint8_t next = next_available_character(cur, +1);
            unet_send_character_select(next);
            g_Game.myCharacter = next;
        }
        g_rtrig_pressed = true;
    } else {
        g_rtrig_pressed = false;
    }

    /* X = cycle stage vote */
    if (jo_is_pad1_key_pressed(JO_KEY_X)) {
        if (!g_x_pressed) {
            uint8_t cur = (g_Game.myStageVote == 0xFF) ? 0xFF : g_Game.myStageVote;
            uint8_t next = (cur == 0xFF) ? 0 : (uint8_t)((cur + 1) % UNET_STAGE_COUNT);
            unet_send_stage_vote(next);
            g_Game.myStageVote = next;
        }
        g_x_pressed = true;
    } else {
        g_x_pressed = false;
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

    /* Clear NBG0 each frame — roster / ready states / vote tallies
     * change every frame and jo_printf doesn't auto-erase overwritten
     * cells, so stale rows would accumulate without this. */
    jo_clear_screen();

    font_draw_centered("UTENYAA LOBBY", FONT_Y(2), 500);
    font_printf(FONT_X(1), FONT_Y(4), 500,
                "PLAYERS: %d/%d", nd->lobby_count, UNET_MAX_PLAYERS);

    if (g_z_held) {
        draw_z_overlay(nd);
        g_z_was_held = true;
        goto skip_player_list;
    } else if (g_z_was_held) {
        g_z_was_held = false;
        g_z_page_timer = 0;
        g_z_page = 0;
    }

    /* Player roster: NAME · CHAR · VOTE · READY */
    font_draw("#  NAME             CHAR  VOTE    STATUS",
              FONT_X(1), FONT_Y(6), 500);
    for (i = 0; i < nd->lobby_count && i < UNET_MAX_PLAYERS; i++) {
        const unet_lobby_player_t* lp = &nd->lobby_players[i];
        const char* vote_str;
        char char_str[4];
        char marker;

        if (!lp->active) continue;
        marker = (lp->id == g_Game.myPlayerID) ? '>' : ' ';

        if (lp->character_id == 0xFF) {
            char_str[0] = '-'; char_str[1] = '-'; char_str[2] = '\0';
        } else {
            sprintf(char_str, "%02d", (int)lp->character_id);
        }

        vote_str = (lp->stage_vote == 0xFF) ? "-----" : STAGE_NAMES[lp->stage_vote];

        font_printf(FONT_X(1), FONT_Y(7 + i), 500,
                    "%c%-2d %-16s  %2s  %-7s %s",
                    marker, i + 1, lp->name, char_str, vote_str,
                    lp->ready ? "READY" : "     ");
    }

    /* Stage vote tally */
    font_draw("STAGE VOTES:", FONT_X(1), FONT_Y(13), 500);
    for (i = 0; i < UNET_STAGE_COUNT; i++) {
        font_printf(FONT_X(2 + i * 9), FONT_Y(14), 500,
                    "%s:%d", STAGE_NAMES[i], nd->stage_vote_tally[i]);
    }

    /* Waiting / start gate */
    if (nd->lobby_count < UNET_MIN_TO_START) {
        font_draw_centered("WAITING FOR 2ND PLAYER...", FONT_Y(17), 500);
    } else {
        font_draw_centered("PRESS START WHEN READY", FONT_Y(17), 500);
    }

    /* Last log line — pad to 38 chars so shorter text doesn't leak
     * residue from a prior longer line (Disasteroids QA-pass fix). */
    if (nd->log_count > 0) {
        font_printf(FONT_X(1), FONT_Y(19), 500, "%-38s",
                    nd->log_lines[nd->log_count - 1]);
    } else {
        font_draw("                                      ", FONT_X(1), FONT_Y(19), 500);
    }

    /* P2 co-op status — full-width pad to erase prior state */
    if (g_Game.hasSecondLocal) {
        font_printf(FONT_X(1), FONT_Y(21), 500, "P2: %-34s", g_Game.playerName2);
    } else {
        font_draw("                                      ", FONT_X(1), FONT_Y(21), 500);
    }

    /* Controls hint — split across 3 lines for readability */
    font_draw("A:RDY  START:GO  L/R:CHAR  X:STAGE",
              FONT_X(1), FONT_Y(25), 500);
    font_draw("UP/DN:BOTS  B:BACK  Y:QUIT  Z:STATS",
              FONT_X(1), FONT_Y(26), 500);

    /* Diagnostic strip — last row. Numbers are persistent counters
     * since lazy_init_online ran. RX = total bytes read from UART,
     * FR = SNCP frames decoded, ER = parse errors (length mismatch),
     * OP = most recent opcode received, AU = auth retry count. */
    font_printf(FONT_X(1), FONT_Y(27), 500,
                "S:%d RX:%lu FR:%lu ER:%lu OP:%02X AU:%lu MY:%d",
                (int)unet_get_state(),
                (unsigned long)nd->diag_rx_bytes,
                (unsigned long)nd->diag_frames_decoded,
                (unsigned long)nd->diag_parse_errors,
                (unsigned)nd->diag_last_op,
                (unsigned long)nd->diag_auth_attempts,
                (int)nd->my_player_id);

    /* Disconnect-confirmation overlay — shown on top when armed. */
    if (g_confirm == LOBBY_CONFIRM_BACK_TO_TITLE) {
        font_draw_centered("                                  ", FONT_Y(13), 600);
        font_draw_centered("  RETURN TO TITLE SCREEN?         ", FONT_Y(14), 600);
        font_draw_centered("  THIS WILL DISCONNECT YOU.       ", FONT_Y(15), 600);
        font_draw_centered("  A = YES   B/Y/START = CANCEL    ", FONT_Y(17), 600);
    } else if (g_confirm == LOBBY_CONFIRM_DISCONNECT) {
        font_draw_centered("                                  ", FONT_Y(13), 600);
        font_draw_centered("  DISCONNECT FROM SERVER?         ", FONT_Y(14), 600);
        font_draw_centered("  YOU WILL HAVE TO RE-DIAL.       ", FONT_Y(15), 600);
        font_draw_centered("  A = YES   B/Y/START = CANCEL    ", FONT_Y(17), 600);
    }

skip_player_list:
    ;
}
