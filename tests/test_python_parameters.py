#!/usr/bin/env python3
"""
Python Parameter Extraction Integration Tests

Mirrors the C++ test suite to validate query and path parameters
work correctly through the full Python stack.
"""

import sys
import time
import urllib.request
import urllib.error
import json
import threading
import subprocess

sys.path.insert(0, "/Users/bengamble/FasterAPI")

from fasterapi.fastapi_compat import FastAPI
import fasterapi

# Test results tracking
tests_passed = 0
tests_failed = 0
test_results = []

def test(name):
    """Decorator to register tests"""
    def decorator(func):
        func.test_name = name
        return func
    return decorator

# ============================================================================
# Create Test App
# ============================================================================

app = FastAPI()

# Query parameter tests
@app.get("/search")
def search(q: str, limit: int = 10):
    """Search endpoint with required and optional query params"""
    return {"query": q, "limit": limit}

@app.get("/filter")
def filter_items(category: str, min_price: float = 0.0, max_price: float = 1000.0):
    """Filter with multiple query params"""
    return {"category": category, "min_price": min_price, "max_price": max_price}

# Path parameter tests
@app.get("/items/{item_id}")
def get_item(item_id: int):
    """Single path parameter"""
    return {"item_id": item_id, "type": "item"}

@app.get("/users/{user_id}/posts/{post_id}")
def get_user_post(user_id: int, post_id: int):
    """Multiple path parameters"""
    return {"user_id": user_id, "post_id": post_id}

# Combined path + query parameters
@app.get("/users/{user_id}/posts")
def get_user_posts(user_id: int, page: int = 1, size: int = 10):
    """Path and query parameters combined"""
    return {"user_id": user_id, "page": page, "size": size}

@app.get("/products/{product_id}/reviews")
def get_product_reviews(product_id: int, rating: int = None, verified: bool = False):
    """Complex combination with optional params"""
    return {"product_id": product_id, "rating": rating, "verified": verified}

# ============================================================================
# Test Functions
# ============================================================================

def run_test(test_func):
    """Run a single test function"""
    global tests_passed, tests_failed

    test_name = getattr(test_func, 'test_name', test_func.__name__)
    print(f"  {test_name}... ", end="", flush=True)

    try:
        test_func()
        print("\u2705 PASS")
        tests_passed += 1
        test_results.append((test_name, "PASS", None))
    except AssertionError as e:
        print(f"\u274c FAIL: {e}")
        tests_failed += 1
        test_results.append((test_name, "FAIL", str(e)))
    except Exception as e:
        print(f"\u274c ERROR: {e}")
        tests_failed += 1
        test_results.append((test_name, "ERROR", str(e)))

def fetch_json(url):
    """Helper to fetch and parse JSON from URL"""
    try:
        resp = urllib.request.urlopen(url, timeout=2)
        return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        raise AssertionError(f"HTTP {e.code}: {e.reason}")
    except Exception as e:
        raise AssertionError(f"Request failed: {e}")

# ============================================================================
# Query Parameter Tests
# ============================================================================

@test("Query params - simple required + default")
def test_query_simple():
    data = fetch_json("http://127.0.0.1:8091/search?q=fastapi&limit=99")
    assert data["query"] == "fastapi", f"Expected query='fastapi', got '{data['query']}'"
    assert data["limit"] == 99, f"Expected limit=99, got {data['limit']}"

@test("Query params - using default value")
def test_query_defaults():
    data = fetch_json("http://127.0.0.1:8091/search?q=test")
    assert data["query"] == "test", f"Expected query='test', got '{data['query']}'"
    assert data["limit"] == 10, f"Expected limit=10 (default), got {data['limit']}"

@test("Query params - multiple params")
def test_query_multiple():
    data = fetch_json("http://127.0.0.1:8091/filter?category=electronics&min_price=50.0&max_price=500.0")
    assert data["category"] == "electronics"
    assert data["min_price"] == 50.0
    assert data["max_price"] == 500.0

@test("Query params - URL encoded")
def test_query_url_encoded():
    data = fetch_json("http://127.0.0.1:8091/search?q=hello%20world&limit=25")
    assert data["query"] == "hello world", f"Expected 'hello world', got '{data['query']}'"
    assert data["limit"] == 25

# ============================================================================
# Path Parameter Tests
# ============================================================================

@test("Path params - single integer")
def test_path_single():
    data = fetch_json("http://127.0.0.1:8091/items/123")
    assert data["item_id"] == 123, f"Expected item_id=123, got {data['item_id']}"
    assert data["type"] == "item"

@test("Path params - multiple")
def test_path_multiple():
    data = fetch_json("http://127.0.0.1:8091/users/42/posts/789")
    assert data["user_id"] == 42
    assert data["post_id"] == 789

# ============================================================================
# Combined Path + Query Parameter Tests
# ============================================================================

@test("Combined - path + query params")
def test_combined_simple():
    data = fetch_json("http://127.0.0.1:8091/users/88/posts?page=5&size=20")
    assert data["user_id"] == 88, f"Expected user_id=88, got {data['user_id']}"
    assert data["page"] == 5, f"Expected page=5, got {data['page']}"
    assert data["size"] == 20, f"Expected size=20, got {data['size']}"

@test("Combined - path + query with defaults")
def test_combined_defaults():
    data = fetch_json("http://127.0.0.1:8091/users/42/posts")
    assert data["user_id"] == 42
    assert data["page"] == 1  # default
    assert data["size"] == 10  # default

@test("Combined - complex optional params")
def test_combined_complex():
    data = fetch_json("http://127.0.0.1:8091/products/999/reviews?rating=5&verified=true")
    assert data["product_id"] == 999
    assert data["rating"] == 5
    # Note: boolean conversion from query params

# ============================================================================
# Real-World Scenario Tests
# ============================================================================

@test("Real-world - search with limit")
def test_realworld_search():
    data = fetch_json("http://127.0.0.1:8091/search?q=fastapi&limit=50")
    assert data["query"] == "fastapi"
    assert data["limit"] == 50

@test("Real-world - pagination")
def test_realworld_pagination():
    data = fetch_json("http://127.0.0.1:8091/users/42/posts?page=3&size=15")
    assert data["user_id"] == 42
    assert data["page"] == 3
    assert data["size"] == 15

@test("Real-world - filter with price range")
def test_realworld_filter():
    data = fetch_json("http://127.0.0.1:8091/filter?category=books&min_price=10.99&max_price=49.99")
    assert data["category"] == "books"
    assert data["min_price"] == 10.99
    assert data["max_price"] == 49.99

# ============================================================================
# Main Test Runner
# ============================================================================

def run_all_tests():
    """Run all test functions"""
    print("\n" + "="*80)
    print("Python Parameter Extraction Integration Tests")
    print("="*80 + "\n")

    # Wait for server to be ready
    print("Waiting for server to start...")
    time.sleep(2)

    # Run all tests
    test_functions = [
        test_query_simple,
        test_query_defaults,
        test_query_multiple,
        test_query_url_encoded,
        test_path_single,
        test_path_multiple,
        test_combined_simple,
        test_combined_defaults,
        test_combined_complex,
        test_realworld_search,
        test_realworld_pagination,
        test_realworld_filter,
    ]

    for test_func in test_functions:
        run_test(test_func)

    # Print summary
    print("\n" + "="*80)
    print(f"Results: {tests_passed} passed, {tests_failed} failed")
    print("="*80)

    if tests_failed > 0:
        print("\nFailed tests:")
        for name, status, error in test_results:
            if status != "PASS":
                print(f"  - {name}: {error}")

    return tests_failed == 0

if __name__ == "__main__":
    print("=" * 80)
    print("Python Parameter Extraction Integration Tests")
    print("=" * 80)
    print()
    print("This test requires uvicorn to run the server.")
    print("To run manually:")
    print("  1. In one terminal: DYLD_LIBRARY_PATH=build/lib uvicorn tests.test_python_parameters:app --port 8091")
    print("  2. In another terminal: curl 'http://127.0.0.1:8091/search?q=test&limit=50'")
    print()
    print("Or just use curl to test against an already-running server:")
    print()

    # Display registered routes
    print("Registered test routes:")
    for route in app.routes():
        print(f"  {route['method']:6s} {route['path_pattern']}")

    print()
    print("Example curl commands to test:")
    print("  curl 'http://127.0.0.1:8091/search?q=fastapi&limit=99'")
    print("  curl 'http://127.0.0.1:8091/users/42/posts?page=5&size=20'")
    print("  curl 'http://127.0.0.1:8091/items/123'")
    print()

    # Try to detect if server is running
    try:
        resp = urllib.request.urlopen("http://127.0.0.1:8091/search?q=test", timeout=1)
        print("\u2713 Server detected on port 8091 - running tests...")
        success = run_all_tests()
        sys.exit(0 if success else 1)
    except:
        print("\u2717 No server running on port 8091")
        print()
        print("Run the tests with:")
        print("  # Terminal 1:")
        print("  cd /Users/bengamble/FasterAPI && DYLD_LIBRARY_PATH=build/lib python3.13 -m uvicorn tests.test_python_parameters:app --port 8091")
        print()
        print("  # Terminal 2:")
        print("  cd /Users/bengamble/FasterAPI && DYLD_LIBRARY_PATH=build/lib python3.13 tests/test_python_parameters.py")
        sys.exit(1)
