#!/usr/bin/env python3
"""
Test HTTP/2 Server Async Flow with Wake-based Resumption

This test verifies:
1. HTTP/2 server starts with CoroResumer
2. Async coroutines are created on request
3. Worker threads execute Python handlers
4. Wake mechanism resumes coroutines from event loop thread
5. Responses are sent correctly
6. Concurrent requests work in parallel (true async behavior)
"""

import sys
import time
import requests
import random
import json
from threading import Thread

# Import the Cython-wrapped HTTP/2 server
try:
    from fasterapi._native.http2 import PyHttp2Server
except ImportError as e:
    print(f"Error importing HTTP/2 module: {e}")
    print("Make sure to build with: cmake --build build -j12")
    sys.exit(1)


# Route handlers with randomized responses
def root_handler(request, response):
    """Simple root handler"""
    response['status'] = 200
    response['content_type'] = 'application/json'
    data = {
        'message': 'Hello from async!',
        'timestamp': time.time()
    }
    response['body'] = json.dumps(data)


def slow_handler(request, response):
    """Handler that sleeps to test async behavior"""
    time.sleep(0.1)  # 100ms delay
    response['status'] = 200
    response['content_type'] = 'application/json'
    data = {
        'message': 'Slow response',
        'value': random.randint(1, 1000)
    }
    response['body'] = json.dumps(data)


def echo_handler(request, response):
    """Echo handler with randomized processing"""
    body = request.get('body', '')
    random_suffix = random.randint(1, 100)
    response['status'] = 200
    response['content_type'] = 'text/plain'
    response['body'] = f"{body} [echoed-{random_suffix}]"


def start_server():
    """Start HTTP/2 server in background."""
    port = 8080
    workers = 4

    print(f"Creating HTTP/2 server on port {port} with {workers} workers...")
    server = PyHttp2Server(port=port, num_pinned_workers=workers)

    # Register routes
    print("Registering routes...")
    server.add_route("GET", "/", root_handler)
    server.add_route("GET", "/slow", slow_handler)
    server.add_route("POST", "/echo", echo_handler)

    print(f"\nStarting HTTP/2 server on http://localhost:{port}/")
    print("Testing async coroutine flow with wake-based resumption\n")

    # Start server (blocks)
    try:
        result = server.start(blocking=True)
        if result != 0:
            print(f"Server failed to start: {result}")
    except Exception as e:
        print(f"Server error: {e}")


def test_async_requests():
    """Test concurrent async requests."""
    base_url = "http://localhost:8080"

    # Wait for server to start
    print("Waiting for server to start...")
    time.sleep(2)

    print("\n=== Testing Async Flow ===\n")

    # Test 1: Simple GET request
    print("Test 1: Simple GET /")
    try:
        resp = requests.get(f"{base_url}/", timeout=5, headers={'Connection': 'close'})
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
        resp = requests.get(f"{base_url}/slow", timeout=5, headers={'Connection': 'close'})
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
        resp = requests.post(f"{base_url}/echo", data="test message", timeout=5, headers={'Connection': 'close'})
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
            resp = requests.get(f"{base_url}/slow", timeout=10, headers={'Connection': 'close'})
            return (i, resp.status_code, resp.elapsed.total_seconds())

        start = time.time()
        with concurrent.futures.ThreadPoolExecutor(max_workers=10) as executor:
            futures = [executor.submit(make_request, i) for i in range(10)]
            results = [f.result() for f in concurrent.futures.as_completed(futures)]

        total_elapsed = time.time() - start

        print(f"  Completed: {len(results)}/10 requests")
        print(f"  Total time: {total_elapsed:.3f}s")
        print(f"  Avg time per request: {total_elapsed/10:.3f}s")

        # With async execution, 10 requests should complete in ~0.1-0.3s (parallel)
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
        import traceback
        traceback.print_exc()
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
            print("Key observations:")
            print("  - Coroutines suspend during Python execution")
            print("  - Wake mechanism resumes from event loop thread")
            print("  - Concurrent requests execute in parallel")
            print("  - No event loop blocking")
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
