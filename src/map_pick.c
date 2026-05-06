/**
 * map_pick.c - Implementation of the in-lobby scrollable map picker.
 * See map_pick.h for design notes.
 */

#include "map_pick.h"
#include "font.h"
#include "net/utenyaa_net.h"
#include "net/utenyaa_game.h"
#include <jo/jo.h>
#include <string.h>

unet_map_pick_t g_map_pick;

/* Visible row count in the picker viewport. The lobby uses rows 4..27;
 * we reserve row 1 for the header, row 2 for the column labels, row 3
 * blank, rows 4..23 for the list (20 rows), row 25 for status text,
 * and row 27 for the controls hint. With infinite scroll the list
 * wraps cleanly even when entry count exceeds the viewport. */
#define MAP_PICK_VIEWPORT_ROWS  20
#define MAP_PICK_FIRST_ROW       4
#define MAP_PICK_NAME_COL_X      4
#define MAP_PICK_AUTHOR_COL_X   28
#define MAP_PICK_VOTES_COL_X    44
#define MAP_PICK_SIZE_COL_X     49

/* Edge-detection state for input — pad polling fires every frame at
 * ~50 Hz (PAL); without rising-edge tracking a held UP/DOWN scrolls
 * past the entire list in 200 ms. */
static bool s_was_up, s_was_down, s_was_a, s_was_start;
static bool s_screen_clear_pending = true;

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
 * is on this row; mark with `*` if it's the committed winner. */
static void s_draw_row(int viewport_row, int entry_idx)
{
    int y = MAP_PICK_FIRST_ROW + viewport_row;
    bool is_cursor = (entry_idx == (int)g_map_pick.cursor);
    bool is_winner = (entry_idx == (int)g_map_pick.result_idx);

    if (entry_idx < 0 || entry_idx >= (int)g_map_pick.count) {
        /* Empty viewport slot — render blanks so any leftover glyphs
         * from a deeper-list view get cleared. font_printf with a
         * fixed-width %-Ns format would also work; the explicit
         * spaces are simpler. */
        font_printf(FONT_X(MAP_PICK_NAME_COL_X), FONT_Y(y), 540,
                    "                                                ");
        return;
    }

    const unet_map_list_entry_t* e = &g_map_pick.entries[entry_idx];
    char marker = is_winner ? '*' : (is_cursor ? '>' : ' ');
    /* font_printf with fixed widths so cursor moves don't leave
     * tail-glyphs from longer prior rows. Name 24, author 16, votes
     * fits in 4 chars (e.g. "v=12"), size in 5 chars ("12.3K"). */
    font_printf(FONT_X(MAP_PICK_NAME_COL_X - 1), FONT_Y(y), 540,
                "%c %-24.24s %-16.16s v=%-2d %2d.%dK",
                marker, e->name[0] ? e->name : e->slug,
                e->author[0] ? e->author : "-",
                (int)e->votes,
                (int)(e->size_bytes / 1024),
                (int)((e->size_bytes % 1024) / 102));
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

    font_draw("UTENYAA - SELECT MAP", FONT_X(8), FONT_Y(1), 540);
    if (g_map_pick.count == 0) {
        font_draw("(no maps yet — waiting for server list)",
                  FONT_X(2), FONT_Y(MAP_PICK_FIRST_ROW + 1), 540);
    } else {
        font_draw("  NAME                     AUTHOR           VOTES SIZE",
                  FONT_X(MAP_PICK_NAME_COL_X - 1), FONT_Y(2), 540);
        for (int i = 0; i < MAP_PICK_VIEWPORT_ROWS; i++) {
            int entry_idx = (int)g_map_pick.scroll_top + i;
            s_draw_row(i, entry_idx);
        }
        /* Scroll position indicator on the last viewport row. */
        if (g_map_pick.count > MAP_PICK_VIEWPORT_ROWS) {
            font_printf(FONT_X(2), FONT_Y(MAP_PICK_FIRST_ROW + MAP_PICK_VIEWPORT_ROWS + 1), 540,
                        "[%2d/%2d]  scroll wraps  %s",
                        (int)(g_map_pick.cursor + 1), (int)g_map_pick.count,
                        g_map_pick.scroll_top + MAP_PICK_VIEWPORT_ROWS < g_map_pick.count
                            ? "(more below)" : "(end of list)");
        }
    }
    font_draw("UP/DOWN scroll  A vote  START commit pick",
              FONT_X(2), FONT_Y(27), 540);
    if (g_map_pick.result_idx != 0xFF) {
        font_draw("MAP COMMITTED — preparing match...",
                  FONT_X(8), FONT_Y(25), 540);
    }
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
     * arrives back via MAP_PICK_TALLY broadcast. */
    if (a && !s_was_a) {
        unet_send_map_pick_vote(g_map_pick.cursor);
    }
    /* START commits — server picks the highest-vote map (random
     * tiebreak) and broadcasts MAP_PICK_RESULT, then GAME_START. */
    if (start && !s_was_start) {
        unet_send_start_game();
    }

    s_was_up    = up;
    s_was_down  = down;
    s_was_a     = a;
    s_was_start = start;
}
