#!/usr/bin/env python3.13
"""
Quick Start Script for FasterAPI Test Harness

This script provides an easy way to run the test harness with common scenarios.
It automatically detects available test files and runs appropriate tests.
"""

import sys
import os
from pathlib import Path

# Add test_harness to path
sys.path.insert(0, str(Path(__file__).parent / "test_harness"))

from test_harness.harness import TestHarness
from test_harness.reporting import ReportGenerator


def find_python_app():
    """Find a suitable Python app file."""
    candidates = [
        "examples/basic_app.py",
        "examples/minimal_test.py",
        "examples/simple_test.py",
        "examples/fastapi_example.py"
    ]

    for candidate in candidates:
        if Path(candidate).exists():
            return candidate

    return None


def find_cpp_binary():
    """Find a suitable C++ binary."""
    candidates = [
        "build/test_server",
        "build/test_http_server",
        "test_simple_server",
        "test_http1_simple"
    ]

    for candidate in candidates:
        if Path(candidate).exists():
            return candidate

    return None


def main():
    """Main entry point."""

    print("="*80)
    print("FasterAPI Test Harness - Quick Start")
    print("="*80)

    harness = TestHarness()
    reporter = ReportGenerator()

    # Check available components
    python_app = find_python_app()
    cpp_binary = find_cpp_binary()

    print("
Detected components:")
    print(f"  Python app: {python_app or 'Not found'}")
    print(f"  C++ binary: {cpp_binary or 'Not found'}")

    if not python_app and not cpp_binary:
        print("\n❌ No test components found!")
        print("Please ensure you have either:")
        print("  - A Python app in examples/")
        print("  - A built C++ binary in build/")
        return 1

    # List available test suites
    print("
Available test suites:")
    for name, suite in harness.config.test_suites.items():
        print(f"  {name}: {len(suite.endpoints)} endpoints")

    results = []

    # Test Python API if available
    if python_app:
        print(f"\n🚀 Testing Python API ({python_app})")

        for suite_name in ["health", "crud"]:
            if suite_name in harness.config.test_suites:
                try:
                    print(f"  Running {suite_name} tests...")
                    result = harness.run_tests("python", suite_name, app_file=python_app)
                    results.append(result)
                    print(f"  ✅ {result.passed_tests}/{result.total_tests} passed")
                except Exception as e:
                    print(f"  ❌ {suite_name} failed: {e}")

    # Test C++ API if available
    if cpp_binary:
        print(f"\n🚀 Testing C++ API ({cpp_binary})")

        for suite_name in ["health"]:
            if suite_name in harness.config.test_suites:
                try:
                    print(f"  Running {suite_name} tests...")
                    result = harness.run_tests("cpp", suite_name, binary_path=cpp_binary)
                    results.append(result)
                    print(f"  ✅ {result.passed_tests}/{result.total_tests} passed")
                except Exception as e:
                    print(f"  ❌ {suite_name} failed: {e}")

    # Run benchmarks if both are available
    if python_app and cpp_binary:
        print("
🏁 Running performance comparison...")

        try:
            comparison = harness.compare_apis(
                test_suite_name="health",
                python_app=python_app,
                cpp_binary=cpp_binary
            )

            print("
📊 Comparison Results:"            print("Tests:")
            py = comparison['python_tests']
            cpp = comparison['cpp_tests']
            print(f"  Python: {py['passed']}/{py['total']} ({py['time']:.2f}s)")
            print(f"  C++:    {cpp['passed']}/{cpp['total']} ({cpp['time']:.2f}s)")

            print("Benchmarks (at concurrency=50):")
            for pb in comparison['python_benchmarks']:
                if pb['concurrency'] == 50:
                    print(f"  Python {pb['endpoint']}: {pb['rps']:.0f} RPS")
            for cb in comparison['cpp_benchmarks']:
                if cb['concurrency'] == 50:
                    print(f"  C++    {cb['endpoint']}: {cb['rps']:.0f} RPS")

        except Exception as e:
            print(f"❌ Comparison failed: {e}")

    # Generate summary report
    if results:
        print("
📋 Generating summary report...")
        try:
            report_file = reporter.generate_test_report(results)
            reporter.print_test_summary(results)
            print(f"\nDetailed report saved to: {report_file}")
        except Exception as e:
            print(f"❌ Report generation failed: {e}")

    print("
" + "="*80)
    print("Test harness run complete!")
    print("="*80)

    # Check if any tests failed
    failed_tests = sum(r.failed_tests for r in results)
    return 1 if failed_tests > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
