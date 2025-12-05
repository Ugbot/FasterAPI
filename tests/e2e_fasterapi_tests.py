#!/usr/bin/env python3.13
"""
E2E Tests for FasterAPI REST API Server
Tests the complete Python → C++ → ZMQ → Python flow
"""

import subprocess
import time
import sys
import os
import json
import random
import requests

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

class E2ETestRunner:
    """E2E test runner with server lifecycle management"""

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
        env["DYLD_LIBRARY_PATH"] = "build/lib:$DYLD_LIBRARY_PATH"
        env["FASTERAPI_LOG_LEVEL"] = "ERROR"  # Suppress logs during tests

        # Start server
        server_script = "/Users/bengamble/FasterAPI/tests/e2e_fasterapi_server.py"
        self.server_process = subprocess.Popen(
            ["python3.13", server_script],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
            cwd="/Users/bengamble/FasterAPI"
        )

        # Wait for server to start
        print(f"{BLUE}Waiting {STARTUP_WAIT}s for server startup...{RESET}")
        time.sleep(STARTUP_WAIT)

        # Verify server is running
        try:
            response = requests.get(f"{BASE_URL}/health", timeout=2)
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
            self.server_process.terminate()
            try:
                self.server_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.server_process.kill()
                self.server_process.wait()
            print(f"{BLUE}Server stopped{RESET}")

    def assert_equal(self, actual, expected, test_name):
        """Assert equality with test tracking"""
        if actual == expected:
            self.tests_passed += 1
            self.test_results.append((test_name, True, None))
            print(f"  {GREEN}✓{RESET} {test_name}")
            return True
        else:
            self.tests_failed += 1
            error = f"Expected {expected}, got {actual}"
            self.test_results.append((test_name, False, error))
            print(f"  {RED}✗{RESET} {test_name}: {error}")
            return False

    def assert_true(self, condition, test_name):
        """Assert condition is true"""
        if condition:
            self.tests_passed += 1
            self.test_results.append((test_name, True, None))
            print(f"  {GREEN}✓{RESET} {test_name}")
            return True
        else:
            self.tests_failed += 1
            error = "Condition was false"
            self.test_results.append((test_name, False, error))
            print(f"  {RED}✗{RESET} {test_name}: {error}")
            return False

    def assert_in(self, item, container, test_name):
        """Assert item is in container"""
        if item in container:
            self.tests_passed += 1
            self.test_results.append((test_name, True, None))
            print(f"  {GREEN}✓{RESET} {test_name}")
            return True
        else:
            self.tests_failed += 1
            error = f"{item} not in {container}"
            self.test_results.append((test_name, False, error))
            print(f"  {RED}✗{RESET} {test_name}: {error}")
            return False

    def test_root_endpoint(self):
        """Test GET / endpoint"""
        print(f"\n{YELLOW}Test: Root Endpoint (GET /){RESET}")

        response = requests.get(f"{BASE_URL}/")
        data = response.json()

        self.assert_equal(response.status_code, 200, "Status code is 200")
        self.assert_in("service", data, "Response contains 'service'")
        self.assert_in("version", data, "Response contains 'version'")
        self.assert_in("status", data, "Response contains 'status'")
        self.assert_equal(data["status"], "running", "Status is 'running'")

    def test_health_endpoint(self):
        """Test GET /health endpoint"""
        print(f"\n{YELLOW}Test: Health Check (GET /health){RESET}")

        response = requests.get(f"{BASE_URL}/health")
        data = response.json()

        self.assert_equal(response.status_code, 200, "Status code is 200")
        self.assert_equal(data["status"], "healthy", "Status is 'healthy'")
        self.assert_in("timestamp", data, "Response contains 'timestamp'")
        self.assert_in("pid", data, "Response contains 'pid'")
        self.assert_true(isinstance(data["pid"], int), "PID is an integer")

    def test_create_user(self):
        """Test POST /users endpoint"""
        print(f"\n{YELLOW}Test: Create User (POST /users){RESET}")

        # Randomized test data
        user_data = {
            "name": f"User_{random.randint(1, 9999)}",
            "email": f"user{random.randint(1, 9999)}@example.com",
            "age": random.randint(18, 80)
        }

        response = requests.post(f"{BASE_URL}/users", json=user_data)
        data = response.json()

        self.assert_equal(response.status_code, 200, "Status code is 200")
        self.assert_equal(data["created"], True, "User created")
        self.assert_in("user", data, "Response contains 'user'")
        self.assert_equal(data["user"]["name"], user_data["name"], "Name matches")
        self.assert_equal(data["user"]["email"], user_data["email"], "Email matches")
        self.assert_equal(data["user"]["age"], user_data["age"], "Age matches")
        self.assert_in("id", data["user"], "User has ID")

        return data["user"]["id"]  # Return ID for subsequent tests

    def test_get_user(self, user_id):
        """Test GET /users/{user_id} endpoint"""
        print(f"\n{YELLOW}Test: Get User (GET /users/{{user_id}}){RESET}")

        response = requests.get(f"{BASE_URL}/users/{user_id}")
        data = response.json()

        self.assert_equal(response.status_code, 200, "Status code is 200")
        self.assert_in("user", data, "Response contains 'user'")
        self.assert_equal(data["user"]["id"], user_id, "User ID matches")

    def test_update_user(self, user_id):
        """Test PUT /users/{user_id} endpoint"""
        print(f"\n{YELLOW}Test: Update User (PUT /users/{{user_id}}){RESET}")

        update_data = {
            "name": f"Updated_User_{random.randint(1, 9999)}",
            "age": random.randint(18, 80)
        }

        response = requests.put(f"{BASE_URL}/users/{user_id}", json=update_data)
        data = response.json()

        self.assert_equal(response.status_code, 200, "Status code is 200")
        self.assert_equal(data["updated"], True, "User updated")
        self.assert_equal(data["user"]["name"], update_data["name"], "Name updated")
        self.assert_equal(data["user"]["age"], update_data["age"], "Age updated")

    def test_list_users(self):
        """Test GET /users with pagination"""
        print(f"\n{YELLOW}Test: List Users (GET /users?limit=10&offset=0){RESET}")

        response = requests.get(f"{BASE_URL}/users", params={"limit": 10, "offset": 0})
        data = response.json()

        self.assert_equal(response.status_code, 200, "Status code is 200")
        self.assert_in("users", data, "Response contains 'users'")
        self.assert_in("total", data, "Response contains 'total'")
        self.assert_in("limit", data, "Response contains 'limit'")
        self.assert_in("offset", data, "Response contains 'offset'")
        self.assert_equal(data["limit"], 10, "Limit is 10")
        self.assert_equal(data["offset"], 0, "Offset is 0")

    def test_delete_user(self, user_id):
        """Test DELETE /users/{user_id} endpoint"""
        print(f"\n{YELLOW}Test: Delete User (DELETE /users/{{user_id}}){RESET}")

        response = requests.delete(f"{BASE_URL}/users/{user_id}")
        data = response.json()

        self.assert_equal(response.status_code, 200, "Status code is 200")
        self.assert_equal(data["deleted"], True, "User deleted")
        self.assert_equal(data["user_id"], user_id, "User ID matches")

        # Verify user is deleted
        get_response = requests.get(f"{BASE_URL}/users/{user_id}")
        get_data = get_response.json()
        self.assert_in("error", get_data, "Deleted user returns error")

    def test_create_item(self):
        """Test POST /items endpoint"""
        print(f"\n{YELLOW}Test: Create Item (POST /items){RESET}")

        # Randomized test data
        item_data = {
            "name": f"Item_{random.randint(1, 9999)}",
            "price": round(random.uniform(1.0, 1000.0), 2),
            "in_stock": random.choice([True, False])
        }

        response = requests.post(f"{BASE_URL}/items", json=item_data)
        data = response.json()

        self.assert_equal(response.status_code, 200, "Status code is 200")
        self.assert_equal(data["created"], True, "Item created")
        self.assert_in("item", data, "Response contains 'item'")
        self.assert_equal(data["item"]["name"], item_data["name"], "Name matches")
        self.assert_equal(data["item"]["price"], item_data["price"], "Price matches")
        self.assert_equal(data["item"]["in_stock"], item_data["in_stock"], "Stock status matches")

        return data["item"]["id"]  # Return ID for subsequent tests

    def test_get_item(self, item_id):
        """Test GET /items/{item_id} endpoint"""
        print(f"\n{YELLOW}Test: Get Item (GET /items/{{item_id}}){RESET}")

        response = requests.get(f"{BASE_URL}/items/{item_id}")
        data = response.json()

        self.assert_equal(response.status_code, 200, "Status code is 200")
        self.assert_in("item", data, "Response contains 'item'")
        self.assert_equal(data["item"]["id"], item_id, "Item ID matches")

    def test_list_items(self):
        """Test GET /items with filters"""
        print(f"\n{YELLOW}Test: List Items (GET /items?in_stock=true){RESET}")

        response = requests.get(f"{BASE_URL}/items", params={
            "in_stock": "true",
            "min_price": 0.0,
            "max_price": 1000.0
        })
        data = response.json()

        self.assert_equal(response.status_code, 200, "Status code is 200")
        self.assert_in("items", data, "Response contains 'items'")
        self.assert_in("total", data, "Response contains 'total'")

    def test_search(self):
        """Test GET /search with query parameters"""
        print(f"\n{YELLOW}Test: Search (GET /search?q=...){RESET}")

        # Randomized query
        search_term = f"query_{random.randint(1, 9999)}"
        page = random.randint(1, 10)
        limit = random.randint(5, 20)

        response = requests.get(f"{BASE_URL}/search", params={
            "q": search_term,
            "page": page,
            "limit": limit,
            "sort": "relevance"
        })
        data = response.json()

        self.assert_equal(response.status_code, 200, "Status code is 200")
        self.assert_equal(data["query"], search_term, "Query matches")
        self.assert_equal(data["page"], page, "Page matches")
        self.assert_equal(data["limit"], limit, "Limit matches")
        self.assert_equal(data["sort"], "relevance", "Sort matches")
        self.assert_in("results", data, "Response contains 'results'")
        self.assert_in("total", data, "Response contains 'total'")

    def test_compute(self):
        """Test POST /compute endpoint (worker pool test)"""
        print(f"\n{YELLOW}Test: Compute (POST /compute){RESET}")

        # Randomized computation
        n = random.randint(10, 100)

        response = requests.post(f"{BASE_URL}/compute", json={
            "n": n,
            "operation": "sum_squares"
        })
        data = response.json()

        # Verify computation
        expected_result = sum(i * i for i in range(n))

        self.assert_equal(response.status_code, 200, "Status code is 200")
        self.assert_equal(data["n"], n, "N matches")
        self.assert_equal(data["operation"], "sum_squares", "Operation matches")
        self.assert_equal(data["result"], expected_result, "Result is correct")
        self.assert_in("worker_pid", data, "Response contains 'worker_pid'")

    def test_random_data(self):
        """Test GET /random endpoint"""
        print(f"\n{YELLOW}Test: Random Data (GET /random){RESET}")

        response = requests.get(f"{BASE_URL}/random")
        data = response.json()

        self.assert_equal(response.status_code, 200, "Status code is 200")
        self.assert_in("random_int", data, "Response contains 'random_int'")
        self.assert_in("random_float", data, "Response contains 'random_float'")
        self.assert_in("random_bool", data, "Response contains 'random_bool'")
        self.assert_in("random_string", data, "Response contains 'random_string'")
        self.assert_in("random_list", data, "Response contains 'random_list'")
        self.assert_true(isinstance(data["random_int"], int), "random_int is int")
        self.assert_true(isinstance(data["random_float"], float), "random_float is float")
        self.assert_true(isinstance(data["random_bool"], bool), "random_bool is bool")
        self.assert_true(isinstance(data["random_string"], str), "random_string is str")
        self.assert_true(isinstance(data["random_list"], list), "random_list is list")

    def test_parameter_types(self):
        """Test C++ parameter extraction with proper types"""
        print(f"\n{YELLOW}Test: Parameter Type Validation{RESET}")

        # Test path parameter (int)
        user_id = random.randint(1, 9999)
        response = requests.post(f"{BASE_URL}/users", json={
            "name": "Test User",
            "email": "test@example.com",
            "age": 25
        })
        created_id = response.json()["user"]["id"]

        get_response = requests.get(f"{BASE_URL}/users/{created_id}")
        user_data = get_response.json()["user"]

        self.assert_true(isinstance(user_data["id"], int), "User ID is int (C++ extracted)")
        self.assert_true(isinstance(user_data["age"], int), "Age is int (C++ extracted)")

        # Test query parameters
        search_response = requests.get(f"{BASE_URL}/search", params={
            "q": "test",
            "page": 2,
            "limit": 15
        })
        search_data = search_response.json()

        self.assert_true(isinstance(search_data["page"], int), "Page is int (C++ extracted)")
        self.assert_true(isinstance(search_data["limit"], int), "Limit is int (C++ extracted)")

    def run_all_tests(self):
        """Run all e2e tests"""
        print("=" * 70)
        print(f"{BLUE}FasterAPI E2E Test Suite{RESET}")
        print("=" * 70)
        print()
        print("Testing:")
        print("  Python REST API → C++ HTTP Server → C++ Router → C++ Params")
        print("  → ZeroMQ IPC → Python Workers → Handler Execution")
        print("  → ZeroMQ IPC → C++ Server → HTTP Response")
        print()

        # Start server
        if not self.start_server():
            print(f"\n{RED}Failed to start server, aborting tests{RESET}")
            return False

        try:
            # Run tests
            self.test_root_endpoint()
            self.test_health_endpoint()

            # User CRUD flow
            user_id = self.test_create_user()
            if user_id:
                self.test_get_user(user_id)
                self.test_update_user(user_id)
            self.test_list_users()
            if user_id:
                self.test_delete_user(user_id)

            # Item CRUD flow
            item_id = self.test_create_item()
            if item_id:
                self.test_get_item(item_id)
            self.test_list_items()

            # Other endpoints
            self.test_search()
            self.test_compute()
            self.test_random_data()
            self.test_parameter_types()

        except Exception as e:
            print(f"\n{RED}Unexpected error during tests: {e}{RESET}")
            import traceback
            traceback.print_exc()
        finally:
            # Stop server
            self.stop_server()

        # Print summary
        print()
        print("=" * 70)
        print(f"{BLUE}Test Summary{RESET}")
        print("=" * 70)
        print(f"Total tests: {self.tests_passed + self.tests_failed}")
        print(f"{GREEN}Passed: {self.tests_passed}{RESET}")
        print(f"{RED}Failed: {self.tests_failed}{RESET}")

        if self.tests_failed > 0:
            print()
            print(f"{RED}Failed tests:{RESET}")
            for name, passed, error in self.test_results:
                if not passed:
                    print(f"  - {name}: {error}")

        print()
        if self.tests_failed == 0:
            print(f"{GREEN}✓ All tests passed!{RESET}")
            return True
        else:
            print(f"{RED}✗ Some tests failed{RESET}")
            return False

if __name__ == "__main__":
    runner = E2ETestRunner()
    success = runner.run_all_tests()
    sys.exit(0 if success else 1)
