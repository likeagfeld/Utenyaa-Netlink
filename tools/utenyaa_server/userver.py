#!/usr/bin/env python3
"""
Utenyaa NetLink Game Server

Manages online multiplayer for Utenyaa (4-player catgirl tank arena on
Sega Saturn). Architecture follows the Disasteroids / Flicky's Flock /
Coup server pattern: bridge-authenticated connections, SNCP binary
framing, lobby management, server-authoritative "passthrough" sync.

Sync model (Disasteroids passthrough default):
  - Clients run local movement for zero-lag feel
  - Server is authoritative for: match timer, sudden death, game-over,
    crate respawn/pickup roll, score/leaderboard
  - Damage is client-authoritative with retries (CLIENT_DEATH) — matches
    the proven Flicky pattern after the April-23 hybridization arc
  - Bullets/mines/bombs are broadcast on client request (server assigns
    monotonic entity IDs)
  - PLAYER_SYNC streams every few ticks from each client, relayed to
    others with the server as a simple passthrough

Usage:
    python3 tools/utenyaa_server/userver.py
    python3 tools/utenyaa_server/userver.py --port 4825 --bots 2
    python3 tools/utenyaa_server/userver.py --admin-port 9093
"""

import argparse
import json
import logging
import os
import queue
import random
import select
import socket
import struct
import sys
import threading
import time
import uuid
from http.server import HTTPServer, BaseHTTPRequestHandler

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("utenyaa_server")

# ==========================================================================
# Constants
# ==========================================================================

HEARTBEAT_TIMEOUT = 60.0
MAX_RECV_BUFFER = 8192
USERNAME_MAX_LEN = 16
UUID_LEN = 36

# Bridge authentication (matches netlink_config.ini entry for dial 199405)
SHARED_SECRET = b"Utenyaa2026!NetLink#Key"
AUTH_MAGIC = b"AUTH"
AUTH_TIMEOUT = 5.0

MAX_PLAYERS = 4
MAX_CHARACTERS = 12
MAX_CRATES = 16
STAGE_COUNT = 4
STAGE_NAMES = ["Island", "Cross", "Valley", "Railway"]
HP_MAX = 6
MATCH_SECONDS_DEFAULT = 120
MIN_TO_START = 2

# Tick rate
SERVER_TICK_RATE = 20        # server ticks per second
SATURN_FPS = 60
TICK_RATIO = SATURN_FPS // SERVER_TICK_RATE

# ---- SNCP auth opcodes ----
MSG_CONNECT = 0x01
MSG_SET_USERNAME = 0x02
MSG_HEARTBEAT = 0x04
MSG_DISCONNECT = 0x05
MSG_USERNAME_REQUIRED = 0x81
MSG_WELCOME = 0x82
MSG_WELCOME_BACK = 0x83
MSG_USERNAME_TAKEN = 0x84

# ---- Utenyaa client->server opcodes ----
UNET_READY = 0x10
UNET_INPUT_STATE = 0x11
UNET_START_GAME_REQ = 0x12
UNET_PAUSE_REQ = 0x13
UNET_PLAYER_STATE = 0x14
UNET_CHARACTER_SELECT = 0x15
UNET_BOT_ADD = 0x16
UNET_BOT_REMOVE = 0x17
UNET_ADD_LOCAL_PLAYER = 0x18
UNET_REMOVE_LOCAL_PLAYER = 0x19
UNET_INPUT_STATE_P2 = 0x1A
UNET_LEADERBOARD_REQ = 0x1B
UNET_CLIENT_DEATH = 0x1C
UNET_CLIENT_DEATH_P2 = 0x1D
UNET_CLIENT_FIRE_BULLET = 0x1E
UNET_CLIENT_DROP_MINE = 0x1F
UNET_CLIENT_THROW_BOMB = 0x20
UNET_CLIENT_PICKUP_CRATE = 0x21
UNET_STAGE_VOTE = 0x22
UNET_STAGE_LOADED_ACK = 0x23
UNET_CHARACTER_SELECT_P2 = 0x24

# ---- Utenyaa server->client opcodes ----
UNET_LOBBY_STATE = 0xA0
UNET_GAME_START = 0xA1
UNET_INPUT_RELAY = 0xA2
UNET_PLAYER_JOIN = 0xA3
UNET_PLAYER_LEAVE = 0xA4
UNET_GAME_OVER = 0xA5
UNET_LOG = 0xA6
UNET_PAUSE_ACK = 0xA7
UNET_MATCH_TIMER = 0xA8
UNET_PLAYER_SYNC = 0xA9
UNET_BULLET_SPAWN = 0xAA
UNET_MINE_SPAWN = 0xAB
UNET_BOMB_SPAWN = 0xAC
UNET_EXPLOSION = 0xAD
UNET_CRATE_SPAWN = 0xAE
UNET_CRATE_DESTROY = 0xAF
UNET_DAMAGE = 0xB0
UNET_PLAYER_KILL = 0xB1
UNET_SUDDEN_DEATH = 0xB2
UNET_SCORE_UPDATE = 0xB3
UNET_LEADERBOARD_DATA = 0xB4
UNET_LOCAL_PLAYER_ACK = 0xB5
UNET_CHARACTER_TAKEN = 0xB6
UNET_STAGE_VOTE_TALLY = 0xB7

PICKUP_HEALTH = 0
PICKUP_BOMB = 1
PICKUP_MINE = 2

BOT_NAMES = ["BOT-A", "BOT-B", "BOT-C", "BOT-D"]


# ==========================================================================
# SNCP Framing helpers
# ==========================================================================

def _clamp16(v: int) -> int:
    return max(-32768, min(32767, int(v)))


def encode_frame(payload: bytes) -> bytes:
    return struct.pack("!H", len(payload)) + payload


def encode_lp_string(s: str) -> bytes:
    raw = s.encode("utf-8")[:255]
    return struct.pack("B", len(raw)) + raw


def encode_uuid(uuid_str: str) -> bytes:
    raw = uuid_str.encode("ascii")[:UUID_LEN]
    return raw.ljust(UUID_LEN, b'\x00')


def read_lp_string(buf: bytes, off: int):
    if off >= len(buf):
        return None, off
    n = buf[off]
    off += 1
    if off + n > len(buf):
        return None, off
    return buf[off:off + n].decode("utf-8", "replace"), off + n


# ==========================================================================
# Message builders
# ==========================================================================

def build_username_required() -> bytes:
    return encode_frame(bytes([MSG_USERNAME_REQUIRED]))


def build_welcome(user_id: int, uuid_str: str, username: str, back=False) -> bytes:
    op = MSG_WELCOME_BACK if back else MSG_WELCOME
    payload = (bytes([op, user_id & 0xFF])
               + encode_uuid(uuid_str)
               + encode_lp_string(username))
    return encode_frame(payload)


def build_username_taken() -> bytes:
    return encode_frame(bytes([MSG_USERNAME_TAKEN]))


def build_lobby_state(players: list) -> bytes:
    count = min(len(players), MAX_PLAYERS)
    payload = bytes([UNET_LOBBY_STATE, count])
    for p in players[:count]:
        payload += struct.pack("B", p["id"])
        payload += encode_lp_string(p["name"])
        payload += struct.pack("B", 1 if p["ready"] else 0)
        payload += struct.pack("B", p.get("character_id", 0xFF) & 0xFF)
        payload += struct.pack("B", p.get("stage_vote", 0xFF) & 0xFF)
    return encode_frame(payload)


def build_stage_vote_tally(tally: list) -> bytes:
    payload = bytes([UNET_STAGE_VOTE_TALLY])
    for i in range(STAGE_COUNT):
        payload += bytes([i & 0xFF, tally[i] & 0xFF])
    return encode_frame(payload)


def build_game_start(seed: int, my_id: int, stage: int, player_count: int,
                     match_seconds: int, crates: list) -> bytes:
    payload = bytes([UNET_GAME_START])
    payload += struct.pack("!I", seed & 0xFFFFFFFF)
    payload += bytes([my_id & 0xFF, stage & 0xFF, player_count & 0xFF])
    payload += struct.pack("!H", match_seconds & 0xFFFF)
    payload += bytes([len(crates) & 0xFF])
    for c in crates:
        payload += bytes([c["slot"] & 0xFF])
        payload += struct.pack("!iii", c["x"] & 0xFFFFFFFF, c["y"] & 0xFFFFFFFF, c["z"] & 0xFFFFFFFF)
        payload += bytes([c["flags"] & 0xFF])
    return encode_frame(payload)


def build_input_relay(pid: int, frame: int, input_bits: int) -> bytes:
    payload = bytes([UNET_INPUT_RELAY, pid & 0xFF])
    payload += struct.pack("!H", frame & 0xFFFF)
    payload += bytes([input_bits & 0xFF])
    return encode_frame(payload)


def build_player_sync(pid: int, x: int, y: int, z: int,
                      dx: int, dy: int, dz: int,
                      angle: int, hp: int, pickup: int) -> bytes:
    payload = bytes([UNET_PLAYER_SYNC, pid & 0xFF])
    payload += struct.pack("!iiiiii", x, y, z, dx, dy, dz)
    payload += struct.pack("!h", _clamp16(angle))
    payload += bytes([hp & 0xFF, pickup & 0xFF])
    return encode_frame(payload)


def build_bullet_spawn(eid: int, origin: int, x, y, z, dx, dy, dz) -> bytes:
    payload = bytes([UNET_BULLET_SPAWN])
    payload += struct.pack("!H", eid & 0xFFFF)
    payload += bytes([origin & 0xFF])
    payload += struct.pack("!iiiiii", x, y, z, dx, dy, dz)
    return encode_frame(payload)


def build_mine_spawn(eid: int, origin: int, x, y, z) -> bytes:
    payload = bytes([UNET_MINE_SPAWN])
    payload += struct.pack("!H", eid & 0xFFFF)
    payload += bytes([origin & 0xFF])
    payload += struct.pack("!iii", x, y, z)
    return encode_frame(payload)


def build_bomb_spawn(eid: int, origin: int, x, y, z, dx, dy, dz) -> bytes:
    payload = bytes([UNET_BOMB_SPAWN])
    payload += struct.pack("!H", eid & 0xFFFF)
    payload += bytes([origin & 0xFF])
    payload += struct.pack("!iiiiii", x, y, z, dx, dy, dz)
    return encode_frame(payload)


def build_crate_spawn(slot: int, x, y, z, flags: int) -> bytes:
    payload = bytes([UNET_CRATE_SPAWN, slot & 0xFF])
    payload += struct.pack("!iii", x, y, z)
    payload += bytes([flags & 0xFF])
    return encode_frame(payload)


def build_crate_destroy(slot: int, picker: int, pickup_type: int) -> bytes:
    return encode_frame(bytes([UNET_CRATE_DESTROY, slot & 0xFF,
                               picker & 0xFF, pickup_type & 0xFF]))


def build_damage(victim: int, attacker: int, amount: int, new_hp: int) -> bytes:
    return encode_frame(bytes([UNET_DAMAGE, victim & 0xFF, attacker & 0xFF,
                               amount & 0xFF, new_hp & 0xFF]))


def build_player_kill(victim: int, attacker: int) -> bytes:
    return encode_frame(bytes([UNET_PLAYER_KILL, victim & 0xFF, attacker & 0xFF]))


def build_match_timer(seconds_left: int) -> bytes:
    return encode_frame(bytes([UNET_MATCH_TIMER])
                        + struct.pack("!H", seconds_left & 0xFFFF))


def build_sudden_death() -> bytes:
    return encode_frame(bytes([UNET_SUDDEN_DEATH]))


def build_score_update(pid: int, kills: int, deaths: int, best_hp: int) -> bytes:
    return encode_frame(bytes([UNET_SCORE_UPDATE, pid & 0xFF,
                               kills & 0xFF, deaths & 0xFF, best_hp & 0xFF]))


def build_game_over(winner_id: int, sudden: bool, scores: dict) -> bytes:
    payload = bytes([UNET_GAME_OVER, winner_id & 0xFF, 1 if sudden else 0])
    items = list(scores.items())[:MAX_PLAYERS]
    payload += bytes([len(items)])
    for pid, s in items:
        payload += bytes([pid & 0xFF, s["hp"] & 0xFF,
                          s["kills"] & 0xFF, s["deaths"] & 0xFF])
    return encode_frame(payload)


def build_player_join(pid: int, name: str, char_id: int) -> bytes:
    payload = bytes([UNET_PLAYER_JOIN, pid & 0xFF])
    payload += encode_lp_string(name)
    payload += bytes([char_id & 0xFF])
    return encode_frame(payload)


def build_player_leave(pid: int) -> bytes:
    return encode_frame(bytes([UNET_PLAYER_LEAVE, pid & 0xFF]))


def build_log(text: str) -> bytes:
    raw = text.encode("utf-8")[:255]
    return encode_frame(bytes([UNET_LOG, len(raw)]) + raw)


def build_pause_ack(paused: bool) -> bytes:
    return encode_frame(bytes([UNET_PAUSE_ACK, 1 if paused else 0]))


def build_local_player_ack(pid: int) -> bytes:
    return encode_frame(bytes([UNET_LOCAL_PLAYER_ACK, pid & 0xFF]))


def build_character_taken(char_id: int) -> bytes:
    return encode_frame(bytes([UNET_CHARACTER_TAKEN, char_id & 0xFF]))


def build_leaderboard_data(entries: list) -> bytes:
    count = min(len(entries), 10)
    payload = bytes([UNET_LEADERBOARD_DATA, count])
    for e in entries[:count]:
        name_bytes = e["name"].encode("utf-8")[:16]
        payload += struct.pack("B", len(name_bytes)) + name_bytes
        payload += struct.pack("!H", min(e.get("wins", 0), 65535))
        payload += bytes([min(e.get("best_hp", 0), 255)])
        payload += struct.pack("!HHH",
                               min(e.get("kills", 0), 65535),
                               min(e.get("deaths", 0), 65535),
                               min(e.get("games_played", 0), 65535))
    return encode_frame(payload)


# ==========================================================================
# Game state
# ==========================================================================

class UtenyaaPlayer:
    def __init__(self, pid: int, name: str, char_id: int):
        self.pid = pid
        self.name = name
        self.character_id = char_id
        self.hp = HP_MAX
        self.kills = 0
        self.deaths = 0
        self.alive = True
        self.x = self.y = self.z = 0
        self.dx = self.dy = self.dz = 0
        self.angle = 0
        self.pickup = 0xFF  # none
        self.is_bot = False
        self.last_input = 0
        self.sent_death = False


class Crate:
    def __init__(self, slot: int, flags: int, x: int, y: int, z: int):
        self.slot = slot
        self.flags = flags   # bitfield: Health|Bomb|Mine
        self.x = x
        self.y = y
        self.z = z
        self.active = True
        self.respawn_timer = 0.0  # seconds until respawn when inactive


class MatchState:
    def __init__(self, stage_id: int, match_seconds: int, seed: int):
        self.stage_id = stage_id
        self.match_seconds_total = match_seconds
        self.match_seconds_left = match_seconds
        self.seed = seed
        self.sudden_death = False
        self.game_over = False
        self.winner_id = 0xFF
        self.crates: list[Crate] = []
        self.next_entity_id = 1
        self.started_monotonic = time.monotonic()

    def alloc_entity_id(self) -> int:
        eid = self.next_entity_id
        self.next_entity_id = (self.next_entity_id + 1) & 0xFFFF
        if self.next_entity_id == 0:
            self.next_entity_id = 1
        return eid


# ==========================================================================
# Crate roster generator (per-stage deterministic from seed)
# ==========================================================================

STAGE_CRATE_POSITIONS = {
    # Simple grid spawns — in fxp 16.16 units (pos * 65536). Real spawn
    # positions come from the .UTE map on the client; the server only
    # needs to agree on WHICH slots exist and their pickup-flag bitmask.
    # Each tuple: (x, y, z, flags). 'flags' = bits(Health|Bomb|Mine).
    0: [  # Island
        (   0 << 16,    0 << 16, 0, 0b111),
        ( 300 << 16,    0 << 16, 0, 0b011),
        (-300 << 16,    0 << 16, 0, 0b101),
        (   0 << 16,  300 << 16, 0, 0b110),
    ],
    1: [  # Cross
        (   0 << 16,    0 << 16, 0, 0b111),
        ( 400 << 16,    0 << 16, 0, 0b010),
        (   0 << 16,  400 << 16, 0, 0b100),
        (-400 << 16,    0 << 16, 0, 0b001),
        (   0 << 16, -400 << 16, 0, 0b111),
    ],
    2: [  # Valley
        ( 200 << 16,  200 << 16, 0, 0b111),
        (-200 << 16,  200 << 16, 0, 0b110),
        ( 200 << 16, -200 << 16, 0, 0b101),
        (-200 << 16, -200 << 16, 0, 0b011),
    ],
    3: [  # Railway
        (   0 << 16, -400 << 16, 0, 0b111),
        (   0 << 16,    0 << 16, 0, 0b011),
        (   0 << 16,  400 << 16, 0, 0b111),
        ( 300 << 16,  200 << 16, 0, 0b010),
        (-300 << 16,  200 << 16, 0, 0b100),
    ]
}
CRATE_RESPAWN_SECONDS = 8.0


def build_crate_roster(stage_id: int) -> list:
    positions = STAGE_CRATE_POSITIONS.get(stage_id, STAGE_CRATE_POSITIONS[0])
    roster = []
    for slot, (x, y, z, flags) in enumerate(positions):
        roster.append({"slot": slot, "x": x, "y": y, "z": z, "flags": flags})
    return roster


# ==========================================================================
# Stub Bot AI — a bot just drifts and occasionally fires. Real tank AI is
# a follow-up task; keep the hook so lobby +bot works end-to-end.
# ==========================================================================

class BotAI:
    """Cycling tank AI modeled after the Disasteroids / Flicky bot pattern:
    HUNT → ATTACK → STRAFE → EVADE → CRUISE, rotating every ~3 seconds
    or when state-specific triggers fire (target lost, low HP, etc.).

    Output each tick: (heading_radians, should_fire, should_drop_mine,
    should_throw_bomb, dx_fxp, dy_fxp) — server applies dx/dy to the
    bot's x/y each tick so its position actually advances instead of
    drifting on a static snapshot.
    """
    STATE_CRUISE = 0
    STATE_HUNT   = 1
    STATE_ATTACK = 2
    STATE_STRAFE = 3
    STATE_EVADE  = 4

    def __init__(self):
        self.tick = 0
        self.heading = 0.0
        self.state = self.STATE_CRUISE
        self.state_timer = 0

    def _pick_target(self, self_pid: int, players: dict):
        """Return (pid, player) of nearest alive non-self player, or None."""
        candidates = [(pid, p) for pid, p in players.items()
                      if pid != self_pid and p.alive and p.hp > 0]
        if not candidates:
            return None
        me = players.get(self_pid)
        if me is None:
            return candidates[0]
        def sqdist(p):
            dx = (p[1].x - me.x) >> 16
            dy = (p[1].y - me.y) >> 16
            return dx * dx + dy * dy
        candidates.sort(key=sqdist)
        return candidates[0]

    def decide(self, self_pid: int, players: dict, crates: list):
        self.tick += 1
        self.state_timer += 1

        me = players.get(self_pid)
        if me is None:
            return 0.0, False, False, False, 0, 0

        # State transitions
        if self.state_timer > 180:   # 9s max per state
            self.state_timer = 0
            self.state = random.choice([self.STATE_HUNT, self.STATE_CRUISE,
                                        self.STATE_STRAFE])
        if me.hp <= 2 and self.state != self.STATE_EVADE:
            self.state = self.STATE_EVADE
            self.state_timer = 0

        target = self._pick_target(self_pid, players)

        # Default outputs
        dx = dy = 0
        fire = False
        drop_mine = False
        throw_bomb = False
        heading = self.heading

        # Speed: 1 fxp unit per tick in world space (~= ~20 units/sec)
        SPEED = 1 << 16

        if self.state == self.STATE_CRUISE:
            # Random walk
            if self.tick % 30 == 0:
                self.heading = random.uniform(-3.14, 3.14)
            heading = self.heading
            dx = int(SPEED * math_cos(heading))
            dy = int(SPEED * math_sin(heading))

        elif self.state in (self.STATE_HUNT, self.STATE_ATTACK):
            if target is None:
                self.state = self.STATE_CRUISE
                self.state_timer = 0
            else:
                tpid, tp = target
                dx_to = tp.x - me.x
                dy_to = tp.y - me.y
                heading = math_atan2(dy_to, dx_to)
                self.heading = heading
                # In HUNT we close distance; in ATTACK we hold and fire
                dist_sq = ((dx_to >> 16) ** 2) + ((dy_to >> 16) ** 2)
                if dist_sq > 50 * 50:
                    self.state = self.STATE_HUNT
                    dx = int(SPEED * math_cos(heading))
                    dy = int(SPEED * math_sin(heading))
                else:
                    self.state = self.STATE_ATTACK
                    fire = (self.tick % 12 == 0)

        elif self.state == self.STATE_STRAFE:
            # Circle around target
            if target is None:
                self.state = self.STATE_CRUISE
            else:
                tpid, tp = target
                dx_to = tp.x - me.x
                dy_to = tp.y - me.y
                toward = math_atan2(dy_to, dx_to)
                heading = toward + 1.57   # 90° sidestep
                self.heading = toward     # face target
                dx = int((SPEED >> 1) * math_cos(heading))
                dy = int((SPEED >> 1) * math_sin(heading))
                fire = (self.tick % 20 == 0)

        elif self.state == self.STATE_EVADE:
            # Retreat away from nearest threat, drop mine occasionally
            if target is None:
                self.state = self.STATE_CRUISE
            else:
                tpid, tp = target
                dx_to = tp.x - me.x
                dy_to = tp.y - me.y
                away = math_atan2(dy_to, dx_to) + 3.14
                heading = away
                self.heading = heading
                dx = int(SPEED * math_cos(heading))
                dy = int(SPEED * math_sin(heading))
                drop_mine = (self.tick % 90 == 0)
                if self.state_timer > 120:
                    self.state = self.STATE_CRUISE
                    self.state_timer = 0

        return heading, fire, drop_mine, throw_bomb, dx, dy


# Tiny math shims so we avoid importing math at module-top above constants
def math_cos(x): import math; return math.cos(x)
def math_sin(x): import math; return math.sin(x)
def math_atan2(y, x): import math; return math.atan2(y, x)


class BotPlayer:
    def __init__(self, name: str, index: int):
        self.name = name
        self.index = index
        self.ai = BotAI()
        self.pid = 0xFF
        self.character_id = (MAX_PLAYERS + index) % MAX_CHARACTERS
        self.stage_vote = index % STAGE_COUNT
        self.ready = True  # bots auto-ready
        self.hp = HP_MAX
        self.kills = 0
        self.deaths = 0
        # Respawn pause — when bot dies, pause AI/physics/SYNC for N ticks
        # (Disasteroids QA-pass fix: otherwise bots kept broadcasting a
        # zombie SHIP_SYNC during their respawn window).
        self.respawn_timer = 0


# ==========================================================================
# Client info
# ==========================================================================

class ClientInfo:
    def __init__(self, sock: socket.socket, address: tuple):
        self.socket = sock
        self.address = address
        self.uuid = ""
        self.username = ""
        self.user_id = 0
        self.authenticated = False
        self.recv_buffer = b""
        self.last_activity = time.time()
        # Lobby
        self.ready = False
        self.character_id = 0xFF
        self.stage_vote = 0xFF
        self.stage_loaded = False
        # Game
        self.in_game = False
        self.game_pid = 0
        # Co-op
        self.local_names: list = []
        self.local_pids: list = []

    def send_raw(self, data: bytes) -> bool:
        try:
            self.socket.sendall(data)
            return True
        except OSError:
            return False


# ==========================================================================
# Utenyaa Server
# ==========================================================================

class UtenyaaServer:
    def __init__(self, host="0.0.0.0", port=4825, num_bots=0,
                 admin_port=0, admin_user="admin", admin_password="utenyaa2026"):
        self.host = host
        self.port = port
        self.clients: dict[int, ClientInfo] = {}
        self.uuid_map: dict[str, int] = {}
        self.server_socket: socket.socket | None = None
        self._running = False
        self._start_time = time.time()

        # Bridge auth
        self.pending_auth: dict = {}
        self.authenticated_bridges: set = set()

        # Match state
        self.match: MatchState | None = None
        self.paused = False
        self.match_seconds = MATCH_SECONDS_DEFAULT

        # Live-tunable knobs (admin REST) — experimental tuning surface.
        # Starting values match the "passthrough default" baseline so the
        # server behaves identically to an untouched build unless the
        # operator actively twists a knob. See /api/tune_* endpoints.
        self.tune = {
            "relay_player_sync":      True,   # rebroadcast PLAYER_STATE as PLAYER_SYNC
            "relay_input":            True,   # rebroadcast INPUT_STATE as INPUT_RELAY
            "crate_respawn_seconds":  CRATE_RESPAWN_SECONDS,
            "match_seconds_default":  MATCH_SECONDS_DEFAULT,
            "friendly_fire":          True,   # user-locked ON but toggleable for test
            "allow_bots_start":       True,   # whether bots alone can meet MIN_TO_START
            "timer_broadcast_hz":     1,      # MATCH_TIMER broadcasts per second
            "sudden_death_enabled":   True,
        }

        # Lobby players (keyed by game_pid 0..3, mapped from client user_id)
        self.game_players: dict[int, UtenyaaPlayer] = {}

        # Bots
        self.bots: list[BotPlayer] = []
        for i in range(num_bots):
            self.bots.append(BotPlayer(BOT_NAMES[i % len(BOT_NAMES)], i))

        # Leaderboard
        self.leaderboard: dict[str, dict] = {}
        self._lb_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                     "utenyaa_leaderboard.json")
        self._load_leaderboard()

        # Admin
        self._admin_port = admin_port
        self._admin_user = admin_user
        self._admin_password = admin_password
        self._admin_cmds = queue.Queue()
        self._admin_httpd = None
        self._admin_thread = None

        # Join history (Flicky pattern — admin portal wants a timeline)
        self._join_history: list = []
        self._join_history_path = os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "utenyaa_join_history.json")
        self._load_join_history()

        self._last_tick = 0.0
        self._tick_interval = 1.0 / SERVER_TICK_RATE

    # ---------- leaderboard persistence ----------

    def _load_leaderboard(self):
        try:
            if os.path.exists(self._lb_path):
                with open(self._lb_path, "r") as f:
                    self.leaderboard = json.load(f).get("players", {})
                log.info("Loaded leaderboard: %d players", len(self.leaderboard))
        except Exception as e:
            log.warning("Failed to load leaderboard: %s", e)
            self.leaderboard = {}

    def _save_leaderboard(self):
        try:
            with open(self._lb_path, "w") as f:
                json.dump({"players": self.leaderboard}, f, indent=2)
        except Exception as e:
            log.warning("Failed to save leaderboard: %s", e)

    def _load_join_history(self):
        try:
            if os.path.exists(self._join_history_path):
                with open(self._join_history_path, "r") as f:
                    self._join_history = json.load(f)
        except Exception as e:
            log.warning("Failed to load join history: %s", e)
            self._join_history = []

    def _save_join_history(self):
        try:
            # Cap at last 500 entries to keep file small
            self._join_history = self._join_history[-500:]
            with open(self._join_history_path, "w") as f:
                json.dump(self._join_history, f, indent=2)
        except Exception as e:
            log.warning("Failed to save join history: %s", e)

    def _record_join(self, username: str, address):
        self._join_history.append({
            "t":    int(time.time()),
            "name": username,
            "addr": str(address[0]) if address else "?"
        })
        self._save_join_history()

    # ---------- player/client helpers ----------

    def _next_game_pid(self) -> int:
        used = set(p.pid for p in self.game_players.values())
        for i in range(MAX_PLAYERS):
            if i not in used:
                return i
        return 0xFF

    def _broadcast(self, data: bytes, exclude_uid: int = -1):
        for uid, c in list(self.clients.items()):
            if uid == exclude_uid:
                continue
            if not c.authenticated:
                continue
            c.send_raw(data)

    def _lobby_roster(self) -> list:
        out = []
        # Humans
        for uid, c in self.clients.items():
            if not c.authenticated:
                continue
            pid = c.game_pid if c.in_game else uid % MAX_PLAYERS
            out.append({
                "id": uid,
                "name": c.username,
                "ready": c.ready,
                "character_id": c.character_id,
                "stage_vote": c.stage_vote
            })
        # Bots
        for bot in self.bots:
            out.append({
                "id": 200 + bot.index,
                "name": bot.name,
                "ready": True,
                "character_id": bot.character_id,
                "stage_vote": bot.stage_vote
            })
        return out[:MAX_PLAYERS]

    def _stage_tally(self) -> list:
        tally = [0] * STAGE_COUNT
        for c in self.clients.values():
            if c.authenticated and 0 <= c.stage_vote < STAGE_COUNT:
                tally[c.stage_vote] += 1
        for bot in self.bots:
            if 0 <= bot.stage_vote < STAGE_COUNT:
                tally[bot.stage_vote] += 1
        return tally

    def _character_taken(self, char_id: int, except_uid: int) -> bool:
        for uid, c in self.clients.items():
            if uid == except_uid:
                continue
            if c.authenticated and c.character_id == char_id:
                return True
        for bot in self.bots:
            if bot.character_id == char_id:
                return True
        return False

    def _broadcast_lobby(self):
        roster = self._lobby_roster()
        self._broadcast(build_lobby_state(roster))
        self._broadcast(build_stage_vote_tally(self._stage_tally()))

    # ---------- lobby actions ----------

    def _on_ready(self, c: ClientInfo):
        c.ready = not c.ready
        self._broadcast_lobby()

    def _on_character_select(self, c: ClientInfo, char_id: int):
        if char_id >= MAX_CHARACTERS:
            return
        if self._character_taken(char_id, c.user_id):
            c.send_raw(build_character_taken(char_id))
            return
        c.character_id = char_id
        self._broadcast_lobby()

    def _on_stage_vote(self, c: ClientInfo, stage_id: int):
        if stage_id >= STAGE_COUNT:
            return
        c.stage_vote = stage_id
        self._broadcast_lobby()

    def _on_bot_add(self):
        if len(self.bots) + self._human_count() >= MAX_PLAYERS:
            return
        idx = len(self.bots)
        self.bots.append(BotPlayer(BOT_NAMES[idx % len(BOT_NAMES)], idx))
        self._broadcast_lobby()

    def _on_bot_remove(self):
        if not self.bots:
            return
        self.bots.pop()
        self._broadcast_lobby()

    def _human_count(self) -> int:
        return sum(1 for c in self.clients.values() if c.authenticated)

    # ---------- game start / run / end ----------

    def _pick_stage(self) -> int:
        tally = self._stage_tally()
        max_votes = max(tally)
        if max_votes == 0:
            return random.randint(0, STAGE_COUNT - 1)
        winners = [i for i, v in enumerate(tally) if v == max_votes]
        return random.choice(winners)

    def _on_start_game_req(self, c: ClientInfo):
        if self.match is not None:
            return
        ready_humans = [cc for cc in self.clients.values()
                        if cc.authenticated and cc.ready]
        # A single console running co-op counts as 2 slots already via
        # ADD_LOCAL_PLAYER, but co-op isn't yet wired to add a bot-equivalent
        # slot in this server revision. Require 1 ready human + any mix of
        # bots / other humans totalling >= MIN_TO_START.
        total_slots = len(ready_humans) + len(self.bots)
        # P2 locals contribute extra slots
        for cc in ready_humans:
            total_slots += len(cc.local_pids)
        if total_slots < MIN_TO_START:
            c.send_raw(build_log(f"Need {MIN_TO_START} players to start"))
            return

        stage = self._pick_stage()
        seed = random.randint(1, 0xFFFFFFFF)
        self.match = MatchState(stage, self.match_seconds, seed)
        self.match.crates = [Crate(x["slot"], x["flags"], x["x"], x["y"], x["z"])
                             for x in build_crate_roster(stage)]

        # Assign game PIDs 0..N-1. Humans first, then bots.
        next_pid = 0
        for cc in ready_humans:
            if next_pid >= MAX_PLAYERS: break
            cc.in_game = True
            cc.game_pid = next_pid
            self.game_players[next_pid] = UtenyaaPlayer(
                next_pid, cc.username, cc.character_id if cc.character_id != 0xFF else next_pid)
            next_pid += 1
            # Local co-op players each get their own PID
            for _ in cc.local_pids[:]:
                if next_pid >= MAX_PLAYERS: break
                self.game_players[next_pid] = UtenyaaPlayer(
                    next_pid, cc.username + "-2", (next_pid + 5) % MAX_CHARACTERS)
                next_pid += 1
        for bot in self.bots:
            if next_pid >= MAX_PLAYERS: break
            bot.pid = next_pid
            bp = UtenyaaPlayer(next_pid, bot.name, bot.character_id)
            bp.is_bot = True
            self.game_players[next_pid] = bp
            next_pid += 1

        crate_roster = [{"slot": k.slot, "x": k.x, "y": k.y, "z": k.z, "flags": k.flags}
                        for k in self.match.crates]

        for cc in self.clients.values():
            if not cc.authenticated or not cc.in_game:
                continue
            cc.send_raw(build_game_start(seed, cc.game_pid, stage, next_pid,
                                         self.match_seconds, crate_roster))
        log.info("Match started: stage=%s seed=%08X players=%d",
                 STAGE_NAMES[stage], seed, next_pid)

    def _end_match(self, sudden: bool = False):
        if self.match is None:
            return
        # Determine winner: highest HP among alive; tiebreaker = most kills
        best_pid = 0xFF
        best_score = (-1, -1)
        for pid, p in self.game_players.items():
            sc = (p.hp, p.kills)
            if sc > best_score:
                best_score = sc
                best_pid = pid
        self.match.winner_id = best_pid
        self.match.game_over = True

        # Leaderboard update (humans only)
        for uid, c in self.clients.items():
            if not c.in_game or c.game_pid == 0xFF:
                continue
            p = self.game_players.get(c.game_pid)
            if not p:
                continue
            entry = self.leaderboard.setdefault(c.username, {
                "name": c.username, "wins": 0, "best_hp": 0,
                "kills": 0, "deaths": 0, "games_played": 0
            })
            entry["games_played"] += 1
            entry["kills"] += p.kills
            entry["deaths"] += p.deaths
            if p.hp > entry.get("best_hp", 0):
                entry["best_hp"] = p.hp
            if c.game_pid == best_pid:
                entry["wins"] += 1
        self._save_leaderboard()

        scores = {pid: {"hp": p.hp, "kills": p.kills, "deaths": p.deaths}
                  for pid, p in self.game_players.items()}
        self._broadcast(build_game_over(best_pid, sudden, scores))

        # Reset for next match
        for c in self.clients.values():
            c.in_game = False
            c.ready = False
            c.stage_loaded = False
        self.game_players.clear()
        self.match = None
        self._broadcast_lobby()

    def _tick_match(self, dt: float):
        if self.match is None or self.match.game_over:
            return

        self.match.match_seconds_left = max(
            0, self.match.match_seconds_total
               - int(time.monotonic() - self.match.started_monotonic))

        # Broadcast timer every second
        if int(self.match.match_seconds_left) != getattr(self, "_last_timer_sent", -1):
            self._last_timer_sent = int(self.match.match_seconds_left)
            self._broadcast(build_match_timer(self.match.match_seconds_left))

        # Crate respawn
        for crate in self.match.crates:
            if not crate.active:
                crate.respawn_timer -= dt
                if crate.respawn_timer <= 0:
                    crate.active = True
                    self._broadcast(build_crate_spawn(
                        crate.slot, crate.x, crate.y, crate.z, crate.flags))

        # Tick bots — HUNT/ATTACK/STRAFE/EVADE cycling AI with respawn pause
        for bot in self.bots:
            if bot.pid == 0xFF: continue
            p = self.game_players.get(bot.pid)
            if not p: continue

            # Respawn pause: if bot was killed, freeze AI + skip broadcasts
            # until the respawn timer expires (3s @ 20 ticks/sec = 60 ticks).
            if not p.alive:
                if bot.respawn_timer > 0:
                    bot.respawn_timer -= 1
                elif bot.respawn_timer == 0:
                    # Respawn the bot
                    p.alive = True
                    p.hp = HP_MAX
                    # Reset position to a cheap default (client World owns
                    # real spawn positions — this is just a placeholder)
                    p.x = random.randint(-400, 400) << 16
                    p.y = random.randint(-400, 400) << 16
                continue

            heading, fire, drop_mine, throw_bomb, ddx, ddy = bot.ai.decide(
                bot.pid, self.game_players, self.match.crates)

            # Advance bot position so it actually moves in the world
            p.x = (p.x + ddx) & 0xFFFFFFFF
            p.y = (p.y + ddy) & 0xFFFFFFFF
            p.angle = int(heading * 1000)
            p.dx = ddx
            p.dy = ddy

            # Fire projectiles as commanded by AI
            if fire:
                eid = self.match.alloc_entity_id()
                # Unit heading vector, ~bullet speed baseline
                import math as _m
                vx = int(_m.cos(heading) * (10 << 16))
                vy = int(_m.sin(heading) * (10 << 16))
                self._broadcast(build_bullet_spawn(
                    eid, p.pid, p.x, p.y, p.z, vx, vy, 0))
            if drop_mine:
                eid = self.match.alloc_entity_id()
                self._broadcast(build_mine_spawn(eid, p.pid, p.x, p.y, p.z))

            # Broadcast a PLAYER_SYNC so remote clients track the bot
            if bot.ai.tick % 4 == 0:
                self._broadcast(build_player_sync(
                    p.pid, p.x, p.y, p.z, p.dx, p.dy, p.dz,
                    p.angle, p.hp, 0xFF))

        # End conditions
        alive_count = sum(1 for p in self.game_players.values() if p.alive)
        if self.match.match_seconds_left <= 0:
            # Check tie
            max_hp = max(p.hp for p in self.game_players.values() if p.alive) \
                if alive_count else 0
            tied = sum(1 for p in self.game_players.values()
                       if p.alive and p.hp == max_hp)
            if tied > 1 and not self.match.sudden_death and self.tune.get("sudden_death_enabled", True):
                self.match.sudden_death = True
                self._broadcast(build_sudden_death())
                # Extend timer indefinitely during sudden death
                self.match.match_seconds_total = 99999
                self.match.started_monotonic = time.monotonic()
            else:
                self._end_match(sudden=self.match.sudden_death)
                return

        if alive_count <= 1 and len(self.game_players) >= 2:
            self._end_match(sudden=self.match.sudden_death)

    # ---------- per-message handlers ----------

    def _handle_frame(self, c: ClientInfo, payload: bytes):
        if not payload: return
        op = payload[0]

        if op == MSG_CONNECT:
            # Payload: [op:1][optional uuid:36]
            if len(payload) >= 1 + UUID_LEN:
                existing_uuid = payload[1:1 + UUID_LEN].decode("ascii", "replace").rstrip("\x00")
                uid = self.uuid_map.get(existing_uuid)
                if uid is not None:
                    c.uuid = existing_uuid
                    c.user_id = uid
                    c.authenticated = True
                    # Remove the pre-auth negative-key slot for this
                    # ClientInfo so it isn't in self.clients twice.
                    for k in list(self.clients.keys()):
                        if self.clients[k] is c and k != uid:
                            del self.clients[k]
                    self.clients[uid] = c
                    # Previous username lookup happens via leaderboard or just reuse
                    c.send_raw(build_welcome(uid, existing_uuid, c.username, back=True))
                    self._broadcast_lobby()
                    return
            # New user — require username
            c.send_raw(build_username_required())
            return

        if op == MSG_SET_USERNAME:
            if len(payload) < 2: return
            nlen = payload[1]
            if len(payload) < 2 + nlen: return
            name = payload[2:2 + nlen].decode("utf-8", "replace").strip()[:USERNAME_MAX_LEN]
            if not name: name = "PLAYER"
            # Allow duplicates by appending a digit
            existing = {cc.username for cc in self.clients.values() if cc.authenticated}
            if name in existing:
                c.send_raw(build_username_taken())
                return
            c.username = name
            c.uuid = str(uuid.uuid4())
            # Positive user_id space only — avoid collisions with the
            # pre-auth negative keys we use for new sockets.
            positive_keys = [k for k in self.clients.keys() if k >= 0]
            c.user_id = (max(positive_keys) + 1) if positive_keys else 1
            c.authenticated = True
            # Remove the pre-auth negative-key slot so this ClientInfo
            # isn't present in self.clients twice (once as negative,
            # once as user_id) — LOBBY_STATE would otherwise double-
            # list the player and the self-row match would pick the
            # wrong id. Also re-key if we were already present
            # (reconnect via same socket).
            for k in list(self.clients.keys()):
                if self.clients[k] is c and k != c.user_id:
                    del self.clients[k]
            self.clients[c.user_id] = c
            self.uuid_map[c.uuid] = c.user_id
            c.send_raw(build_welcome(c.user_id, c.uuid, c.username, back=False))
            self._record_join(c.username, c.address)
            self._broadcast_lobby()
            return

        if op == MSG_HEARTBEAT:
            c.last_activity = time.time()
            return

        if op == MSG_DISCONNECT:
            self._drop_client(c.user_id)
            return

        if not c.authenticated:
            return

        # Game / lobby opcodes
        if op == UNET_READY:
            self._on_ready(c)
        elif op == UNET_CHARACTER_SELECT and len(payload) >= 2:
            self._on_character_select(c, payload[1])
        elif op == UNET_STAGE_VOTE and len(payload) >= 2:
            self._on_stage_vote(c, payload[1])
        elif op == UNET_BOT_ADD:
            self._on_bot_add()
        elif op == UNET_BOT_REMOVE:
            self._on_bot_remove()
        elif op == UNET_START_GAME_REQ:
            self._on_start_game_req(c)
        elif op == UNET_PAUSE_REQ:
            self.paused = not self.paused
            self._broadcast(build_pause_ack(self.paused))
        elif op == UNET_INPUT_STATE and len(payload) >= 4:
            if self.match is None: return
            if not self.tune.get("relay_input", True): return
            frame = (payload[1] << 8) | payload[2]
            bits = payload[3]
            self._broadcast(build_input_relay(c.game_pid, frame, bits),
                            exclude_uid=c.user_id)
        elif op == UNET_PLAYER_STATE:
            if self.match is None: return
            # P1 format: op(1) + 6×i32(24) + angle(i16) + hp(1) = 28 bytes
            # P2 format: op(1) + pid(1) + 6×i32(24) + angle(i16) + hp(1) = 29 bytes
            # Dispatch by length — client sends the P2 variant via
            # unet_send_player_state_p2() which prepends the co-op pid.
            if len(payload) == 29:
                target_pid = payload[1]
                x, y, z, dx, dy, dz = struct.unpack("!iiiiii", payload[2:26])
                angle = struct.unpack("!h", payload[26:28])[0]
                hp = payload[28]
            elif len(payload) >= 28:
                target_pid = c.game_pid
                x, y, z, dx, dy, dz = struct.unpack("!iiiiii", payload[1:25])
                angle = struct.unpack("!h", payload[25:27])[0]
                hp = payload[27]
            else:
                return
            p = self.game_players.get(target_pid)
            if p:
                p.x, p.y, p.z, p.dx, p.dy, p.dz = x, y, z, dx, dy, dz
                p.angle = angle
                p.hp = hp
            if self.tune.get("relay_player_sync", True):
                self._broadcast(build_player_sync(target_pid, x, y, z, dx, dy, dz,
                                                  angle, hp, p.pickup if p else 0xFF),
                                exclude_uid=c.user_id)
        elif op == UNET_CLIENT_FIRE_BULLET and len(payload) >= 1 + 24:
            if self.match is None: return
            x, y, z, dx, dy, dz = struct.unpack("!iiiiii", payload[1:25])
            eid = self.match.alloc_entity_id()
            self._broadcast(build_bullet_spawn(eid, c.game_pid, x, y, z, dx, dy, dz))
        elif op == UNET_CLIENT_DROP_MINE and len(payload) >= 1 + 12:
            if self.match is None: return
            x, y, z = struct.unpack("!iii", payload[1:13])
            eid = self.match.alloc_entity_id()
            self._broadcast(build_mine_spawn(eid, c.game_pid, x, y, z))
        elif op == UNET_CLIENT_THROW_BOMB and len(payload) >= 1 + 24:
            if self.match is None: return
            x, y, z, dx, dy, dz = struct.unpack("!iiiiii", payload[1:25])
            eid = self.match.alloc_entity_id()
            self._broadcast(build_bomb_spawn(eid, c.game_pid, x, y, z, dx, dy, dz))
        elif op == UNET_CLIENT_PICKUP_CRATE and len(payload) >= 2:
            if self.match is None: return
            slot = payload[1]
            if 0 <= slot < len(self.match.crates):
                crate = self.match.crates[slot]
                if crate.active:
                    # Pick a pickup type from allowed bits
                    options = []
                    if crate.flags & 0b001: options.append(PICKUP_HEALTH)
                    if crate.flags & 0b010: options.append(PICKUP_BOMB)
                    if crate.flags & 0b100: options.append(PICKUP_MINE)
                    if options:
                        ptype = random.choice(options)
                        crate.active = False
                        crate.respawn_timer = self.tune["crate_respawn_seconds"]
                        self._broadcast(build_crate_destroy(slot, c.game_pid, ptype))
                        # Health pickup applies directly
                        p = self.game_players.get(c.game_pid)
                        if p and ptype == PICKUP_HEALTH:
                            p.hp = min(HP_MAX, p.hp + 2)
                            self._broadcast(build_damage(p.pid, p.pid, 0, p.hp))
        elif op == UNET_CLIENT_DEATH:
            if self.match is None: return
            p = self.game_players.get(c.game_pid)
            if p and p.alive:
                p.alive = False
                p.hp = 0
                p.deaths += 1
                self._broadcast(build_player_kill(p.pid, 0xFF))
                self._broadcast(build_score_update(p.pid, p.kills, p.deaths,
                                                   max(p.hp, 0)))
                # If any bot was the attacker attribution, bump their kill
                # count here in a future revision. For now: trigger respawn
                # timer if THIS player was a bot (wouldn't happen normally —
                # bots don't send CLIENT_DEATH — but future client-side
                # damage attribution might). Bot respawn is managed via
                # the alive=False gate in the tick loop.
                for bot in self.bots:
                    if bot.pid == p.pid:
                        bot.respawn_timer = 60   # 3s @ 20 ticks/sec
                        break
        elif op == UNET_ADD_LOCAL_PLAYER:
            name, _ = read_lp_string(payload, 1)
            if name:
                c.local_names.append(name[:USERNAME_MAX_LEN])
                # pid assigned at match start
                c.send_raw(build_local_player_ack(0xFF))
                self._broadcast_lobby()
        elif op == UNET_REMOVE_LOCAL_PLAYER:
            if c.local_names:
                c.local_names.pop()
                c.local_pids = []
                self._broadcast_lobby()
        elif op == UNET_STAGE_LOADED_ACK:
            c.stage_loaded = True
        elif op == UNET_LEADERBOARD_REQ:
            top = sorted(self.leaderboard.values(),
                         key=lambda e: (-e.get("wins", 0), -e.get("best_hp", 0)))[:10]
            c.send_raw(build_leaderboard_data(top))
        else:
            # unknown opcode — ignore
            pass

    def _drop_client(self, uid: int):
        c = self.clients.pop(uid, None)
        if not c: return
        try: c.socket.close()
        except OSError: pass
        if c.authenticated:
            self._broadcast(build_player_leave(uid))
            self._broadcast_lobby()
        log.info("Client disconnected: %s", c.username or c.address)

    # ---------- bridge auth (handshake before SNCP framing begins) ----------

    def _try_bridge_auth(self, c: ClientInfo) -> bool:
        """Consumes the AUTH magic + shared-secret handshake. Returns True
        once authenticated and the SNCP stream can begin. Same shape as
        Disasteroids/Flicky so existing DreamPi/netlink_bridge works."""
        need = len(AUTH_MAGIC) + len(SHARED_SECRET)
        if len(c.recv_buffer) < need:
            return False
        if c.recv_buffer[:len(AUTH_MAGIC)] != AUTH_MAGIC:
            return True  # No auth header — assume legacy / direct client
        if c.recv_buffer[len(AUTH_MAGIC):need] != SHARED_SECRET:
            log.warning("Bridge auth failed from %s", c.address)
            c.socket.close()
            return False
        c.recv_buffer = c.recv_buffer[need:]
        return True

    # ---------- main loop ----------

    def run(self):
        self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server_socket.bind((self.host, self.port))
        self.server_socket.listen(16)
        self.server_socket.setblocking(False)
        self._running = True
        self._last_tick = time.monotonic()
        log.info("Listening on %s:%d", self.host, self.port)
        if self._admin_port:
            self._start_admin_server()

        try:
            while self._running:
                rlist = [self.server_socket] + [c.socket for c in self.clients.values()]
                r, _, _ = select.select(rlist, [], [], 0.01)
                for sock in r:
                    if sock is self.server_socket:
                        try:
                            cs, addr = self.server_socket.accept()
                            cs.setblocking(False)
                            ci = ClientInfo(cs, addr)
                            # Temporary uid key = address until auth (negative id)
                            self.clients[-id(ci)] = ci
                        except BlockingIOError:
                            pass
                    else:
                        self._read_from(sock)

                self._process_admin_commands()

                now = time.monotonic()
                if now - self._last_tick >= self._tick_interval:
                    dt = now - self._last_tick
                    self._last_tick = now
                    self._tick_match(dt)

                # Heartbeat timeout sweep
                now_wall = time.time()
                for uid in list(self.clients.keys()):
                    c = self.clients.get(uid)
                    if c and c.authenticated and now_wall - c.last_activity > HEARTBEAT_TIMEOUT:
                        log.info("Timing out %s", c.username)
                        self._drop_client(uid)
        except KeyboardInterrupt:
            log.info("Shutting down")
        finally:
            self._save_leaderboard()
            try: self.server_socket.close()
            except OSError: pass

    def _read_from(self, sock):
        # Find client
        c = None
        key = None
        for k, cc in self.clients.items():
            if cc.socket is sock:
                c = cc; key = k; break
        if c is None: return
        try:
            data = sock.recv(MAX_RECV_BUFFER)
        except (BlockingIOError, InterruptedError):
            return
        except OSError:
            self._drop_client(key); return
        if not data:
            self._drop_client(key); return
        c.recv_buffer += data
        c.last_activity = time.time()

        # Bridge auth handshake (one-shot per connection)
        if id(c) not in self.authenticated_bridges:
            if not self._try_bridge_auth(c):
                return
            self.authenticated_bridges.add(id(c))

        # Parse SNCP frames
        while len(c.recv_buffer) >= 2:
            flen = (c.recv_buffer[0] << 8) | c.recv_buffer[1]
            if flen == 0 or flen > MAX_RECV_BUFFER:
                log.warning("Bad frame length %d from %s, dropping", flen, c.address)
                self._drop_client(key)
                return
            if len(c.recv_buffer) < 2 + flen:
                break
            payload = c.recv_buffer[2:2 + flen]
            c.recv_buffer = c.recv_buffer[2 + flen:]
            self._handle_frame(c, payload)

    # ---------- admin ----------

    def _process_admin_commands(self):
        while not self._admin_cmds.empty():
            try:
                cmd = self._admin_cmds.get_nowait()
            except queue.Empty:
                break
            try:
                fn = cmd[0]
                args = cmd[1] if len(cmd) > 1 else ()
                fn(*args)
            except Exception as e:
                log.warning("admin cmd failed: %s", e)

    def _start_admin_server(self):
        server_ref = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *a, **kw): pass

            def _auth_ok(self) -> bool:
                # Trust nginx forwarding header (pattern from saturn-admin)
                if self.headers.get("X-Admin-Auth") == "nginx-verified":
                    return True
                return False

            def do_GET(self):
                if not self._auth_ok():
                    self.send_response(401); self.end_headers(); return
                path = self.path
                if path.startswith("/api/state"):
                    # Schema matches the unified saturn-admin portal contract:
                    # {uptime, total_joins, players[...], game{...}}
                    # (see /opt/saturn-admin/unified_admin.py header docstring).
                    players_in_game = len([p for p in server_ref.game_players.values() if p.alive]) \
                        if server_ref.match else 0
                    players_list = []
                    for uid, cc in server_ref.clients.items():
                        if not cc.authenticated: continue
                        players_list.append({
                            "id": uid,
                            "name": cc.username,
                            "address": str(cc.address[0]) if cc.address else "?",
                            "ready": cc.ready,
                            "in_game": cc.in_game,
                            "character": cc.character_id,
                            "stage_vote": cc.stage_vote,
                        })
                    for bot in server_ref.bots:
                        players_list.append({
                            "id": 200 + bot.index,
                            "name": bot.name + " (bot)",
                            "address": "bot",
                            "ready": True,
                            "in_game": bot.pid != 0xFF,
                            "character": bot.character_id,
                            "stage_vote": bot.stage_vote,
                        })
                    game_dict = {
                        "active":           server_ref.match is not None,
                        "human_count":      server_ref._human_count(),
                        "bot_count":        len(server_ref.bots),
                        "players_in_game":  players_in_game,
                        "stage":            STAGE_NAMES[server_ref.match.stage_id] if server_ref.match else None,
                        "match_seconds_left": server_ref.match.match_seconds_left if server_ref.match else 0,
                        "sudden_death":     server_ref.match.sudden_death if server_ref.match else False,
                        "stage_vote_tally": server_ref._stage_tally(),
                    }
                    st = {
                        "uptime":      int(time.time() - server_ref._start_time),
                        "total_joins": len(server_ref._join_history),
                        "players":     players_list,
                        "game":        game_dict,
                    }
                    body = json.dumps(st).encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                if path.startswith("/api/leaderboard"):
                    body = json.dumps(server_ref.leaderboard).encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                if path.startswith("/api/tune"):
                    body = json.dumps(server_ref.tune).encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                if path.startswith("/api/join_history"):
                    body = json.dumps(server_ref._join_history).encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                if path.startswith("/api/clients"):
                    clients = []
                    for uid, cc in server_ref.clients.items():
                        if not cc.authenticated: continue
                        clients.append({
                            "uid":       uid,
                            "username":  cc.username,
                            "address":   str(cc.address[0]) if cc.address else "?",
                            "ready":     cc.ready,
                            "in_game":   cc.in_game,
                            "character": cc.character_id,
                            "vote":      cc.stage_vote,
                        })
                    body = json.dumps(clients).encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                self.send_response(404); self.end_headers()

            def do_POST(self):
                if not self._auth_ok():
                    self.send_response(401); self.end_headers(); return
                if self.path == "/api/kick_all":
                    server_ref._admin_cmds.put((server_ref._admin_kick_all, ()))
                    self.send_response(200); self.end_headers(); return
                if self.path == "/api/end_match":
                    server_ref._admin_cmds.put((server_ref._end_match, (False,)))
                    self.send_response(200); self.end_headers(); return
                if self.path.startswith("/api/kick/"):
                    try:
                        uid = int(self.path[len("/api/kick/"):])
                    except ValueError:
                        self.send_response(400); self.end_headers(); return
                    server_ref._admin_cmds.put((server_ref._drop_client, (uid,)))
                    self.send_response(200); self.end_headers(); return
                if self.path.startswith("/api/tune/"):
                    key = self.path[len("/api/tune/"):]
                    length = int(self.headers.get("Content-Length", "0") or "0")
                    raw = self.rfile.read(length) if length > 0 else b""
                    try:
                        payload = json.loads(raw) if raw else {}
                    except Exception:
                        payload = {}
                    if key in server_ref.tune and "value" in payload:
                        cur = server_ref.tune[key]
                        try:
                            if isinstance(cur, bool):
                                server_ref.tune[key] = bool(payload["value"])
                            elif isinstance(cur, int):
                                server_ref.tune[key] = int(payload["value"])
                            elif isinstance(cur, float):
                                server_ref.tune[key] = float(payload["value"])
                            else:
                                server_ref.tune[key] = payload["value"]
                        except Exception:
                            self.send_response(400); self.end_headers(); return
                        # Apply match_seconds_default to live match_seconds too
                        if key == "match_seconds_default":
                            server_ref.match_seconds = server_ref.tune[key]
                        log.info("admin tune: %s = %r", key, server_ref.tune[key])
                        self.send_response(200)
                        self.send_header("Content-Type", "application/json")
                        body = json.dumps({"key": key, "value": server_ref.tune[key]}).encode()
                        self.send_header("Content-Length", str(len(body)))
                        self.end_headers()
                        self.wfile.write(body)
                        return
                    self.send_response(400); self.end_headers(); return
                self.send_response(404); self.end_headers()

        try:
            self._admin_httpd = HTTPServer(("127.0.0.1", self._admin_port), Handler)
            self._admin_thread = threading.Thread(target=self._admin_httpd.serve_forever,
                                                  daemon=True)
            self._admin_thread.start()
            log.info("Admin HTTP on 127.0.0.1:%d", self._admin_port)
        except Exception as e:
            log.warning("Failed to start admin server: %s", e)

    def _admin_kick_all(self):
        for uid in list(self.clients.keys()):
            self._drop_client(uid)


# ==========================================================================
# Entry point
# ==========================================================================

def main():
    ap = argparse.ArgumentParser(description="Utenyaa NetLink game server")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=4825)
    ap.add_argument("--bots", type=int, default=0)
    ap.add_argument("--admin-port", type=int, default=9093)
    ap.add_argument("--admin-user", default="admin")
    ap.add_argument("--admin-password", default="utenyaa2026")
    args = ap.parse_args()

    srv = UtenyaaServer(host=args.host, port=args.port, num_bots=args.bots,
                        admin_port=args.admin_port,
                        admin_user=args.admin_user,
                        admin_password=args.admin_password)
    srv.run()


if __name__ == "__main__":
    main()
