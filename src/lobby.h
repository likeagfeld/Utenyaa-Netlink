#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void lobby_init(void);
void lobby_input(void);
void lobby_update(void);
void lobby_draw(void);
/* Reset just the per-screen input-edge static flags. Called from
 * main.cxx's match-end → lobby teardown so a still-held L/R/A from
 * the gameplay frame doesn't get mistaken for a fresh edge in the
 * lobby tick that follows. lobby_init() does the full init (incl.
 * leaderboard fetch + jo_clear_screen) and is too heavy for this
 * mid-session re-entry. */
void lobby_reset_edge_state(void);

#ifdef __cplusplus
}
#endif
