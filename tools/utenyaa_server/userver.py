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
import hashlib
import json
import logging
import os
import queue
import random
import select
import socket
import struct
import sys
from collections import deque
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

# Bridge authentication. DreamPi/netlink.py speaks the Disasteroids/Flicky
# variant of the protocol: AUTH magic + 1-byte length + secret bytes. Server
# replies with a single AUTH_OK byte on success. Don't change to raw bytes —
# DreamPi won't dial us correctly.
SHARED_SECRET = b"Utenyaa2026!NetLink#Key"
AUTH_MAGIC = b"AUTH"
AUTH_OK = 0x01
AUTH_TIMEOUT = 5.0

MAX_PLAYERS = 4         # max in-match player count (game_players cap)
MAX_LOBBY   = 8         # max lobby connections (first 4 to ready play next match)
MAX_CHARACTERS = 12
MAX_CRATES = 16
STAGE_COUNT = 4
STAGE_NAMES = ["Island", "Cross", "Valley", "Railway"]
HP_MAX = 6
# Number of distinct character sprites the C++ client actually has
# loaded from CHARS.PAK (5 chars × 5 frames each — see
# unet_glue_num_characters() in src/main.cxx). The protocol-level
# UNET_MAX_CHARACTERS = 12 ceiling is bigger than the asset count;
# server-side stable assignment must mod by the actual count or
# clients render an out-of-range frame as a black sprite.
RENDERED_CHARACTER_COUNT = 5

def _stable_character_for(username: str) -> int:
    """Deterministic character index per username. Survives server
    restarts and round-to-round pid reshuffles. SHA-1 is overkill but
    cheap and explicitly stable — Python's built-in hash() randomizes
    string seeds per process which would defeat the purpose."""
    if not username:
        return 0
    digest = hashlib.sha1(username.encode("utf-8", "replace")).digest()
    return int.from_bytes(digest[:4], "big") % RENDERED_CHARACTER_COUNT
# UNETv2 PLAYER_SYNC packs HP into the low nibble of byte 11 (alongside
# pickup in the high nibble). Anything > 15 silently wraps on the wire,
# which would break damage tracking in nasty ways. Guard at startup so
# a future HP_MAX bump can't ship without widening the wire field.
assert HP_MAX <= 0x0F, "HP_MAX > 15 requires PLAYER_SYNC v2 hp field widen"
# Damage per bullet hit. Mirrors C client's Entities::Bullet::Damage = 2
# in src/Entities/Bullet.hpp. Used by the server-side lag-comp validator
# so its damage assignment matches what the victim's local bullet sim
# would have applied — no double-counting.
BULLET_DAMAGE = 2
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
UNET_CLIENT_DBG_LOG   = 0x25
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
UNET_MAP_BEGIN = 0xB8
UNET_MAP_CHUNK = 0xB9
UNET_MAP_END   = 0xBA

# Sentinel stage_id meaning "this match uses a streamed custom map".
# When the server sees use_live_map=True and live_map_slug names a
# valid .UTE in the editor's maps dir, it sends MAP_BEGIN/CHUNK/END
# before GAME_START with stage_id = UNET_STAGE_STREAMED. Saturn-side
# the World ctor consumes the streamed buffer instead of reading CD.
UNET_STAGE_STREAMED   = 0xFE
UNET_MAP_CHUNK_DATA_MAX = 120     # fits in 128-byte TX frame after op+idx+len header
UNET_MAP_MAX_SIZE       = 16384   # alloc guard, well above 11 KB .UTE
UNET_MAP_MAX_CHUNKS     = 255     # u8 chunk index

PICKUP_HEALTH = 0
PICKUP_BOMB = 1
PICKUP_MINE = 2

BOT_NAMES = ["BOT-A", "BOT-B", "BOT-C", "BOT-D"]


# ==========================================================================
# SNCP Framing helpers
# ==========================================================================

def _clamp16(v: int) -> int:
    return max(-32768, min(32767, int(v)))


def _signed32(v: int) -> int:
    """Wrap a Python int into signed int32 range for struct.pack('!i').
    Values like -19660800 (a negative fxp crate position) when masked
    with `& 0xFFFFFFFF` become 4275306496 (unsigned 32-bit) which
    overflows the !i format's -2^31..2^31-1 range. Use this helper
    instead of bare masking when packing as signed."""
    v = int(v) & 0xFFFFFFFF
    return v if v < 0x80000000 else v - 0x100000000


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
    # Lobby can hold MAX_LOBBY entries — broadcast all, not just
    # in-game-eligible MAX_PLAYERS.
    count = min(len(players), MAX_LOBBY)
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
        payload += struct.pack("!iii", _signed32(c["x"]), _signed32(c["y"]), _signed32(c["z"]))
        payload += bytes([c["flags"] & 0xFF])
    return encode_frame(payload)


def build_input_relay(pid: int, frame: int, input_bits: int) -> bytes:
    payload = bytes([UNET_INPUT_RELAY, pid & 0xFF])
    payload += struct.pack("!H", frame & 0xFFFF)
    payload += bytes([input_bits & 0xFF])
    return encode_frame(payload)


def _q_pos(fxp: int) -> int:
    """Quantize fxp 16.16 position → int16 with shift 9 (fxp 7.9 in int16).
       Range ±256 world units, 1/128-unit precision. Mirrors the C inline
       `unet_q_pos` in utenyaa_protocol.h. Clamp to int16 to avoid pack
       overflow on out-of-arena bots."""
    q = fxp >> 9
    if q > 32767:  q = 32767
    if q < -32768: q = -32768
    return q

def _q_z(fxp: int) -> int:
    """fxp 16.16 z → int8 (raw integer world unit, ±127)."""
    q = fxp >> 16
    if q > 127:  q = 127
    if q < -128: q = -128
    return q

def _q_vel(fxp: int) -> int:
    """fxp 16.16 per-frame velocity → int8 (shift 10, ±~2 units/frame)."""
    q = fxp >> 10
    if q > 127:  q = 127
    if q < -128: q = -128
    return q

def _q_angle(a16: int) -> int:
    """SGL u16 angle → high byte (256 levels = 1.4° resolution)."""
    return (a16 & 0xFFFF) >> 8

def build_player_sync(pid: int, x: int, y: int, z: int,
                      dx: int, dy: int, dz: int,
                      angle: int, hp: int, pickup: int) -> bytes:
    """PLAYER_SYNC v2 — 12 bytes total (was 30 in v1).
       Wire layout matches utenyaa_protocol.h's UNET_PLAYER_SYNC_BYTES."""
    payload = bytes([UNET_PLAYER_SYNC, pid & 0xFF])
    payload += struct.pack("!hh", _q_pos(x), _q_pos(y))
    payload += struct.pack("!bbbb",
                           _q_z(z),
                           _q_vel(dx), _q_vel(dy), _q_vel(dz))
    payload += struct.pack("!BB",
                           _q_angle(angle),
                           ((pickup & 0x0F) << 4) | (hp & 0x0F))
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


# ----------------------------------------------------------------------
# Streamed-map (.UTE byte transfer) builders. See UNET_MAP_BEGIN/CHUNK/END
# in protocol.h on the Saturn side; mirror those exactly.
# ----------------------------------------------------------------------

def crc16_ccitt_false(data: bytes) -> int:
    """CRC-16/CCITT-FALSE — poly 0x1021, init 0xFFFF, no reflection,
    no xor-out. Identical to unet_map_stream_crc16() on the Saturn so
    the END opcode's CRC validates client-side without library deps."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def build_map_begin(total_size: int, num_chunks: int, crc: int) -> bytes:
    return encode_frame(bytes([UNET_MAP_BEGIN]) +
                        struct.pack("!H", total_size & 0xFFFF) +
                        bytes([num_chunks & 0xFF]) +
                        struct.pack("!H", crc & 0xFFFF))


def build_map_chunk(chunk_idx: int, chunk_data: bytes) -> bytes:
    if len(chunk_data) > UNET_MAP_CHUNK_DATA_MAX:
        raise ValueError(f"chunk too large: {len(chunk_data)} > {UNET_MAP_CHUNK_DATA_MAX}")
    return encode_frame(bytes([UNET_MAP_CHUNK,
                               chunk_idx & 0xFF,
                               len(chunk_data) & 0xFF]) + chunk_data)


def build_map_end() -> bytes:
    return encode_frame(bytes([UNET_MAP_END]))


# ----------------------------------------------------------------------
# In-lobby map picker — server-driven scrollable list. Sent to all
# in-game clients when the first START_GAME_REQ of a round arrives;
# clients transition to UGAME_STATE_MAP_PICK, vote, and the second
# START_GAME_REQ commits the highest-vote map.
# ----------------------------------------------------------------------

UNET_MAP_LIST_MAX_ITEMS = 64
UNET_MAP_SLUG_MAX       = 16
UNET_MAP_NAME_MAX       = 24
UNET_MAP_AUTHOR_MAX     = 16


def _enc_lp_string(s: str, max_len: int) -> bytes:
    """Length-prefixed UTF-8 string, truncated to max_len bytes.
    Mirror of Saturn-side s_read_lp_string in map_pick.c."""
    raw = (s or "").encode("utf-8", "replace")[:max_len]
    return bytes([len(raw)]) + raw


def build_map_list_begin(count: int) -> bytes:
    return encode_frame(bytes([UNET_MAP_LIST_BEGIN, count & 0xFF]))


def build_map_list_item(idx: int, source: int, stage_id: int,
                        slug: str, name: str, author: str,
                        size_bytes: int) -> bytes:
    payload = bytes([
        UNET_MAP_LIST_ITEM,
        idx & 0xFF,
        source & 0xFF,
        stage_id & 0xFF,
    ])
    payload += _enc_lp_string(slug,   UNET_MAP_SLUG_MAX)
    payload += _enc_lp_string(name,   UNET_MAP_NAME_MAX)
    payload += _enc_lp_string(author, UNET_MAP_AUTHOR_MAX)
    payload += struct.pack("!H", size_bytes & 0xFFFF)
    return encode_frame(payload)


def build_map_pick_tally(tally: dict[int, int]) -> bytes:
    """tally: {idx: votes}. Encodes as count + (idx, votes) pairs."""
    items = [(idx & 0xFF, min(255, max(0, v)))
             for idx, v in tally.items() if v > 0]
    payload = bytes([UNET_MAP_PICK_TALLY, len(items) & 0xFF])
    for idx, v in items:
        payload += bytes([idx, v])
    return encode_frame(payload)


def build_map_pick_result(idx: int) -> bytes:
    return encode_frame(bytes([UNET_MAP_PICK_RESULT, idx & 0xFF]))


# Server-side opcode constants for the new map-pick traffic.
UNET_MAP_LIST_BEGIN  = 0xBB
UNET_MAP_LIST_ITEM   = 0xBC
UNET_MAP_PICK_TALLY  = 0xBD
UNET_MAP_PICK_RESULT = 0xBE
UNET_MAP_PICK_VOTE   = 0x26


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
        # True once the client has sent its first PLAYER_STATE (op 0x14).
        # Until then, server's x/y/z are constructor defaults (0,0,0)
        # which is the arena corner — broadcasting that as PLAYER_SYNC
        # would warp the OTHER player's local view to the corner. Gate
        # the broadcast on this so we only emit real positions.
        self.state_received = False
        # Position history for lag-compensated bullet validation. deque
        # of (server_tick, x, y, z) tuples in fxp 16.16 raw. Sized to
        # cover the worst-case round-trip lag we want to compensate for:
        # 30 ticks @ 20 Hz = 1.5 s. Older entries are evicted automatically.
        self.pos_history = deque(maxlen=30)


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
    def __init__(self, stage_id: int, match_seconds: int, seed: int,
                 streamed: bool = False, stream_chunks: int = 0):
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
        # Server-tick counter used by the bullet lag-comp rewind. Increments
        # each call to _tick_match (so rates other than 20Hz are correctly
        # tracked by SERVER_TICK_RATE consumers). 32-bit wide; matches don't
        # last 36+ minutes so wrap is irrelevant.
        self.server_tick = 0
        # Streaming-aware stall grace. For non-streamed matches the
        # default 100-tick (5 s) grace catches black-screened clients
        # quickly. For STREAMED matches, the Saturn is busy draining
        # MAP_CHUNK frames for several seconds before it can send its
        # first PLAYER_STATE — using the 5 s threshold there causes
        # the stall detector to mark the player dead BEFORE gameplay
        # even begins (observed in 0.6 hardware test). Bump grace to
        # cover stream wire time + 5 s safety: 14.4k baud = 1440 B/s,
        # so an N-chunk stream needs at most 8.5 ms × N seconds plus
        # the chunk header overhead. Round up generously: 0.1 s/chunk
        # + 5 s safety. Stored on MatchState so _tick_match doesn't
        # need to re-derive every tick.
        self.streamed = streamed
        self.stall_grace_ticks = (
            int(stream_chunks * 0.1 * SERVER_TICK_RATE) + 100
            if streamed else 100
        )

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
        # Monotonic timestamp of the toggle that turned ready True.
        # Reset to 0.0 when ready is toggled False or match ends.
        # Used by _on_start_game_req to pick "first 4 ready" when the
        # lobby holds more than MAX_PLAYERS connections.
        self.ready_at = 0.0
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
            # relay_player_sync removed in UNETv2 — PLAYER_SYNC is always
            # broadcast at SERVER_TICK_RATE with idle gating; no flag needed.
            "relay_input":            True,   # rebroadcast INPUT_STATE as INPUT_RELAY
            "crate_respawn_seconds":  CRATE_RESPAWN_SECONDS,
            "match_seconds_default":  MATCH_SECONDS_DEFAULT,
            "friendly_fire":          True,   # user-locked ON but toggleable for test
            "allow_bots_start":       True,   # whether bots alone can meet MIN_TO_START
            "timer_broadcast_hz":     1,      # MATCH_TIMER broadcasts per second
            "sudden_death_enabled":   False,  # default off — timer=0 ends match cleanly with highest-HP winner. Admin can flip via /api/tune_sudden_death.
            "verbose_rx_log":         True,   # alpha: log every RX while debugging start-flow
            # Per-stage enable mask (1 = available for match-start RNG).
            # Order: [Island, Cross, Valley, Railway]. CROSS disabled by
            # default — its road/grass intersection geometry has tank
            # collision dead spots where players get stuck. Admin can
            # re-enable from the portal once the geometry is fixed.
            "stage_enabled_island":   True,
            "stage_enabled_cross":    False,
            "stage_enabled_valley":   True,
            "stage_enabled_railway":  True,
            # Live custom map: when use_live_map=True and live_map_slug
            # names a .UTE in the editor's maps dir, the next match
            # streams that map to all clients (overriding the four
            # baked-in stages — Stage Pool checkboxes ignored). Saturn
            # side receives via MAP_BEGIN/CHUNK/END before GAME_START
            # and parses the buffer instead of reading CD .UTE files.
            "use_live_map":           False,
            "live_map_slug":          "",
        }
        # Where the editor stores its .UTE outputs. The editor service
        # (utenyaa-editor.service) writes here; we read here. Override
        # via env var if the editor is moved.
        self.editor_maps_dir = os.environ.get(
            "UTENYAA_EDITOR_MAPS_DIR",
            "/opt/utenyaa-editor/webapp/maps")

        # Lobby players (keyed by game_pid 0..3, mapped from client user_id)
        self.game_players: dict[int, UtenyaaPlayer] = {}

        # PLAYER_SYNC idle-gating + keyframe tracking. Maps pid →
        # (x, y, z, angle, hp, pickup, ticks_since_last_send). The tick
        # loop compares current state to this cache; sends only on change
        # or when keyframe interval elapsed (forces resync against loss).
        self._last_sync_state: dict[int, tuple] = {}

        # Bots
        self.bots: list[BotPlayer] = []
        for i in range(num_bots):
            self.bots.append(BotPlayer(BOT_NAMES[i % len(BOT_NAMES)], i))

        # Map-pick phase state. Populated when the first START_GAME_REQ
        # of a round arrives; cleared once the picker commits to a
        # winner and the regular match-start flow runs.
        self.map_picking: bool = False
        self.map_pick_list: list[dict] = []
        self.map_pick_votes: dict[int, int] = {}   # user_id → idx

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

    def _record_join_event(self, event: str, username: str, address,
                           reason: str = ""):
        """Append a unified join-history entry. Events: join, rejoin,
        leave, kick, timeout, dropped. Mirrored format with the rest
        of the saturn-admin fleet so the unified portal renders it
        with the same Time / Event / Name / IP / Reason columns."""
        self._join_history.append({
            "t":      int(time.time()),
            "event":  event,
            "name":   username or "",
            "ip":     str(address[0]) if address else "",
            "reason": reason,
        })
        self._save_join_history()

    def _record_join(self, username: str, address):
        # Back-compat shim — still called from the welcome path.
        self._record_join_event("join", username, address)

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
        # Humans + their P2 co-op slots (one row per local name)
        for uid, c in self.clients.items():
            if not c.authenticated:
                continue
            out.append({
                "id": uid,
                "name": c.username,
                "ready": c.ready,
                "character_id": c.character_id,
                "stage_vote": c.stage_vote
            })
            # Co-op P2 names registered via ADD_LOCAL_PLAYER. Synthetic
            # id = 100+offset so the client knows it's a P2 slot (not a
            # real server uid). Inherits the parent's ready flag.
            local_chars = getattr(c, "local_characters", [])
            for j, p2_name in enumerate(c.local_names):
                p2_char = local_chars[j] if j < len(local_chars) else 0xFF
                out.append({
                    "id": 100 + (uid * 4) + j,
                    "name": p2_name,
                    "ready": c.ready,
                    "character_id": p2_char,
                    "stage_vote": 0xFF
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
        # Cap at lobby size — lobby holds up to MAX_LOBBY connections
        # (each plus any P2 co-op slots), not just MAX_PLAYERS.
        return out[:MAX_LOBBY]

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
        # Stamp the time of the True-toggle so _start_game can pick
        # the FIRST MAX_PLAYERS to ready when more than 4 are armed.
        c.ready_at = time.monotonic() if c.ready else 0.0
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

    # ------------------------------------------------------------------
    # Streamed custom maps (Editor → Saturn)
    # ------------------------------------------------------------------

    def list_custom_maps(self) -> list[dict]:
        """Enumerate .UTE files in the editor's maps dir. Returns a list
        of {slug, size_bytes, mtime} sorted by slug. Used by the admin
        portal to populate the live-map dropdown."""
        out = []
        try:
            for name in sorted(os.listdir(self.editor_maps_dir)):
                if not name.lower().endswith(".ute"):
                    continue
                full = os.path.join(self.editor_maps_dir, name)
                try:
                    st = os.stat(full)
                except OSError:
                    continue
                slug = name[:-4]  # strip ".UTE" / ".ute"
                # Editor saves slugs as lowercase but the .UTE file
                # itself is uppercase by convention. Normalize the slug
                # to lowercase so the round-trip with /api/tune is stable.
                out.append({
                    "slug": slug.lower(),
                    "filename": name,
                    "size_bytes": st.st_size,
                    "mtime": int(st.st_mtime),
                })
        except (FileNotFoundError, NotADirectoryError, PermissionError) as e:
            log.warning("list_custom_maps: cannot read %s: %s",
                        self.editor_maps_dir, e)
        return out

    def _load_live_map_bytes(self) -> bytes | None:
        """Read the .UTE bytes for the currently-pinned live map.
        Returns None if the slug is empty, the file is missing, or
        the file looks invalid (header check). Validates size against
        UNET_MAP_MAX_SIZE so a corrupt huge file can't kill the
        server's TX path."""
        slug = (self.tune.get("live_map_slug") or "").strip().lower()
        if not slug:
            return None
        # Defense-in-depth against path traversal — slugs are produced
        # by the editor's slugify() (which the editor docs constrain
        # to [a-z0-9_-]) but we don't want a malicious admin-portal
        # request to bypass that.
        if "/" in slug or "\\" in slug or ".." in slug:
            log.warning("live map slug rejected (suspicious chars): %r", slug)
            return None
        # Try uppercase filename first (editor convention), then exact.
        for fname in (slug.upper() + ".UTE", slug + ".UTE", slug + ".ute"):
            full = os.path.join(self.editor_maps_dir, fname)
            if os.path.isfile(full):
                try:
                    data = open(full, "rb").read()
                except OSError as e:
                    log.warning("live map %r read failed: %s", fname, e)
                    return None
                if len(data) < 4 or data[:3] != b"UTE":
                    log.warning("live map %r: bad header (not 'UTE')", fname)
                    return None
                if len(data) > UNET_MAP_MAX_SIZE:
                    log.warning("live map %r: %d bytes exceeds cap %d",
                                fname, len(data), UNET_MAP_MAX_SIZE)
                    return None
                return data
        log.info("live map slug %r: no file found in %s",
                 slug, self.editor_maps_dir)
        return None

    def _stream_live_map_to_in_game_clients(self, data: bytes) -> None:
        """Send MAP_BEGIN, MAP_CHUNK[*], MAP_END to every authenticated
        in-game client. The chunks are pre-built once, then broadcast —
        same byte-for-byte payload to all peers (no per-client state).
        Saturn dispatcher consumes via unet_map_stream_on_*. CRC-16/
        CCITT-FALSE matches the Saturn implementation."""
        total = len(data)
        crc = crc16_ccitt_false(data)
        chunks = []
        idx = 0
        for off in range(0, total, UNET_MAP_CHUNK_DATA_MAX):
            chunks.append(data[off:off + UNET_MAP_CHUNK_DATA_MAX])
            idx += 1
        if idx > UNET_MAP_MAX_CHUNKS:
            log.error("live map needs %d chunks, max %d — refusing",
                      idx, UNET_MAP_MAX_CHUNKS)
            return

        begin = build_map_begin(total, idx, crc)
        end_frame = build_map_end()
        chunk_frames = [build_map_chunk(i, c) for i, c in enumerate(chunks)]
        log.info("Streaming live map: %d bytes in %d chunks crc=%04X",
                 total, idx, crc)
        for cc in self.clients.values():
            if not cc.authenticated or not cc.in_game:
                continue
            cc.send_raw(begin)
            for cf in chunk_frames:
                cc.send_raw(cf)
            cc.send_raw(end_frame)

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

    # Stage IDs match UNET_STAGE_* in utenyaa_protocol.h:
    #   0 = Island, 1 = Cross, 2 = Valley, 3 = Railway
    _STAGE_TUNE_KEYS = (
        "stage_enabled_island",
        "stage_enabled_cross",
        "stage_enabled_valley",
        "stage_enabled_railway",
    )

    def _enabled_stages(self) -> list:
        """Return list of stage IDs that are admin-enabled. Always non-
        empty: if every flag is off (admin misconfig), fall back to
        stage 0 (Island) so match-start can't deadlock."""
        out = [i for i in range(STAGE_COUNT)
               if self.tune.get(self._STAGE_TUNE_KEYS[i], True)]
        return out if out else [0]

    def _pick_stage(self) -> int:
        enabled = self._enabled_stages()
        # Vote tally restricted to enabled stages — disabled stage
        # votes are ignored even if a client UI somehow casts one.
        tally = self._stage_tally()
        max_votes = max((tally[i] for i in enabled), default=0)
        if max_votes == 0:
            return random.choice(enabled)
        winners = [i for i in enabled if tally[i] == max_votes]
        return random.choice(winners)

    def _on_map_pick_vote(self, c: ClientInfo, idx: int):
        """Player picked a map in the in-lobby selector. Record their
        vote and re-broadcast the tally so peers see it live."""
        if not self.map_picking:
            return
        if idx >= len(self.map_pick_list):
            return
        # `c.user_id` may be the negative pre-auth key; only authenticated
        # clients reach this path so we always have a positive id.
        self.map_pick_votes[c.user_id] = idx
        self._broadcast_map_pick_tally()

    def _broadcast_map_pick_tally(self):
        tally: dict[int, int] = {}
        for idx in self.map_pick_votes.values():
            tally[idx] = tally.get(idx, 0) + 1
        self._broadcast(build_map_pick_tally(tally))

    def _build_map_pick_list(self) -> list[dict]:
        """Combined list shown to clients:
            [0..3] = the four baked-in CD stages, source=0
            [4..]  = every .UTE in the editor's maps dir, source=1
        Author/name pulled from the JSON sidecar when available; falls
        back to the slug if the sidecar is missing or malformed."""
        out = []
        # 4 baked stages
        for stage_id, name in enumerate(STAGE_NAMES):
            out.append({
                "idx": len(out),
                "source": 0,
                "stage_id": stage_id,
                "slug": name.lower(),
                "name": name,
                "author": "ReyeMe / DannyDuarte",
                "size_bytes": 0,
            })
        # Custom maps from disk
        try:
            entries = sorted(os.listdir(self.editor_maps_dir))
        except OSError:
            entries = []
        for fname in entries:
            if not fname.lower().endswith(".ute"):
                continue
            slug = fname[:-4].lower()
            full = os.path.join(self.editor_maps_dir, fname)
            try:
                size = os.path.getsize(full)
            except OSError:
                continue
            display_name = slug
            display_author = ""
            sidecar = os.path.join(self.editor_maps_dir, slug + ".json")
            if os.path.isfile(sidecar):
                try:
                    j = json.loads(open(sidecar, "r").read())
                    display_name   = (j.get("name")   or display_name)[:UNET_MAP_NAME_MAX]
                    display_author = (j.get("author") or "")[:UNET_MAP_AUTHOR_MAX]
                except Exception:
                    pass
            out.append({
                "idx": len(out),
                "source": 1,
                "stage_id": 0xFF,
                "slug": slug,
                "name": display_name,
                "author": display_author,
                "size_bytes": size,
            })
            if len(out) >= UNET_MAP_LIST_MAX_ITEMS:
                break
        return out

    def _enter_map_pick_phase(self):
        """First START_GAME_REQ of a round → broadcast list, transition
        all in-game clients to MAP_PICK. The C++ Saturn glue watches
        for MAP_LIST_BEGIN to flip its game state."""
        self.map_pick_list = self._build_map_pick_list()
        self.map_pick_votes = {}
        self.map_picking = True
        log.info("MAP_PICK phase entered: %d maps in pool",
                 len(self.map_pick_list))
        self._broadcast(build_map_list_begin(len(self.map_pick_list)))
        for m in self.map_pick_list:
            self._broadcast(build_map_list_item(
                m["idx"], m["source"], m["stage_id"],
                m["slug"], m["name"], m["author"], m["size_bytes"]))
        # Initial empty tally so the picker UI shows v=0 instead of stale.
        self._broadcast_map_pick_tally()

    def _commit_map_pick(self) -> tuple[str, int]:
        """Resolve the vote tally to a winning map. Returns
        (slug, idx). Highest votes wins; random tiebreak; if no
        votes at all, pick a random map from the pool."""
        if not self.map_pick_list:
            return ("", 0)
        tally: dict[int, int] = {}
        for idx in self.map_pick_votes.values():
            tally[idx] = tally.get(idx, 0) + 1
        if tally:
            top = max(tally.values())
            winners = [i for i, v in tally.items() if v == top]
            chosen = random.choice(winners)
        else:
            chosen = random.randrange(len(self.map_pick_list))
        m = self.map_pick_list[chosen]
        return (m["slug"], chosen)

    def _on_start_game_req(self, c: ClientInfo):
        if self.match is not None:
            log.info("START_GAME_REQ from %s rejected: match already active", c.username)
            return

        # Two-phase START flow:
        #   Phase 1: not yet picking — broadcast map list, transition
        #            clients to MAP_PICK, await votes.
        #   Phase 2: already picking — commit the highest-vote map and
        #            run the regular match-start (with streaming if the
        #            chosen map is a custom one).
        if not self.map_picking:
            # Same readiness check the existing flow used to gate START.
            all_ready = sorted(
                [cc for cc in self.clients.values()
                 if cc.authenticated and cc.ready],
                key=lambda cc: cc.ready_at)
            slots_used = len(self.bots)
            ready_humans = []
            for cc in all_ready:
                need = 1 + len(cc.local_names)
                if slots_used + need > MAX_PLAYERS: continue
                ready_humans.append(cc)
                slots_used += need
            if slots_used < MIN_TO_START:
                c.send_raw(build_log(f"Need {MIN_TO_START} players to start"))
                log.info("START_GAME_REQ rejected: insufficient slots")
                return
            self._enter_map_pick_phase()
            return
        # Phase 2 — commit the pick. The chosen slug feeds into the
        # existing match-start flow below; if it names a custom map we
        # set use_live_map=true + live_map_slug for this match only,
        # so _load_live_map_bytes finds it. The original tune values
        # are restored after match start so the operator's pinned map
        # (if any) isn't permanently overwritten.

        # ALL ready humans, sorted by the timestamp they toggled ready
        # (oldest first). When the lobby holds more than MAX_PLAYERS
        # connections, only the first 4 ready (and their P2 co-op
        # slots, fitting within MAX_PLAYERS total) are admitted to
        # the match — the rest stay ready-armed in the lobby and get
        # auto-picked next round if still ready.
        all_ready = sorted(
            [cc for cc in self.clients.values()
             if cc.authenticated and cc.ready],
            key=lambda cc: cc.ready_at)

        # Greedy fill: walk in ready_at order, admit each player + its
        # P2 names if there's room left in MAX_PLAYERS. Skip a player
        # whose addition would overflow (a future ready toggle picks
        # them up).
        slots_used = len(self.bots)
        ready_humans = []
        for cc in all_ready:
            need = 1 + len(cc.local_names)
            if slots_used + need > MAX_PLAYERS:
                continue
            ready_humans.append(cc)
            slots_used += need

        total_slots = slots_used
        log.info("START_GAME_REQ from %s: ready_humans=%d/%d_armed bots=%d "
                 "p2_names=%d total_slots=%d (need %d, max %d)",
                 c.username, len(ready_humans), len(all_ready),
                 len(self.bots),
                 sum(len(cc.local_names) for cc in ready_humans),
                 total_slots, MIN_TO_START, MAX_PLAYERS)
        if total_slots < MIN_TO_START:
            c.send_raw(build_log(f"Need {MIN_TO_START} players to start"))
            log.info("START_GAME_REQ rejected: insufficient slots")
            return

        # MAP_PICK has resolved a winner. Translate that pick into the
        # existing live-map / stage-pool inputs so the rest of the
        # match-start flow (including streaming) doesn't need to know
        # the picker happened.
        pick_slug, pick_idx = self._commit_map_pick()
        picked = self.map_pick_list[pick_idx] if 0 <= pick_idx < len(self.map_pick_list) else None
        # Tell clients which row won (UI flash). Do this BEFORE the
        # ~7-8 s streaming window so the picker's "MAP COMMITTED"
        # banner is visible during the wait.
        if picked is not None:
            self._broadcast(build_map_pick_result(pick_idx))
        # End the picker phase regardless of pick outcome.
        self.map_picking = False
        self.map_pick_votes = {}

        # Saved tune state — restored after match start so the
        # operator's pinned map (if any) is preserved across matches
        # that picked something else.
        saved_use_live = self.tune.get("use_live_map", False)
        saved_live_slug = self.tune.get("live_map_slug", "")
        if picked is not None and picked["source"] == 1:
            # Custom map win — feed the slug into the streaming path.
            self.tune["use_live_map"]  = True
            self.tune["live_map_slug"] = picked["slug"]
        elif picked is not None and picked["source"] == 0:
            # Baked-stage win — make sure no streaming kicks in even if
            # a custom slug was previously pinned.
            self.tune["use_live_map"]  = False
        # ELSE: shouldn't happen — fall through to legacy logic.

        live_bytes = self._load_live_map_bytes() if self.tune.get("use_live_map") else None
        if live_bytes is not None:
            stage = UNET_STAGE_STREAMED
            log.info("Match using LIVE MAP slug=%r size=%d bytes",
                     self.tune.get("live_map_slug", ""), len(live_bytes))
        elif picked is not None and picked["source"] == 0:
            # Force the picker's chosen baked stage even if Stage Pool
            # has it disabled — explicit player vote overrides the
            # pool mask. (Would-be desync if we returned a different
            # stage from _pick_stage now.)
            stage = picked["stage_id"]
        else:
            stage = self._pick_stage()
        # Restore the persistent tune values (the per-match override
        # above is consumed; future matches go back to whatever the
        # admin had pinned).
        self.tune["use_live_map"]  = saved_use_live
        self.tune["live_map_slug"] = saved_live_slug
        seed = random.randint(1, 0xFFFFFFFF)
        # MatchState wants a 0-3 stage id for crate roster lookup. For the
        # streamed override we use the slug's source stage if known via
        # the JSON sidecar later; for now reuse stage 0's crate layout
        # so matches always have crates. (The streamed .UTE itself
        # carries entity data inline — server crates are independent.)
        ms_stage = stage if stage != UNET_STAGE_STREAMED else 0
        # When streaming, pass the chunk count so MatchState can size
        # its stall-grace window to cover the stream wire time. Without
        # this, the 5-s default grace fires DURING streaming and marks
        # all players dead before gameplay starts (0.6 hardware bug).
        stream_chunks = 0
        if stage == UNET_STAGE_STREAMED and live_bytes is not None:
            stream_chunks = (len(live_bytes) + UNET_MAP_CHUNK_DATA_MAX - 1) // UNET_MAP_CHUNK_DATA_MAX
        self.match = MatchState(ms_stage, self.match_seconds, seed,
                                streamed=(stage == UNET_STAGE_STREAMED),
                                stream_chunks=stream_chunks)
        self.match.crates = [Crate(x["slot"], x["flags"], x["x"], x["y"], x["z"])
                             for x in build_crate_roster(ms_stage)]

        # Assign game PIDs 0..N-1. Humans first, then bots.
        next_pid = 0
        for cc in ready_humans:
            if next_pid >= MAX_PLAYERS: break
            cc.in_game = True
            cc.game_pid = next_pid
            # Character assignment: stable per-username hash so a
            # given player keeps the same cat sprite across rounds
            # AND across server restarts. Previous code defaulted to
            # next_pid (the current ready_at-order pid) which churned
            # every match — user-reported as "cat color keeps changing
            # between rounds". Once cc.character_id is set we honour
            # it so future character-select UI can override.
            char = (cc.character_id if cc.character_id != 0xFF
                    else _stable_character_for(cc.username))
            cc.character_id = char         # remember for next round
            self.game_players[next_pid] = UtenyaaPlayer(
                next_pid, cc.username, char)
            next_pid += 1
            # Local co-op: allocate one pid per registered P2 name. Store
            # the assigned pid in cc.local_pids so subsequent P2-prefixed
            # client messages (INPUT_STATE_P2, CLIENT_DEATH_P2, etc.)
            # validate against the right slot. Send LOCAL_PLAYER_ACK with
            # the actual pid (NOT 0xFF) so the client's myPlayerID2
            # gets populated before GAME_START fires.
            cc.local_pids = []
            for p2_name in cc.local_names:
                if next_pid >= MAX_PLAYERS: break
                cc.local_pids.append(next_pid)
                self.game_players[next_pid] = UtenyaaPlayer(
                    next_pid, p2_name or (cc.username + "-2"),
                    (next_pid + 5) % MAX_CHARACTERS)
                cc.send_raw(build_local_player_ack(next_pid))
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

        # If live-map mode is on, push the .UTE bytes BEFORE GAME_START
        # so each client's map_stream RX state is filled by the time
        # GAME_START arrives. The Saturn-side dispatcher tolerates
        # CHUNK arrival across many frames at the modem rate
        # (UNET_RX_MAX_PER_POLL=192) — total wire time ~7-8s for 11 KB
        # at 14.4k baud. Failure to load (slug missing) falls back
        # silently to baked stages above.
        if stage == UNET_STAGE_STREAMED and live_bytes is not None:
            self._stream_live_map_to_in_game_clients(live_bytes)
            # 500 ms safety pause AFTER queuing the stream and BEFORE
            # GAME_START. TCP order guarantees END arrives before
            # GAME_START on each client socket, but the Saturn drains
            # only UNET_RX_MAX_PER_POLL=192 bytes per tick, so a
            # GAME_START dispatched on the same tick as the first
            # CHUNKs would be processed before the buffer is filled —
            # main.cxx's `unet_map_stream_is_ready()` would be false
            # → fallback to ISLAND. The pause lets the Saturn drain
            # most of the in-flight stream before the GAME_START byte
            # appears on the wire. (~7-8 s wire-time + 0.5 s = total
            # transfer wall clock; well within UNET_AUTH_TIMEOUT.)
            time.sleep(0.5)

        for cc in self.clients.values():
            if not cc.authenticated or not cc.in_game:
                continue
            cc.send_raw(build_game_start(seed, cc.game_pid, stage, next_pid,
                                         self.match_seconds, crate_roster))
        stage_label = (f"STREAMED({self.tune.get('live_map_slug','?')})"
                       if stage == UNET_STAGE_STREAMED
                       else STAGE_NAMES[stage] if 0 <= stage < len(STAGE_NAMES)
                       else f"#{stage}")
        log.info("Match started: stage=%s seed=%08X players=%d",
                 stage_label, seed, next_pid)

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

        # Reset for next match — clear ready (forces fresh A press
        # in lobby; first 4 to re-arm play next round).
        for c in self.clients.values():
            c.in_game = False
            c.ready = False
            c.ready_at = 0.0
            c.stage_loaded = False
        self.game_players.clear()
        self._last_sync_state.clear()
        self.match = None
        # Reset map-pick state so the next round's first START_GAME_REQ
        # re-enters the picker (otherwise we'd skip straight to "Phase 2
        # commit" with a stale list and zero votes).
        self.map_picking = False
        self.map_pick_votes = {}
        self.map_pick_list = []
        self._broadcast_lobby()

    def _lag_comp_bullet_check(self, shooter_pid: int,
                               ox: int, oy: int, oz: int,
                               dx: int, dy: int, dz: int,
                               rewind_ticks: int = 3,
                               cone_radius: int = 12,
                               damage: int = BULLET_DAMAGE,
                               max_range_units: int = 300) -> bool:
        """Server-authoritative bullet hit validation along the FULL trajectory.

        ox/oy/oz, dx/dy/dz are fxp 16.16 raw values from the shooter's
        FIRE_BULLET packet. (dx,dy) is a unit vector — magnitude 65536.

        The previous version only validated POINT-BLANK hits (target
        within `cone_radius` world units of the muzzle at fire time).
        Symptom: "bullets not affecting other players" for any shot
        beyond ~12 units. Root cause was that the design relied on the
        VICTIM's local bullet sim catching the hit, but the broadcast
        BULLET_SPAWN reaches the victim's Saturn ~100ms after fire,
        the bullet then flies along the ORIGINAL aim direction, and
        the victim has moved ~3-9 units off that aim point in the
        meantime — so the local-sim collision misses too. Combined
        with the muzzle-only cone check, no authority registered the
        hit, no DAMAGE broadcast went out, no client saw HP drop.

        Now: walk the bullet path forward in `step_units`-unit world
        increments up to `max_range_units`, testing every alive non-
        shooter target's REWOUND position (rewind_ticks ago) against
        the bullet's current step. First hit wins. Forward-half-space
        test at every step ensures the bullet only "hits" targets
        ahead of its current position, not behind (so a target
        standing behind the shooter when they fire forward isn't
        treated as a hit).

        Tuning rationale (open to admin-tunable later):
          - cone_radius=12: matches the prior point-blank value;
            client-side AABB has padded half-size 10 for remote
            players (Player::Size 4 + NET_PAD 6), so 12 includes a
            small margin for rewind-mismatch slop.
          - step_units=4: half a Player::Size, guarantees no target
            is skipped past between samples (any target within
            cone_radius of the path will be within cone_radius of at
            least one step's position).
          - max_range_units=300: covers full-arena shots across a
            ~256-unit map diagonal. Bullets that visually stop at
            terrain in the local sim will still register a hit on the
            server up to this range — over-hits past walls are rare
            in tank-arena layouts and the user-facing trade-off is
            far better than the current "shots don't register".

        Known gap: the server has no terrain map, so an obstructed
        shot through a wall would register a server hit but show no
        visible impact on clients. Acceptable for now; the rare
        "ghost hit" case is far less disruptive than the prior
        "hits never register" baseline.

        Returns True if a hit was registered.
        """
        if self.match is None:
            return False

        target_tick = self.match.server_tick - rewind_ticks
        hit_r_sq = cone_radius * cone_radius
        step_units = 4
        n_steps = max_range_units // step_units + 1

        # Pre-fetch each non-shooter target's REWOUND position in
        # world units. Walking history newest→oldest, take the first
        # entry whose tick is <= target_tick (i.e. recent enough to
        # be the rewound sample). Empty history or all-too-recent
        # history falls back to the deque's eldest entry, or to the
        # current position if no history at all.
        rewound = []
        for pid, p in self.game_players.items():
            if pid == shooter_pid: continue
            if not p.alive: continue
            # Don't validate against players who haven't sent their first
            # PLAYER_STATE — server has them at (0,0,0) which would yield
            # a phantom hit at the arena corner.
            if not p.state_received and not p.is_bot: continue
            tx, ty = p.x, p.y
            if p.pos_history:
                tx, ty = p.pos_history[0][1], p.pos_history[0][2]
                for entry in reversed(p.pos_history):
                    if entry[0] <= target_tick:
                        tx, ty = entry[1], entry[2]
                        break
            rewound.append((pid, p, tx >> 16, ty >> 16))

        if not rewound:
            return False

        # Bullet position in fxp 16.16; advance by `step_units` world
        # units per iteration. dx,dy are fxp 16.16 with magnitude
        # 65536 (unit vector), so dx*step_units is fxp 16.16 of
        # (step_units world units along x). Same for dy.
        pos_x = ox
        pos_y = oy
        step_dx_fxp = dx * step_units
        step_dy_fxp = dy * step_units

        for i in range(n_steps):
            bx_w = pos_x >> 16
            by_w = pos_y >> 16
            for pid, p, tx_w, ty_w in rewound:
                ddx = tx_w - bx_w
                ddy = ty_w - by_w
                # Forward-half-space test: target must be ahead of
                # the bullet's current heading (or coincident, dot=0).
                # ddx,ddy are world units (~int8 magnitude); dx,dy
                # are fxp 16.16 raw — the product is fxp scaled but
                # we only check sign so the scale doesn't matter.
                if ddx * dx + ddy * dy < 0:
                    continue
                if ddx * ddx + ddy * ddy <= hit_r_sq:
                    # === HIT ===
                    new_hp = max(0, p.hp - damage)
                    p.hp = new_hp
                    self._broadcast(build_damage(pid, shooter_pid, damage, new_hp))
                    log.info("LAG-COMP HIT: shooter=%d → victim=%d hp %d→%d "
                             "(rewound %d ticks, step=%d/%d, dist²=%d/%d)",
                             shooter_pid, pid, p.hp + damage, new_hp,
                             rewind_ticks, i, n_steps,
                             ddx * ddx + ddy * ddy, hit_r_sq)
                    if new_hp == 0:
                        p.alive = False
                        p.deaths += 1
                        shooter = self.game_players.get(shooter_pid)
                        if shooter:
                            shooter.kills += 1
                        self._broadcast(build_player_kill(pid, shooter_pid))
                        self._broadcast(build_score_update(
                            pid, p.kills, p.deaths, max(p.hp, 0)))
                        if shooter:
                            self._broadcast(build_score_update(
                                shooter_pid, shooter.kills, shooter.deaths,
                                max(shooter.hp, 0)))
                    return True
            pos_x += step_dx_fxp
            pos_y += step_dy_fxp

        return False

    def _tick_match(self, dt: float):
        if self.match is None or self.match.game_over:
            return

        # Advance the lag-comp rewind clock. Must run BEFORE position
        # snapshotting so the deque entries are stamped with the tick
        # whose state they represent.
        self.match.server_tick += 1

        # Stalled-player liveness check. If a real (non-bot) player
        # hasn't sent a single PLAYER_STATE within the grace window,
        # their client black-screened during the LOBBY → GAMEPLAY
        # transition. Mark them dead so the alive_count condition
        # below can fire and the match ends rather than running 120 s
        # waiting for someone who isn't there.
        # Grace window is per-match: short for CD-baked stages, longer
        # for streamed matches (Saturn busy receiving MAP_CHUNKs).
        STALL_GRACE_TICKS = self.match.stall_grace_ticks
        if self.match.server_tick == STALL_GRACE_TICKS:
            for pid, p in self.game_players.items():
                if p.is_bot: continue
                if p.alive and not p.state_received:
                    log.info("Stalled player pid=%d (%s) — no PLAYER_STATE in %d ticks; marking dead",
                             pid, p.name, STALL_GRACE_TICKS)
                    p.alive = False
                    p.hp = 0
                    p.deaths += 1
                    self._broadcast(build_player_kill(pid, 0xFF))
                    self._broadcast(build_score_update(
                        pid, p.kills, p.deaths, max(p.hp, 0)))

        # Snapshot every player's position into their pos_history deque
        # for lag-compensated bullet validation. Keep the latest 30 ticks
        # = 1.5 s of history at SERVER_TICK_RATE=20 — comfortably covers
        # worst-case (modem + bridge + server tick) one-way of ~150 ms
        # plus the 100 ms client interpolation lag the shooter saw.
        for pid, p in self.game_players.items():
            p.pos_history.append((self.match.server_tick, p.x, p.y, p.z))

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
            # Bot positions stay in signed int32 range (consumed by
            # build_player_sync's '!iiiiii' pack which expects signed).
            # Don't unsigned-mask — that pushes values past 2^31-1.
            p.x = _signed32(p.x + ddx)
            p.y = _signed32(p.y + ddy)
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

            # (Bot PLAYER_SYNC handled by the unified broadcast loop below
            # so all 4 pid streams use the same 20Hz idle-gated cadence.)

        # === Per-pid PLAYER_SYNC broadcast (UNETv2 smoothness pass) ===
        # Tick at SERVER_TICK_RATE (20 Hz default). For each in-match pid,
        # send a PLAYER_SYNC iff the QUANTIZED state changed since last
        # send (so the gate fires exactly when a wire byte would differ —
        # not on sub-quantum jitter). Force-send a keyframe every 1.0 s
        # for packet-loss recovery. Idle players burn a single ~14-byte
        # keyframe per second; active players send ~14 B every 50 ms.
        # Worst-case 4P active total: 4 × 20 × 14 ≈ 1120 B/s — fits
        # 14.4 kbps Japanese modems with headroom for events.
        keyframe_interval = SERVER_TICK_RATE  # 20 ticks = 1.0s
        for pid, p in self.game_players.items():
            # Skip dead players. _drop_client and CLIENT_DEATH set
            # alive=False but leave (x,y,z) at last-known. Without this
            # gate, the keyframe path keeps re-broadcasting the corpse's
            # last position every 1s, leaving a still tank visible at
            # the spot where the player vanished. Cosmetic but
            # confusing. PLAYER_KILL was already broadcast separately
            # so receivers know to stop rendering.
            if not p.alive:
                continue
            # Skip players who haven't sent their first PLAYER_STATE
            # yet — server has them at constructor default (0,0,0).
            # Bots are an exception: their position is server-driven
            # by AI tick, so they always have a real position.
            if not p.state_received and not p.is_bot:
                continue
            last = self._last_sync_state.get(pid)  # (q_tuple, ticks_since)
            ticks_since = (last[1] + 1) if last else (keyframe_interval + 1)
            # Compute the EXACT 9-tuple of values that go on the wire.
            # Comparing these directly means the idle gate fires only when
            # a downstream byte would actually differ.
            cur_q = (
                _q_pos(p.x), _q_pos(p.y), _q_z(p.z),
                _q_vel(p.dx), _q_vel(p.dy), _q_vel(p.dz),
                _q_angle(p.angle), p.hp & 0x0F, p.pickup & 0x0F,
            )
            changed = (last is None) or (cur_q != last[0])
            if changed or ticks_since >= keyframe_interval:
                self._broadcast(build_player_sync(
                    pid, p.x, p.y, p.z, p.dx, p.dy, p.dz,
                    p.angle, p.hp, p.pickup))
                self._last_sync_state[pid] = (cur_q, 0)
            else:
                self._last_sync_state[pid] = (last[0], ticks_since)

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
        # Per-frame diag log — gated by tune knob so it can be silenced
        # in production but ON by default during alpha testing.
        if self.tune.get("verbose_rx_log", True):
            who = c.username if c.authenticated else f"<pre-auth@{c.address[0]}>"
            log.info("RX op=0x%02X len=%d from=%s state=%s",
                     op, len(payload), who,
                     "auth" if c.authenticated else "pre")

        if op == MSG_CONNECT:
            # Payload: [op:1][optional uuid:36]
            if len(payload) >= 1 + UUID_LEN:
                existing_uuid = payload[1:1 + UUID_LEN].decode("ascii", "replace").rstrip("\x00")
                uid = self.uuid_map.get(existing_uuid)
                if uid is not None:
                    # Look up the previous ClientInfo if still present —
                    # we want to restore username, character, vote, etc.
                    # so the player picks up where they left off rather
                    # than getting a blank lobby row after reconnect.
                    prev = self.clients.get(uid)
                    if prev is not None and prev is not c:
                        c.username = prev.username
                        c.character_id = prev.character_id
                        c.stage_vote = prev.stage_vote
                        c.ready = prev.ready
                        # Close the dead socket from the prior session.
                        try: prev.socket.close()
                        except Exception: pass
                    c.uuid = existing_uuid
                    c.user_id = uid
                    c.authenticated = True
                    # Remove the pre-auth negative-key slot for this
                    # ClientInfo so it isn't in self.clients twice.
                    for k in list(self.clients.keys()):
                        if self.clients[k] is c and k != uid:
                            del self.clients[k]
                    self.clients[uid] = c
                    c.send_raw(build_welcome(uid, existing_uuid, c.username, back=True))
                    log.info("WELCOME_BACK → %s uid=%d name=%r",
                             c.address[0], uid, c.username)
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
            log.info("WELCOME → %s uid=%d uuid=%s name=%r",
                     c.address[0], c.user_id, c.uuid[:8], c.username)
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
        elif op == UNET_MAP_PICK_VOTE and len(payload) >= 2:
            self._on_map_pick_vote(c, payload[1])
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
                # Mark this pid as having a real position now. The
                # PLAYER_SYNC broadcast loop in _tick_match gates on
                # this so peers don't see a (0,0,0) corner-warp before
                # the player's first PLAYER_STATE arrives.
                p.state_received = True
                # HP is FULLY server-authoritative under UNETv2:
                #   - Damage applied by _lag_comp_bullet_check on FIRE_BULLET
                #   - Heal applied by CLIENT_PICKUP_CRATE (PICKUP_HEALTH branch)
                #   - Death finalized by CLIENT_DEATH (server sets hp=0,alive=False)
                # Client's `hp` byte in PLAYER_STATE is IGNORED to prevent the
                # round-trip where: server lag-comp sets p.hp=4 → broadcasts
                # DAMAGE → next client PLAYER_STATE arrives with stale hp=6 →
                # would otherwise clobber back to 6. The previous "downward
                # only" gate also broke healing crates (heal raises HP), so
                # ignoring the byte entirely is the only consistent rule.
                # (void) hp -- intentionally unused.
            # No immediate relay anymore — the per-pid PLAYER_SYNC tick
            # loop in _tick_match handles broadcast at SERVER_TICK_RATE
            # (20 Hz) with idle gating + 1s keyframe. Eliminates the
            # 50 Hz fan-out from clients sending per-frame PLAYER_STATE
            # which previously hit ~6 KB/s downstream — well over the
            # 14.4k modem budget. See UNETv2 design notes.
        elif op == UNET_CLIENT_FIRE_BULLET and len(payload) >= 1 + 24:
            if self.match is None: return
            x, y, z, dx, dy, dz = struct.unpack("!iiiiii", payload[1:25])
            eid = self.match.alloc_entity_id()
            self._broadcast(build_bullet_spawn(eid, c.game_pid, x, y, z, dx, dy, dz))
            # === Lag-compensated point-blank validation ===
            # The shooter sees remote tanks at "now - ~100ms" (interp
            # buffer) and we add ~50ms modem one-way. Rewind targets to
            # ~3 ticks ago (150ms @ 20 Hz) — a position they had on the
            # shooter's screen at fire time. Cone-check vs each rewound
            # target; on point-blank match (≤8 world-unit cone, dot>0),
            # apply damage server-authoritatively and broadcast DAMAGE.
            # The bullet entity still travels normally for cosmetic
            # purposes, and victim's local bullet sim still runs — but
            # the authoritative HP delta is set HERE so it can't be
            # missed by network glitches.
            if c.game_pid != 0xFF:
                # cone_radius=12: matches the client's online padded
                # hitbox (Player::Size 4 + NET pad 6 = 10) plus 2 units
                # of rewind-mismatch slop. Smaller than 10 misses hits
                # the victim's local sim would still register; larger
                # creates "hit-claimy" feeling.
                self._lag_comp_bullet_check(c.game_pid, x, y, z, dx, dy, dz,
                                            rewind_ticks=3,
                                            cone_radius=12,
                                            damage=BULLET_DAMAGE)
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
        elif op == UNET_INPUT_STATE_P2 and len(payload) >= 5:
            # P2 co-op input. Payload: [op][p2_pid][frame:2 BE][input]
            if self.match is None: return
            if not self.tune.get("relay_input", True): return
            p2_pid = payload[1]
            # Validate pid belongs to this client's co-op slots
            if p2_pid not in c.local_pids: return
            frame = (payload[2] << 8) | payload[3]
            bits = payload[4]
            self._broadcast(build_input_relay(p2_pid, frame, bits),
                            exclude_uid=c.user_id)
        elif op == UNET_CLIENT_DEATH_P2 and len(payload) >= 2:
            if self.match is None: return
            p2_pid = payload[1]
            if p2_pid not in c.local_pids: return
            p = self.game_players.get(p2_pid)
            if p and p.alive:
                p.alive = False
                p.hp = 0
                p.deaths += 1
                # Same DAMAGE-then-KILL pattern as P1 path so peers see
                # this co-op slot reach hp=0 reliably.
                self._broadcast(build_damage(p.pid, 0xFF, 0, 0))
                self._broadcast(build_player_kill(p.pid, 0xFF))
                self._broadcast(build_score_update(p.pid, p.kills, p.deaths,
                                                   max(p.hp, 0)))
        elif op == UNET_CHARACTER_SELECT_P2 and len(payload) >= 3:
            # P2 co-op character pick — only meaningful pre-match. We
            # store it on the client and apply at match-start when the
            # P2 pid is allocated. (Pre-match c.local_pids is empty.)
            char_id = payload[2]
            if char_id < MAX_CHARACTERS:
                # Stash in a per-client dict keyed by P2 slot index.
                if not hasattr(c, "local_characters"):
                    c.local_characters = []
                # payload[1] is the P2 pid (0xFF if not yet allocated)
                # — the client uses 0xFF pre-match so we treat it as
                # "first co-op slot"; for now just append to a list.
                if len(c.local_characters) < len(c.local_names):
                    c.local_characters.append(char_id)
                else:
                    c.local_characters = (c.local_characters[:-1] + [char_id]
                                          if c.local_characters else [char_id])
                self._broadcast_lobby()
        elif op == UNET_CLIENT_DEATH:
            if self.match is None: return
            p = self.game_players.get(c.game_pid)
            if p and p.alive:
                p.alive = False
                p.hp = 0
                p.deaths += 1
                # Broadcast DAMAGE first with new_hp=0 so peers' local
                # Player.health gets clamped to 0 via cb_damage. Without
                # this, only PLAYER_KILL is sent and peers whose local
                # bullet sim never reached this victim (packet loss /
                # interp lag) keep showing the victim alive in the HUD.
                # User-reported as "portrait doesn't show dead even
                # though they are". DAMAGE-then-KILL ordering matters:
                # cb_damage updates Player.health, cb_player_kill
                # double-checks the clamp.
                self._broadcast(build_damage(p.pid, 0xFF, 0, 0))
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
        elif op == UNET_CLIENT_DBG_LOG and len(payload) >= 2:
            # Free-form debug trace from client. Frame body after op byte:
            # [text_len:1][text:N]. Used to localise client-side crashes
            # whose on-screen output we can't read from the TV — every
            # checkpoint the client crosses lands in our journal.
            tlen = payload[1]
            if 2 + tlen <= len(payload):
                msg = payload[2:2 + tlen].decode("ascii", errors="replace")
                log.info("CLIENT_DBG[%s]: %s", c.username or "?", msg)
        elif op == UNET_LEADERBOARD_REQ:
            top = sorted(self.leaderboard.values(),
                         key=lambda e: (-e.get("wins", 0), -e.get("best_hp", 0)))[:10]
            c.send_raw(build_leaderboard_data(top))
        else:
            # unknown opcode — ignore
            pass

    def _drop_client(self, uid: int, reason: str = "disconnected"):
        c = self.clients.pop(uid, None)
        if not c: return
        try: c.socket.close()
        except OSError: pass
        if c.authenticated:
            # Record a leave event so the admin portal's join-history
            # panel shows arrivals + departures, not just arrivals.
            self._record_join_event("leave", c.username, c.address, reason)
            self._broadcast(build_player_leave(uid))
            self._broadcast_lobby()
            # If they were in an active match, mark the dropped player
            # (and any P2 co-op slots they owned) dead so the alive_count
            # check in _tick_match can fire and end the match cleanly.
            # Otherwise the match runs forever while bots tick against
            # ghost players who left ages ago.
            if self.match is not None and c.in_game:
                affected_pids = [c.game_pid] + list(c.local_pids)
                for pid in affected_pids:
                    p = self.game_players.get(pid)
                    if p and p.alive:
                        p.alive = False
                        p.hp = 0
                        self._broadcast(build_player_kill(pid, 0xFF))
                # If no humans are left in the match at all, just end it.
                any_human_left = any(
                    cc.in_game for cc in self.clients.values()
                    if cc.authenticated and cc is not c
                )
                if not any_human_left:
                    log.info("Last human left mid-match — ending match.")
                    self._end_match(sudden=False)
        log.info("Client disconnected: %s", c.username or c.address)

    # ---------- bridge auth (handshake before SNCP framing begins) ----------

    def _try_bridge_auth(self, c: ClientInfo) -> bool:
        """Consumes the AUTH magic + length-prefixed shared-secret handshake
        used by DreamPi and netlink.py. Wire format:

            AUTH<u8 secret_len><secret bytes>

        Server replies with a single AUTH_OK byte once accepted, then SNCP
        framing begins. Returns True when the handshake is complete and
        bytes after it can be parsed as SNCP. Returns False if more bytes
        are needed OR the auth was bad and the socket is now closed (the
        caller checks for that via the closed flag we set on c)."""
        magic_len = len(AUTH_MAGIC)
        buf = c.recv_buffer
        if len(buf) < magic_len:
            return False
        if buf[:magic_len] != AUTH_MAGIC:
            log.info("Bridge auth: no AUTH magic from %s — legacy direct client",
                     c.address)
            return True
        if len(buf) < magic_len + 1:
            return False
        secret_len = buf[magic_len]
        total_needed = magic_len + 1 + secret_len
        if len(buf) < total_needed:
            return False
        received_secret = buf[magic_len + 1:total_needed]
        if received_secret != SHARED_SECRET:
            log.warning("Bridge auth FAILED from %s — wrong secret (got %d bytes)",
                        c.address, secret_len)
            self._mark_for_drop(c)
            return False
        try:
            c.socket.sendall(bytes([AUTH_OK]))
        except OSError:
            self._mark_for_drop(c)
            return False
        log.info("Bridge auth OK from %s — SNCP stream open", c.address)
        c.recv_buffer = buf[total_needed:]
        return True

    def _mark_for_drop(self, c: ClientInfo):
        """Close the socket and remove the ClientInfo from self.clients
        so the next select.select() doesn't see a closed FD (that raises
        ValueError: file descriptor cannot be a negative integer)."""
        try:
            c.socket.close()
        except OSError:
            pass
        for k in list(self.clients.keys()):
            if self.clients[k] is c:
                del self.clients[k]
                break

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
                # Defensively skip any client whose socket has been closed
                # (fileno() == -1) — passing -1 to select() raises a
                # ValueError that takes the whole server down.
                rlist = [self.server_socket]
                for c in list(self.clients.values()):
                    try:
                        if c.socket.fileno() < 0:
                            self._mark_for_drop(c)
                            continue
                    except (OSError, AttributeError):
                        self._mark_for_drop(c)
                        continue
                    rlist.append(c.socket)
                try:
                    r, _, _ = select.select(rlist, [], [], 0.01)
                except (ValueError, OSError) as e:
                    log.warning("select() rejected fd: %s — pruning closed sockets", e)
                    continue
                for sock in r:
                    if sock is self.server_socket:
                        try:
                            cs, addr = self.server_socket.accept()
                            cs.setblocking(False)
                            # Lobby capacity: hard-cap at MAX_LOBBY (8).
                            # Authenticated clients count as a slot;
                            # pre-auth pending sockets also count so a
                            # flood can't squeeze past the cap. The
                            # rejected client gets a TCP RST when we
                            # close their socket.
                            if len(self.clients) >= MAX_LOBBY:
                                log.info("Lobby full (%d/%d) — rejecting %s",
                                         len(self.clients), MAX_LOBBY, addr)
                                try: cs.close()
                                except OSError: pass
                                continue
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
                if path == "/api/list_custom_maps":
                    # Editor → admin portal data link. Lists .UTE files
                    # sitting in the editor's maps dir so the operator
                    # can pick one to pin as the live map.
                    payload = {
                        "maps_dir": server_ref.editor_maps_dir,
                        "live_map_slug": server_ref.tune.get("live_map_slug", ""),
                        "use_live_map":  bool(server_ref.tune.get("use_live_map", False)),
                        "maps": server_ref.list_custom_maps(),
                    }
                    body = json.dumps(payload).encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                if path.startswith("/api/join_history"):
                    # Parse optional ?limit=N (newest-first slice).
                    limit = None
                    qs = self.path.split("?", 1)
                    if len(qs) == 2:
                        from urllib.parse import parse_qs
                        try:
                            v = parse_qs(qs[1]).get("limit", [None])[0]
                            if v is not None: limit = int(v)
                        except Exception:
                            limit = None
                    hist = server_ref._join_history
                    if limit is not None and limit > 0:
                        rows = hist[-limit:]
                    else:
                        rows = hist
                    # Normalize legacy entries (which used 'addr' instead
                    # of 'ip' and had no 'event' or 'reason' fields).
                    norm = []
                    for e in reversed(rows):  # newest first
                        norm.append({
                            "t":      e.get("t", 0),
                            "event":  e.get("event", "join"),
                            "name":   e.get("name", ""),
                            "ip":     e.get("ip", e.get("addr", "")),
                            "reason": e.get("reason", ""),
                        })
                    body = json.dumps({
                        "events": norm,
                        "total": len(hist),
                    }).encode()
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
