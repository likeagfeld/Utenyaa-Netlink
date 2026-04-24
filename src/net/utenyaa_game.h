/**
 * utenyaa_game.h - Shared C-linkage online game state
 *
 * Bridges the pure-C net/screen modules with the C++ game engine.
 * All fields here are written from one side and read from the other —
 * treat as an ABI, not an implementation detail.
 *
 * The C++ engine assigns an online mode flag + player name; the C
 * screen code drives the connecting/lobby/name-entry FSM and populates
 * player IDs, ready state, character selections, etc. as the server
 * reports them back.
 */

#ifndef UTENYAA_GAME_H
#define UTENYAA_GAME_H

#include <stdint.h>
#include <stdbool.h>
#include "utenyaa_net.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Online-mode game states (decoupled from the C++ Menu::Screen enum so the
 * C layer can drive its own screen FSM without pulling in C++ headers)
 *============================================================================*/

typedef enum {
    UGAME_STATE_NONE = 0,          /* Offline / not in online flow */
    UGAME_STATE_TITLE_SCREEN,      /* Main menu (ONLINE option visible) */
    UGAME_STATE_NAME_ENTRY,        /* Entering player name */
    UGAME_STATE_CONNECTING,        /* Modem FSM running */
    UGAME_STATE_LOBBY,             /* In lobby (char select, stage vote, ready) */
    UGAME_STATE_GAMEPLAY,          /* In match */
    UGAME_STATE_GAME_OVER          /* Results screen, then back to lobby */
} utenyaa_gamestate_t;

typedef struct {
    bool    pressedABC;
    bool    pressedLT;
    bool    pressedRT;
    bool    pressedXYZ;
    bool    pressedStart;
    bool    pressedUp;
    bool    pressedDown;
    bool    pressedLeft;
    bool    pressedRight;
} utenyaa_input_t;

typedef struct {
    /* Mode flags */
    bool    isOnlineMode;          /* True once we enter the online flow */
    utenyaa_gamestate_t gameState;

    /* Per-pad edge-detection state (P1). Extra pads read directly. */
    utenyaa_input_t input;

    /* Identity */
    char    playerName[UNET_MAX_NAME + 1];
    char    playerName2[UNET_MAX_NAME + 1];
    bool    hasSecondLocal;
    uint8_t myPlayerID;            /* Server-assigned, 0xFF before WELCOME */
    uint8_t myPlayerID2;           /* 0xFF if no P2 */

    /* Lobby choices */
    uint8_t myCharacter;           /* 0..UNET_MAX_CHARACTERS-1, 0xFF=unset */
    uint8_t myCharacter2;
    uint8_t myStageVote;           /* 0..UNET_STAGE_COUNT-1, 0xFF=unset */

    /* Title-screen cursor state (preserves position when returning from
     * connecting / lobby / gameplay so ONLINE stays highlighted). */
    uint8_t titleScreenChoice;
} utenyaa_game_t;

/*============================================================================
 * Shared singletons — defined in main.cxx (or a dedicated TU)
 *============================================================================*/

extern utenyaa_game_t g_Game;

#ifdef __cplusplus
}
#endif

#endif /* UTENYAA_GAME_H */
