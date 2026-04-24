![logo](https://github.com/ReyeMe/Utenyaa-public/blob/main/README_logo.png?raw=true)

# Utenyaa — NetLink Edition

**Catgirls in armored vehicles, now online.** 4-player arena deathmatch over the Sega Saturn NetLink modem.

> 💝 **Huge thanks to [ReyeMe](https://github.com/ReyeMe) and [robertoduarte](https://github.com/robertoduarte)** for the original Utenyaa — the engine, the art, the gameplay, the years of tank-arena polish. This fork adds networking on top of their work; every line of the original code and every pixel of the assets is theirs. If you enjoy this port, go star the upstream repo: https://github.com/ReyeMe/Utenyaa-public

---

## NetLink Online Play

Online multiplayer for **up to 4 players** over the Sega Saturn NetLink modem, same connectivity method as Flicky's Flock and the other NetLink revival games. Just update to the latest DreamPi or netlink.py PC tunnel script — nothing extra to configure.

### What's in the box

- **Up to 4 players** with server-authoritative match state (timer, sudden death, crate respawn, game-over)
- **Client-authoritative local movement** for zero-lag feel, with 50% lerp + 3-frame extrapolation to smooth remote tanks
- **Unique character selection** (server enforces — two players can't pick the same catgirl/tank)
- **Stage voting** across all four original stages (Island / Cross / Valley / Railway) — highest tally wins, ties broken by server RNG
- **Sudden death overtime** when the 120-second timer expires on a tied top HP — first kill wins
- **Custom name entry** with Saturn backup-RAM persistence (cartridge key `UTEN_NAME`)
- **Follow-winner spectator camera** post-elimination — camera auto-tracks the leader until the match ends
- **Persistent online leaderboard** — wins, best HP, kills, deaths, matches — with clear full-word column headers
- **Local co-op + online**: plug in a second controller in the lobby, it registers automatically as a second player slot on this Saturn
- **Server bots** with full tank AI (hunt / attack / strafe / evade cycling) for solo testing or under-filled lobbies
- **NetLink LED heartbeat** during connecting / lobby / gameplay — same activity light pattern the modem uses on every other NetLink title
- **Delta-compressed input** with 15-frame keepalive so the 14400 baud link isn't overwhelmed

### Flow

```
TITLE → Play Online → Name Entry (backup RAM) → Connecting → Lobby
  (ready, character, stage vote, bots, P2 co-op)
  → Match (server timer, sudden death on tie, follow-winner spectator)
  → Results (5s pause w/ winner banner) → back to Lobby
```

### Lobby controls

| Button | Action |
|---|---|
| A / C  | Toggle ready |
| L / R  | Cycle character (skips already-taken) |
| X      | Cycle stage vote |
| ↑ / ↓  | Add / remove bot |
| START  | Request game start (needs 2+ total slots) |
| B      | Back to title (stay connected for quick rejoin) |
| Y      | Disconnect |
| Z      | Hold — stats overlay (last-match results ↔ leaderboard) |

---

## Building

Same toolchain as upstream — **Yaul + jo-engine tools**. Run `make build` (or `compile.bat` on Windows) with `sh2eb-elf-gcc` / `sh2eb-elf-g++` on `PATH`. Produces `build.iso` + `build.cue` in the project root.

---

## Credits

- **Upstream game:** [ReyeMe](https://github.com/ReyeMe) & [robertoduarte](https://github.com/robertoduarte) (https://github.com/ReyeMe/Utenyaa-public). Art, engine, gameplay, and all the suffering that went into making a Saturn arena shooter in 2024.
- **Original credits screen:** ReyMe, DannyDuarte, am.25, Random, AnriFox.
- **NetLink online port (this fork):** [@likeagfeld](https://github.com/likeagfeld).

### Disclaimer

NetLink online functionality developed with assistance from AI (Claude). The underlying Utenyaa game code is entirely ReyeMe and robertoduarte's work — this fork only *adds* a networking layer on top.

---

## Upstream README (verbatim)

**This repository does not contains compiler! The dev env used for this is a concoction of Yaul compiler and jo-engine tools.**<br />
**This gave us a great deal of pain, and a proper dev env for c++ with SGL is already in works, so stay tuned for that on SX :D**

People who suffered while coding this:<br />
https://github.com/ReyeMe<br />
https://github.com/robertoduarte
