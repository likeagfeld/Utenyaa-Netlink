#!/usr/bin/env python3
"""
Round-trip test for PLAYER_SYNC v2 quantization.

Mirrors the C inline helpers in src/net/utenyaa_protocol.h:
  unet_q_pos / unet_d_pos    (fxp 16.16 → int16, shift 9)
  unet_q_z   / unet_d_z      (fxp 16.16 → int8,  shift 16)
  unet_q_vel / unet_d_vel    (fxp 16.16 → int8,  shift 10)
  unet_q_angle / unet_d_angle (uint16   → uint8, shift 8)

Confirms that pack→unpack preserves the upper bits expected by the
game's collision and rendering math, and that edge values (boundary
fxp values, signed extremes) round-trip with the documented precision.
Run before deploying the server change to catch encoding regressions.

usage:  python3 test_quantize.py
exit:   0 = all pass, 1 = any failure
"""

import struct
import sys

# Replicate the server-side helpers from userver.py (importing it would
# pull asyncio + the whole framework, this is a focused test).

def _q_pos(fxp):
    q = fxp >> 9
    if q > 32767:  q = 32767
    if q < -32768: q = -32768
    return q

def _d_pos(q):
    return q << 9

def _q_z(fxp):
    q = fxp >> 16
    if q > 127:  q = 127
    if q < -128: q = -128
    return q

def _d_z(q):
    return q << 16

def _q_vel(fxp):
    q = fxp >> 10
    if q > 127:  q = 127
    if q < -128: q = -128
    return q

def _d_vel(q):
    return q << 10

def _q_angle(a16):
    return (a16 & 0xFFFF) >> 8

def _d_angle(q):
    return q << 8


# Pack/unpack as the C client would see it (big-endian for multi-byte).

def pack_player_sync(pid, x, y, z, dx, dy, dz, angle, hp, pickup):
    payload = bytes([0xA9, pid & 0xFF])
    payload += struct.pack("!hh", _q_pos(x), _q_pos(y))
    payload += struct.pack("!bbbb",
                           _q_z(z),
                           _q_vel(dx), _q_vel(dy), _q_vel(dz))
    payload += struct.pack("!BB",
                           _q_angle(angle),
                           ((pickup & 0x0F) << 4) | (hp & 0x0F))
    assert len(payload) == 12, f"PLAYER_SYNC v2 must be 12 bytes, got {len(payload)}"
    return payload

def unpack_player_sync(b):
    """Mirror the client's on_player_sync dequant: returns dict."""
    assert len(b) == 12 and b[0] == 0xA9
    pid = b[1]
    x_q, y_q = struct.unpack("!hh", b[2:6])
    z_q, dx_q, dy_q, dz_q = struct.unpack("!bbbb", b[6:10])
    angle_q, hp_pickup = struct.unpack("!BB", b[10:12])
    return {
        "pid": pid,
        "x": _d_pos(x_q),  "y": _d_pos(y_q),  "z": _d_z(z_q),
        "dx": _d_vel(dx_q),"dy": _d_vel(dy_q),"dz": _d_vel(dz_q),
        "angle": _d_angle(angle_q),
        "hp": hp_pickup & 0x0F,
        "pickup": (hp_pickup >> 4) & 0x0F,
    }


# === Test cases ===

failures = 0
def check(name, got, expected, tolerance=0):
    global failures
    if isinstance(expected, int):
        delta = abs(got - expected)
        if delta > tolerance:
            print(f"  FAIL {name}: got {got}, expected {expected} (±{tolerance})")
            failures += 1
        else:
            pass  # silent on pass
    elif got != expected:
        print(f"  FAIL {name}: got {got!r}, expected {expected!r}")
        failures += 1


print("Test 1: zero/origin position round-trip")
b = pack_player_sync(0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
r = unpack_player_sync(b)
check("zero.pid", r["pid"], 0)
for k in ("x","y","z","dx","dy","dz","angle","hp","pickup"):
    check(f"zero.{k}", r[k], 0)

print("Test 2: arena-corner position (160 world units ≈ 10485760 fxp)")
# 160 world units = 160 << 16 = 10485760. After >> 9 = 20480 (fits int16). << 9 = 10485760. exact.
b = pack_player_sync(1, 160 << 16, 160 << 16, 0, 0, 0, 0, 0, 6, 0)
r = unpack_player_sync(b)
check("arena.x", r["x"], 160 << 16, tolerance=(1 << 9) - 1)  # quantum = 1 << 9
check("arena.y", r["y"], 160 << 16, tolerance=(1 << 9) - 1)
check("arena.hp", r["hp"], 6)

print("Test 3: negative position (mirror of arena corner)")
b = pack_player_sync(2, -(160 << 16), -(160 << 16), 0, 0, 0, 0, 0, 6, 0)
r = unpack_player_sync(b)
check("neg.x", r["x"], -(160 << 16), tolerance=(1 << 9) - 1)
check("neg.y", r["y"], -(160 << 16), tolerance=(1 << 9) - 1)

print("Test 4: position clamp at int16 boundary (256 world units)")
# 256 << 16 = 16777216 fxp. >> 9 = 32768, clamps to 32767. << 9 = 16776704. Off by 512.
b = pack_player_sync(0, 256 << 16, 0, 0, 0, 0, 0, 0, 0, 0)
r = unpack_player_sync(b)
# Expect clamp; decoded should be ≤ 256 world units
assert r["x"] <= 256 << 16, f"position clamp failed: {r['x']}"
assert r["x"] >= 255 << 16, f"position clamp lost too much: {r['x']}"

print("Test 5: position clamp at int16 boundary (-260 world units)")
b = pack_player_sync(0, -260 << 16, 0, 0, 0, 0, 0, 0, 0, 0)
r = unpack_player_sync(b)
# Expect clamp; decoded should be >= -257 world units
assert r["x"] >= -257 << 16, f"neg position clamp failed: {r['x']}"
assert r["x"] <= -255 << 16, f"neg position clamp lost too much: {r['x']}"

print("Test 6: velocity at MovementSpeed=30 units/sec @50fps = 0.6 unit/frame")
# 0.6 * 65536 = 39322 fxp. >> 10 = 38. << 10 = 38912. expected ≈ 39322 ± 1024
vel_fxp = int(0.6 * 65536)
b = pack_player_sync(0, 0, 0, 0, vel_fxp, vel_fxp, 0, 0, 0, 0)
r = unpack_player_sync(b)
check("vel.dx", r["dx"], vel_fxp, tolerance=1024)  # quantum = 1 << 10
check("vel.dy", r["dy"], vel_fxp, tolerance=1024)

print("Test 7: velocity int8 saturation (large dx)")
b = pack_player_sync(0, 0, 0, 0, 200 << 16, 0, 0, 0, 0, 0)
r = unpack_player_sync(b)
# Should clamp at int8 max << 10 = 127 << 10 = 130048 ≈ 1.98 world unit/frame
assert r["dx"] == 130048, f"vel clamp failed: {r['dx']}"

print("Test 8: angle round-trip at every 90°")
# SGL angle: 16-bit, 0..65535 = 0..360°
for deg, sgl in [(0, 0), (90, 16384), (180, 32768), (270, 49152)]:
    b = pack_player_sync(0, 0, 0, 0, 0, 0, 0, sgl, 0, 0)
    r = unpack_player_sync(b)
    # Quantum = 256 (8 bits dropped). Values within ±255 of original are OK.
    check(f"angle.{deg}", r["angle"], sgl, tolerance=255)

print("Test 9: angle wrap (65535 → 65280 expected after >>8 << 8)")
b = pack_player_sync(0, 0, 0, 0, 0, 0, 0, 65535, 0, 0)
r = unpack_player_sync(b)
# 65535 >> 8 = 255; 255 << 8 = 65280. lossy by 255 — acceptable.
check("angle.wrap", r["angle"], 65280)

print("Test 10: hp+pickup nibble pack")
for hp, pickup in [(0,0), (6,0), (3,1), (6,2), (0,15)]:
    b = pack_player_sync(0, 0, 0, 0, 0, 0, 0, 0, hp, pickup)
    r = unpack_player_sync(b)
    check(f"hp_pick({hp},{pickup}).hp", r["hp"], hp & 0x0F)
    check(f"hp_pick({hp},{pickup}).pickup", r["pickup"], pickup & 0x0F)

print("Test 11: pickup 0xFF (server's 'no pickup' sentinel) maps to 0x0F")
b = pack_player_sync(0, 0, 0, 0, 0, 0, 0, 0, 6, 0xFF)
r = unpack_player_sync(b)
# 0xFF & 0x0F = 0x0F. Client must interpret 0x0F as "no pickup".
check("sentinel.pickup", r["pickup"], 0x0F)
check("sentinel.hp", r["hp"], 6)

print("Test 12: z-axis ground-plane (z = -2 to +5 world units)")
for zw in [-2, -1, 0, 1, 2, 3, 5]:
    b = pack_player_sync(0, 0, 0, zw << 16, 0, 0, 0, 0, 0, 0)
    r = unpack_player_sync(b)
    check(f"z.{zw}", r["z"], zw << 16)  # exact (shift 16 = no precision loss for integer z)

print("Test 13: pid range 0..3")
for pid in [0, 1, 2, 3]:
    b = pack_player_sync(pid, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    r = unpack_player_sync(b)
    check(f"pid.{pid}", r["pid"], pid)

print("Test 14: idempotency — pack(unpack(pack(x))) == pack(x)")
import random
random.seed(42)
for _ in range(100):
    pid = random.randint(0, 3)
    x = random.randint(-160 << 16, 160 << 16)
    y = random.randint(-160 << 16, 160 << 16)
    z = random.randint(-2 << 16, 5 << 16)
    dx = random.randint(-1 << 16, 1 << 16)
    dy = random.randint(-1 << 16, 1 << 16)
    dz = 0
    angle = random.randint(0, 65535)
    hp = random.randint(0, 6)
    pickup = random.choice([0, 1, 2, 0xFF])
    b1 = pack_player_sync(pid, x, y, z, dx, dy, dz, angle, hp, pickup)
    r = unpack_player_sync(b1)
    b2 = pack_player_sync(pid, r["x"], r["y"], r["z"],
                          r["dx"], r["dy"], r["dz"], r["angle"],
                          r["hp"], r["pickup"])
    assert b1 == b2, f"non-idempotent at pid={pid} x={x} y={y}"

print()
if failures == 0:
    print("All tests passed.")
    sys.exit(0)
else:
    print(f"{failures} failure(s).")
    sys.exit(1)
