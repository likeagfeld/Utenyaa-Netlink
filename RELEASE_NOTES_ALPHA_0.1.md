# Utenyaa NetLink — Online Multiplayer (Alpha 0.1)

First alpha release of online multiplayer support for Utenyaa via the Sega Saturn NetLink modem.

💝 **Massive thanks to [ReyeMe](https://github.com/ReyeMe) and [robertoduarte](https://github.com/robertoduarte)** for the original Utenyaa game — the engine, the tank arena, the art, the catgirls. This fork only *adds* a networking layer; everything you're actually playing is their work. Give the upstream a star: https://github.com/ReyeMe/Utenyaa-public

## Features

- Up to **4 players online** with server-authoritative match state (timer, sudden death, crate respawn, game-over)
- Client-authoritative local movement for **zero-lag feel**, with 50% lerp + 3-frame extrapolation smoothing for remote tanks
- **Unique character selection** — server rejects duplicates, L/R in lobby skips already-taken
- **Stage voting** across the four original stages (Island / Cross / Valley / Railway) — highest tally wins
- **Sudden death** overtime when the 120-second timer expires on a tied top HP — first kill wins, with on-screen banner
- **Custom name entry** with Saturn backup-RAM persistence (cartridge key `UTEN_NAME`)
- **Follow-winner spectator camera** post-elimination — camera auto-tracks the current leader
- **5-second results-pause banner** with winner + full per-player HP / kills / deaths strip before returning to lobby
- **Persistent online leaderboard** — clearer full-word column headers (WINS · BEST HP · KILLS · DEATHS · MATCHES)
- **Local co-op + online** — second controller on the same Saturn registers as an additional player slot
- **Server bots** with real tank AI (hunt / attack / strafe / evade state cycling) including respawn pauses
- **NetLink LED heartbeat** on Japanese and US modems during connecting / lobby / gameplay
- **Delta-compressed input** with 15-frame keepalive for efficient 14400 baud
- **Online collision padding** (+3 units) compensating for network desync so bullets hit remote tanks reliably
- **Server-broadcast match timer** routed straight to the HUD — both Saturns read the same clock

## Online setup

No bridge script needed on your end. Add this block to your DreamPi or PC `netlink_config.ini` and dial `199405`:

```ini
[server:199405]
name = Utenyaa
host = saturncoup.duckdns.org
port = 4825
shared_secret = Utenyaa2026!NetLink#Key
auth_magic = AUTH
auth_timeout = 5.0
```

Server is already live at `saturncoup.duckdns.org:4825` — same VM that hosts Coup, Disasteroids, and Flicky's Flock. Admin portal tab at `/admin/utenyaa/`.

## How to play

Load the `.cue` (not the `.iso`) on your emulator or burn to disc. This alpha zip contains source + online config; if you want a pre-built ISO, build locally with `make build` (Yaul + jo-engine toolchain required — same as upstream).

Supply your own audio tracks — `track02.wav` / `track03.wav` / `track04.wav` are included from upstream.

## Controls

**Lobby:** A/C ready · L/R character · X stage vote · ↑↓ add/remove bot · START go · B back · Y disconnect · Z (hold) stats

**In match:** ← → rotate · ↑ ↓ drive · A shoot · B use pickup · C throw bomb · START pause

## Known items

- Pre-built ISO not included in this release — build locally or wait for the next drop
- Bot attribution for kills is stubbed (future revision will credit the bot that scored the hit)
- Crate level-load slot binding to client Entities::Crate pointers is a no-op callback — world spawns work, runtime respawn broadcast arrives but the C++ side doesn't yet recycle the local entity (server state stays correct)

## Disclaimer

Online NetLink functionality implemented with assistance from AI (Claude). The underlying Utenyaa game — its engine, entities, assets, stages, and all gameplay — is entirely the work of ReyeMe and robertoduarte. Please credit them if you share gameplay.
