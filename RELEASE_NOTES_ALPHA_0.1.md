# Utenyaa NetLink — Online Multiplayer (Alpha 0.1)

First alpha release of online multiplayer support for Utenyaa via the Sega Saturn NetLink modem.

💝 **Massive thanks to [ReyeMe](https://github.com/ReyeMe) and [robertoduarte](https://github.com/robertoduarte)** for the original Utenyaa game — the engine, the tank arena, the art, the catgirls. This fork only *adds* a networking layer; everything you're actually playing is their work. Give the upstream a star: https://github.com/ReyeMe/Utenyaa-public

## Online setup

No manual config editing needed. Update to the **latest DreamPi** or **latest netlink.py PC tunnel script** — the Utenyaa entry is already shipped in the default `netlink_config.ini` alongside the other NetLink revival games. The server is already live and running; just dial in from the Saturn and the bridge takes care of the rest.

## How to play

Load `START GAME.CUE` (not `game.iso`) on your emulator or ODE. The cue references both the game ISO track and the three CD audio tracks (title / pause / gameplay).

## What's in this alpha

The online flow has been kept deliberately minimal so the underlying protocol and engine can be hardened first; some upstream lobby features (stage voting, character picker, bots) were stripped while we stabilise the LOBBY → GAMEPLAY transition. The sections below describe **only what's actually in the alpha 0.1 build** — not future plans.

### Online flow

- **Title screen → Play Online**: select Play Online from the main menu to enter the network flow
- **Name entry**: type a 1–16 character name (saved across sessions via Saturn backup RAM, so reconnecting keeps your name and leaderboard stats)
- **Connecting**: dial-up + auto-detect of NetLink modem; the connecting screen shows handshake progress and error states
- **Lobby**: see who's connected, toggle ready, start the match
- **Gameplay**: full Utenyaa 3D arena combat, server-relayed
- **Game over**: WINNER banner appears for ~1.5 s on top of the gameplay screen, then back to lobby with a persistent **`LAST WINNER: <name>`** line that stays visible until the next match starts

### Lobby controls (simplified)

- **A / C** — toggle READY (your row shows "READY" when armed)
- **START** — request game start (server fires when ≥ 2 ready players + co-op slots present)
- **Y** — disconnect and return to title screen

That's it. The lobby is intentionally three buttons in this build. Stage and character are auto-assigned by the server (round-robin pid → character ID). Bots, stage voting, character cycling, disconnect-confirmation prompts — **all stripped** for now and can return in alpha 0.2 once the online flow is rock-solid.

### Match rules

- Match timer counts down server-authoritatively (default 120 seconds, admin-tunable)
- **Match ends when**:
  - any player's HP hits 0 and `alive_count <= 1` (instant — typical "1v1 last-tank-standing" win)
  - **OR** the match timer hits 0 (highest-HP player wins; no sudden death by default — admin can enable via tune knob)
- Server detects death within one tick (50 ms) and broadcasts `GAME_OVER`; client shows the WINNER banner and returns to the lobby in ~1.5 s

### Co-op (P2 on the same Saturn)

- Plug a second controller into port B in the lobby — the server allocates a P2 slot for you on match start, no extra dial-in needed
- Both P1 and P2 are tracked as separate game players; either dying drops `alive_count` and can end the match
- Hot-unplug during a match cleanly removes the P2 slot from the server

### Controls (in-game)

- **D-pad** — tank controls by default (UP/DOWN forward/reverse along facing, LEFT/RIGHT rotate)
- **A** — fire bullet
- **B** — drop mine (if held)
- **C** — throw bomb (if held)
- **Saturn 3D Control Pad in analog mode** (peripheral ID `0x16`): switches automatically to 8-direction screen-relative movement (UP = world +Y, etc.) with auto-rotate-to-face — analog stick gives smooth magnitude with a 32-unit deadzone

### Networking

- **Wire**: SNCP-framed binary protocol over the NetLink 16550 UART → modem → DreamPi/PC bridge → TCP → game server
- **Sync model**: passthrough — local pad input drives local movement (zero perceived lag); server relays `INPUT_STATE` and `PLAYER_STATE` to peers; remote tanks lerp + 3-frame extrapolate to smooth jitter
- **Authority split**: server owns match timer, kill detection, crate spawns/destroys, explosion damage attribution, leaderboard. Client owns local movement, local fire-trigger, and the entity render loop
- **NetLink LED** blinks on board ctrl bit 7 during CONNECTING / LOBBY / GAMEPLAY (modem detect tells us we're alive)

### Persistence

- **Saturn backup RAM**: player name + reconnect UUID survive power-off (so the leaderboard recognises you across sessions)
- **Server leaderboard**: top players by wins, best HP, kills, deaths, matches played — visible via the admin portal (operator-side)

## Critical fixes shipped in this build

| Bug | Root cause | Fix |
|---|---|---|
| LOBBY → GAMEPLAY hang at match start | `sgl.linker` only collected `*(.bss)` — C++ inline-static template members in `.bss._ZN…` orphan sections were placed inside `_global_memory.6`'s 250 KB pool address range. Pool block-walk read garbage `block->size` from a static-init guard byte and infinite-looped | Project-local `utenyaa.linker` explicitly claims `*(.bss.*)` / `*(.data.*)` / `*(.rodata.*)` / `*(.text.*)` |
| HUD timer "560 minutes" wrap after expiry | `Messages::UpdateTime currentTime` is `size_t` (unsigned); negative `startTime` cast wrapped to ≈33600 → 560 min display | Online HUD now reads `nd->match_seconds_left` directly from server (already clamped to 0); offline path clamps local `startTime ≥ 0` |
| Top-of-screen text strobing | `jo_clear_screen()` (1200 NBG0 cell-writes) was running mid-display every frame from `menu.Update`'s double-clear in gameplay and `lobby_draw`'s top-of-frame clear | Skip `menu.Update` when online + GAMEPLAY; remove per-frame clear in lobby; all writes now use fixed-width formats that overwrite in place |
| 2nd-game lobby: `A` then `START` flips ready off | `g_net.my_ready` (client) wasn't reset on match-end while server's `c.ready` was → mismatch caused `START` handler's resync logic to fire a redundant READY toggle that turned ready off | New `unet_reset_ready_state()` called at match-end → LOBBY transition; input flags also reset |
| Game didn't end on kill | `g_Game.myPlayerID2` was never synced from `g_net.my_player_id2` (set by `LOCAL_PLAYER_ACK`) → death-check guard `if (g_Game.myPlayerID2 != 0xFF)` always saw 0xFF → `CLIENT_DEATH_P2` never sent → server's `alive_count` never dropped | Per-frame sync in `unet_glue_tick_frame` now mirrors `my_player_id2` the same way `my_player_id` is mirrored |
| Game didn't end on timer expiry | Server's `sudden_death_enabled` was on by default and extended the timer to 99999 s when 2 alive players were tied at full HP | `sudden_death_enabled` default flipped to **off** — timer = 0 ends match immediately with highest-HP player as winner. Admin can re-enable via `/api/tune_sudden_death` |
| Heap-corruption crash `jo_free Bad pointer 0x..323A20` (during early debugging) | Toolchain mismatch with newlib's `_malloc_r`/`_sbrk` heap fighting jo_engine's static pool | `src/libc_stubs.c` wrappers route `malloc`/`free`/reentrant variants to `jo_malloc`/`jo_free`; `_sbrk` hard-fails. Single heap, single pool |

## Known limitations & next steps (alpha 0.2)

- Stage voting, character picker, bot add/remove — stripped for now; will return as polished UI elements once the core flow is locked
- 5-player+ matches not exercised yet (cap is `UNET_MAX_PLAYERS = 4`)
- Disconnect prompt is a single Y-press without a confirmation overlay — easy to leave by accident; will add a Y-twice gating in alpha 0.2
- HUD timer and `DrawGameplayOverlay`'s "TIME" line both render on different rows — visually fine but redundant; will consolidate to one source

## Disclaimer

Online NetLink functionality implemented with assistance from AI (Claude). The underlying Utenyaa game — its engine, entities, assets, stages, and all gameplay — is entirely the work of ReyeMe and robertoduarte. Please credit them if you share gameplay.
