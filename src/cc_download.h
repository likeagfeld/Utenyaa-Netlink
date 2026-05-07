/**
 * cc_download.h — Phase C custom-character download state machine.
 *
 * Driven from utenyaa_main_glue.c when g_Game.gameState ==
 * UGAME_STATE_DOWNLOAD_CHARS. Handshake:
 *
 *   1. cc_download_init() — called once on entry from connecting.c
 *      after auth. Resets state, sends UNET_CC_LIST_REQ.
 *
 *   2. cc_download_tick() — runs every frame. Walks the local FSM
 *      from WAITING_LIST → DOWNLOADING (one char at a time) → DONE
 *      / FAILED. Net-side state lives on g_net.cc_* (utenyaa_net.h);
 *      this module is just the "do the next thing" driver.
 *
 *   3. cc_download_input() — A button at DONE/FAILED state triggers
 *      disconnect + fresh-boot back to UGAME_STATE_TITLE_SCREEN.
 *
 *   4. cc_download_draw() — paints status text + progress bar.
 *
 * Phase C ships the download-only flow; Phase D will integrate the
 * received per-character payloads into the lobby char picker and
 * the engine's sprite-slot system.
 */

#ifndef CC_DOWNLOAD_H
#define CC_DOWNLOAD_H

#ifdef __cplusplus
extern "C" {
#endif

void cc_download_init(void);
void cc_download_tick(void);
void cc_download_draw(void);
void cc_download_input(void);

/* Returns the byte buffer for downloaded character `idx` (0..N-1
 * indexed into the just-received list), or NULL if not yet downloaded.
 * Phase D will use this to load the sprites into VDP1 VRAM. */
const unsigned char* cc_download_get_payload(int idx, int* out_size);

/* Number of characters successfully downloaded this session.
 * Phase D integration reads this to size the lobby char picker. */
int cc_download_count(void);

/* Phase D — VDP1 sprite slot lookup for a downloaded custom
 * character. `local_idx` is 0..cc_download_count()-1 (= server
 * character_id minus 5). Returns the FIRST sprite ID (south frame)
 * for that character, with the next 4 frames at consecutive IDs,
 * or -1 if the character hasn't been registered as VDP1 sprites
 * yet. Only the first CC_MAX_REGISTERED downloads get VDP1 slots
 * to keep VRAM usage bounded. */
int cc_download_get_sprite_id(int local_idx);

/* How many of the downloaded characters were successfully written
 * into VDP1 sprite slots. Capped at CC_MAX_REGISTERED. */
int cc_download_registered_count(void);

#ifdef __cplusplus
}
#endif

#endif /* CC_DOWNLOAD_H */
