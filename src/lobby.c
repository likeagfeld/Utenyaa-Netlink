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
#include "net/utenyaa_map_stream.h"   /* streamed-map progress overlay */
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

    /* START = request game start. PURE start request — does NOT auto-
     * toggle ready. The previous auto-ready logic ("if !server_says_
     * ready && !unet_is_ready() then send_ready") could fire incorrectly
     * after a match-end → lobby return: when the local optimistic
     * my_ready flag and the server-broadcast lobby_players[].ready
     * field were briefly out of sync, the guard occasionally evaluated
     * to true even though the user had already pressed A to ready up,
     * sending a redundant unet_send_ready() that the server interpreted
     * as a TOGGLE — un-readying them. Operator-reported: "pressing
     * start just un-readies my player and doesnt advance".
     *
     * Simple model now: user presses A (or C) to ready, START to
     * advance. Server enforces the "all ready" check on START_GAME_REQ.
     */
    if (jo_is_pad1_key_pressed(JO_KEY_START)) {
        if (!g_Game.input.pressedStart) {
            unet_send_start_game();
        }
        g_Game.input.pressedStart = true;
    } else {
        g_Game.input.pressedStart = false;
    }

    /* L / R triggers cycle the local player's character selection.
     * Operator-requested visual sprite picker — pressing L picks the
     * previous available character, R picks the next, where "available"
     * means not already taken by another lobby player (per the existing
     * character_taken / next_available_character helpers).
     *
     * The actual sprite preview is drawn in lobby_draw — this handler
     * is just the input edge-detect + server send. */
    if (jo_is_pad1_key_pressed(JO_KEY_L)) {
        if (!g_ltrig_pressed) {
            const unet_state_data_t* nd = unet_get_data();
            uint8_t cur = 0;
            for (int k = 0; k < nd->lobby_count; k++) {
                if (nd->lobby_players[k].id == g_Game.myPlayerID) {
                    cur = nd->lobby_players[k].character_id;
                    if (cur >= (uint8_t)unet_glue_num_characters()) cur = 0;
                    break;
                }
            }
            uint8_t next = next_available_character(cur, -1);
            if (next != cur) unet_send_character_select(next);
        }
        g_ltrig_pressed = true;
    } else {
        g_ltrig_pressed = false;
    }
    if (jo_is_pad1_key_pressed(JO_KEY_R)) {
        if (!g_rtrig_pressed) {
            const unet_state_data_t* nd = unet_get_data();
            uint8_t cur = 0;
            for (int k = 0; k < nd->lobby_count; k++) {
                if (nd->lobby_players[k].id == g_Game.myPlayerID) {
                    cur = nd->lobby_players[k].character_id;
                    if (cur >= (uint8_t)unet_glue_num_characters()) cur = 0;
                    break;
                }
            }
            uint8_t next = next_available_character(cur, +1);
            if (next != cur) unet_send_character_select(next);
        }
        g_rtrig_pressed = true;
    } else {
        g_rtrig_pressed = false;
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
        /* Leaderboard page. NBG0 grid is 40 cols × 28 rows (320×224 PAL,
         * 8px cells); font.c silently drops draws with col < 0 or > 39,
         * so the format MUST stay ≤ 40 chars wide and start at FONT_X(0).
         * Previous "...WINS BEST HP KILLS DEATHS MATCHES" header was 57
         * chars from FONT_X(-6) — both off-screen — which is why the
         * page rendered as a blank under the title. */
        font_draw_centered("ONLINE LEADERBOARD", FONT_Y(6), 500);
        if (nd->leaderboard_count > 0) {
            /* Header (37 chars):
             *   "#  NAME             W  HP   K   D  GP" */
            font_draw("#  NAME             W  HP  K  D   GP",
                      FONT_X(2), FONT_Y(9), 500);
            for (i = 0; i < nd->leaderboard_count && i < 10; i++) {
                /* Per-row format (37 chars):
                 *   "<2d> <16s> <3d> <3d> <2d> <2d> <3d>" */
                font_printf(FONT_X(2), FONT_Y(10 + i), 500,
                            "%-2d %-16s %3d %3d %2d %2d %3d",
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
     * so it doesn't leave stale text.
     *
     * NB: game_roster is filled by lobby slot index in on_game_start,
     * but each entry stores the player's server-assigned game_pid
     * (game_roster[i].id). winner_id is a pid, NOT a slot — must
     * look up by id match. Previous code did game_roster[winner_id]
     * direct-index, which displayed the WRONG name whenever the
     * ready-order pid assignment didn't match the lobby join order
     * (e.g. player A in slot 0 ready'd second → got pid 1; player B
     * in slot 1 ready'd first → got pid 0; on B's win, banner showed
     * A's name). Found from playtest log: "wrong winner displayed
     * back at lobby... not always but often". */
    {
        const char* winner_name = NULL;
        if (nd->has_last_results && nd->last_winner_id < UNET_MAX_PLAYERS) {
            for (int k = 0; k < nd->game_roster_count; k++) {
                if (nd->game_roster[k].active &&
                    nd->game_roster[k].id == nd->last_winner_id) {
                    winner_name = nd->game_roster[k].name;
                    break;
                }
            }
        }
        if (winner_name) {
            font_printf(FONT_X(1), FONT_Y(5), 500,
                        "LAST WINNER: %-20s", winner_name);
        } else {
            font_draw("                                      ",
                      FONT_X(1), FONT_Y(5), 500);
        }
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

    /* Waiting / start gate.
     *
     * "PRESS START WHEN READY" is 22 chars and "WAITING FOR 2ND
     * PLAYER..." is 25, so when font_draw_centered switches from
     * WAITING to PRESS the centered column shifts by ~1.5 chars,
     * leaving the leading 'W' from WAITING bleeding through at the
     * left edge of the PRESS line ("WPRESS START..."). The text
     * plane has no per-frame clear at this row, so prior content
     * survives to the next draw. Pad both messages to identical
     * 25-char width so they share the same centered start column
     * and overwrite each other's footprint cleanly. */
    if (nd->lobby_count < UNET_MIN_TO_START) {
        font_draw_centered("WAITING FOR 2ND PLAYER...", FONT_Y(17), 500);
    } else {
        font_draw_centered(" PRESS START WHEN READY  ", FONT_Y(17), 500);
    }

    /* Last log line — pad to 38 chars so shorter text doesn't leak
     * residue from a prior longer line. */
    if (nd->log_count > 0) {
        font_printf(FONT_X(1), FONT_Y(19), 500, "%-38s",
                    nd->log_lines[nd->log_count - 1]);
    } else {
        font_draw("                                      ", FONT_X(1), FONT_Y(19), 500);
    }

    /* Controls hint — bumped to include L/R character cycle */
    font_draw("A:READY  START:GO  L/R:CHAR  Y:OUT",
              FONT_X(1), FONT_Y(25), 500);
    font_draw("HOLD Z FOR LEADERBOARD",
              FONT_X(1), FONT_Y(26), 500);

    /* Character sprite preview for the LOCAL player. Draws the
     * "south" frame (frame 0) of whichever character_id the server
     * has assigned/confirmed for our pid.
     *
     * Two operator-reported regressions fixed here:
     *
     *  (a) The previous "YOU" label used `font_draw_centered("YOU",
     *      30, 500)` — but font_draw_centered's y arg is in PIXEL
     *      coords; px_to_row(30) = (30+112)/8 = 17. That landed the
     *      "YOU" string on row 17, smack in the middle of the
     *      "PRESS START WHEN READY" line, producing the operator-
     *      reported visual corruption "PRESS STARYOUHEN READY".
     *      Use font_draw with FONT_X/FONT_Y so the row is explicit.
     *
     *  (b) jo_sprite_draw3D uses slDispSprite which is 3D-projected,
     *      requiring a valid SGL camera matrix. The online-screen
     *      branch in main.cxx short-circuits the gameplay world-
     *      render path (which is what normally calls
     *      jo_3d_camera_look_at) — so the matrix was stale and the
     *      sprite projected to undefined / off-screen coords.
     *      Fixed by adding a one-line jo_3d_camera_look_at(&camera)
     *      at the top of the per-frame loop in main.cxx so the
     *      matrix is current before any online-screen sprite call. */
    {
        uint8_t my_char = 0xFF;
        for (int k = 0; k < nd->lobby_count; k++) {
            if (nd->lobby_players[k].id == g_Game.myPlayerID) {
                my_char = nd->lobby_players[k].character_id;
                break;
            }
        }
        if (my_char != 0xFF
            && my_char < (uint8_t)unet_glue_num_characters()) {
            int sprite_id = unet_glue_character_sprite_for(my_char);
            if (sprite_id >= 0) {
                /* Position in screen-centered coords (origin at TV
                 * middle). x=110 = right side, y=-72 = upper area
                 * (negative Y is up in SGL screen space). z=500
                 * matches the font plane depth. */
                jo_sprite_draw3D(sprite_id, 110, -72, 500);
            }
            /* Numeric character index hint so the operator gets
             * unambiguous L/R feedback even when two characters
             * happen to share a similar built-in pose. */
            font_printf(FONT_X(31), FONT_Y(3), 500, "CHAR %d", (int)my_char);
        } else {
            /* Pad the label area when no preview is drawable so a
             * previous frame's CHAR-N label doesn't linger. */
            font_draw("       ", FONT_X(31), FONT_Y(3), 500);
        }
        /* "YOU" label above the preview — col 32 puts it near the
         * sprite (sprite x=110 in centered coords ≈ pixel col 33).
         * Row 2 is above the player-list header (row 6) and clear of
         * the "UTENYAA LOBBY" centered title (cols ~13-26). */
        font_draw("YOU", FONT_X(32), FONT_Y(2), 500);
    }

    /* Disconnect-confirmation overlay removed in simplified mode —
     * B now leaves lobby immediately (see lobby_input). */

skip_player_list:

    /* Streamed-map progress overlay. When the server is mid-stream
     * (g_map_stream.active = true between MAP_BEGIN and MAP_END), the
     * client is still in lobby state — overlay a single-line progress
     * bar at the bottom of the screen so the operator/players know
     * the wait is intentional. The full transfer is ~7-8s @ 14.4k for
     * an 11 KB .UTE; a frozen-looking lobby would be confusing.
     * Once g_map_stream.ready flips true, the next GAME_START will
     * transition to gameplay and the World ctor consumes the buffer. */
    if (g_map_stream.active && g_map_stream.total_size > 0) {
        int pct = (int)((long long)g_map_stream.received_bytes * 100
                        / (long long)g_map_stream.total_size);
        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;
        font_printf(FONT_X(1), FONT_Y(27), 540,
                    "STREAMING MAP... %3d%% (%d/%d B)",
                    pct,
                    g_map_stream.received_bytes,
                    g_map_stream.total_size);
    } else if (g_map_stream.ready) {
        /* Ready — buffered, validated, awaiting GAME_START. */
        font_draw("MAP LOADED — WAITING FOR MATCH START",
                  FONT_X(1), FONT_Y(27), 540);
    }
    ;
}
