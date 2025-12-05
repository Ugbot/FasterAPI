#!/usr/bin/env python3
"""
Simple test to verify query parameter extraction works correctly.
"""

import sys
import time
import urllib.request
import json

sys.path.insert(0, "/Users/bengamble/FasterAPI")

from fasterapi.fastapi_compat import FastAPI

# Create app
app = FastAPI()

# Test endpoint with query parameters
@app.get("/search")
def search(q: str, limit: int = 10):
    """Search endpoint that requires query parameters"""
    return {
        "query": q,
        "limit": limit,
        "message": f"Searching for '{q}' with limit {limit}"
    }

# Test endpoint with path and query parameters
@app.get("/users/{user_id}/posts")
def get_user_posts(user_id: int, page: int = 1, size: int = 10):
    """User posts endpoint with path and query parameters"""
    return {
        "user_id": user_id,
        "page": page,
        "size": size,
        "posts": [f"post_{i}" for i in range((page-1)*size, page*size)]
    }

if __name__ == "__main__":
    print("Starting test server on :8080...")

    # Start server in background
    import threading
    server_thread = threading.Thread(
        target=lambda: app.run(host="127.0.0.1", port=8080),
        daemon=True
    )
    server_thread.start()

    # Wait for server to start
    time.sleep(2)

    print("\nTesting query parameters...")

    # Test 1: Query params only
    print("\n1. Testing /search?q=fastapi&limit=99")
    try:
        resp = urllib.request.urlopen("http://127.0.0.1:8080/search?q=fastapi&limit=99")
        data = json.loads(resp.read())
        print(f"   Response: {data}")
        assert data["query"] == "fastapi", f"Expected query='fastapi', got '{data['query']}'"
        assert data["limit"] == 99, f"Expected limit=99, got {data['limit']}"
        print("   ✅ PASS")
    except Exception as e:
        print(f"   ❌ FAIL: {e}")

    # Test 2: Path + query params
    print("\n2. Testing /users/42/posts?page=5&size=20")
    try:
        resp = urllib.request.urlopen("http://127.0.0.1:8080/users/42/posts?page=5&size=20")
        data = json.loads(resp.read())
        print(f"   Response: {data}")
        assert data["user_id"] == 42, f"Expected user_id=42, got {data['user_id']}"
        assert data["page"] == 5, f"Expected page=5, got {data['page']}"
        assert data["size"] == 20, f"Expected size=20, got {data['size']}"
        print("   ✅ PASS")
    except Exception as e:
        print(f"   ❌ FAIL: {e}")

    # Test 3: Default query params
    print("\n3. Testing /search?q=test (using default limit)")
    try:
        resp = urllib.request.urlopen("http://127.0.0.1:8080/search?q=test")
        data = json.loads(resp.read())
        print(f"   Response: {data}")
        assert data["query"] == "test", f"Expected query='test', got '{data['query']}'"
        assert data["limit"] == 10, f"Expected limit=10 (default), got {data['limit']}"
        print("   ✅ PASS")
    except Exception as e:
        print(f"   ❌ FAIL: {e}")

    print("\n✅ All tests completed!")
    print("Server will continue running... Press Ctrl+C to stop.")

    # Keep server running
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nShutting down...")
