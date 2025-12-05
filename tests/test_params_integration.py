#!/usr/bin/env python3
"""
End-to-End Parameter Extraction Tests

Tests query and path parameters through the full Python->C++ stack.
Uses native FasterAPI server (not uvicorn).
"""

import sys
import time
import urllib.request
import urllib.error
import json
import subprocess
import signal

sys.path.insert(0, "/Users/bengamble/FasterAPI")

from fasterapi.fastapi_compat import FastAPI

# Create test app
app = FastAPI()

# Test endpoints
@app.get("/search")
def search(q: str, limit: int = 10):
    """Search with required q and optional limit"""
    return {"query": q, "limit": limit, "endpoint": "search"}

@app.get("/filter")
def filter_items(category: str, min_price: float = 0.0, max_price: float = 1000.0):
    """Filter with multiple params"""
    return {
        "category": category,
        "min_price": min_price,
        "max_price": max_price,
        "endpoint": "filter"
    }

@app.get("/items/{item_id}")
def get_item(item_id: int):
    """Single path parameter"""
    return {"item_id": item_id, "endpoint": "get_item"}

@app.get("/users/{user_id}/posts")
def get_user_posts(user_id: int, page: int = 1, size: int = 10):
    """Path + query parameters"""
    return {
        "user_id": user_id,
        "page": page,
        "size": size,
        "endpoint": "get_user_posts"
    }

@app.get("/products/{product_id}/reviews/{review_id}")
def get_review(product_id: int, review_id: int):
    """Multiple path parameters"""
    return {
        "product_id": product_id,
        "review_id": review_id,
        "endpoint": "get_review"
    }

# Test runner
def run_tests():
    """Run integration tests against running server"""

    PORT = 8092
    BASE = f"http://127.0.0.1:{PORT}"

    tests_passed = 0
    tests_failed = 0

    print("\n" + "="*80)
    print("Parameter Extraction Integration Tests")
    print("="*80 + "\n")

    def test(name, url, expected_checks):
        """Run a single test"""
        nonlocal tests_passed, tests_failed

        print(f"  {name}... ", end="", flush=True)
        try:
            resp = urllib.request.urlopen(f"{BASE}{url}", timeout=2)
            data = json.loads(resp.read())

            # Check all expected conditions
            for key, expected_value in expected_checks.items():
                actual_value = data.get(key)
                if actual_value != expected_value:
                    raise AssertionError(
                        f"Expected {key}={expected_value}, got {actual_value}"
                    )

            print(f"✅ PASS")
            tests_passed += 1
            return True

        except AssertionError as e:
            print(f"❌ FAIL: {e}")
            print(f"     URL: {url}")
            print(f"     Response: {data if 'data' in locals() else 'N/A'}")
            tests_failed += 1
            return False

        except Exception as e:
            print(f"❌ ERROR: {e}")
            tests_failed += 1
            return False

    # Wait for server
    print("Waiting for server to start...")
    for i in range(10):
        try:
            urllib.request.urlopen(f"{BASE}/", timeout=1)
            print("✓ Server ready\n")
            break
        except:
            time.sleep(0.5)
    else:
        print("❌ Server failed to start")
        return False

    print("Running tests:\n")

    # Query parameter tests
    print("Query Parameters:")
    test("Simple query params",
         "/search?q=fastapi&limit=99",
         {"query": "fastapi", "limit": 99})

    test("Query with defaults",
         "/search?q=test",
         {"query": "test", "limit": 10})

    test("Multiple query params",
         "/filter?category=electronics&min_price=50.5&max_price=500.99",
         {"category": "electronics", "min_price": 50.5, "max_price": 500.99})

    test("Query with URL encoding",
         "/search?q=hello%20world&limit=25",
         {"query": "hello world", "limit": 25})

    print("\nPath Parameters:")
    test("Single path param",
         "/items/123",
         {"item_id": 123})

    test("Multiple path params",
         "/products/456/reviews/789",
         {"product_id": 456, "review_id": 789})

    print("\nCombined Path + Query:")
    test("Path + query with values",
         "/users/42/posts?page=5&size=20",
         {"user_id": 42, "page": 5, "size": 20})

    test("Path + query with defaults",
         "/users/88/posts",
         {"user_id": 88, "page": 1, "size": 10})

    test("Path + query partial",
         "/users/99/posts?page=3",
         {"user_id": 99, "page": 3, "size": 10})

    print("\nReal-World Scenarios:")
    test("Search with limit",
         "/search?q=python&limit=50",
         {"query": "python", "limit": 50})

    test("Filter with price range",
         "/filter?category=books&min_price=10.99&max_price=49.99",
         {"category": "books", "min_price": 10.99, "max_price": 49.99})

    test("Pagination",
         "/users/1/posts?page=2&size=15",
         {"user_id": 1, "page": 2, "size": 15})

    # Summary
    print("\n" + "="*80)
    print(f"Results: {tests_passed} passed, {tests_failed} failed")
    print("="*80 + "\n")

    return tests_failed == 0


if __name__ == "__main__":
    import os

    # Check if we're being imported for server startup
    if os.environ.get("RUN_SERVER"):
        print("Starting test server on port 8092...")
        print("Registered routes:")
        for route in app.routes():
            print(f"  {route['method']:6s} {route['path_pattern']}")
        print("\nServer running...\n")

        # Note: This would need proper server.run() implementation
        # For now, document what needs to happen
        print("ERROR: Native server.run() not yet implemented")
        print("Use the test script instead: tests/run_integration_tests.sh")
        sys.exit(1)
    else:
        # Run tests (expects server already running)
        success = run_tests()
        sys.exit(0 if success else 1)
