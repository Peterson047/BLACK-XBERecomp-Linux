#!/usr/bin/env python3
"""
test_adversarial_m1.py - Adversarial Stress & Empirical Verification Suite for M1.

Empirically challenges tools/extract_mp_map.py and verifies binary integrity
of assets/levels/level_mp00/.
Covers:
1. Corrupt/truncated ISO handling (zero-byte, partial header, truncated payload, garbage data)
2. Output directory resilience (non-existent, deeply nested, Unicode/spaces, regular file collision)
3. Re-extraction idempotency & in-place corruption recovery
4. CLI parameter validation (--verify-only, --include-ambience, --no-destruction, --dest-name, invalid args)
5. Deep binary integrity (Unit_01.bin item offsets, Collide.awd GUID & TOC, LevelDat.bin, StLevel.bin,
   StUnit01.bin GAME_RIG, guns.bin LODs, speech.slb resource tables, fpguns pairs, campaign exclusion).
"""

import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
EXTRACTOR_PATH = REPO_ROOT / "tools" / "extract_mp_map.py"
ISO_PATH = REPO_ROOT / "assets" / "Black.iso"
MP00_DIR = REPO_ROOT / "assets" / "levels" / "level_mp00"


def run_extractor(*args, cwd=REPO_ROOT):
    """Executes extract_mp_map.py with given arguments and returns (returncode, stdout, stderr)."""
    cmd = [sys.executable, str(EXTRACTOR_PATH)] + list(args)
    proc = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    return proc.returncode, proc.stdout, proc.stderr


class TestAdversarialIsoHandling(unittest.TestCase):
    """Stress-tests Xbox ISO parsing against truncated, non-existent, and corrupt ISO files."""

    def test_adv_iso_nonexistent_path(self):
        """Rejects non-existent ISO file path with non-zero exit code and error message."""
        rc, out, err = run_extractor("--iso", "/nonexistent/path/Black_fake.iso", "--out", "/tmp/adv_m1_out")
        self.assertNotEqual(rc, 0, "Extractor must exit with error on non-existent ISO")
        combined = out + err
        self.assertTrue("ISO file not found" in combined or "FileNotFoundError" in combined)

    def test_adv_iso_zero_bytes(self):
        """Rejects 0-byte ISO file cleanly without hanging or uncaught memory fault."""
        with tempfile.NamedTemporaryFile(suffix=".iso") as tf:
            rc, out, err = run_extractor("--iso", tf.name, "--out", "/tmp/adv_m1_out")
            self.assertNotEqual(rc, 0, "Extractor must fail on 0-byte ISO")
            self.assertIn("No valid Xbox Media volume descriptor found", out + err)

    def test_adv_iso_random_noise_small(self):
        """Rejects 4 KB random byte stream without valid XDVDFS magic."""
        with tempfile.NamedTemporaryFile(suffix=".iso") as tf:
            tf.write(os.urandom(4096))
            tf.flush()
            rc, out, err = run_extractor("--iso", tf.name, "--out", "/tmp/adv_m1_out")
            self.assertNotEqual(rc, 0)
            self.assertIn("No valid Xbox Media volume descriptor found", out + err)

    def test_adv_iso_random_noise_large(self):
        """Rejects 1 MB random byte stream without valid XDVDFS magic."""
        with tempfile.NamedTemporaryFile(suffix=".iso") as tf:
            tf.write(os.urandom(1024 * 1024))
            tf.flush()
            rc, out, err = run_extractor("--iso", tf.name, "--out", "/tmp/adv_m1_out")
            self.assertNotEqual(rc, 0)
            self.assertIn("No valid Xbox Media volume descriptor found", out + err)

    def test_adv_iso_truncated_at_volume_descriptor(self):
        """Rejects ISO truncated immediately after volume descriptor without directory tables."""
        if not ISO_PATH.is_file():
            self.skipTest(f"Black.iso not found at {ISO_PATH}")
        with open(ISO_PATH, "rb") as f:
            head = f.read(0x10000 + 4096)  # sector 32 + 2 sectors
        with tempfile.NamedTemporaryFile(suffix=".iso") as tf:
            tf.write(head)
            tf.flush()
            with tempfile.TemporaryDirectory() as tmpout:
                rc, out, err = run_extractor("--iso", tf.name, "--out", tmpout)
                self.assertNotEqual(rc, 0, "Extractor must fail when directory table is missing")
                self.assertIn("[FAIL] Missing file", out)


class TestAdversarialOutputDirectory(unittest.TestCase):
    """Stress-tests output directory creation, permission issues, and path representations."""

    def test_adv_outdir_deeply_nested_creation(self):
        """Creates deeply nested non-existent directory tree on the fly."""
        if not ISO_PATH.is_file():
            self.skipTest(f"Black.iso not found at {ISO_PATH}")
        with tempfile.TemporaryDirectory() as tmpdir:
            nested_out = Path(tmpdir) / "sub1" / "sub2" / "sub3" / "level_mp00"
            self.assertFalse(nested_out.exists())
            rc, out, err = run_extractor("--iso", str(ISO_PATH), "--out", str(nested_out))
            self.assertEqual(rc, 0, f"Extraction failed with nested dir: {err}")
            self.assertTrue((nested_out / "Unit_01.bin").is_file())
            self.assertTrue((nested_out / "speech.slb").is_file())

    def test_adv_outdir_unicode_spaces(self):
        """Handles directory paths with spaces and Unicode accents safely."""
        if not ISO_PATH.is_file():
            self.skipTest(f"Black.iso not found at {ISO_PATH}")
        with tempfile.TemporaryDirectory() as tmpdir:
            unicode_out = Path(tmpdir) / "Arena Veblensk - Nível 0 (Multiplayer) áéíóú" / "level_mp00"
            rc, out, err = run_extractor("--iso", str(ISO_PATH), "--out", str(unicode_out))
            self.assertEqual(rc, 0, f"Extraction failed with Unicode dir: {err}")
            self.assertTrue((unicode_out / "Unit_01.bin").is_file())

    def test_adv_outdir_is_regular_file(self):
        """Aborts cleanly with non-zero exit code when output target is an existing regular file."""
        if not ISO_PATH.is_file():
            self.skipTest(f"Black.iso not found at {ISO_PATH}")
        with tempfile.NamedTemporaryFile() as tf:
            rc, out, err = run_extractor("--iso", str(ISO_PATH), "--out", tf.name)
            self.assertNotEqual(rc, 0, "Must reject file path as directory target")


class TestAdversarialIdempotencyAndRepair(unittest.TestCase):
    """Tests idempotency of repeated extractions and in-place recovery of corrupted files."""

    def test_adv_idempotency_clean_rerun(self):
        """Re-extracting over an existing valid map directory produces 100% identical file hashes."""
        if not ISO_PATH.is_file():
            self.skipTest(f"Black.iso not found at {ISO_PATH}")

        def hash_dir(d: Path):
            hashes = {}
            for p in d.rglob("*"):
                if p.is_file():
                    hashes[str(p.relative_to(d))] = hashlib.sha256(p.read_bytes()).hexdigest()
            return hashes

        with tempfile.TemporaryDirectory() as tmpdir:
            out_path = Path(tmpdir) / "level_mp00"
            rc1, _, _ = run_extractor("--iso", str(ISO_PATH), "--out", str(out_path))
            self.assertEqual(rc1, 0)
            h1 = hash_dir(out_path)
            self.assertEqual(len(h1), 45)

            # Second execution over existing directory
            rc2, _, _ = run_extractor("--iso", str(ISO_PATH), "--out", str(out_path))
            self.assertEqual(rc2, 0)
            h2 = hash_dir(out_path)

            self.assertEqual(h1, h2, "File hashes must be bitwise identical after re-extraction")

    def test_adv_idempotency_damaged_file_recovery(self):
        """Re-running extraction over a damaged directory repairs all files back to 100% integrity."""
        if not ISO_PATH.is_file():
            self.skipTest(f"Black.iso not found at {ISO_PATH}")

        with tempfile.TemporaryDirectory() as tmpdir:
            out_path = Path(tmpdir) / "level_mp00"
            run_extractor("--iso", str(ISO_PATH), "--out", str(out_path))

            # Intentionally corrupt Unit_01.bin and speech.slb
            (out_path / "Unit_01.bin").write_bytes(b"\x00" * 128)
            (out_path / "speech.slb").write_bytes(b"INVALID_MAGIC_HEADER")

            # Verify-only should detect damage
            rc_bad, _, _ = run_extractor("--verify-only", "--out", str(out_path))
            self.assertNotEqual(rc_bad, 0, "Verify-only must detect corrupt files")

            # Repair via re-extraction
            rc_repair, _, _ = run_extractor("--iso", str(ISO_PATH), "--out", str(out_path))
            self.assertEqual(rc_repair, 0)

            # Verify-only should now pass cleanly
            rc_good, _, _ = run_extractor("--verify-only", "--out", str(out_path))
            self.assertEqual(rc_good, 0, "Map package must be repaired to valid state")


class TestAdversarialCliParameters(unittest.TestCase):
    """Stress-tests command line parameters, flags, and options."""

    def test_adv_cli_verify_only_pass(self):
        """--verify-only succeeds with exit code 0 on assets/levels/level_mp00."""
        self.assertTrue(MP00_DIR.is_dir(), f"level_mp00 directory not found at {MP00_DIR}")
        rc, out, err = run_extractor("--verify-only", "--out", str(MP00_DIR))
        self.assertEqual(rc, 0, f"verify-only failed on valid assets: {out}")
        self.assertIn("[PASS] Unit_01.bin", out)
        # the speech bank is the retail one: the loader indexes its resource tables, so the
        # extractor refuses a package without them
        match = re.search(r"\[PASS\] speech\.slb retains (\d+) resource entries", out)
        self.assertIsNotNone(match, f"the speech bank was not verified: {out}")
        self.assertGreater(int(match.group(1)), 0, "the speech bank has to keep its resource entries")
        self.assertIn("[PASS] fpguns/ contains 16 .db and 16 .wdd weapon files", out)

    def test_adv_cli_verify_only_fail_missing_dir(self):
        """--verify-only returns exit code 1 when target directory does not exist."""
        rc, out, err = run_extractor("--verify-only", "--out", "/tmp/does_not_exist_at_all_adv")
        self.assertNotEqual(rc, 0)

    def test_adv_cli_invalid_flags(self):
        """Rejects unknown CLI argument with argparse error exit code 2."""
        rc, out, err = run_extractor("--unknown-flag-adversarial-test")
        self.assertEqual(rc, 2)
        self.assertIn("unrecognized arguments", err)

    def test_adv_cli_include_ambience(self):
        """--include-ambience extracts Ambience.bks (19.6 MB) and Ambience.ssh (72 bytes)."""
        if not ISO_PATH.is_file():
            self.skipTest(f"Black.iso not found at {ISO_PATH}")
        with tempfile.TemporaryDirectory() as tmpdir:
            out_path = Path(tmpdir) / "level_mp00"
            rc, out, err = run_extractor("--iso", str(ISO_PATH), "--out", str(out_path), "--include-ambience")
            self.assertEqual(rc, 0)
            amb_bks = out_path / "Ambience.bks"
            amb_ssh = out_path / "Ambience.ssh"
            self.assertTrue(amb_bks.is_file(), "Ambience.bks must be extracted")
            self.assertTrue(amb_ssh.is_file(), "Ambience.ssh must be extracted")
            self.assertEqual(amb_bks.stat().st_size, 19628032)
            self.assertEqual(amb_ssh.stat().st_size, 72)

    def test_adv_cli_no_destruction(self):
        """--no-destruction omits Destruct.bks and passes verification with --no-destruction."""
        if not ISO_PATH.is_file():
            self.skipTest(f"Black.iso not found at {ISO_PATH}")
        with tempfile.TemporaryDirectory() as tmpdir:
            out_path = Path(tmpdir) / "level_mp00"
            rc, out, err = run_extractor("--iso", str(ISO_PATH), "--out", str(out_path), "--no-destruction")
            self.assertEqual(rc, 0)
            self.assertFalse((out_path / "Destruct.bks").exists())
            self.assertFalse((out_path / "Destruct.ssh").exists())

            # Verification with --no-destruction should pass
            rc_v, _, _ = run_extractor("--verify-only", "--out", str(out_path), "--no-destruction")
            self.assertEqual(rc_v, 0)

    def test_adv_cli_custom_dest_name(self):
        """--dest-name is accepted and the package keeps the retail speech bank."""
        if not ISO_PATH.is_file():
            self.skipTest(f"Black.iso not found at {ISO_PATH}")
        with tempfile.TemporaryDirectory() as tmpdir:
            out_path = Path(tmpdir) / "custom_arena"
            rc, out, err = run_extractor("--iso", str(ISO_PATH), "--out", str(out_path), "--dest-name", "custom_arena")
            self.assertEqual(rc, 0)
            slb_bytes = (out_path / "speech.slb").read_bytes()
            # the bank is the retail one, copied as it is: it names the level it was taken
            # from, and the resource table it holds has to point inside the file
            prefix = slb_bytes[0x10:0x50].split(b"\x00")[0].decode("ascii")
            self.assertEqual(prefix, "levels/level_00/speech")
            table, = struct.unpack_from("<I", slb_bytes, 0x5C)
            self.assertTrue(104 <= table < len(slb_bytes), "the resource table has to be inside the bank")


class TestAdversarialExtractedAssetsIntegrity(unittest.TestCase):
    """Validates low-level binary layout, magic bytes, pointer tables, and structures in assets/levels/level_mp00/."""

    @classmethod
    def setUpClass(cls):
        if not MP00_DIR.is_dir():
            raise unittest.SkipTest(f"Target directory {MP00_DIR} does not exist")

    def test_adv_binary_unit01_deep_structure(self):
        """Unit_01.bin: magic 0x18, subtables, 31 mesh items * 48 bytes spanning 0xA0..0x670."""
        u1_path = MP00_DIR / "Unit_01.bin"
        self.assertTrue(u1_path.is_file())
        self.assertEqual(u1_path.stat().st_size, 10526464)

        with open(u1_path, "rb") as f:
            hdr = f.read(0xA0)
            magic, p_geom, p_tex, p_mat, u1, u2, u3, off_items, u4, off_items_end = struct.unpack_from("<10I", hdr, 0)
            item_count, = struct.unpack_from("<H", hdr, 0x90)

            self.assertEqual(magic, 0x18)
            self.assertEqual(p_geom, 0x00668900)
            self.assertEqual(p_tex, 0x0070E9B0)
            self.assertEqual(p_mat, 0x0080C8D0)
            self.assertEqual(off_items, 0xA0)
            self.assertEqual(off_items_end, 0x670)
            self.assertEqual(item_count, 31)
            self.assertEqual(off_items_end - off_items, item_count * 48)

    def test_adv_binary_collide_awd_structure(self):
        """Collide.awd: magic 0x809, hdr_sz 0x2C, Criterion AWD GUID, TOC base 0xB8."""
        col_path = MP00_DIR / "Collide.awd"
        self.assertTrue(col_path.is_file())
        self.assertEqual(col_path.stat().st_size, 2973696)

        with open(col_path, "rb") as f:
            hdr = f.read(0x40)
            magic, flags, align, hdr_sz, pad, payload_sz = struct.unpack_from("<6I", hdr, 0)
            g0, g1, g2, g3 = struct.unpack_from("<4I", hdr, 0x18)
            toc_off, toc_base = struct.unpack_from("<II", hdr, 0x38)

            self.assertEqual(magic, 0x809)
            self.assertEqual(flags, 0)
            self.assertEqual(align, 0x3000)
            self.assertEqual(hdr_sz, 0x2C)
            self.assertEqual(payload_sz, 0x002D2C02)
            self.assertEqual((g0, g1, g2, g3), (0x453A2D04, 0x4BC8E45F, 0x75DFF081, 0x73F2018B))
            self.assertEqual(toc_off, 0x2B58)
            self.assertEqual(toc_base, 0xB8)

    def test_adv_binary_level_awd_structure(self):
        """Level.awd: valid AWD container header (magic 0x809, hdr_sz 0x2C)."""
        lvl_path = MP00_DIR / "Level.awd"
        self.assertTrue(lvl_path.is_file())
        self.assertEqual(lvl_path.stat().st_size, 968704)
        with open(lvl_path, "rb") as f:
            magic, _, _, hdr_sz = struct.unpack_from("<4I", f.read(16), 0)
            self.assertEqual(magic, 0x809)
            self.assertEqual(hdr_sz, 0x2C)

    def test_adv_binary_leveldat_structure(self):
        """LevelDat.bin: magic 0x11, 11 subtables, primary subtable offset 0x4E20C."""
        ldat_path = MP00_DIR / "LevelDat.bin"
        self.assertTrue(ldat_path.is_file())
        self.assertEqual(ldat_path.stat().st_size, 711088)
        with open(ldat_path, "rb") as f:
            magic, subtable_count, p_sub_off = struct.unpack_from("<3I", f.read(12), 0)
            self.assertEqual(magic, 0x11)
            self.assertEqual(subtable_count, 11)
            self.assertEqual(p_sub_off, 0x4E20C)

    def test_adv_binary_stlevel_structure(self):
        """stg_0001/StLevel.bin: magic 0xA, 7 weapon templates in Sec 1, contains bc1_* models."""
        stg_path = MP00_DIR / "stg_0001" / "StLevel.bin"
        self.assertTrue(stg_path.is_file())
        self.assertEqual(stg_path.stat().st_size, 2378464)
        with open(stg_path, "rb") as f:
            magic, sec1_off, sec1_count, sec2_off, sec3_off = struct.unpack_from("<5I", f.read(20), 0)
            self.assertEqual(magic, 0xA)
            self.assertEqual(sec1_off, 0x80)
            self.assertEqual(sec1_count, 7)
            self.assertEqual(sec2_off, 0xA00)
            self.assertEqual(sec3_off, 0x242F80)
            f.seek(0)
            data = f.read()
            self.assertIn(b"bc1_so1_mil", data)
            self.assertIn(b"bc1_lr1_mil", data)
            self.assertIn(b"bc1_asr_goggles", data)
            self.assertIn(b"bg1_ak1", data)

    def test_adv_binary_stunit01_structure(self):
        """stg_0001/StUnit01.bin: magic 0x3, contains GAME_RIG bone hierarchy & bc1_rg1_mil."""
        unit_path = MP00_DIR / "stg_0001" / "StUnit01.bin"
        self.assertTrue(unit_path.is_file())
        self.assertEqual(unit_path.stat().st_size, 265504)
        with open(unit_path, "rb") as f:
            magic, = struct.unpack_from("<I", f.read(4), 0)
            self.assertEqual(magic, 3)
            f.seek(0)
            data = f.read()
            for bone in [b"GAME_RIG", b"Root", b"LowerSpine", b"UpperSpine", b"Head"]:
                self.assertIn(bone, data)
            self.assertIn(b"bc1_rg1_mil", data)

    def test_adv_binary_guns_bin_structure(self):
        """stg_0001/guns.bin: magic 0x23000, guns_s.bin: magic 0x23300."""
        guns_path = MP00_DIR / "stg_0001" / "guns.bin"
        guns_s_path = MP00_DIR / "stg_0001" / "guns_s.bin"
        self.assertTrue(guns_path.is_file())
        self.assertTrue(guns_s_path.is_file())

        with open(guns_path, "rb") as f:
            magic, = struct.unpack_from("<I", f.read(4), 0)
            self.assertEqual(magic, 0x00023000)
            f.seek(0)
            data = f.read()
            self.assertIn(b"bg1_ak1_lod", data)
            self.assertIn(b"bg1_asr_LOD", data)

        with open(guns_s_path, "rb") as f:
            magic_s, = struct.unpack_from("<I", f.read(4), 0)
            self.assertEqual(magic_s, 0x00023300)

    def test_adv_binary_stage_awds(self):
        """stg_0001/AIWpns.awd & Emitter.awd: AWD containers with magic 0x809, hdr_sz 0x2C."""
        for name, expected_sz in [("AIWpns.awd", 606208), ("Emitter.awd", 4096)]:
            p = MP00_DIR / "stg_0001" / name
            self.assertTrue(p.is_file(), f"Missing {name}")
            self.assertEqual(p.stat().st_size, expected_sz)
            with open(p, "rb") as f:
                magic, _, _, hdr_sz = struct.unpack_from("<4I", f.read(16), 0)
                self.assertEqual(magic, 0x809)
                self.assertEqual(hdr_sz, 0x2C)

    def test_adv_binary_speech_slb_retail_bank(self):
        """speech.slb: the retail bank, magic KING, version 1, its resource tables inside the file.

        The extractor keeps the retail speech.slb on purpose: the level loader indexes the
        resource tables, so a bank without them breaks the level. 104 is the size of the
        bank header, the offset the resource table is read at, NOT the size of the file.
        """
        slb_path = MP00_DIR / "speech.slb"
        self.assertTrue(slb_path.is_file())
        data = slb_path.read_bytes()
        self.assertGreater(len(data), 104, "The bank is not a truncated header")

        ver, magic = struct.unpack_from("<I4s", data, 0)
        self.assertEqual(ver, 1)
        self.assertEqual(magic, b"KING")

        prefix = data[0x10:0x50].split(b"\x00")[0].decode("ascii")
        self.assertEqual(prefix, "levels/level_00/speech")

        c50, cues, off58, off5c, hdr_sz, res = struct.unpack_from("<6I", data, 0x50)
        self.assertGreater(cues, 0, "The bank has to keep its cues")
        self.assertEqual(hdr_sz, 104, "Header size must match the offset of the resource table")
        self.assertTrue(104 <= off5c < len(data), "The resource table has to point inside the bank")

    def test_adv_binary_fpguns_weapons_catalogue(self):
        """fpguns/ contains exactly 16 .db and 16 .wdd weapon files, all non-empty."""
        fpguns_dir = MP00_DIR / "fpguns"
        self.assertTrue(fpguns_dir.is_dir())
        files = list(fpguns_dir.iterdir())
        dbs = [f for f in files if f.suffix == ".db"]
        wdds = [f for f in files if f.suffix == ".wdd"]
        self.assertEqual(len(dbs), 16)
        self.assertEqual(len(wdds), 16)
        self.assertEqual(len(files), 32)

        expected_weapons = [
            "bg1_ak1", "bg1_ak5", "bg1_as5", "bg1_asr",
            "bg1_bns", "bg1_hv5", "bg1_hvy", "bg1_ps5",
            "bg1_pst", "bg1_rpg", "bg1_sh5", "bg1_shg",
            "bg1_sm5", "bg1_smg", "bg1_sn5", "bg1_snr"
        ]
        db_stems = {f.stem for f in dbs}
        wdd_stems = {f.stem for f in wdds}
        for wp in expected_weapons:
            self.assertIn(wp, db_stems)
            self.assertIn(wp, wdd_stems)
        for f in files:
            self.assertGreater(f.stat().st_size, 0)

    def test_adv_campaign_artifacts_completely_stripped(self):
        """Guarantees 0 .xmv videos and 0 spch_* voice banks in level_mp00."""
        forbidden = []
        for p in MP00_DIR.rglob("*"):
            if p.is_file():
                if p.suffix.lower() == ".xmv" or p.name.lower().startswith("spch_"):
                    forbidden.append(str(p.relative_to(MP00_DIR)))
        self.assertEqual(len(forbidden), 0, f"Found forbidden singleplayer files: {forbidden}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
