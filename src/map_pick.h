/**
 * map_pick.h - In-lobby scrollable map selection screen.
 *
 * Lifecycle:
 *   1. Player presses START in lobby → server transitions ALL clients
 *      into UGAME_STATE_MAP_PICK and broadcasts MAP_LIST_BEGIN +
 *      N × MAP_LIST_ITEM. The map_pick module receives those into a
 *      fixed-capacity list and renders a scrollable view.
 *   2. UP/DOWN scroll the cursor; the cursor's row vote-highlights
 *      that map. A sends UNET_MAP_PICK_VOTE so peers see the live
 *      tally. START fires UNET_START_GAME_REQ which the server treats
 *      as "commit current winner".
 *   3. Server broadcasts MAP_PICK_RESULT, then the existing
 *      GAME_START / streaming flow runs.
 *
 * Defensive screen clears on enter and exit to avoid leftover lobby
 * glyphs ghosting behind the new render — observed during the 0.6
 * playtest as overlapping text when transitioning screens.
 */

#ifndef UTENYAA_MAP_PICK_H
#define UTENYAA_MAP_PICK_H

#include <stdint.h>
#include <stdbool.h>
#include "net/utenyaa_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  source;       /* 0=baked stage, 1=custom streamed */
    uint8_t  stage_id;     /* 0..3 when source=0, 0xFF otherwise */
    uint16_t size_bytes;
    uint8_t  votes;        /* live tally, updated by MAP_PICK_TALLY */
    char     slug   [UNET_MAP_SLUG_MAX   + 1];
    char     name   [UNET_MAP_NAME_MAX   + 1];
    char     author [UNET_MAP_AUTHOR_MAX + 1];
} unet_map_list_entry_t;

typedef struct {
    bool     active;       /* True while in UGAME_STATE_MAP_PICK */
    uint8_t  count;        /* Items received so far */
    uint8_t  cursor;       /* Cursor row in the list (0..count-1) */
    uint8_t  scroll_top;   /* First visible row (auto-adjusted) */
    uint8_t  result_idx;   /* Server-committed winner; 0xFF until set */
    unet_map_list_entry_t entries[UNET_MAP_LIST_MAX_ITEMS];
} unet_map_pick_t;

extern unet_map_pick_t g_map_pick;

/* Net-layer entry points (called from the protocol dispatcher) */
void unet_map_pick_on_list_begin(const uint8_t* payload, int len);
void unet_map_pick_on_list_item (const uint8_t* payload, int len);
void unet_map_pick_on_tally     (const uint8_t* payload, int len);
void unet_map_pick_on_result    (const uint8_t* payload, int len);

/* Saturn-side render + input — called from main loop while
 * gameState == UGAME_STATE_MAP_PICK. */
void map_pick_draw(void);
void map_pick_input(void);

/* Reset the entire pick session. Called on returning to lobby and on
 * fresh enter (before the first MAP_LIST_BEGIN of a new round). */
void map_pick_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* UTENYAA_MAP_PICK_H */
