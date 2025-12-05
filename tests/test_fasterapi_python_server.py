#!/usr/bin/env python3.13
"""
E2E Tests for Fixed Python FasterAPI Server

Tests the complete Python → C++ → ZMQ → Python flow with the fixed
run_cpp_fastapi_server.py example.
"""

import subprocess
import time
import sys
import os
import json
import requests
import signal

# Test configuration
SERVER_PORT = 8000
SERVER_HOST = "127.0.0.1"
BASE_URL = f"http://{SERVER_HOST}:{SERVER_PORT}"
STARTUP_WAIT = 5  # seconds to wait for server startup

# Color codes for terminal output
GREEN = "\033[92m"
RED = "\033[91m"
YELLOW = "\033[93m"
BLUE = "\033[94m"
RESET = "\033[0m"


class TestRunner:
    """Test runner with server lifecycle management"""

    def __init__(self):
        self.server_process = None
        self.tests_passed = 0
        self.tests_failed = 0
        self.test_results = []

    def start_server(self):
        """Start the FasterAPI server in a subprocess"""
        print(f"{BLUE}Starting FasterAPI server...{RESET}")

        # Set environment variables
        env = os.environ.copy()
        dyld_path = os.path.join(os.getcwd(), "build", "lib")
        env["DYLD_LIBRARY_PATH"] = f"{dyld_path}:{env.get('DYLD_LIBRARY_PATH', '')}"
        env["FASTERAPI_LOG_LEVEL"] = "ERROR"  # Suppress logs during tests

        # Start server
        server_script = "examples/run_cpp_fastapi_server.py"
        self.server_process = subprocess.Popen(
            ["python3.13", server_script],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
            preexec_fn=os.setsid  # Create new process group for clean shutdown
        )

        # Wait for server to start
        print(f"{BLUE}Waiting {STARTUP_WAIT}s for server startup...{RESET}")
        time.sleep(STARTUP_WAIT)

        # Verify server is running
        try:
            response = requests.get(f"{BASE_URL}/health", timeout=3)
            if response.status_code == 200:
                print(f"{GREEN}✓ Server started successfully{RESET}")
                return True
        except Exception as e:
            print(f"{RED}✗ Failed to connect to server: {e}{RESET}")
            self.stop_server()
            return False

        return True

    def stop_server(self):
        """Stop the FasterAPI server"""
        if self.server_process:
            print(f"{BLUE}Stopping server...{RESET}")
            try:
                # Kill entire process group
                os.killpg(os.getpgid(self.server_process.pid), signal.SIGTERM)
                self.server_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(os.getpgid(self.server_process.pid), signal.SIGKILL)
                self.server_process.wait()
            except Exception as e:
                print(f"{YELLOW}Warning: Error stopping server: {e}{RESET}")
            print(f"{BLUE}Server stopped{RESET}")

    def test_get(self, path, expected_key, expected_value=None, desc=""):
        """Test GET request"""
        try:
            response = requests.get(f"{BASE_URL}{path}", timeout=3)

            if response.status_code != 200:
                self.tests_failed += 1
                print(f"  {RED}✗{RESET} {desc}: HTTP {response.status_code}")
                return False

            data = response.json()

            if expected_key not in data:
                self.tests_failed += 1
                print(f"  {RED}✗{RESET} {desc}: Key '{expected_key}' not in response")
                return False

            if expected_value is not None and data[expected_key] != expected_value:
                self.tests_failed += 1
                print(f"  {RED}✗{RESET} {desc}: Expected {expected_value}, got {data[expected_key]}")
                return False

            self.tests_passed += 1
            print(f"  {GREEN}✓{RESET} {desc}")
            return True

        except Exception as e:
            self.tests_failed += 1
            print(f"  {RED}✗{RESET} {desc}: {e}")
            return False

    def test_post(self, path, payload, expected_key, desc=""):
        """Test POST request"""
        try:
            response = requests.post(
                f"{BASE_URL}{path}",
                json=payload,
                timeout=3,
                headers={"Content-Type": "application/json"}
            )

            if response.status_code not in [200, 201]:
                self.tests_failed += 1
                print(f"  {RED}✗{RESET} {desc}: HTTP {response.status_code}")
                return False

            data = response.json()

            if expected_key not in data:
                self.tests_failed += 1
                print(f"  {RED}✗{RESET} {desc}: Key '{expected_key}' not in response")
                return False

            self.tests_passed += 1
            print(f"  {GREEN}✓{RESET} {desc}")
            return True

        except Exception as e:
            self.tests_failed += 1
            print(f"  {RED}✗{RESET} {desc}: {e}")
            return False

    def run_tests(self):
        """Run all tests"""
        print(f"\n{BLUE}{'='*70}{RESET}")
        print(f"{BLUE}Running FasterAPI Python Server E2E Tests{RESET}")
        print(f"{BLUE}{'='*70}{RESET}\n")

        # Test 1: Root endpoint
        print(f"{BLUE}Test Suite 1: Basic Endpoints{RESET}")
        self.test_get("/", "message", "Welcome to FasterAPI!", "GET / (root)")
        self.test_get("/health", "status", "healthy", "GET /health")

        # Test 2: Items API
        print(f"\n{BLUE}Test Suite 2: Items API{RESET}")
        self.test_get("/items", "0", desc="GET /items (list)")
        self.test_get("/items/1", "name", "Widget", "GET /items/1")
        self.test_get("/items/2", "name", "Gadget", "GET /items/2")
        self.test_get("/items/3", "name", "Doohickey", "GET /items/3")

        # Test 3: Query parameters
        print(f"\n{BLUE}Test Suite 3: Query Parameters{RESET}")
        try:
            # Test with query params
            response = requests.get(f"{BASE_URL}/items?skip=0&limit=2", timeout=3)
            if response.status_code == 200:
                items = response.json()
                if isinstance(items, list) and len(items) == 2:
                    self.tests_passed += 1
                    print(f"  {GREEN}✓{RESET} GET /items?skip=0&limit=2")
                else:
                    self.tests_failed += 1
                    print(f"  {RED}✗{RESET} GET /items?skip=0&limit=2: Expected 2 items")
            else:
                self.tests_failed += 1
                print(f"  {RED}✗{RESET} GET /items?skip=0&limit=2: HTTP {response.status_code}")
        except Exception as e:
            self.tests_failed += 1
            print(f"  {RED}✗{RESET} GET /items?skip=0&limit=2: {e}")

        # Test 4: Users API
        print(f"\n{BLUE}Test Suite 4: Users API{RESET}")
        self.test_get("/users", "0", desc="GET /users (list)")
        self.test_get("/users/1", "username", "alice", "GET /users/1")
        self.test_get("/users/2", "username", "bob", "GET /users/2")
        self.test_get("/users/3", "username", "charlie", "GET /users/3")

        # Test 5: POST requests
        print(f"\n{BLUE}Test Suite 5: POST Requests{RESET}")
        new_item = {
            "name": "TestItem",
            "description": "A test item",
            "price": 99.99,
            "tax": 9.99
        }
        self.test_post("/items", new_item, "id", "POST /items (create)")

        # Test 6: OpenAPI
        print(f"\n{BLUE}Test Suite 6: OpenAPI{RESET}")
        self.test_get("/openapi.json", "openapi", desc="GET /openapi.json")

        # Print summary
        print(f"\n{BLUE}{'='*70}{RESET}")
        print(f"{BLUE}Test Results{RESET}")
        print(f"{BLUE}{'='*70}{RESET}")
        print(f"Passed: {GREEN}{self.tests_passed}{RESET}")
        print(f"Failed: {RED}{self.tests_failed}{RESET}")
        print(f"Total:  {self.tests_passed + self.tests_failed}")
        print(f"{BLUE}{'='*70}{RESET}\n")

        return self.tests_failed == 0


def main():
    """Main test entry point"""
    runner = TestRunner()

    try:
        # Start server
        if not runner.start_server():
            print(f"{RED}Failed to start server{RESET}")
            return 1

        # Run tests
        success = runner.run_tests()

        return 0 if success else 1

    except KeyboardInterrupt:
        print(f"\n{YELLOW}Tests interrupted{RESET}")
        return 1
    finally:
        # Always stop server
        runner.stop_server()


if __name__ == "__main__":
    sys.exit(main())
