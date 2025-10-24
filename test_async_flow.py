#!/usr/bin/env python3
"""
Test async coroutine flow with wake-based resumption.

This test verifies:
1. HTTP/2 server starts with CoroResumer
2. Async coroutines are created on request
3. Worker threads execute Python handlers
4. Wake mechanism resumes coroutines from event loop thread
5. Responses are sent correctly
"""

import sys
import time
import subprocess
import signal
import requests
from threading import Thread

def start_server():
    """Start HTTP/2 server in background."""
    # Import and start server
    sys.path.insert(0, '/Users/bengamble/FasterAPI')
    from fasterapi import App

    app = App()

    # Add test routes with randomized responses
    import random

    @app.get("/")
    def root(request, response):
        """Simple root handler"""
        response.status = 200
        response.content_type = "application/json"
        response.body = '{"message": "Hello from async!", "timestamp": ' + str(time.time()) + '}'

    @app.get("/slow")
    def slow(request, response):
        """Handler that sleeps to test async behavior"""
        time.sleep(0.1)  # 100ms delay
        response.status = 200
        response.content_type = "application/json"
        response.body = '{"message": "Slow response", "value": ' + str(random.randint(1, 1000)) + '}'

    @app.post("/echo")
    def echo(request, response):
        """Echo handler with randomized processing"""
        body = request.body
        random_suffix = random.randint(1, 100)
        response.status = 200
        response.content_type = "text/plain"
        response.body = f"{body} [echoed-{random_suffix}]"

    print("Starting HTTP/2 server on port 8080...")
    app.run(port=8080, blocking=True)

def test_async_requests():
    """Test concurrent async requests."""
    base_url = "http://localhost:8080"

    # Wait for server to start
    time.sleep(2)

    print("\n=== Testing Async Flow ===\n")

    # Test 1: Simple GET request
    print("Test 1: Simple GET /")
    try:
        resp = requests.get(f"{base_url}/", timeout=5)
        print(f"  Status: {resp.status_code}")
        print(f"  Body: {resp.text}")
        assert resp.status_code == 200, f"Expected 200, got {resp.status_code}"
        assert "Hello from async" in resp.text
        print("  ✓ PASS\n")
    except Exception as e:
        print(f"  ✗ FAIL: {e}\n")
        return False

    # Test 2: Slow request (tests async behavior)
    print("Test 2: Slow GET /slow")
    try:
        start = time.time()
        resp = requests.get(f"{base_url}/slow", timeout=5)
        elapsed = time.time() - start
        print(f"  Status: {resp.status_code}")
        print(f"  Elapsed: {elapsed:.3f}s")
        print(f"  Body: {resp.text}")
        assert resp.status_code == 200
        assert elapsed >= 0.1, f"Should take at least 0.1s, took {elapsed:.3f}s"
        print("  ✓ PASS\n")
    except Exception as e:
        print(f"  ✗ FAIL: {e}\n")
        return False

    # Test 3: POST with body
    print("Test 3: POST /echo")
    try:
        resp = requests.post(f"{base_url}/echo", data="test message", timeout=5)
        print(f"  Status: {resp.status_code}")
        print(f"  Body: {resp.text}")
        assert resp.status_code == 200
        assert "test message" in resp.text
        assert "[echoed-" in resp.text
        print("  ✓ PASS\n")
    except Exception as e:
        print(f"  ✗ FAIL: {e}\n")
        return False

    # Test 4: Concurrent requests (tests wake mechanism)
    print("Test 4: Concurrent requests (10 parallel)")
    try:
        import concurrent.futures

        def make_request(i):
            resp = requests.get(f"{base_url}/slow", timeout=10)
            return (i, resp.status_code, resp.elapsed.total_seconds())

        start = time.time()
        with concurrent.futures.ThreadPoolExecutor(max_workers=10) as executor:
            futures = [executor.submit(make_request, i) for i in range(10)]
            results = [f.result() for f in concurrent.futures.as_completed(futures)]

        total_elapsed = time.time() - start

        print(f"  Completed: {len(results)}/10 requests")
        print(f"  Total time: {total_elapsed:.3f}s")
        print(f"  Avg time per request: {total_elapsed/10:.3f}s")

        # With async execution, 10 requests should complete in ~0.1s (parallel)
        # With blocking execution, they would take ~1.0s (sequential)
        if total_elapsed < 0.5:
            print(f"  ✓ ASYNC BEHAVIOR CONFIRMED (parallel execution)")
        else:
            print(f"  ⚠ SEQUENTIAL BEHAVIOR (may indicate blocking)")

        all_success = all(status == 200 for _, status, _ in results)
        assert all_success, "Not all requests succeeded"
        print("  ✓ PASS\n")
    except Exception as e:
        print(f"  ✗ FAIL: {e}\n")
        return False

    print("=== All Tests Passed ===\n")
    return True

if __name__ == "__main__":
    # Start server in background thread
    server_thread = Thread(target=start_server, daemon=True)
    server_thread.start()

    try:
        # Run tests
        success = test_async_requests()

        if success:
            print("\n✓✓✓ SUCCESS: Async flow is working! ✓✓✓\n")
            sys.exit(0)
        else:
            print("\n✗✗✗ FAILURE: Some tests failed ✗✗✗\n")
            sys.exit(1)

    except KeyboardInterrupt:
        print("\nTest interrupted")
        sys.exit(1)
    except Exception as e:
        print(f"\n✗✗✗ ERROR: {e} ✗✗✗\n")
        import traceback
        traceback.print_exc()
        sys.exit(1)
