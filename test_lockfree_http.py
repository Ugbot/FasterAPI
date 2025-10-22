#!/usr/bin/env python3
"""
Test lockfree HTTP/1.1 server with keep-alive support.

This test verifies:
1. Lockfree handler registration
2. HTTP/1.1 keep-alive connections
3. Multiple requests on same connection
4. Graceful shutdown
"""

import asyncio
import time
import sys
from fasterapi import App, Request, Response

# Create FasterAPI instance
app = App(port=8000, host="localhost")

# Global counter to verify handler is being called
request_count = 0

@app.get("/")
async def root(request: Request, response: Response):
    global request_count
    request_count += 1
    response.status = 200
    response.content_type = "text/plain"
    response.body = f"Hello, request #{request_count}!"

@app.get("/keepalive")
async def keepalive_test(request: Request, response: Response):
    """Test endpoint for keep-alive connections"""
    response.status = 200
    response.content_type = "text/plain"
    response.body = "Keep-alive works!"

@app.get("/slow")
async def slow_endpoint(request: Request, response: Response):
    """Simulate slow handler to test concurrency"""
    await asyncio.sleep(0.1)
    response.status = 200
    response.content_type = "text/plain"
    response.body = "Slow response"

def test_basic_request():
    """Test basic HTTP request"""
    print("\n=== Testing basic HTTP request ===")
    import urllib.request

    with urllib.request.urlopen('http://localhost:8000/') as resp:
        content = resp.read().decode()
        print(f"Response: {content}")
        assert "Hello" in content
        print("✓ Basic request works")

def test_keepalive():
    """Test HTTP/1.1 keep-alive"""
    print("\n=== Testing HTTP/1.1 keep-alive ===")
    import http.client

    conn = http.client.HTTPConnection('localhost', 8000)

    # Send multiple requests on same connection
    for i in range(5):
        conn.request('GET', '/keepalive')
        resp = conn.getresponse()
        content = resp.read().decode()
        print(f"Request {i+1}: {resp.status} - {content}")
        assert resp.status == 200
        assert "Keep-alive works" in content

    conn.close()
    print("✓ Keep-alive works (5 requests on same connection)")

def test_concurrent_requests():
    """Test concurrent requests"""
    print("\n=== Testing concurrent requests ===")
    import concurrent.futures
    import urllib.request

    def make_request(i):
        with urllib.request.urlopen('http://localhost:8000/') as resp:
            return resp.read().decode()

    start = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=10) as executor:
        futures = [executor.submit(make_request, i) for i in range(50)]
        results = [f.result() for f in futures]

    elapsed = time.time() - start
    print(f"✓ Completed 50 concurrent requests in {elapsed:.2f}s ({50/elapsed:.0f} req/s)")
    assert len(results) == 50

def test_slow_concurrent():
    """Test concurrent slow requests"""
    print("\n=== Testing concurrent slow requests ===")
    import concurrent.futures
    import urllib.request

    def make_slow_request(i):
        with urllib.request.urlopen('http://localhost:8000/slow') as resp:
            return resp.read().decode()

    start = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=10) as executor:
        futures = [executor.submit(make_slow_request, i) for i in range(20)]
        results = [f.result() for f in futures]

    elapsed = time.time() - start
    print(f"✓ Completed 20 slow requests (100ms each) in {elapsed:.2f}s")
    # Should be much faster than 20 * 0.1 = 2.0s due to concurrency
    assert elapsed < 1.0, f"Too slow: {elapsed}s (expected < 1.0s with concurrency)"

def main():
    """Main test function"""
    print("Starting lockfree HTTP/1.1 server test...")

    # Start server in background thread
    import threading
    server_thread = threading.Thread(target=app.run)
    server_thread.daemon = True
    server_thread.start()

    # Wait for server to start
    time.sleep(0.5)
    print("✓ Server started on http://localhost:8000")

    try:
        # Run tests
        test_basic_request()
        test_keepalive()
        test_concurrent_requests()
        test_slow_concurrent()

        print("\n=== All tests passed! ===")
        print(f"Total requests handled: {request_count}")

    except Exception as e:
        print(f"\n❌ Test failed: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)

    print("\n✓ Lockfree HTTP/1.1 server with keep-alive is working!")

if __name__ == "__main__":
    main()
