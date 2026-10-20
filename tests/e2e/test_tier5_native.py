#!/usr/bin/env python3
"""
Tier 5: Native C++ Kernel & Redirection Adversarial Test Suite.
Executes native binary tests/test_adversarial_redirection compiled against:
- files.cpp.o
- disc.cpp.o
- log.cpp.o
Directly verifies kernel filesystem resolution, ext4 case insensitivity,
clean asset redirection priority, ISO disc fallback, and multi-threaded stress.
"""

import unittest
from tests.e2e.common import (
    ensure_native_test_binary,
    run_native_adversarial_suite,
    resolve_native_path,
    read_native_game_file_size,
)


class TestTier5NativeAdversarial(unittest.TestCase):
    """Tier 5: Empirical Adversarial Testing of Native C++ Kernel Filesystem."""

    @classmethod
    def setUpClass(cls):
        """Ensures native test binary is compiled and fresh before test execution."""
        ensure_native_test_binary()

    def test_f2_t5_native_all_55_adversarial_stress_cases(self):
        """F2-T5: All empirical C++ adversarial tests execute and pass without failure."""
        rc, out = run_native_adversarial_suite()
        self.assertEqual(rc, 0, f"Native adversarial test harness reported failures:\n{out}")

    def test_f2_t5_native_dot_segment_resolution(self):
        """F2-T5: Native C++ kernel collapses '.' and '..' to the very same host file."""
        p_plain = resolve_native_path(r"D:\Levels\Level_00\LevelDat.bin")
        self.assertIsNotNone(p_plain, "Plain path must resolve to clean host LevelDat.bin")
        self.assertTrue("level_mp00/LevelDat.bin" in p_plain or "level_00/LevelDat.bin" in p_plain)

        p_dot = resolve_native_path(r"D:\Levels\.\Level_00\.\LevelDat.bin")
        self.assertIsNotNone(p_dot, "Dot path must resolve to clean host LevelDat.bin")
        self.assertTrue("level_mp00/LevelDat.bin" in p_dot or "level_00/LevelDat.bin" in p_dot)
        self.assertEqual(p_dot, p_plain, "The dot path must reach the very same host file")

        p_dotdot = resolve_native_path(r"D:\Levels\Level_01\..\Level_00\LevelDat.bin")
        self.assertIsNotNone(p_dotdot, "Dot-dot path must resolve to clean host LevelDat.bin")
        self.assertTrue("level_mp00/LevelDat.bin" in p_dotdot or "level_00/LevelDat.bin" in p_dotdot)
        self.assertEqual(p_dotdot, p_plain, "The dot-dot path must reach the very same host file")

    def test_f2_t5_native_directory_read_crash_guard(self):
        """F2-T5: Native ReadGameFile on directory safely returns false without std::__ios_failure crash."""
        ok, sz, err = read_native_game_file_size(r"D:\Levels\Level_00")
        self.assertIsNone(err, f"Directory read caused crash or exception: {err}")
        self.assertFalse(ok, "Directory read must return false cleanly")

    def test_f2_t5_native_clean_speech_bank_isolation(self):
        """F2-T5: The speech bank reached through a dot path is the whole bank the plain path reaches."""
        ok_plain, sz_plain, err = read_native_game_file_size(r"D:\Levels\Level_00\speech.slb")
        self.assertTrue(ok_plain, f"Failed to read speech.slb: {err}")

        ok_dot, sz_dot, err = read_native_game_file_size(r"D:\Levels\.\Level_00\speech.slb")
        self.assertTrue(ok_dot, f"Failed to read speech.slb through the dot path: {err}")

        # 104 is the offset of the speech resource table (read at 0x5C), not the size of the
        # bank: the clean map keeps the retail bank, because the level loader indexes those
        # tables and a truncated one breaks the level.
        self.assertGreater(sz_dot, 104, f"Truncated speech bank: {sz_dot} bytes")
        self.assertEqual(sz_dot, sz_plain, "The dot path must reach the very same speech bank")


if __name__ == "__main__":
    unittest.main()
