/**
 * map_pick.c - Implementation of the in-lobby scrollable map picker.
 * See map_pick.h for design notes.
 */

#include "map_pick.h"
#include "font.h"
#include "net/utenyaa_net.h"
#include "net/utenyaa_game.h"
#include "net/utenyaa_map_stream.h"   /* streaming progress overlay */
#include <jo/jo.h>
#include <string.h>

unet_map_pick_t g_map_pick;

/* Saturn 320x240 PAL with 8x8 font fits ~38 columns total before the
 * right edge clips. Picker layout (zero-indexed col positions):
 *
 *   col 1: cursor marker (1)  '>'  '*' (winner)  ' '
 *   col 3..18: name (16 chars max, truncated)
 *   col 20..31: author (12 chars max, truncated)
 *   col 33..36: vote count, formatted as "v=N" or "v=NN"
 *
 * Total = ~37 cols, fits within the visible 38-col window. The size
 * column was dropped (less useful than seeing votes during picks),
 * matching the user's "text bleeding offscreen" feedback. */
#define MAP_PICK_VIEWPORT_ROWS  18
#define MAP_PICK_FIRST_ROW       4
#define MAP_PICK_ROW_X           1

/* Edge-detection state for input — pad polling fires every frame at
 * ~50 Hz (PAL); without rising-edge tracking a held UP/DOWN scrolls
 * past the entire list in 200 ms. */
static bool s_was_up, s_was_down, s_was_a, s_was_start;
static bool s_screen_clear_pending = true;
/* When MAP_PICK is entered, the operator's START press from the
 * lobby may still be held — without this guard the held button
 * reads as a NEW press on frame 1 of MAP_PICK and instantly
 * commits the pick. Track which controller buttons we should treat
 * as "already pressed" until they're physically released. */
static bool s_swallow_held = true;
/* Local "did I vote, and for what?" — for UI feedback. Doesn't
 * affect server-side tally. */
static int  s_my_vote = -1;

static void s_clamp_cursor_into_view(void)
{
    if (g_map_pick.count == 0) {
        g_map_pick.cursor = 0;
        g_map_pick.scroll_top = 0;
        return;
    }
    if (g_map_pick.cursor >= g_map_pick.count) g_map_pick.cursor = g_map_pick.count - 1;
    if (g_map_pick.cursor < g_map_pick.scroll_top)
        g_map_pick.scroll_top = g_map_pick.cursor;
    else if (g_map_pick.cursor >= g_map_pick.scroll_top + MAP_PICK_VIEWPORT_ROWS)
        g_map_pick.scroll_top = (uint8_t)(g_map_pick.cursor - MAP_PICK_VIEWPORT_ROWS + 1);
}

void map_pick_reset(void)
{
    g_map_pick.active = false;
    g_map_pick.count = 0;
    g_map_pick.cursor = 0;
    g_map_pick.scroll_top = 0;
    g_map_pick.result_idx = 0xFF;
    /* Don't memset entries — keep the buffers but their `slug[0]=0`
     * marker is set on next on_list_begin/clear. Overwriting 64 ×
     * sizeof(entry) every reset is wasteful. */
    s_was_up = s_was_down = s_was_a = s_was_start = false;
    s_screen_clear_pending = true;
    s_swallow_held = true;
    s_my_vote = -1;
}

/* Length-prefixed string copy: reads [u8 len][bytes...] from `p` at
 * offset *off. Writes up to dst_max-1 bytes into dst (NUL-terminated).
 * Advances *off past the consumed bytes. Returns true on success. */
static bool s_read_lp_string(const uint8_t* p, int len, int* off,
                             char* dst, int dst_max)
{
    if (*off >= len) { dst[0] = 0; return false; }
    int slen = (int)p[*off];
    *off += 1;
    if (*off + slen > len) { dst[0] = 0; return false; }
    int n = (slen < dst_max - 1) ? slen : (dst_max - 1);
    for (int i = 0; i < n; i++) dst[i] = (char)p[*off + i];
    dst[n] = 0;
    *off += slen;
    return true;
}

void unet_map_pick_on_list_begin(const uint8_t* payload, int len)
{
    if (len < 1 + 1) return;
    int count = (int)payload[1];
    if (count > UNET_MAP_LIST_MAX_ITEMS) count = UNET_MAP_LIST_MAX_ITEMS;
    /* New list — reset selection state but stay active. */
    g_map_pick.active = true;
    g_map_pick.count = (uint8_t)count;
    g_map_pick.cursor = 0;
    g_map_pick.scroll_top = 0;
    g_map_pick.result_idx = 0xFF;
    /* Wipe entries so half-received lists show as empty rows rather
     * than stale data from a previous round. */
    for (int i = 0; i < count; i++) {
        g_map_pick.entries[i].slug[0]   = 0;
        g_map_pick.entries[i].name[0]   = 0;
        g_map_pick.entries[i].author[0] = 0;
        g_map_pick.entries[i].votes     = 0;
    }
    /* Drive the C++ game state. main.cxx watches gameState transitions
     * to switch screen rendering between lobby_draw / map_pick_draw. */
    g_Game.gameState = UGAME_STATE_MAP_PICK;
    s_screen_clear_pending = true;
    /* Treat any currently-held button as "already pressed" so the
     * lobby's still-held START doesn't fire an immediate Phase-2
     * commit on frame 1 of MAP_PICK. The user must physically
     * release every button before they're treated as new presses. */
    s_swallow_held = true;
    s_my_vote = -1;
    unet_send_dbg_log("CKPT-MP1 list begin");
}

void unet_map_pick_on_list_item(const uint8_t* payload, int len)
{
    /* Body: [op:1][idx:1][source:1][stage_id:1][slug_lp][name_lp][author_lp][size:2 BE] */
    if (len < 1 + 3) return;
    int idx = (int)payload[1];
    if (idx >= UNET_MAP_LIST_MAX_ITEMS || idx >= g_map_pick.count) return;
    unet_map_list_entry_t* e = &g_map_pick.entries[idx];
    e->source   = payload[2];
    e->stage_id = payload[3];
    int off = 4;
    if (!s_read_lp_string(payload, len, &off, e->slug,   sizeof(e->slug)))   return;
    if (!s_read_lp_string(payload, len, &off, e->name,   sizeof(e->name)))   return;
    if (!s_read_lp_string(payload, len, &off, e->author, sizeof(e->author))) return;
    if (off + 2 > len) return;
    e->size_bytes = (uint16_t)(((uint16_t)payload[off] << 8) | (uint16_t)payload[off + 1]);
}

void unet_map_pick_on_tally(const uint8_t* payload, int len)
{
    /* Body: [op:1][count:1] [{idx:1, votes:1} × count] */
    if (len < 1 + 1) return;
    int count = (int)payload[1];
    int off = 2;
    /* Zero all current votes so dropped votes don't linger. */
    for (int i = 0; i < g_map_pick.count; i++)
        g_map_pick.entries[i].votes = 0;
    for (int i = 0; i < count && off + 2 <= len; i++) {
        uint8_t idx = payload[off++];
        uint8_t v   = payload[off++];
        if (idx < g_map_pick.count) g_map_pick.entries[idx].votes = v;
    }
}

void unet_map_pick_on_result(const uint8_t* payload, int len)
{
    if (len < 1 + 1) return;
    g_map_pick.result_idx = payload[1];
    /* Caller (main loop) will transition to GAMEPLAY when GAME_START
     * arrives; we just remember the winner so the picker can flash
     * the chosen row briefly. */
    unet_send_dbg_log("CKPT-MP9 pick committed");
}

/* Draw a single list row at fixed grid position. Highlight if cursor
 * is on this row; mark with `*` if it's the committed winner; mark
 * with `+` if THIS player voted for it. */
static void s_draw_row(int viewport_row, int entry_idx)
{
    int y = MAP_PICK_FIRST_ROW + viewport_row;
    bool is_cursor = (entry_idx == (int)g_map_pick.cursor);
    bool is_winner = (entry_idx == (int)g_map_pick.result_idx);
    bool is_my_vote = (entry_idx == s_my_vote);

    if (entry_idx < 0 || entry_idx >= (int)g_map_pick.count) {
        /* Empty viewport slot — render blanks so any leftover glyphs
         * from a deeper-list view get cleared. Total row width 38. */
        font_draw("                                      ",
                  FONT_X(MAP_PICK_ROW_X), FONT_Y(y), 540);
        return;
    }

    const unet_map_list_entry_t* e = &g_map_pick.entries[entry_idx];
    char marker = is_winner ? '*' : (is_cursor ? '>' : (is_my_vote ? '+' : ' '));
    /* Tight format that fits ~38 columns: marker(1) + name(16) +
     * author(12) + "v=NN" (4) plus single-space gutters = 35 cols.
     * Names/authors longer than the slot truncate to "%.NN" so the
     * column boundaries are stable as the cursor moves. */
    font_printf(FONT_X(MAP_PICK_ROW_X), FONT_Y(y), 540,
                "%c%-16.16s %-12.12s v=%-2d",
                marker,
                e->name[0]   ? e->name   : e->slug,
                e->author[0] ? e->author : "-",
                (int)e->votes);
}

void map_pick_draw(void)
{
    if (g_Game.gameState != UGAME_STATE_MAP_PICK) return;

    /* One-time clear to wipe the lobby render. Without this, the lobby
     * roster + footer ghost behind the picker rows. */
    if (s_screen_clear_pending) {
        jo_clear_screen();
        s_screen_clear_pending = false;
    }

    s_clamp_cursor_into_view();

    font_draw("SELECT MAP", FONT_X(14), FONT_Y(1), 540);
    if (g_map_pick.count == 0) {
        font_draw("(no maps yet - waiting for list...)",
                  FONT_X(2), FONT_Y(MAP_PICK_FIRST_ROW + 1), 540);
    } else {
        font_draw(" NAME             AUTHOR       VOTE",
                  FONT_X(MAP_PICK_ROW_X), FONT_Y(2), 540);
        for (int i = 0; i < MAP_PICK_VIEWPORT_ROWS; i++) {
            int entry_idx = (int)g_map_pick.scroll_top + i;
            s_draw_row(i, entry_idx);
        }
        /* Scroll position indicator on the row just below the viewport. */
        if (g_map_pick.count > MAP_PICK_VIEWPORT_ROWS) {
            font_printf(FONT_X(MAP_PICK_ROW_X), FONT_Y(MAP_PICK_FIRST_ROW + MAP_PICK_VIEWPORT_ROWS + 1), 540,
                        "%2d/%-2d  %s            ",
                        (int)(g_map_pick.cursor + 1), (int)g_map_pick.count,
                        g_map_pick.scroll_top + MAP_PICK_VIEWPORT_ROWS < g_map_pick.count
                            ? "(scroll for more)" : "(end of list)   ");
        }
    }
    /* Streaming-progress overlay — when the picker has committed a
     * pick AND the resulting map is being streamed, show progress
     * here so the wait isn't a black silence. The map_stream module
     * lives in the same lobby/picker render layer; reusing its
     * state means no duplication. */
    if (g_map_stream.active && g_map_stream.total_size > 0) {
        int pct = (int)((long long)g_map_stream.received_bytes * 100
                        / (long long)g_map_stream.total_size);
        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;
        font_printf(FONT_X(2), FONT_Y(25), 540,
                    "STREAMING MAP... %3d%% (%d/%d B)            ",
                    pct, g_map_stream.received_bytes, g_map_stream.total_size);
    } else if (g_map_pick.result_idx != 0xFF) {
        font_draw("MAP COMMITTED - preparing match...     ",
                  FONT_X(2), FONT_Y(25), 540);
    } else if (g_map_stream.ready) {
        font_draw("MAP LOADED - awaiting match start      ",
                  FONT_X(2), FONT_Y(25), 540);
    } else {
        font_draw("                                       ",
                  FONT_X(2), FONT_Y(25), 540);
    }
    font_draw("UP/DOWN: scroll  A: vote  START: pick", FONT_X(1), FONT_Y(27), 540);
}

void map_pick_input(void)
{
    if (g_Game.gameState != UGAME_STATE_MAP_PICK) return;

    /* Server transitioned us to PLAYING → leave the picker. lobby.c
     * does the same check while in LOBBY; we mirror it here so a
     * MAP_PICK → GAMEPLAY transition (after pick commit + optional
     * stream + GAME_START) actually fires the world ctor. */
    if (unet_get_state() == UNET_STATE_PLAYING) {
        const unet_state_data_t* nd = unet_get_data();
        g_Game.isOnlineMode = true;
        g_Game.myPlayerID = nd->my_player_id;
        jo_clear_screen();
        g_Game.gameState = UGAME_STATE_GAMEPLAY;
        map_pick_reset();
        return;
    }
    if (unet_get_state() == UNET_STATE_DISCONNECTED) {
        jo_clear_screen();
        g_Game.gameState = UGAME_STATE_TITLE_SCREEN;
        map_pick_reset();
        return;
    }

    if (g_map_pick.count == 0) return;

    bool up    = jo_is_input_key_pressed(0, JO_KEY_UP);
    bool down  = jo_is_input_key_pressed(0, JO_KEY_DOWN);
    bool a     = jo_is_input_key_pressed(0, JO_KEY_A);
    bool start = jo_is_input_key_pressed(0, JO_KEY_START);

    /* "Swallow held" — until every button is physically released
     * (after entering MAP_PICK), treat all currently-pressed
     * buttons as already-handled. Without this, the still-held
     * lobby START fires an immediate Phase-2 commit on frame 1 of
     * MAP_PICK, before the user has a chance to scroll/vote.
     * Reported as: "transitions to map selection screen and seems
     * to have already chosen a resulting map". */
    if (s_swallow_held) {
        if (!up && !down && !a && !start) {
            s_swallow_held = false;
        }
        s_was_up    = up;
        s_was_down  = down;
        s_was_a     = a;
        s_was_start = start;
        return;
    }

    /* Edge-trigger UP/DOWN — wraps both ends so the list feels
     * "infinite" per the user's UX request. */
    if (up && !s_was_up) {
        if (g_map_pick.cursor == 0) g_map_pick.cursor = (uint8_t)(g_map_pick.count - 1);
        else                        g_map_pick.cursor--;
        s_clamp_cursor_into_view();
    }
    if (down && !s_was_down) {
        g_map_pick.cursor++;
        if (g_map_pick.cursor >= g_map_pick.count) g_map_pick.cursor = 0;
        s_clamp_cursor_into_view();
    }
    /* A votes for the cursor's current map. The server tally update
     * arrives back via MAP_PICK_TALLY broadcast and the local
     * `s_my_vote` flips so the row gets a `+` marker for visual
     * feedback (otherwise the only feedback was the v=N column,
     * which clipped offscreen on the original layout). */
    if (a && !s_was_a) {
        unet_send_map_pick_vote(g_map_pick.cursor);
        s_my_vote = (int)g_map_pick.cursor;
    }
    /* START commits — server picks the highest-vote map (random
     * tiebreak) and broadcasts MAP_PICK_RESULT, then GAME_START.
     * Already-committed result blocks further START presses so a
     * spammed button can't trigger spurious match-starts. */
    if (start && !s_was_start && g_map_pick.result_idx == 0xFF) {
        unet_send_start_game();
    }

    s_was_up    = up;
    s_was_down  = down;
    s_was_a     = a;
    s_was_start = start;
}
