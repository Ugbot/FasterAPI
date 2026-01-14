#!/usr/bin/env python3
"""
Autobahn WebSocket Testsuite Runner for FasterAPI

This script:
1. Builds the C++ WebSocket echo server
2. Starts the server
3. Runs the Autobahn fuzzing client testsuite
4. Parses and reports results
5. Exits with appropriate code for CI integration

Requirements:
    pip install autobahntestsuite

Usage:
    python tests/autobahn/run_autobahn_tests.py [--quick] [--cases CASES]

Options:
    --quick     Run only a subset of tests (faster, for development)
    --cases     Comma-separated list of case patterns (e.g., "1.*,2.*")
    --port      Server port (default: 9001)
    --timeout   Test timeout in seconds (default: 300)
"""

import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional


# Colors for terminal output
class Colors:
    GREEN = "\033[92m"
    RED = "\033[91m"
    YELLOW = "\033[93m"
    BLUE = "\033[94m"
    BOLD = "\033[1m"
    END = "\033[0m"


def colored(text: str, color: str) -> str:
    """Apply color if stdout is a terminal."""
    if sys.stdout.isatty():
        return f"{color}{text}{Colors.END}"
    return text


class AutobahnTestRunner:
    """Runs the Autobahn WebSocket testsuite against FasterAPI."""

    def __init__(
        self,
        port: int = 9001,
        timeout: int = 300,
        quick: bool = False,
        cases: Optional[str] = None,
    ):
        self.port = port
        self.timeout = timeout
        self.quick = quick
        self.cases = cases
        self.server_process: Optional[subprocess.Popen] = None

        # Paths
        self.project_root = Path(__file__).parent.parent.parent
        self.autobahn_dir = Path(__file__).parent
        self.reports_dir = self.autobahn_dir / "reports"
        self.config_file = self.autobahn_dir / "fuzzingclient.json"

    def build_server(self) -> bool:
        """Build the WebSocket echo server."""
        print(colored("Building WebSocket echo server...", Colors.BLUE))

        result = subprocess.run(
            ["cmake", "--build", "build", "--target", "websocket_echo_server"],
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )

        if result.returncode != 0:
            print(colored("Build failed!", Colors.RED))
            print(result.stderr)
            return False

        print(colored("Build successful", Colors.GREEN))
        return True

    def start_server(self) -> bool:
        """Start the WebSocket echo server."""
        print(colored(f"Starting echo server on port {self.port}...", Colors.BLUE))

        server_path = self.project_root / "build" / "tests" / "websocket_echo_server"
        if not server_path.exists():
            print(colored(f"Server binary not found: {server_path}", Colors.RED))
            return False

        env = os.environ.copy()
        env["DYLD_LIBRARY_PATH"] = str(self.project_root / "build" / "lib")
        env["LD_LIBRARY_PATH"] = str(self.project_root / "build" / "lib")

        self.server_process = subprocess.Popen(
            [str(server_path), str(self.port)],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=self.project_root,
        )

        # Wait for server to start
        for _ in range(30):
            try:
                import socket

                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(0.5)
                sock.connect(("127.0.0.1", self.port))
                sock.close()
                print(colored("Server started", Colors.GREEN))
                return True
            except (socket.error, socket.timeout):
                time.sleep(0.2)

        print(colored("Server failed to start!", Colors.RED))
        return False

    def stop_server(self):
        """Stop the WebSocket echo server."""
        if self.server_process:
            print(colored("Stopping server...", Colors.BLUE))
            self.server_process.terminate()
            try:
                self.server_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.server_process.kill()
            self.server_process = None

    def prepare_config(self):
        """Prepare the Autobahn config file."""
        config = {
            "outdir": str(self.reports_dir),
            "servers": [{"agent": "FasterAPI-WebSocket", "url": f"ws://127.0.0.1:{self.port}"}],
            "cases": ["*"],
            "exclude-cases": [],
            "exclude-agent-cases": {},
        }

        if self.quick:
            # Quick mode: only basic tests
            config["cases"] = [
                "1.*",  # Text messages
                "2.*",  # Binary messages
                "3.*",  # Fragmentation
                "5.*",  # Ping/Pong
                "6.1.*",  # UTF-8 basics
                "7.1.*",  # Close basics
            ]
        elif self.cases:
            config["cases"] = self.cases.split(",")

        # Write config
        with open(self.config_file, "w") as f:
            json.dump(config, f, indent=2)

        # Create reports directory
        self.reports_dir.mkdir(exist_ok=True)

    def run_testsuite(self) -> bool:
        """Run the Autobahn fuzzing client testsuite."""
        print(colored("Running Autobahn testsuite...", Colors.BLUE))

        # Check if wstest is available
        if not shutil.which("wstest"):
            print(colored("wstest not found! Install with: pip install autobahntestsuite", Colors.RED))
            return False

        result = subprocess.run(
            ["wstest", "-m", "fuzzingclient", "-s", str(self.config_file)],
            cwd=self.autobahn_dir,
            timeout=self.timeout,
        )

        return result.returncode == 0

    def parse_results(self) -> dict:
        """Parse the Autobahn test results."""
        results = {
            "total": 0,
            "passed": 0,
            "failed": 0,
            "non_strict": 0,
            "unimplemented": 0,
            "informational": 0,
            "failures": [],
        }

        # Find the index file
        index_file = self.reports_dir / "index.json"
        if not index_file.exists():
            print(colored("No results found!", Colors.YELLOW))
            return results

        with open(index_file) as f:
            data = json.load(f)

        agent_key = "FasterAPI-WebSocket"
        if agent_key not in data:
            # Try to find any agent
            agents = list(data.keys())
            if agents:
                agent_key = agents[0]
            else:
                return results

        agent_results = data[agent_key]

        for case_id, result in agent_results.items():
            results["total"] += 1
            behavior = result.get("behavior", "FAILED")

            if behavior == "OK":
                results["passed"] += 1
            elif behavior == "NON-STRICT":
                results["non_strict"] += 1
            elif behavior == "UNIMPLEMENTED":
                results["unimplemented"] += 1
            elif behavior == "INFORMATIONAL":
                results["informational"] += 1
            else:
                results["failed"] += 1
                results["failures"].append(
                    {"case": case_id, "behavior": behavior, "message": result.get("behaviorClose", "")}
                )

        return results

    def print_results(self, results: dict):
        """Print formatted test results."""
        print()
        print(colored("=" * 60, Colors.BOLD))
        print(colored("AUTOBAHN TESTSUITE RESULTS", Colors.BOLD))
        print(colored("=" * 60, Colors.BOLD))
        print()

        total = results["total"]
        passed = results["passed"]
        failed = results["failed"]
        non_strict = results["non_strict"]
        unimpl = results["unimplemented"]
        info = results["informational"]

        print(f"Total tests:     {total}")
        print(colored(f"Passed:          {passed}", Colors.GREEN))

        if non_strict > 0:
            print(colored(f"Non-strict:      {non_strict}", Colors.YELLOW))
        if unimpl > 0:
            print(colored(f"Unimplemented:   {unimpl}", Colors.YELLOW))
        if info > 0:
            print(f"Informational:   {info}")

        if failed > 0:
            print(colored(f"Failed:          {failed}", Colors.RED))

        # Print failures
        if results["failures"]:
            print()
            print(colored("FAILURES:", Colors.RED))
            for failure in results["failures"][:20]:  # Show first 20
                print(f"  - {failure['case']}: {failure['behavior']}")

            if len(results["failures"]) > 20:
                print(f"  ... and {len(results['failures']) - 20} more")

        # Success/failure summary
        print()
        if failed == 0:
            print(colored("All tests passed!", Colors.GREEN + Colors.BOLD))
        else:
            print(colored(f"{failed} tests failed!", Colors.RED + Colors.BOLD))

        # Report location
        print()
        print(f"Full report: {self.reports_dir / 'index.html'}")

    def run(self) -> int:
        """Run the complete test process."""
        try:
            # Build
            if not self.build_server():
                return 1

            # Prepare config
            self.prepare_config()

            # Start server
            if not self.start_server():
                return 1

            # Run tests
            test_ok = self.run_testsuite()

            # Parse and print results
            results = self.parse_results()
            self.print_results(results)

            # Return code
            if results["failed"] > 0:
                return 1
            return 0

        except KeyboardInterrupt:
            print("\nInterrupted!")
            return 130

        except Exception as e:
            print(colored(f"Error: {e}", Colors.RED))
            return 1

        finally:
            self.stop_server()


def main():
    parser = argparse.ArgumentParser(
        description="Run Autobahn WebSocket testsuite against FasterAPI"
    )
    parser.add_argument(
        "--quick",
        action="store_true",
        help="Run only basic tests (faster)",
    )
    parser.add_argument(
        "--cases",
        type=str,
        help="Comma-separated list of case patterns (e.g., '1.*,2.*')",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=9001,
        help="Server port (default: 9001)",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=300,
        help="Test timeout in seconds (default: 300)",
    )

    args = parser.parse_args()

    runner = AutobahnTestRunner(
        port=args.port,
        timeout=args.timeout,
        quick=args.quick,
        cases=args.cases,
    )

    sys.exit(runner.run())


if __name__ == "__main__":
    main()
