#pragma once
/**
 * utenyaa_online_bridge.hpp - C++ ↔ net layer bridge
 *
 * The net layer (utenyaa_net.c) speaks C. The game entities are C++.
 * This bridge owns the binding:
 *   - Registers the unet_callbacks_t at startup so server messages
 *     (BULLET_SPAWN, EXPLOSION, DAMAGE, GAME_OVER, etc.) dispatch into
 *     C++ entity spawns.
 *   - Provides input routing: Helpers::IsControllerButton* asks the
 *     bridge whether a given player controller is LOCAL (poll Saturn
 *     pad) or REMOTE (decode from unet's per-player input buffer).
 *   - Provides OnlineBridge::TickLocalPlayers(...) called each frame
 *     to send INPUT_STATE delta + PLAYER_STATE sync to the server for
 *     whichever player IDs this Saturn owns.
 */

#include <jo/Jo.hpp>
#include "net/utenyaa_net.h"
#include "net/utenyaa_game.h"

struct OnlineBridge
{
    /** Called once from main.cxx after unet_glue_init() — installs
     *  unet_callbacks_t pointing at this file's static handlers. */
    static void Install();

    /** True if the given player controller/pid is "owned" by this
     *  Saturn (local pad). In offline mode, always true. */
    static inline bool IsLocal(int controller)
    {
        if (!g_Game.isOnlineMode) return true;
        if ((uint8_t)controller == g_Game.myPlayerID)  return true;
        if ((uint8_t)controller == g_Game.myPlayerID2) return true;
        return false;
    }

    /** Current input bitmask for a remote player, as reported by the
     *  server relay buffer. Returns 0 if no relay data available. */
    static inline uint8_t GetRemoteInputBits(int controller)
    {
        if (controller < 0 || controller >= UNET_MAX_PLAYERS) return 0;
        const unet_state_data_t* nd = unet_get_data();
        int bits = unet_get_remote_input(nd->local_frame, (uint8_t)controller);
        if (bits < 0) return 0;
        return (uint8_t)bits;
    }

    /** True if a remote player currently holds the given key.
     *  (Edge-triggered "pressed" is simulated via prior-bits tracking
     *  kept internally per controller.) */
    static bool RemoteKeyDown(int controller, jo_gamepad_keys key);
    static bool RemoteKeyPressed(int controller, jo_gamepad_keys key);

    /** Called once per frame from the game loop while in an online match.
     *  Polls local pad(s), derives the Utenyaa input bitmask, and emits
     *  INPUT_STATE + PLAYER_STATE to the server on behalf of each
     *  Saturn-owned player. Also sends CLIENT_FIRE_BULLET / DROP_MINE /
     *  THROW_BOMB when the local player triggers those actions. */
    static void TickLocalPlayers();

    /** Called each frame AFTER IUpdatable::Update() in online gameplay.
     *  Reads the latest PLAYER_SYNC snapshot for each remote pid from
     *  the net layer and applies it to the matching C++ Player with
     *  50% lerp + 3-frame velocity extrapolation. */
    static void ApplyRemoteSnapshots();

    /** Gameplay HUD overlay: online-only text (SUDDEN DEATH banner,
     *  server match timer, spectator hint when local player is dead,
     *  game-over results pause banner). Drawn each frame during the
     *  online gameplay branch of main's while loop. */
    static void DrawGameplayOverlay();

    /** Manage match-end UX state: when the server sends GAME_OVER we
     *  don't rip the player back to lobby instantly. This runs a
     *  countdown frame-by-frame, keeps the world rendered behind a
     *  "WINNER: ..." banner for a few seconds, then transitions. */
    static void TickMatchEndPause();

    /** For spectator follow-winner: returns the pid of the player to
     *  aim the camera at (top HP · kills tiebreaker). 0xFF if no
     *  valid target. */
    static uint8_t GetSpectatorTargetPid();

    /** True when the local player (myPlayerID) has HP<=0 — i.e. we
     *  should be in spectator follow-winner mode. */
    static bool LocalIsDeadSpectator();
};
