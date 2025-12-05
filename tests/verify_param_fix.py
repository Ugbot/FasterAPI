#!/usr/bin/env python3.13
"""
Verify Parameter Extraction Fix
Tests that query parameters are extracted correctly after the http1_connection.cpp fix.
"""

import sys
import os
import time
import json
import urllib.request

# Add parent directory to path
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from fasterapi.fastapi_compat import FastAPI
from fasterapi.http.server import Server
from fasterapi._fastapi_native import connect_route_registry_to_server

# Create test app
app = FastAPI(title="Parameter Fix Test", version="1.0.0")

# Test endpoints
@app.get("/test_query")
def test_query(name: str, age: int = 25):
    """Test query parameters"""
    return {"name": name, "age": age, "test": "query"}

@app.get("/test_path/{item_id}")
def test_path(item_id: int):
    """Test path parameter"""
    return {"item_id": item_id, "test": "path"}

@app.get("/test_both/{user_id}")
def test_both(user_id: int, active: str = "yes"):
    """Test path + query"""
    return {"user_id": user_id, "active": active, "test": "both"}

def run_tests():
    """Run tests against the server"""
    print("\n" + "="*80)
    print("Parameter Extraction Verification Tests")
    print("="*80)

    # Give server time to start
    time.sleep(1)

    passed = 0
    failed = 0

    def test(name, url, expected):
        nonlocal passed, failed
        print(f"\nTest: {name}")
        print(f"  URL: {url}")
        try:
            resp = urllib.request.urlopen(f"http://127.0.0.1:9999{url}", timeout=2)
            data = json.loads(resp.read())
            print(f"  Response: {data}")

            for key, value in expected.items():
                if data.get(key) != value:
                    print(f"  ❌ FAIL: Expected {key}={value}, got {data.get(key)}")
                    failed += 1
                    return

            print(f"  ✅ PASS")
            passed += 1
        except Exception as e:
            print(f"  ❌ ERROR: {e}")
            failed += 1

    # Run tests
    test("Query params",
         "/test_query?name=alice&age=30",
         {"name": "alice", "age": 30})

    test("Query with defaults",
         "/test_query?name=bob",
         {"name": "bob", "age": 25})

    test("Path param",
         "/test_path/123",
         {"item_id": 123})

    test("Path + query",
         "/test_both/99?active=no",
         {"user_id": 99, "active": "no"})

    test("Path + query defaults",
         "/test_both/42",
         {"user_id": 42, "active": "yes"})

    print("\n" + "="*80)
    print(f"Results: {passed} passed, {failed} failed")
    print("="*80 + "\n")

    return failed == 0

if __name__ == "__main__":
    print("\n" + "="*80)
    print("Starting Parameter Fix Verification Server")
    print("="*80)

    # Connect RouteRegistry
    connect_route_registry_to_server()

    # Create server
    server = Server(
        port=9999,
        host="127.0.0.1",
        enable_h2=False,
        enable_h3=False
    )

    print("\n✅ Server initialized on port 9999")
    print("✅ RouteRegistry connected")
    print("\nStarting server...")

    # Start server in a thread so we can run tests
    import threading
    server_thread = threading.Thread(target=server.start, daemon=True)
    server_thread.start()

    # Run tests
    success = run_tests()

    # Stop server
    server.stop()

    sys.exit(0 if success else 1)
