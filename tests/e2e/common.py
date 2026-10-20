"""
Common test utilities, structures, math, and mock harness for BLACK E2E tests.
Derived authoritatively from ORIGINAL_REQUEST.md, PROJECT.md,
and source implementations in src/net/ and src/game/.
"""

import math
import os
import socket
import struct
import subprocess
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple

REPO_ROOT = Path(__file__).resolve().parent.parent.parent


def get_native_test_binary_path() -> Path:
    return REPO_ROOT / "tests" / "test_adversarial_redirection"


def ensure_native_test_binary() -> Path:
    """Ensures tests/test_adversarial_redirection exists and is up to date."""
    bin_path = get_native_test_binary_path()
    cpp_src = REPO_ROOT / "tests" / "test_adversarial_redirection.cpp"
    obj_files = [
        REPO_ROOT / "out/build/release/CMakeFiles/Black.dir/sdk/runtime/kernel/files.cpp.o",
        REPO_ROOT / "out/build/release/CMakeFiles/Black.dir/sdk/runtime/kernel/disc.cpp.o",
        REPO_ROOT / "out/build/release/CMakeFiles/Black.dir/sdk/runtime/core/log.cpp.o",
    ]
    stale = not bin_path.exists()
    if not stale:
        bin_mtime = bin_path.stat().st_mtime
        if cpp_src.stat().st_mtime > bin_mtime:
            stale = True
        for obj in obj_files:
            if obj.exists() and obj.stat().st_mtime > bin_mtime:
                stale = True
                break
    if stale:
        if not all(obj.exists() for obj in obj_files):
            raise RuntimeError("Cannot compile native test harness: missing object files in out/build/release/")
        cmd = [
            "g++", "-std=c++20", "-O2",
            "-I", str(REPO_ROOT / "src"),
            "-I", str(REPO_ROOT / "sdk" / "runtime"),
            str(cpp_src),
            *[str(o) for o in obj_files],
            "-o", str(bin_path)
        ]
        res = subprocess.run(cmd, cwd=str(REPO_ROOT), capture_output=True, text=True)
        if res.returncode != 0:
            raise RuntimeError(f"Failed to compile native test harness:\n{res.stderr}")
    return bin_path


def resolve_native_path(xbox_path: str) -> Optional[str]:
    """Resolves an Xbox path to host path via native C++ kernel::ResolvePath."""
    bin_path = ensure_native_test_binary()
    proc = subprocess.run([str(bin_path), "--resolve", xbox_path], cwd=str(REPO_ROOT), capture_output=True, text=True)
    if proc.returncode == 0:
        return proc.stdout.strip()
    return None


def read_native_game_file_size(xbox_path: str) -> Tuple[bool, int, Optional[str]]:
    """Reads a file via native C++ kernel::ReadGameFile and returns (success, size, error_msg)."""
    bin_path = ensure_native_test_binary()
    proc = subprocess.run([str(bin_path), "--read-size", xbox_path], cwd=str(REPO_ROOT), capture_output=True, text=True)
    if proc.returncode == 0:
        out = proc.stdout.strip()
        if out.startswith("SIZE "):
            return True, int(out.split()[1]), None
        return True, 0, None
    elif proc.returncode == 2:
        return False, 0, proc.stderr.strip()
    elif proc.returncode < 0:
        return False, 0, f"CRASH_SIGNAL_{abs(proc.returncode)}"
    return False, 0, None


def run_native_adversarial_suite() -> Tuple[int, str]:
    """Executes the full native C++ adversarial test suite."""
    bin_path = ensure_native_test_binary()
    proc = subprocess.run([str(bin_path)], cwd=str(REPO_ROOT), capture_output=True, text=True)
    return proc.returncode, proc.stdout


# Protocol Constants
MAGIC_PROTOCOL = 0x424C4143  # 'BLAC'
DEFAULT_PORT = 1508
MAX_PLAYERS = 8
TIMEOUT_SECONDS = 5.0

# Message Types (from src/net/network.h)
MSG_PING = 0
MSG_PONG = 1
MSG_JOIN_REQUEST = 2
MSG_JOIN_ACK = 3
MSG_STATE_UPDATE = 4
MSG_SHOOT = 5
MSG_HIT = 6
MSG_DISCONNECT = 7
MSG_CHAT = 8
# Co-op. The host owns the world and the guest reacts to it.
MSG_COOP_ROOM = 9
MSG_COOP_READY = 10
MSG_COOP_SNAPSHOT = 11
MSG_COOP_EVENT = 12
MSG_COOP_ACK = 13

# Binary Packet Struct Formats (little-endian, packed)
HEADER_FMT = "<IBBHI"  # magic (uint32), type (uint8), player_id (uint8), sequence (uint16), timestamp_ms (uint32)
HEADER_SIZE = struct.calcsize(HEADER_FMT)  # 12 bytes

STATE_DATA_FMT = "<ffffffffIIfHH"  # pos[3], forward[3], yaw, pitch, weapon_id, flags, health, frags, deaths
STATE_DATA_SIZE = struct.calcsize(STATE_DATA_FMT)  # 48 bytes

PACKET_STATE_SIZE = HEADER_SIZE + STATE_DATA_SIZE  # 60 bytes
JOIN_REQ_FMT = "<" + HEADER_FMT[1:] + "32s"  # 12 + 32 = 44 bytes
JOIN_ACK_FMT = "<" + HEADER_FMT[1:] + "BB32s"  # 12 + 1 + 1 + 32 = 46 bytes
SHOOT_FMT = "<" + HEADER_FMT[1:] + "ffffffI"  # 12 + 24 + 4 = 40 bytes
HIT_FMT = "<" + HEADER_FMT[1:] + "Bffff"  # 12 + 1 + 4 + 12 = 29 bytes
CHAT_FMT = "<" + HEADER_FMT[1:] + "64s"  # 12 + 64 = 76 bytes

# Engine Memory Layout Constants (from src/game/layout.h and explorer reports)
ADDR_GAME = 0x002D45F0
ADDR_WORLD = 0x002D45D4
ADDR_CHAR_MGR = 0x002D1980
ADDR_SPEECH_FLAG = 0x002D45D8 + 0xB14
ADDR_LEVEL_STATE_VTBL = 0x00295338
ADDR_MISSION_RULES_VTBL = 0x002952F4
ADDR_SUB_DIRECT_LAUNCH = 0x0020DDD0
ADDR_SUB_TRIGGER_FAILED = 0x0020C630
ADDR_FRAME_LOOP = 0x0020F6A0

ACTOR_POOL_OFFSET = 0xA0
ACTOR_SLOT_SIZE = 960  # 0x3C0
ACTOR_POOL_SLOTS = 32
ACTOR_ACTIVE_COUNT_OFFSET = 0x79B4
ACTOR_TABLE_OFFSET = 0x79BC

# 10 Tactical Spawns across Mission 1 (Veblensk City Street)
MISSION1_SPAWNS = [
    (-4.0, 0.0, 2.0),     # Spawn 0: Start alley
    (12.0, 0.0, 18.0),    # Spawn 1: Behind burned truck
    (-14.0, 0.0, 32.0),   # Spawn 2: Inside bombed house
    (8.0, 0.0, 48.0),     # Spawn 3: Sandbag barricade
    (-2.0, 0.0, 65.0),    # Spawn 4: Central plaza entry
    (16.0, 0.0, 82.0),    # Spawn 5: Right flanking alley
    (-12.0, 0.0, 98.0),   # Spawn 6: Sniper window ruin
    (0.0, 0.0, 115.0),    # Spawn 7: Main crossroads
    (10.0, 0.0, 130.0),   # Spawn 8: Memorial square
    (-8.0, 0.0, 145.0)    # Spawn 9: Street end barricade
]

# CS 1.6 Damage Specifications
DAMAGE_HEAD = 85.0
DAMAGE_CHEST = 34.0
DAMAGE_STOMACH = 30.0
DAMAGE_LEGS = 22.0

# Hitbox Zones
ZONE_LEGS = 0
ZONE_STOMACH = 1
ZONE_CHEST = 2
ZONE_HEAD = 3

# Operator Model Archetypes (from StLevel.bin analysis)
OPERATOR_ARCHETYPES = [
    "bc1_so1_mil",       # Standard Military Soldier
    "bc1_asr_goggles",   # Assault Operator with Goggles
    "bc1_lr1_mil",       # Long-Range Marksman
    "bc1_rg1_mil",       # Heavy Weapons / RPG Operator
    "bc1_sk1_mil"        # Specialist / Shotgunner
]


class Vec3:
    """3D Vector matching engine representation."""
    def __init__(self, x: float = 0.0, y: float = 0.0, z: float = 0.0):
        self.x = float(x)
        self.y = float(y)
        self.z = float(z)

    def __add__(self, o: "Vec3") -> "Vec3":
        return Vec3(self.x + o.x, self.y + o.y, self.z + o.z)

    def __sub__(self, o: "Vec3") -> "Vec3":
        return Vec3(self.x - o.x, self.y - o.y, self.z - o.z)

    def __mul__(self, s: float) -> "Vec3":
        return Vec3(self.x * s, self.y * s, self.z * s)

    def length(self) -> float:
        return math.sqrt(self.x * self.x + self.y * self.y + self.z * self.z)

    def normalize(self) -> "Vec3":
        l = self.length()
        if l <= 0.0001:
            return Vec3(0.0, 0.0, 1.0)
        return Vec3(self.x / l, self.y / l, self.z / l)

    def dot(self, o: "Vec3") -> float:
        return self.x * o.x + self.y * o.y + self.z * o.z

    def cross(self, o: "Vec3") -> "Vec3":
        return Vec3(
            self.y * o.z - self.z * o.y,
            self.z * o.x - self.x * o.z,
            self.x * o.y - self.y * o.x
        )

    def as_tuple(self) -> Tuple[float, float, float]:
        return (self.x, self.y, self.z)


def pack_header(msg_type: int, player_id: int, sequence: int, timestamp_ms: int = 0) -> bytes:
    """Pack 12-byte protocol header."""
    return struct.pack(HEADER_FMT, MAGIC_PROTOCOL, msg_type, player_id, sequence, timestamp_ms)


def unpack_header(data: bytes) -> Optional[Tuple[int, int, int, int, int]]:
    """Unpack 12-byte header: returns (magic, type, player_id, sequence, timestamp_ms) or None."""
    if len(data) < HEADER_SIZE:
        return None
    return struct.unpack(HEADER_FMT, data[:HEADER_SIZE])


def pack_state_packet(player_id: int, sequence: int, pos: Tuple[float, float, float],
                      forward: Tuple[float, float, float], yaw: float, pitch: float,
                      weapon_id: int = 1, flags: int = 0, health: float = 100.0,
                      frags: int = 0, deaths: int = 0, timestamp_ms: int = 0) -> bytes:
    """Pack 60-byte PacketState."""
    header = pack_header(MSG_STATE_UPDATE, player_id, sequence, timestamp_ms)
    state = struct.pack(STATE_DATA_FMT,
                       pos[0], pos[1], pos[2],
                       forward[0], forward[1], forward[2],
                       yaw, pitch, weapon_id, flags, health, frags, deaths)
    return header + state


def unpack_state_packet(data: bytes) -> Optional[Dict]:
    """Unpack PacketState data dictionary."""
    if len(data) < PACKET_STATE_SIZE:
        return None
    hdr = unpack_header(data[:HEADER_SIZE])
    if not hdr or hdr[0] != MAGIC_PROTOCOL:
        return None
    fields = struct.unpack(STATE_DATA_FMT, data[HEADER_SIZE:PACKET_STATE_SIZE])
    return {
        "magic": hdr[0],
        "type": hdr[1],
        "player_id": hdr[2],
        "sequence": hdr[3],
        "timestamp_ms": hdr[4],
        "pos": (fields[0], fields[1], fields[2]),
        "forward": (fields[3], fields[4], fields[5]),
        "yaw": fields[6],
        "pitch": fields[7],
        "weapon_id": fields[8],
        "flags": fields[9],
        "health": fields[10],
        "frags": fields[11],
        "deaths": fields[12],
    }


def pack_join_request(player_name: str, sequence: int = 1) -> bytes:
    """Pack PacketJoinRequest."""
    hdr = pack_header(MSG_JOIN_REQUEST, 0, sequence, int(time.time() * 1000) & 0xFFFFFFFF)
    name_bytes = player_name.encode("utf-8")[:31].ljust(32, b"\x00")
    return hdr + name_bytes


def pack_join_ack(assigned_id: int, max_players: int = MAX_PLAYERS,
                  server_name: str = "BLACK Dedicated", sequence: int = 1) -> bytes:
    """Pack PacketJoinAck."""
    hdr = pack_header(MSG_JOIN_ACK, assigned_id, sequence, int(time.time() * 1000) & 0xFFFFFFFF)
    sname_bytes = server_name.encode("utf-8")[:31].ljust(32, b"\x00")
    return hdr + struct.pack("<BB32s", assigned_id, max_players, sname_bytes)


def pack_hit(shooter_id: int, victim_id: int, damage: float,
             hit_pos: Tuple[float, float, float], sequence: int = 1) -> bytes:
    """Pack PacketHit."""
    hdr = pack_header(MSG_HIT, shooter_id, sequence, int(time.time() * 1000) & 0xFFFFFFFF)
    return hdr + struct.pack("<Bffff", victim_id, damage, hit_pos[0], hit_pos[1], hit_pos[2])


def pack_shoot(shooter_id: int, origin: Tuple[float, float, float],
               direction: Tuple[float, float, float], weapon_id: int = 1, sequence: int = 1) -> bytes:
    """Pack PacketShoot."""
    hdr = pack_header(MSG_SHOOT, shooter_id, sequence, int(time.time() * 1000) & 0xFFFFFFFF)
    return hdr + struct.pack("<ffffffI", origin[0], origin[1], origin[2],
                             direction[0], direction[1], direction[2], weapon_id)


def pack_chat(player_id: int, message: str, sequence: int = 1) -> bytes:
    """Pack PacketChat."""
    hdr = pack_header(MSG_CHAT, player_id, sequence, int(time.time() * 1000) & 0xFFFFFFFF)
    msg_bytes = message.encode("utf-8")[:63].ljust(64, b"\x00")
    return hdr + msg_bytes


def simulate_hit_detection(origin: Vec3, dir: Vec3, target_pos: Vec3,
                           is_crouching: bool = False,
                           strict_3d_sphere: bool = False) -> Optional[Tuple[int, float, Vec3]]:
    """
    Simulates authoritative CS 1.6 4-zone hitbox calculation.
    Returns (hit_zone, damage, hit_position) or None if missed.
    According to PROJECT.md F8, hitboxes are Cylindrical (radius 0.85m).
    If strict_3d_sphere is True, mirrors multiplayer.cpp:348 bug where Dot(diff, diff)
    includes Y distance, prematurely rejecting leg shots.
    """
    to_target = target_pos - origin
    proj = to_target.dot(dir)
    if proj <= 0.0 or proj > 120.0:
        return None

    closest = origin + (dir * proj)
    diff = target_pos - closest

    if strict_3d_sphere:
        dist_sq = diff.dot(diff)
    else:
        # True cylindrical radius: horizontal distance squared (X^2 + Z^2)
        dist_sq = diff.x * diff.x + diff.z * diff.z

    # 0.85m cylindrical body radius
    if dist_sq >= (0.85 * 0.85):
        return None

    # Stance height calculation
    feet_y = target_pos.y - (1.05 if is_crouching else 1.65)
    hit_h = closest.y - feet_y
    max_h = 1.25 if is_crouching else 1.85

    if hit_h < -0.10 or hit_h > max_h:
        return None

    if is_crouching:
        if hit_h >= 0.90:
            return (ZONE_HEAD, DAMAGE_HEAD, closest)
        elif hit_h >= 0.60:
            return (ZONE_CHEST, DAMAGE_CHEST, closest)
        elif hit_h >= 0.35:
            return (ZONE_STOMACH, DAMAGE_STOMACH, closest)
        else:
            return (ZONE_LEGS, DAMAGE_LEGS, closest)
    else:
        if hit_h >= 1.45:
            return (ZONE_HEAD, DAMAGE_HEAD, closest)
        elif hit_h >= 1.05:
            return (ZONE_CHEST, DAMAGE_CHEST, closest)
        elif hit_h >= 0.70:
            return (ZONE_STOMACH, DAMAGE_STOMACH, closest)
        else:
            return (ZONE_LEGS, DAMAGE_LEGS, closest)


def compute_puppet_matrix(pos: Vec3, forward: Vec3, yaw_rad: float, pitch_rad: float,
                          move_rate: float, is_crouching: bool) -> Dict:
    """
    Computes puppet 4x4 matrix and animation controller state
    matching MultiplayerManager::UpdateEnemyPuppet.
    """
    fwd = Vec3(forward.x, 0.0, forward.z).normalize()
    up = Vec3(0.0, 1.0, 0.0)
    right = up.cross(fwd).normalize()

    base_y = pos.y - (1.05 if is_crouching else 1.65)

    yaw_deg = yaw_rad * (180.0 / math.pi)
    pitch_deg = pitch_rad * (180.0 / math.pi)

    return {
        "right": (right.x, right.y, right.z, 0.0),
        "up": (up.x, up.y, up.z, 0.0),
        "forward": (fwd.x, fwd.y, fwd.z, 0.0),
        "pos": (pos.x, base_y, pos.z, 1.0),
        "crouch_flag": 1 if is_crouching else 0,
        "move_rate": move_rate,
        "yaw_deg": yaw_deg,
        "pitch_deg": pitch_deg,
        "lod": 0,
        "active": 1
    }


def select_best_spawn(enemy_positions: List[Vec3], min_safe_dist: float = 15.0) -> Tuple[int, Vec3]:
    """
    Selects spawn point with anti-spawncamp protection.
    Finds spawn point that has no enemy within min_safe_dist (15.0m).
    If all spawns have nearby enemies, picks the spawn with maximum min-distance.
    """
    spawns = [Vec3(s[0], s[1], s[2]) for s in MISSION1_SPAWNS]
    if not enemy_positions:
        return (0, spawns[0])

    safe_spawns = []
    max_min_dist = -1.0
    best_fallback_idx = 0

    for idx, sp in enumerate(spawns):
        closest_dist = min((sp - ep).length() for ep in enemy_positions)
        if closest_dist >= min_safe_dist:
            safe_spawns.append((idx, sp, closest_dist))
        if closest_dist > max_min_dist:
            max_min_dist = closest_dist
            best_fallback_idx = idx

    if safe_spawns:
        # Return first safe spawn
        return (safe_spawns[0][0], safe_spawns[0][1])
    return (best_fallback_idx, spawns[best_fallback_idx])


def read_iso_volume_descriptor(iso_path: str) -> Optional[Tuple[int, int]]:
    """
    Finds and reads the Xbox Volume Descriptor in an ISO.
    Checks sector 32 from start (offset 0x10000) or sector 32 from raw partition (0x18300000 + 0x10000).
    Returns (root_sector, root_size) or None.
    """
    if not os.path.isfile(iso_path):
        return None
    with open(iso_path, "rb") as f:
        for base in [0, 0x18300000]:
            f.seek(base + 32 * 2048)
            magic = f.read(20)
            if magic == b"MICROSOFT*XBOX*MEDIA":
                root_sector, root_size = struct.unpack("<II", f.read(8))
                return (root_sector, root_size)
    return None
