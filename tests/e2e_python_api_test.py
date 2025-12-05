#!/usr/bin/env python3.13
"""
End-to-End Python API Tests
Tests the FastAPI compatibility layer with native C++ backend.
Validates parameter extraction, type conversion, and default values.
"""

import sys
import os
import time
import json
import urllib.request
import urllib.error
import random
from typing import Optional

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from fasterapi.fastapi_compat import FastAPI
from fasterapi.http.server import Server
from fasterapi._fastapi_native import connect_route_registry_to_server

# Create FastAPI app
app = FastAPI(title="E2E Python API Test", version="1.0.0")

# Test results
tests_passed = 0
tests_failed = 0


# Define test endpoints with various parameter patterns
@app.get("/api/simple")
def simple_get():
    """No parameters"""
    return {"message": "simple", "random": random.randint(1, 1000)}


@app.get("/api/query/required")
def query_required(name: str, age: int):
    """Required query parameters with type conversion"""
    return {
        "name": name,
        "age": age,
        "age_type": type(age).__name__
    }


@app.get("/api/query/optional")
def query_optional(name: str, age: int = 25, active: bool = True):
    """Query parameters with defaults"""
    return {
        "name": name,
        "age": age,
        "active": active,
        "types": {
            "name": type(name).__name__,
            "age": type(age).__name__,
            "active": type(active).__name__
        }
    }


@app.get("/api/path/{item_id}")
def path_single(item_id: int):
    """Single path parameter"""
    return {
        "item_id": item_id,
        "item_id_type": type(item_id).__name__
    }


@app.get("/api/path/{category}/items/{item_id}")
def path_multiple(category: str, item_id: int):
    """Multiple path parameters"""
    return {
        "category": category,
        "item_id": item_id,
        "types": {
            "category": type(category).__name__,
            "item_id": type(item_id).__name__
        }
    }


@app.get("/api/combined/{user_id}/posts")
def combined_params(user_id: int, page: int = 1, size: int = 10, sort: str = "date"):
    """Path + query parameters with defaults"""
    return {
        "user_id": user_id,
        "page": page,
        "size": size,
        "sort": sort,
        "types": {
            "user_id": type(user_id).__name__,
            "page": type(page).__name__,
            "size": type(size).__name__,
            "sort": type(sort).__name__
        }
    }


@app.get("/api/special/url-encoded")
def url_encoded(text: str, emoji: str = "👍"):
    """URL-encoded parameters"""
    return {
        "text": text,
        "emoji": emoji,
        "text_length": len(text)
    }


@app.post("/api/items")
def create_item():
    """POST endpoint"""
    return {"created": True, "id": random.randint(100, 999)}


@app.put("/api/items/{item_id}")
def update_item(item_id: int):
    """PUT with path parameter"""
    return {"updated": True, "item_id": item_id}


@app.delete("/api/items/{item_id}")
def delete_item(item_id: int):
    """DELETE with path parameter"""
    return {"deleted": True, "item_id": item_id}


def run_test(name, url, expected_checks=None, method="GET"):
    """Run a single E2E test"""
    global tests_passed, tests_failed

    print(f"\n  Testing: {name}")
    print(f"    {method} {url}")

    try:
        req = urllib.request.Request(
            f"http://127.0.0.1:8766{url}",
            method=method
        )
        resp = urllib.request.urlopen(req, timeout=3)

        data = json.loads(resp.read().decode('utf-8'))
        print(f"    Response: {data}")

        # Validate expected checks
        if expected_checks:
            for key, expected_val in expected_checks.items():
                actual_val = data.get(key)
                if actual_val != expected_val:
                    raise AssertionError(
                        f"Expected {key}={expected_val}, got {actual_val}"
                    )

        print(f"    ✅ PASS")
        tests_passed += 1
        return True

    except Exception as e:
        print(f"    ❌ FAIL: {e}")
        tests_failed += 1
        return False


def main():
    print("="*80)
    print("End-to-End Python API Tests (FastAPI Compatibility)")
    print("="*80)

    # Connect registry and create server
    connect_route_registry_to_server()

    server = Server(
        port=8766,
        host="127.0.0.1",
        enable_h2=False,
        enable_h3=False
    )

    print("\n✅ Server initialized")
    print("✅ Routes registered via @app decorators")

    # Start server
    import threading
    server_thread = threading.Thread(target=server.start, daemon=True)
    server_thread.start()
    time.sleep(1.5)

    print("\n" + "="*80)
    print("Running Tests")
    print("="*80)

    # Test Suite 1: Basic Endpoints
    print("\n[Test Suite 1: No Parameters]")
    run_test("GET /api/simple",
             "/api/simple")

    # Verify not caching (should get different random value)
    resp1 = urllib.request.urlopen("http://127.0.0.1:8766/api/simple", timeout=3)
    data1 = json.loads(resp1.read())
    time.sleep(0.1)
    resp2 = urllib.request.urlopen("http://127.0.0.1:8766/api/simple", timeout=3)
    data2 = json.loads(resp2.read())

    if data1["random"] != data2["random"]:
        print(f"\n  Testing: No response caching")
        print(f"    ✅ PASS (values differ: {data1['random']} vs {data2['random']})")
        tests_passed += 1
    else:
        print(f"\n  Testing: No response caching")
        print(f"    ❌ FAIL (same value: {data1['random']})")
        tests_failed += 1

    # Test Suite 2: Query Parameters
    print("\n[Test Suite 2: Query Parameters]")

    run_test("Required query params",
             "/api/query/required?name=Alice&age=30",
             {"name": "Alice", "age": 30, "age_type": "int"})

    run_test("Query with defaults (all provided)",
             "/api/query/optional?name=Bob&age=25&active=true",
             {"name": "Bob", "age": 25, "active": True})

    run_test("Query with defaults (partial)",
             "/api/query/optional?name=Charlie&age=35",
             {"name": "Charlie", "age": 35, "active": True})

    run_test("Query with defaults (only required)",
             "/api/query/optional?name=Dave",
             {"name": "Dave", "age": 25, "active": True})

    # Test Suite 3: Path Parameters
    print("\n[Test Suite 3: Path Parameters]")

    run_test("Single path param",
             "/api/path/123",
             {"item_id": 123, "item_id_type": "int"})

    run_test("Multiple path params",
             "/api/path/electronics/items/456",
             {"category": "electronics", "item_id": 456})

    # Test Suite 4: Combined Path + Query
    print("\n[Test Suite 4: Combined Parameters]")

    run_test("Path + query (all provided)",
             "/api/combined/99/posts?page=5&size=20&sort=votes",
             {"user_id": 99, "page": 5, "size": 20, "sort": "votes"})

    run_test("Path + query (use defaults)",
             "/api/combined/42/posts",
             {"user_id": 42, "page": 1, "size": 10, "sort": "date"})

    run_test("Path + query (partial defaults)",
             "/api/combined/88/posts?page=3",
             {"user_id": 88, "page": 3, "size": 10, "sort": "date"})

    # Test Suite 5: Special Characters & Encoding
    print("\n[Test Suite 5: URL Encoding]")

    run_test("URL-encoded spaces",
             "/api/special/url-encoded?text=hello%20world",
             {"text": "hello world", "text_length": 11})

    run_test("Plus sign as space",
             "/api/special/url-encoded?text=hello+world",
             {"text": "hello world", "text_length": 11})

    run_test("Special characters",
             "/api/special/url-encoded?text=test%21%40%23&emoji=%F0%9F%8E%89",
             {"text": "test!@#"})

    # Test Suite 6: HTTP Methods
    print("\n[Test Suite 6: HTTP Methods]")

    run_test("POST /api/items",
             "/api/items",
             {"created": True},
             method="POST")

    run_test("PUT /api/items/789",
             "/api/items/789",
             {"updated": True, "item_id": 789},
             method="PUT")

    run_test("DELETE /api/items/999",
             "/api/items/999",
             {"deleted": True, "item_id": 999},
             method="DELETE")

    # Test Suite 7: Type Conversion
    print("\n[Test Suite 7: Type Conversion]")

    # Test int conversion
    resp = urllib.request.urlopen(
        "http://127.0.0.1:8766/api/path/777",
        timeout=3
    )
    data = json.loads(resp.read())
    if data["item_id"] == 777 and data["item_id_type"] == "int":
        print(f"\n  Testing: Int type conversion")
        print(f"    Response: {data}")
        print(f"    ✅ PASS")
        tests_passed += 1
    else:
        print(f"\n  Testing: Int type conversion")
        print(f"    ❌ FAIL: {data}")
        tests_failed += 1

    # Test bool conversion
    resp = urllib.request.urlopen(
        "http://127.0.0.1:8766/api/query/optional?name=Test&active=false",
        timeout=3
    )
    data = json.loads(resp.read())
    if data["active"] is False and data["types"]["active"] == "bool":
        print(f"\n  Testing: Bool type conversion")
        print(f"    Response active={data['active']} type={data['types']['active']}")
        print(f"    ✅ PASS")
        tests_passed += 1
    else:
        print(f"\n  Testing: Bool type conversion")
        print(f"    ❌ FAIL: {data}")
        tests_failed += 1

    # Test Suite 8: Stress Test
    print("\n[Test Suite 8: Concurrent Requests]")

    def make_request(i):
        try:
            url = f"http://127.0.0.1:8766/api/combined/{i}/posts?page={i%10+1}"
            resp = urllib.request.urlopen(url, timeout=5)
            data = json.loads(resp.read())
            return data["user_id"] == i and data["page"] == (i % 10 + 1)
        except Exception as e:
            print(f"      Request {i} failed: {e}")
            return False

    import concurrent.futures
    with concurrent.futures.ThreadPoolExecutor(max_workers=20) as executor:
        futures = [executor.submit(make_request, i) for i in range(50)]
        results = [f.result() for f in concurrent.futures.as_completed(futures)]

    success_count = sum(results)
    print(f"\n  Testing: 50 concurrent requests with different params")
    print(f"    Success: {success_count}/50")
    if success_count == 50:
        print(f"    ✅ PASS")
        tests_passed += 1
    else:
        print(f"    ❌ FAIL")
        tests_failed += 1

    # Print summary
    print("\n" + "="*80)
    print("Test Summary")
    print("="*80)
    print(f"Total: {tests_passed + tests_failed}")
    print(f"✅ Passed: {tests_passed}")
    print(f"❌ Failed: {tests_failed}")
    print(f"Success Rate: {100 * tests_passed / (tests_passed + tests_failed):.1f}%")
    print("="*80)

    server.stop()

    return tests_failed == 0


if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)
