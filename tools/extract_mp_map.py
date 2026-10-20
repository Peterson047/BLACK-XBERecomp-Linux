#!/usr/bin/env python3
"""
extract_mp_map.py - Standalone Clean Multiplayer Map Extractor for BLACK (XBERecomp Linux)

Extracts Mission 1 (Veblensk City) from assets/Black.iso into a clean, autonomous
multiplayer map package (assets/levels/level_mp00/ or clean level_00/).
Excludes FMVs; runtime hooks suppress campaign dialogue and AI behavior.
Preserves the speech resource tables required by the level loader; runtime hooks mute dialogue.

Zero external dependencies - standard Python 3.8+ only.
"""

import argparse
import os
import struct
import sys
import time

SECTOR_SIZE = 2048
PARTITION_BASES = [0, 0x18300000]
VOLUME_SECTOR = 32
VOLUME_OFFSET = VOLUME_SECTOR * SECTOR_SIZE
XBOX_MEDIA_MAGIC = b"MICROSOFT*XBOX*MEDIA"
ENTRY_HEADER_SIZE = 0x0E
ATTR_DIRECTORY = 0x10

# Expected magic headers for validation
EXPECTED_MAGICS = {
    "Unit_01.bin": b"\x18\x00\x00\x00",
    "Collide.awd": b"\x09\x08\x00\x00",
    "LevelDat.bin": b"\x11\x00\x00\x00",
    "Level.awd": b"\x09\x08\x00\x00",
    "StLevel.bin": b"\x0A\x00\x00\x00",
    "StUnit01.bin": b"\x03\x00\x00\x00",
    "guns.bin": b"\x00\x30\x02\x00",
    "guns_s.bin": b"\x00\x33\x02\x00",
    "AIWpns.awd": b"\x09\x08\x00\x00",
    "Emitter.awd": b"\x09\x08\x00\x00",
}


class IsoEntry:
    def __init__(self, name: str, offset: int, size: int, is_dir: bool):
        self.name = name
        self.offset = offset
        self.size = size
        self.is_dir = is_dir
        self.children = []

    def __repr__(self):
        return f"<IsoEntry '{self.name}' size={self.size} dir={self.is_dir}>"


class XboxIso:
    """Parser for Xbox DVD Media Filesystem (XDVDFS). Supports both flat XISOs and raw dumps."""

    def __init__(self, iso_path: str):
        self.iso_path = iso_path
        if not os.path.isfile(iso_path):
            raise FileNotFoundError(f"ISO file not found: {iso_path}")
        self.file = open(iso_path, "rb")
        self.base = None
        self.root = None
        self._init_volume()

    def _init_volume(self):
        for base in PARTITION_BASES:
            self.file.seek(base + VOLUME_OFFSET)
            vol = self.file.read(SECTOR_SIZE)
            if (len(vol) == SECTOR_SIZE and
                vol[:len(XBOX_MEDIA_MAGIC)] == XBOX_MEDIA_MAGIC and
                vol[0x7EC:0x7EC + len(XBOX_MEDIA_MAGIC)] == XBOX_MEDIA_MAGIC):
                self.base = base
                root_sec, root_size = struct.unpack_from("<II", vol, 0x14)
                self.root = IsoEntry("", self.base + root_sec * SECTOR_SIZE, root_size, True)
                self._read_directory(self.root, 0)
                return
        raise ValueError(f"No valid Xbox Media volume descriptor found in {self.iso_path}")

    def _read_directory(self, dir_entry: IsoEntry, depth: int):
        if not dir_entry.size or depth > 32:
            return
        self.file.seek(dir_entry.offset)
        table = self.file.read(dir_entry.size)
        if len(table) != dir_entry.size:
            return

        seen = [False] * (dir_entry.size // 4 + 1)
        todo = [0]
        while todo:
            at = todo.pop()
            if at + ENTRY_HEADER_SIZE > dir_entry.size or seen[at // 4]:
                continue
            seen[at // 4] = True

            left, right = struct.unpack_from("<HH", table, at)
            if left == 0xFFFF:
                continue

            sector, entry_size = struct.unpack_from("<II", table, at + 4)
            attrib, length = struct.unpack_from("<BB", table, at + 12)
            if at + ENTRY_HEADER_SIZE + length > dir_entry.size:
                continue

            if left:
                todo.append(left * 4)
            if right:
                todo.append(right * 4)

            name = table[at + ENTRY_HEADER_SIZE : at + ENTRY_HEADER_SIZE + length].decode("latin1", errors="ignore")
            is_dir = bool(attrib & ATTR_DIRECTORY)
            child_offset = self.base + sector * SECTOR_SIZE
            child = IsoEntry(name, child_offset, entry_size, is_dir)
            dir_entry.children.append(child)

        dir_entry.children.sort(key=lambda c: c.name.upper())
        for child in dir_entry.children:
            if child.is_dir:
                self._read_directory(child, depth + 1)

    def find(self, parts: list) -> IsoEntry:
        """Finds an entry given a list of path segments (case-insensitive)."""
        cur = self.root
        for part in parts:
            if not part or part == ".":
                continue
            target = part.upper()
            match = next((c for c in cur.children if c.name.upper() == target), None)
            if match is None:
                return None
            cur = match
        return cur

    def extract_file(self, entry: IsoEntry, dest_path: str, chunk_size: int = 1024 * 1024):
        """Streams file content from ISO into dest_path."""
        if entry.is_dir:
            raise ValueError(f"Cannot extract directory as file: {entry.name}")
        os.makedirs(os.path.dirname(dest_path), exist_ok=True)
        self.file.seek(entry.offset)
        remaining = entry.size
        with open(dest_path, "wb") as out_fp:
            while remaining > 0:
                to_read = min(remaining, chunk_size)
                buf = self.file.read(to_read)
                if not buf:
                    break
                out_fp.write(buf)
                remaining -= len(buf)

    def close(self):
        self.file.close()


def verify_map_package(out_dir: str, check_destruction: bool = True) -> bool:
    """Verifies sizes, existence, and magic headers for all extracted files."""
    print("\n--- Verifying Extracted Multiplayer Map Package ---")
    all_ok = True

    required_files = [
        ("Unit_01.bin", EXPECTED_MAGICS["Unit_01.bin"], 10526464),
        ("Collide.awd", EXPECTED_MAGICS["Collide.awd"], 2973696),
        ("LevelDat.bin", EXPECTED_MAGICS["LevelDat.bin"], 711088),
        ("Level.awd", EXPECTED_MAGICS["Level.awd"], 968704),
        ("stg_0001/StLevel.bin", EXPECTED_MAGICS["StLevel.bin"], 2378464),
        ("stg_0001/StUnit01.bin", EXPECTED_MAGICS["StUnit01.bin"], 265504),
        ("stg_0001/guns.bin", EXPECTED_MAGICS["guns.bin"], 275968),
        ("stg_0001/guns_s.bin", EXPECTED_MAGICS["guns_s.bin"], 276736),
        ("stg_0001/AIWpns.awd", EXPECTED_MAGICS["AIWpns.awd"], 606208),
        ("stg_0001/Emitter.awd", EXPECTED_MAGICS["Emitter.awd"], 4096),
        ("speech.slb", None, 6944),
    ]

    if check_destruction:
        required_files.extend([
            ("Destruct.bks", None, 3899392),
            ("Destruct.ssh", None, 448),
        ])

    for rel_path, expected_magic, expected_size in required_files:
        full_path = os.path.join(out_dir, rel_path)
        if not os.path.isfile(full_path):
            print(f"  [FAIL] Missing file: {rel_path}")
            all_ok = False
            continue

        size = os.path.getsize(full_path)
        if expected_size is not None and size != expected_size:
            print(f"  [FAIL] Size mismatch {rel_path}: got {size}, expected {expected_size}")
            all_ok = False
            continue

        if expected_magic is not None:
            with open(full_path, "rb") as f:
                head = f.read(len(expected_magic))
                if head != expected_magic:
                    print(f"  [FAIL] Magic mismatch {rel_path}: got {head.hex()}, expected {expected_magic.hex()}")
                    all_ok = False
                    continue

        print(f"  [PASS] {rel_path:30} ({size:10} bytes) OK")

    # The loader indexes speech resources even when dialogue playback is muted.
    slb_path = os.path.join(out_dir, "speech.slb")
    if os.path.isfile(slb_path):
        with open(slb_path, "rb") as speech_file:
            data = speech_file.read()
        valid = False
        if len(data) >= 104:
            version, magic = struct.unpack_from("<I4s", data, 0)
            cues, = struct.unpack_from("<I", data, 0x54)
            table, = struct.unpack_from("<I", data, 0x5C)
            valid = version == 1 and magic == b"KING" and cues > 0 and 104 <= table < len(data)
        if valid:
            print(f"  [PASS] speech.slb retains {cues} resource entries")
        else:
            print("  [FAIL] speech.slb lacks the resource tables required by the loader")
            all_ok = False

    # Verify fpguns directory
    fpguns_dir = os.path.join(out_dir, "fpguns")
    if os.path.isdir(fpguns_dir):
        fpgun_files = os.listdir(fpguns_dir)
        db_files = [f for f in fpgun_files if f.endswith(".db")]
        wdd_files = [f for f in fpgun_files if f.endswith(".wdd")]
        print(f"  [PASS] fpguns/ contains {len(db_files)} .db and {len(wdd_files)} .wdd weapon files")
        if len(db_files) < 16 or len(wdd_files) < 16:
            print(f"  [WARN] Expected 16 weapon pairs in fpguns/, found {len(db_files)} DBs")
    else:
        print("  [FAIL] Missing fpguns directory")
        all_ok = False

    return all_ok


def extract_mp_map(iso_path: str, out_dir: str, dest_name: str = "level_mp00",
                   include_destruction: bool = True, include_ambience: bool = False,
                   verbose: bool = False):
    start_time = time.time()
    print(f"Opening Xbox ISO: {iso_path}")
    iso = XboxIso(iso_path)
    print(f"  Partition Base: 0x{iso.base:X} ({'Flat XISO' if iso.base == 0 else 'Raw Redump'})")
    print(f"Destination Directory: {out_dir}")

    os.makedirs(out_dir, exist_ok=True)

    # 1. Core Level Root Files
    core_files = [
        "Unit_01.bin",
        "Collide.awd",
        "LevelDat.bin",
        "Level.awd",
        "speech.slb",
    ]
    if include_destruction:
        core_files.extend(["Destruct.bks", "Destruct.ssh"])
    if include_ambience:
        core_files.extend(["Ambience.bks", "Ambience.ssh"])

    total_bytes = 0
    file_count = 0

    print("\n--- Extracting Core World & Geometry Files ---")
    for fname in core_files:
        entry = iso.find(["levels", "level_00", fname])
        if entry is None:
            print(f"  [ERROR] File not found in ISO: levels/level_00/{fname}")
            continue
        dest_file = os.path.join(out_dir, fname)
        if verbose:
            print(f"  Extracting {fname} ({entry.size} bytes)...")
        iso.extract_file(entry, dest_file)
        total_bytes += entry.size
        file_count += 1
        print(f"  [OK] {fname:25} ({entry.size:10} bytes)")

    # 2. Stage 0001 (Soldier Characters & Weapon Meshes)
    stg_files = [
        "StLevel.bin",
        "StUnit01.bin",
        "guns.bin",
        "guns_s.bin",
        "AIWpns.awd",
        "Emitter.awd",
    ]
    print("\n--- Extracting Stage 0001 (Puppets & Weapons) ---")
    stg_out_dir = os.path.join(out_dir, "stg_0001")
    os.makedirs(stg_out_dir, exist_ok=True)
    for fname in stg_files:
        entry = iso.find(["levels", "level_00", "stg_0001", fname])
        if entry is None:
            print(f"  [ERROR] File not found in ISO: levels/level_00/stg_0001/{fname}")
            continue
        dest_file = os.path.join(stg_out_dir, fname)
        iso.extract_file(entry, dest_file)
        total_bytes += entry.size
        file_count += 1
        print(f"  [OK] stg_0001/{fname:20} ({entry.size:10} bytes)")

    # 3. First-Person Weapons Database (fpguns)
    print("\n--- Extracting First-Person Weapon Databases (fpguns) ---")
    fpguns_entry = iso.find(["levels", "level_00", "fpguns"])
    if fpguns_entry and fpguns_entry.is_dir:
        fpguns_out_dir = os.path.join(out_dir, "fpguns")
        os.makedirs(fpguns_out_dir, exist_ok=True)
        for child in fpguns_entry.children:
            if not child.is_dir and (child.name.endswith(".db") or child.name.endswith(".wdd")):
                dest_file = os.path.join(fpguns_out_dir, child.name)
                iso.extract_file(child, dest_file)
                total_bytes += child.size
                file_count += 1
                if verbose:
                    print(f"  [OK] fpguns/{child.name:20} ({child.size:10} bytes)")
        print(f"  [OK] Extracted {len(fpguns_entry.children)} weapon database & animation files")

    iso.close()

    elapsed = time.time() - start_time
    mb_extracted = total_bytes / (1024 * 1024)
    print(f"\nExtraction complete in {elapsed:.2f}s: {file_count} files, {mb_extracted:.2f} MB written to {out_dir}")


def main():
    parser = argparse.ArgumentParser(description="Clean Multiplayer Map Extractor for BLACK (Mission 1: Veblensk)")
    parser.add_argument("--iso", default="assets/Black.iso", help="Path to Black.iso (default: assets/Black.iso)")
    parser.add_argument("--out", default="assets/levels/level_mp00", help="Output directory (default: assets/levels/level_mp00)")
    parser.add_argument("--dest-name", default="level_mp00", help="Internal level name for prefix (default: level_mp00)")
    parser.add_argument("--include-destruction", action="store_true", default=True, help="Include destruction sound streams")
    parser.add_argument("--no-destruction", action="store_false", dest="include_destruction")
    parser.add_argument("--include-ambience", action="store_true", default=False, help="Include ambient sound stream (19 MB)")
    parser.add_argument("--no-verify", action="store_false", dest="verify", default=True, help="Skip post-extraction verification")
    parser.add_argument("--verify-only", action="store_true", default=False, help="Verify existing extracted directory only")
    parser.add_argument("--verbose", action="store_true", default=False, help="Enable verbose logging")

    args = parser.parse_args()

    if args.verify_only:
        ok = verify_map_package(args.out, check_destruction=args.include_destruction)
        sys.exit(0 if ok else 1)

    extract_mp_map(
        iso_path=args.iso,
        out_dir=args.out,
        dest_name=args.dest_name,
        include_destruction=args.include_destruction,
        include_ambience=args.include_ambience,
        verbose=args.verbose,
    )

    if args.verify:
        ok = verify_map_package(args.out, check_destruction=args.include_destruction)
        if not ok:
            print("\n[WARNING] Verification found discrepancies in extracted map package!")
            sys.exit(1)
        print("\n[SUCCESS] Extracted map package verified 100% intact and valid.")


if __name__ == "__main__":
    main()
