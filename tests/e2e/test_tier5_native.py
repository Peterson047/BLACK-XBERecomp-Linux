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
        """F2-T5: All 55 empirical C++ adversarial tests execute and pass without failure."""
        rc, out = run_native_adversarial_suite()
        self.assertEqual(rc, 0, f"Native adversarial test harness reported failures:\n{out}")

    def test_f2_t5_native_dot_segment_resolution(self):
        """F2-T5: Native C++ kernel collapses '.' and '..' without leaking retail ISO data."""
        p_dot = resolve_native_path(r"D:\Levels\.\Level_00\.\LevelDat.bin")
        self.assertIsNotNone(p_dot, "Dot path must resolve to clean host LevelDat.bin")
        self.assertTrue("level_mp00/LevelDat.bin" in p_dot or "level_00/LevelDat.bin" in p_dot)

        p_dotdot = resolve_native_path(r"D:\Levels\Level_01\..\Level_00\LevelDat.bin")
        self.assertIsNotNone(p_dotdot, "Dot-dot path must resolve to clean host LevelDat.bin")
        self.assertTrue("level_mp00/LevelDat.bin" in p_dotdot or "level_00/LevelDat.bin" in p_dotdot)

    def test_f2_t5_native_directory_read_crash_guard(self):
        """F2-T5: Native ReadGameFile on directory safely returns false without std::__ios_failure crash."""
        ok, sz, err = read_native_game_file_size(r"D:\Levels\Level_00")
        self.assertIsNone(err, f"Directory read caused crash or exception: {err}")
        self.assertFalse(ok, "Directory read must return false cleanly")

    def test_f2_t5_native_clean_speech_bank_isolation(self):
        """F2-T5: Querying speech.slb through dot path returns 104-byte clean dummy file."""
        ok, sz, err = read_native_game_file_size(r"D:\Levels\.\Level_00\speech.slb")
        self.assertTrue(ok, f"Failed to read speech.slb: {err}")
        self.assertEqual(sz, 104, f"Clean dummy speech bank bypassed! Read {sz} bytes (retail ISO speech.slb is 6944 bytes)")


if __name__ == "__main__":
    unittest.main()
