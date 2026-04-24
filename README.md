![logo](https://github.com/ReyeMe/Utenyaa-public/blob/main/README_logo.png?raw=true)

Catgirls in armored vehicles — **NetLink Edition**

## NetLink Online Play (this fork)

Online multiplayer for up to 4 players, dialing the Saturn NetLink modem
into `saturncoup.duckdns.org:4825` (same pattern as the sibling revival
servers for Disasteroids, Flicky's Flock, Coup).

**Flow:** title → Play Online → name entry (saved to cartridge) → dials
199405 → lobby. In lobby: L/R cycle character (unique per player), X
cycles stage vote, A readies, START kicks off the match, UP/DN add/remove
bots, Y disconnects, B returns to title (stays connected). Hold Z for
stats overlay alternating between last-match results and the persistent
online leaderboard.

**Sync model:** Disasteroids-style passthrough — local movement is
client-authoritative (zero-lag), while match timer, crate respawn
roll, score, game-over, and sudden death are server-authoritative.
RNG is seeded from the server at match start so pickup rolls agree
across peers.

**Server + deploy:** see `tools/utenyaa_server/DEPLOY.md`. Admin portal
at `/admin/utenyaa/` on the unified saturncoup VM; live-tunable knobs
via `POST /api/tune/<key>`.

**Dial config (for DreamPi / netlink.py PC bridges):**

```ini
[server:199405]
name = Utenyaa
host = saturncoup.duckdns.org
port = 4825
shared_secret = Utenyaa2026!NetLink#Key
auth_magic = AUTH
auth_timeout = 5.0
```

---

## Upstream README

**This repository does not contains compiler! The dev env used for this is a concoction of Yaul compiler and jo-engine tools.**<br />
**This gave us a great deal of pain, and a proper dev env for c++ with SGL is already in works, so stay tuned for that on SX :D**

People who suffered while coding this:<br />
https://github.com/ReyeMe<br />
https://github.com/robertoduarte