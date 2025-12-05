#!/usr/bin/env python3.13
"""
End-to-End C++ API Tests
Tests the native C++ HTTP server with Python callbacks using modern Python API.
"""

import sys
import os
import time
import json
import urllib.request
import urllib.error

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from fasterapi.http.server import Server
from test_handlers import health_handler, random_handler, post_handler, put_handler, delete_handler

# Test results tracking
tests_passed = 0
tests_failed = 0
test_details = []


def run_test(name, url, method="GET", expected_status=200, expected_keys=None, expected_values=None):
    """Run a single E2E test"""
    global tests_passed, tests_failed, test_details

    print(f"\n  Testing: {name}")
    print(f"    URL: {url} ({method})")

    try:
        req = urllib.request.Request(f"http://127.0.0.1:8765{url}", method=method)
        resp = urllib.request.urlopen(req, timeout=3)

        status = resp.getcode()
        data = json.loads(resp.read().decode('utf-8'))

        print(f"    Status: {status}")
        print(f"    Response: {data}")

        # Validate status
        if status != expected_status:
            raise AssertionError(f"Expected status {expected_status}, got {status}")

        # Validate expected keys exist
        if expected_keys:
            for key in expected_keys:
                if key not in data:
                    raise AssertionError(f"Missing expected key: {key}")

        # Validate expected values
        if expected_values:
            for key, expected_val in expected_values.items():
                actual_val = data.get(key)
                if actual_val != expected_val:
                    raise AssertionError(
                        f"Expected {key}={expected_val}, got {actual_val}"
                    )

        print(f"    ✅ PASS")
        tests_passed += 1
        test_details.append({"name": name, "status": "PASS", "url": url})
        return True

    except urllib.error.HTTPError as e:
        if e.code == expected_status:
            print(f"    ✅ PASS (expected error {expected_status})")
            tests_passed += 1
            test_details.append({"name": name, "status": "PASS", "url": url})
            return True
        else:
            print(f"    ❌ FAIL: HTTP {e.code} - {e.reason}")
            tests_failed += 1
            test_details.append({"name": name, "status": "FAIL", "url": url, "error": str(e)})
            return False

    except Exception as e:
        print(f"    ❌ FAIL: {e}")
        tests_failed += 1
        test_details.append({"name": name, "status": "FAIL", "url": url, "error": str(e)})
        return False


def main():
    print("="*80)
    print("End-to-End C++ API Tests")
    print("="*80)

    # Create server
    server = Server(
        port=8765,
        host="127.0.0.1",
        enable_h2=False,
        enable_h3=False
    )

    # Register test routes using modern Python API
    print("\nRegistering test routes...")

    # Routes use imported handlers from test_handlers module
    # This allows worker processes to import and execute them in parallel
    server.add_route("GET", "/api/health", health_handler)
    server.add_route("GET", "/api/random", random_handler)
    server.add_route("POST", "/api/items", post_handler)
    server.add_route("PUT", "/api/items/1", put_handler)
    server.add_route("DELETE", "/api/items/1", delete_handler)

    print("✅ Routes registered")

    # Start server in background thread
    print("\nStarting server on port 8765...")
    import threading
    server_thread = threading.Thread(target=server.start, daemon=True)
    server_thread.start()
    time.sleep(1.5)  # Wait for server to start

    print("\n" + "="*80)
    print("Running Tests")
    print("="*80)

    # Test Suite 1: Basic HTTP Methods
    print("\n[Test Suite 1: HTTP Methods]")
    run_test("GET /api/health",
             "/api/health",
             expected_keys=["status", "api"],
             expected_values={"status": "healthy", "api": "cpp"})

    run_test("POST /api/items",
             "/api/items",
             method="POST",
             expected_status=200,
             expected_values={"created": True})

    run_test("PUT /api/items/1",
             "/api/items/1",
             method="PUT",
             expected_values={"updated": True})

    # Test Suite 2: Multiple Requests (verify no caching/state issues)
    print("\n[Test Suite 2: Multiple Requests]")

    # Call random endpoint twice, verify different values
    req1 = urllib.request.urlopen("http://127.0.0.1:8765/api/random", timeout=3)
    data1 = json.loads(req1.read())

    time.sleep(0.1)  # Small delay

    req2 = urllib.request.urlopen("http://127.0.0.1:8765/api/random", timeout=3)
    data2 = json.loads(req2.read())

    if data1["random"] != data2["random"]:
        print(f"\n  Testing: Random values differ (no caching)")
        print(f"    Request 1: {data1['random']}")
        print(f"    Request 2: {data2['random']}")
        print(f"    ✅ PASS")
        tests_passed += 1
        test_details.append({"name": "No state/caching", "status": "PASS", "url": "/api/random"})
    else:
        print(f"\n  Testing: Random values differ (no caching)")
        print(f"    ❌ FAIL: Got same value twice: {data1['random']}")
        tests_failed += 1
        test_details.append({"name": "No state/caching", "status": "FAIL", "url": "/api/random"})

    # Test Suite 3: Error Handling
    print("\n[Test Suite 3: Error Handling]")
    run_test("404 for unknown route",
             "/api/unknown",
             expected_status=404)

    # Test Suite 4: Concurrent Requests
    print("\n[Test Suite 4: Concurrent Requests]")
    import concurrent.futures

    def make_request(i):
        try:
            resp = urllib.request.urlopen("http://127.0.0.1:8765/api/health", timeout=3)
            return resp.getcode() == 200
        except:
            return False

    with concurrent.futures.ThreadPoolExecutor(max_workers=10) as executor:
        futures = [executor.submit(make_request, i) for i in range(20)]
        results = [f.result() for f in concurrent.futures.as_completed(futures)]

    success_count = sum(results)
    if success_count == 20:
        print(f"\n  Testing: 20 concurrent requests")
        print(f"    Success: {success_count}/20")
        print(f"    ✅ PASS")
        tests_passed += 1
        test_details.append({"name": "Concurrent requests", "status": "PASS", "url": "/api/health"})
    else:
        print(f"\n  Testing: 20 concurrent requests")
        print(f"    Success: {success_count}/20")
        print(f"    ❌ FAIL")
        tests_failed += 1
        test_details.append({"name": "Concurrent requests", "status": "FAIL", "url": "/api/health"})

    # Print summary
    print("\n" + "="*80)
    print("Test Summary")
    print("="*80)
    print(f"Total: {tests_passed + tests_failed}")
    print(f"✅ Passed: {tests_passed}")
    print(f"❌ Failed: {tests_failed}")
    print(f"Success Rate: {100 * tests_passed / (tests_passed + tests_failed):.1f}%")
    print("="*80)

    # Stop server gracefully
    try:
        server.stop()
        time.sleep(0.5)  # Give server time to shut down cleanly
    except Exception as e:
        print(f"Warning: Server shutdown error (ignoring): {e}")

    return tests_failed == 0


if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)
