#!/usr/bin/env python3
"""
Run FastAPI compatibility tests against both frameworks.

Usage:
    python run_tests.py                    # Run all tests
    python run_tests.py --fastapi          # Run only against FastAPI
    python run_tests.py --fasterapi        # Run only against FasterAPI
    python run_tests.py --test crud        # Run only CRUD tests
    python run_tests.py --verbose          # Verbose output
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path
from typing import List, Optional


def run_pytest(
    framework: str,
    test_pattern: Optional[str] = None,
    verbose: bool = False,
    extra_args: Optional[List[str]] = None,
) -> int:
    """Run pytest for a specific framework."""
    env = os.environ.copy()
    env["TEST_FRAMEWORK"] = framework

    cmd = [sys.executable, "-m", "pytest"]

    # Add test path
    test_dir = Path(__file__).parent
    if test_pattern:
        # Find matching test files
        test_files = list(test_dir.glob(f"test_*{test_pattern}*.py"))
        if test_files:
            cmd.extend([str(f) for f in test_files])
        else:
            print(f"No test files matching pattern: {test_pattern}")
            return 1
    else:
        cmd.append(str(test_dir))

    # Add options
    if verbose:
        cmd.append("-v")
    else:
        cmd.append("-q")

    # Add extra args
    if extra_args:
        cmd.extend(extra_args)

    # Don't capture output
    cmd.extend(["--tb=short", "-x"])

    print(f"\n{'=' * 60}")
    print(f"Running tests with {framework.upper()}")
    print(f"{'=' * 60}")
    print(f"Command: {' '.join(cmd)}")
    print()

    result = subprocess.run(cmd, env=env, cwd=str(test_dir))
    return result.returncode


def main():
    parser = argparse.ArgumentParser(description="Run FastAPI compatibility tests")
    parser.add_argument("--fastapi", action="store_true", help="Test only FastAPI")
    parser.add_argument("--fasterapi", action="store_true", help="Test only FasterAPI")
    parser.add_argument(
        "--test", type=str, help="Test pattern to match (e.g., 'crud', 'auth')"
    )
    parser.add_argument("-v", "--verbose", action="store_true", help="Verbose output")
    parser.add_argument(
        "--compare", action="store_true", help="Compare both frameworks"
    )
    parser.add_argument("pytest_args", nargs="*", help="Additional pytest arguments")

    args = parser.parse_args()

    results = {}

    # Determine which frameworks to test
    frameworks = []
    if args.fastapi:
        frameworks.append("fastapi")
    elif args.fasterapi:
        frameworks.append("fasterapi")
    else:
        # Test both by default
        frameworks = ["fastapi", "fasterapi"]

    # Run tests
    for framework in frameworks:
        result = run_pytest(
            framework=framework,
            test_pattern=args.test,
            verbose=args.verbose,
            extra_args=args.pytest_args,
        )
        results[framework] = result

    # Print summary
    print(f"\n{'=' * 60}")
    print("SUMMARY")
    print(f"{'=' * 60}")

    all_passed = True
    for framework, result in results.items():
        status = "PASSED" if result == 0 else "FAILED"
        print(f"{framework:15} {status}")
        if result != 0:
            all_passed = False

    if args.compare and len(results) == 2:
        print()
        if results["fastapi"] == 0 and results["fasterapi"] == 0:
            print("Both frameworks pass all tests - compatibility confirmed!")
        elif results["fastapi"] == 0 and results["fasterapi"] != 0:
            print("FasterAPI has compatibility issues to fix.")
        elif results["fastapi"] != 0 and results["fasterapi"] == 0:
            print("FastAPI tests failed but FasterAPI passed (unexpected).")
        else:
            print("Both frameworks have test failures.")

    return 0 if all_passed else 1


if __name__ == "__main__":
    sys.exit(main())
