#!/usr/bin/env python3
"""
BLACK Multiplayer Unified E2E Test Suite Runner.
Executes and validates Features F1-F10 across Tiers 1-4 (>=115 tests).
Outputs color-coded console summaries and optional structured JSON reports.
"""

import argparse
import io
import json
import os
import sys
import time
import unittest
from pathlib import Path

# Ensure repo root and tests are on path
REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT))

from tests.e2e.test_tier1_feature_coverage import TestTier1FeatureCoverage
from tests.e2e.test_tier2_boundary_corner import TestTier2BoundaryCorner
from tests.e2e.test_tier3_cross_feature import TestTier3CrossFeature
from tests.e2e.test_tier4_real_world import TestTier4RealWorldScenarios
from tests.e2e.test_tier5_native import TestTier5NativeAdversarial


TIER_MAP = {
    1: ("Tier 1: Feature Coverage (F1-F10)", TestTier1FeatureCoverage),
    2: ("Tier 2: Boundary & Corner Cases (F1-F10)", TestTier2BoundaryCorner),
    3: ("Tier 3: Cross-Feature Combinations", TestTier3CrossFeature),
    4: ("Tier 4: Real-World Scenarios", TestTier4RealWorldScenarios),
    5: ("Tier 5: Native C++ Kernel Adversarial Stress", TestTier5NativeAdversarial),
}


class Color:
    RESET = "\033[0m"
    BOLD = "\033[1m"
    RED = "\033[31m"
    GREEN = "\033[32m"
    YELLOW = "\033[33m"
    CYAN = "\033[36m"
    GRAY = "\033[90m"


def list_tests():
    """Lists all available test cases by tier."""
    loader = unittest.TestLoader()
    print(f"\n{Color.BOLD}{Color.CYAN}BLACK Multiplayer E2E Test Catalog{Color.RESET}\n")
    total = 0
    for tier_num, (tier_title, test_class) in TIER_MAP.items():
        suite = loader.loadTestsFromTestCase(test_class)
        tests = list(suite)
        total += len(tests)
        print(f"{Color.BOLD}--- {tier_title} ({len(tests)} tests) ---{Color.RESET}")
        for t in tests:
            doc = (t._testMethodDoc or "").strip()
            print(f"  {Color.GRAY}{t.id().split('.')[-1]}{Color.RESET} - {doc}")
        print()
    print(f"{Color.BOLD}Total Cataloged Tests: {total}{Color.RESET}\n")


def build_filtered_suite(selected_tier=None, selected_feature=None, skip_native=False):
    """Builds a unittest.TestSuite based on tier and feature filters."""
    suite = unittest.TestSuite()
    loader = unittest.TestLoader()

    for tier_num, (tier_title, test_class) in TIER_MAP.items():
        if selected_tier is not None and tier_num != selected_tier:
            continue
        if skip_native and tier_num == 5:
            continue

        test_names = loader.getTestCaseNames(test_class)
        for name in test_names:
            if selected_feature:
                feat_lower = selected_feature.lower()
                if feat_lower not in name.lower():
                    continue
            suite.addTest(test_class(name))

    return suite


def run_tests(args):
    """Executes the test suite and produces console summary and optional JSON report."""
    if args.list:
        list_tests()
        return 0

    if args.native:
        args.tier = 5

    suite = build_filtered_suite(args.tier, args.feature, skip_native=args.skip_native)
    test_count = suite.countTestCases()

    if test_count == 0:
        print(f"{Color.YELLOW}No tests matched criteria (Tier={args.tier}, Feature={args.feature}){Color.RESET}")
        return 0

    print(f"{Color.BOLD}{Color.CYAN}=================================================================={Color.RESET}")
    print(f"{Color.BOLD}{Color.CYAN}       BLACK Multiplayer E2E Automated Test Suite Execution        {Color.RESET}")
    print(f"{Color.BOLD}{Color.CYAN}=================================================================={Color.RESET}")
    if args.tier:
        print(f"Filter: Tier {args.tier} ({TIER_MAP[args.tier][0]})")
    if args.feature:
        print(f"Filter: Feature {args.feature}")
    print(f"Total Test Cases to Execute: {test_count}\n")

    stream = io.StringIO()
    runner = unittest.TextTestRunner(stream=stream, verbosity=2 if args.verbose else 1)

    start_time = time.time()
    result = runner.run(suite)
    duration = time.time() - start_time

    output = stream.getvalue()
    if args.verbose:
        print(output)

    # Detailed tier breakdown
    tier_stats = {}
    for tier_num, (tier_title, test_class) in TIER_MAP.items():
        if args.tier is not None and tier_num != args.tier:
            continue
        t_suite = unittest.TestLoader().loadTestsFromTestCase(test_class)
        t_count = t_suite.countTestCases()
        tier_stats[f"tier_{tier_num}"] = {
            "title": tier_title,
            "count": t_count
        }

    passed = result.testsRun - len(result.failures) - len(result.errors) - len(result.skipped)
    failed = len(result.failures)
    errors = len(result.errors)
    skipped = len(result.skipped)

    print(f"{Color.BOLD}------------------------------------------------------------------{Color.RESET}")
    print(f"{Color.BOLD}Test Execution Summary:{Color.RESET}")
    print(f"  Tests Executed: {result.testsRun}")
    print(f"  Passed:         {Color.GREEN}{passed}{Color.RESET}")
    print(f"  Failed:         {Color.RED if failed else Color.GRAY}{failed}{Color.RESET}")
    print(f"  Errors:         {Color.RED if errors else Color.GRAY}{errors}{Color.RESET}")
    print(f"  Skipped:        {Color.YELLOW if skipped else Color.GRAY}{skipped}{Color.RESET}")
    print(f"  Elapsed Time:   {duration:.3f} seconds")
    print(f"{Color.BOLD}------------------------------------------------------------------{Color.RESET}")

    if result.failures:
        print(f"\n{Color.BOLD}{Color.RED}FAILURES:{Color.RESET}")
        for test, tb in result.failures:
            print(f"[{test.id()}]\n{tb}")

    if result.errors:
        print(f"\n{Color.BOLD}{Color.RED}ERRORS:{Color.RESET}")
        for test, tb in result.errors:
            print(f"[{test.id()}]\n{tb}")

    # JSON Report
    if args.json_report:
        report_data = {
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "total": result.testsRun,
            "passed": passed,
            "failed": failed,
            "errors": errors,
            "skipped": skipped,
            "duration_seconds": round(duration, 4),
            "all_passed": result.wasSuccessful(),
            "tier_breakdown": tier_stats,
            "failures": [{"test": t.id(), "traceback": tb} for t, tb in result.failures],
            "errors": [{"test": t.id(), "traceback": tb} for t, tb in result.errors],
        }
        try:
            report_path = Path(args.json_report)
            report_path.parent.mkdir(parents=True, exist_ok=True)
            report_path.write_text(json.dumps(report_data, indent=2))
            print(f"{Color.CYAN}Structured JSON test report written to: {report_path}{Color.RESET}")
        except Exception as e:
            print(f"{Color.YELLOW}Warning: Could not write JSON report to {args.json_report}: {e}{Color.RESET}")

    if result.wasSuccessful():
        print(f"\n{Color.BOLD}{Color.GREEN}>>> ALL TESTS PASSED SUCCESSFULLY! <<<{Color.RESET}\n")
        return 0
    else:
        print(f"\n{Color.BOLD}{Color.RED}>>> TEST SUITE COMPLETED WITH FAILURES / ERRORS <<<{Color.RESET}\n")
        return 1


def main():
    parser = argparse.ArgumentParser(description="BLACK Multiplayer E2E Test Suite Runner")
    parser.add_argument("--tier", type=int, choices=[1, 2, 3, 4, 5], help="Run only specific test tier (1-5)")
    parser.add_argument("--native", action="store_true", help="Run only native C++ adversarial test suite (Tier 5)")
    parser.add_argument("--skip-native", action="store_true", help="Skip native C++ adversarial tests (run Tiers 1-4 only)")
    parser.add_argument("--feature", type=str, help="Filter tests by feature identifier (e.g. F1, F2, ... F10)")
    parser.add_argument("-v", "--verbose", action="store_true", help="Enable verbose test output")
    parser.add_argument("--json-report", type=str, help="File path to write JSON test report")
    parser.add_argument("--list", action="store_true", help="List all cataloged test cases and exit")

    args = parser.parse_args()
    sys.exit(run_tests(args))


if __name__ == "__main__":
    main()
