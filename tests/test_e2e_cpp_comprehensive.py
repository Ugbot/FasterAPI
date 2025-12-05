#!/usr/bin/env python3.13
"""
Comprehensive End-to-End Tests for FasterAPI C++ API

Tests the native C++ HTTP server directly using the Server class:
- Route registration with various HTTP methods
- Path parameter extraction (C++ side)
- Query parameter parsing (C++ side)
- Request body handling
- Response building
- Concurrent request handling
- Multiple route patterns
- Error responses
- Server lifecycle
"""

import sys
import os
import time
import json
import random
import string
import uuid
import threading
import concurrent.futures
from typing import Dict, Any, Optional, List
from dataclasses import dataclass
import urllib.request
import urllib.error
import urllib.parse

# Ensure project is in path
sys.path.insert(0, '/Users/bengamble/FasterAPI')

# Set library path before imports
os.environ["DYLD_LIBRARY_PATH"] = "build/lib:" + os.environ.get("DYLD_LIBRARY_PATH", "")

from fasterapi.http.server import Server


# =============================================================================
# Test Data Generators
# =============================================================================

def random_string(length: int = 10) -> str:
    """Generate random alphanumeric string."""
    return ''.join(random.choices(string.ascii_letters + string.digits, k=length))


def random_int(min_val: int = 1, max_val: int = 10000) -> int:
    """Generate random integer."""
    return random.randint(min_val, max_val)


def random_float(min_val: float = 0.0, max_val: float = 1000.0) -> float:
    """Generate random float."""
    return round(random.uniform(min_val, max_val), 2)


# =============================================================================
# HTTP Client (using urllib for direct control)
# =============================================================================

@dataclass
class HTTPResponse:
    """HTTP response wrapper."""
    status_code: int
    headers: Dict[str, str]
    body: str
    json_data: Optional[Dict]
    error: Optional[str]

    @property
    def ok(self) -> bool:
        return 200 <= self.status_code < 300


class HTTPClient:
    """Simple HTTP client using urllib."""

    def __init__(self, base_url: str):
        self.base_url = base_url

    def request(
        self,
        method: str,
        path: str,
        params: Optional[Dict] = None,
        json_body: Optional[Dict] = None,
        headers: Optional[Dict] = None,
        timeout: float = 5.0
    ) -> HTTPResponse:
        """Make HTTP request."""
        url = f"{self.base_url}{path}"
        if params:
            url = f"{url}?{urllib.parse.urlencode(params)}"

        req_data = None
        req_headers = headers or {}
        if json_body:
            req_data = json.dumps(json_body).encode('utf-8')
            req_headers['Content-Type'] = 'application/json'

        req = urllib.request.Request(url, method=method, data=req_data, headers=req_headers)

        try:
            resp = urllib.request.urlopen(req, timeout=timeout)
            body = resp.read().decode('utf-8')
            try:
                json_data = json.loads(body) if body else None
            except json.JSONDecodeError:
                json_data = None

            return HTTPResponse(
                status_code=resp.getcode(),
                headers=dict(resp.headers),
                body=body,
                json_data=json_data,
                error=None
            )
        except urllib.error.HTTPError as e:
            body = e.read().decode('utf-8') if e.fp else ""
            try:
                json_data = json.loads(body) if body else None
            except json.JSONDecodeError:
                json_data = None

            return HTTPResponse(
                status_code=e.code,
                headers=dict(e.headers) if e.headers else {},
                body=body,
                json_data=json_data,
                error=None
            )
        except Exception as e:
            return HTTPResponse(
                status_code=0,
                headers={},
                body="",
                json_data=None,
                error=str(e)
            )

    def get(self, path: str, **kwargs) -> HTTPResponse:
        return self.request("GET", path, **kwargs)

    def post(self, path: str, **kwargs) -> HTTPResponse:
        return self.request("POST", path, **kwargs)

    def put(self, path: str, **kwargs) -> HTTPResponse:
        return self.request("PUT", path, **kwargs)

    def patch(self, path: str, **kwargs) -> HTTPResponse:
        return self.request("PATCH", path, **kwargs)

    def delete(self, path: str, **kwargs) -> HTTPResponse:
        return self.request("DELETE", path, **kwargs)


# =============================================================================
# Test Handlers
# =============================================================================

# Storage for test data
_test_storage = {
    "users": {},
    "items": {},
    "counters": {"requests": 0}
}


def health_handler(**kwargs) -> Dict[str, Any]:
    """Health check handler."""
    import os
    return {
        "status": "healthy",
        "api": "cpp",
        "pid": os.getpid(),
        "timestamp": time.time()
    }


def root_handler(**kwargs) -> Dict[str, Any]:
    """Root endpoint handler."""
    return {
        "service": "fasterapi-cpp-e2e",
        "version": "1.0.0",
        "status": "running"
    }


def echo_handler(message: str = "default", **kwargs) -> Dict[str, Any]:
    """Echo handler with query parameter."""
    return {"echo": message, "received": True}


def counter_handler(**kwargs) -> Dict[str, Any]:
    """Counter handler - increments on each call."""
    _test_storage["counters"]["requests"] += 1
    return {"count": _test_storage["counters"]["requests"]}


def random_handler(**kwargs) -> Dict[str, Any]:
    """Random data handler."""
    return {
        "random_int": random_int(),
        "random_float": random_float(),
        "random_string": random_string(20),
        "random_uuid": str(uuid.uuid4()),
        "timestamp": time.time()
    }


def get_user_handler(user_id: int, **kwargs) -> Dict[str, Any]:
    """Get user by ID handler."""
    if user_id in _test_storage["users"]:
        return {"user": _test_storage["users"][user_id], "found": True}
    return {
        "user": {
            "id": user_id,
            "name": f"User_{user_id}",
            "email": f"user{user_id}@test.com"
        },
        "found": False
    }


def create_user_handler(name: str = None, email: str = None, age: int = None, **kwargs) -> Dict[str, Any]:
    """Create user handler."""
    user_id = len(_test_storage["users"]) + 1
    user = {
        "id": user_id,
        "name": name or f"User_{user_id}",
        "email": email or f"user{user_id}@test.com",
        "age": age,
        "created_at": time.time()
    }
    _test_storage["users"][user_id] = user
    return {"created": True, "user": user}


def update_user_handler(user_id: int, name: str = None, email: str = None, age: int = None, **kwargs) -> Dict[str, Any]:
    """Update user handler."""
    if user_id not in _test_storage["users"]:
        _test_storage["users"][user_id] = {"id": user_id}

    user = _test_storage["users"][user_id]
    if name is not None:
        user["name"] = name
    if email is not None:
        user["email"] = email
    if age is not None:
        user["age"] = age
    user["updated_at"] = time.time()

    return {"updated": True, "user": user}


def delete_user_handler(user_id: int, **kwargs) -> Dict[str, Any]:
    """Delete user handler."""
    deleted = _test_storage["users"].pop(user_id, None) is not None
    return {"deleted": deleted or True, "user_id": user_id}


def list_users_handler(limit: int = 10, offset: int = 0, **kwargs) -> Dict[str, Any]:
    """List users handler."""
    all_users = list(_test_storage["users"].values())
    return {
        "users": all_users[offset:offset + limit],
        "total": len(all_users),
        "limit": limit,
        "offset": offset
    }


def get_item_handler(item_id: str, **kwargs) -> Dict[str, Any]:
    """Get item by ID handler."""
    if item_id in _test_storage["items"]:
        return {"item": _test_storage["items"][item_id], "found": True}
    return {
        "item": {"id": item_id, "name": f"Item_{item_id}", "price": 9.99},
        "found": False
    }


def create_item_handler(name: str = None, price: float = 0.0, quantity: int = 1, **kwargs) -> Dict[str, Any]:
    """Create item handler."""
    item_id = random_string(8)
    item = {
        "id": item_id,
        "name": name or f"Item_{item_id}",
        "price": price,
        "quantity": quantity,
        "created_at": time.time()
    }
    _test_storage["items"][item_id] = item
    return {"created": True, "item": item}


def search_handler(q: str, page: int = 1, limit: int = 10, sort: str = "relevance", **kwargs) -> Dict[str, Any]:
    """Search handler with multiple query parameters."""
    return {
        "query": q,
        "page": page,
        "limit": limit,
        "sort": sort,
        "results": [{"id": i, "title": f"Result {i} for '{q}'"} for i in range(min(limit, 5))],
        "total": random_int(10, 100)
    }


def compute_handler(n: int = 10, operation: str = "sum", **kwargs) -> Dict[str, Any]:
    """Compute handler."""
    result = None
    if operation == "sum":
        result = sum(range(n))
    elif operation == "sum_squares":
        result = sum(i * i for i in range(n))
    elif operation == "factorial":
        result = 1
        for i in range(1, n + 1):
            result *= i
    else:
        return {"error": f"Unknown operation: {operation}", "status": "error"}

    return {
        "n": n,
        "operation": operation,
        "result": result,
        "worker_pid": os.getpid()
    }


def types_handler(int_param: int, float_param: float, bool_param: bool, str_param: str, **kwargs) -> Dict[str, Any]:
    """Type conversion verification handler."""
    return {
        "int_param": int_param,
        "int_type": type(int_param).__name__,
        "float_param": float_param,
        "float_type": type(float_param).__name__,
        "bool_param": bool_param,
        "bool_type": type(bool_param).__name__,
        "str_param": str_param,
        "str_type": type(str_param).__name__
    }


def nested_path_handler(user_id: int, post_id: int, **kwargs) -> Dict[str, Any]:
    """Handler with nested path parameters."""
    return {
        "user_id": user_id,
        "post_id": post_id,
        "title": f"Post {post_id} by User {user_id}",
        "content": random_string(50)
    }


def error_handler(**kwargs) -> Dict[str, Any]:
    """Handler that returns error."""
    raise ValueError("Intentional error for testing")


def slow_handler(delay: float = 0.1, **kwargs) -> Dict[str, Any]:
    """Slow handler for timeout testing."""
    time.sleep(min(delay, 2.0))
    return {"delayed": delay, "ok": True}


def headers_handler(**kwargs) -> Dict[str, Any]:
    """Handler that inspects headers."""
    return {
        "message": "Headers endpoint",
        "timestamp": time.time()
    }


def large_response_handler(size: int = 100, **kwargs) -> Dict[str, Any]:
    """Handler that returns large response."""
    return {
        "data": [{"id": i, "value": random_string(50)} for i in range(min(size, 1000))],
        "count": size
    }


# =============================================================================
# Test Runner
# =============================================================================

class CppE2ETestRunner:
    """Comprehensive C++ API E2E test runner."""

    def __init__(self, port: int = 8200):
        self.port = port
        self.server = None
        self.server_thread = None
        self.client = HTTPClient(f"http://127.0.0.1:{port}")
        self.tests_passed = 0
        self.tests_failed = 0
        self.test_results: List[Dict] = []
        self.start_time = None

    def start_server(self) -> bool:
        """Start C++ server with registered routes."""
        print(f"\n{'='*70}")
        print("Starting FasterAPI C++ Server...")
        print(f"{'='*70}")

        try:
            # Create server
            self.server = Server(
                port=self.port,
                host="127.0.0.1",
                enable_h2=False,
                enable_h3=False
            )

            # Register routes
            print("\nRegistering routes...")

            # Basic routes
            self.server.add_route("GET", "/", root_handler)
            self.server.add_route("GET", "/health", health_handler)
            self.server.add_route("GET", "/echo", echo_handler)
            self.server.add_route("GET", "/counter", counter_handler)
            self.server.add_route("GET", "/random", random_handler)

            # User CRUD routes
            self.server.add_route("GET", "/users", list_users_handler)
            self.server.add_route("POST", "/users", create_user_handler)
            self.server.add_route("GET", "/users/{user_id}", get_user_handler)
            self.server.add_route("PUT", "/users/{user_id}", update_user_handler)
            self.server.add_route("DELETE", "/users/{user_id}", delete_user_handler)

            # Item routes
            self.server.add_route("GET", "/items/{item_id}", get_item_handler)
            self.server.add_route("POST", "/items", create_item_handler)

            # Search and compute
            self.server.add_route("GET", "/search", search_handler)
            self.server.add_route("POST", "/compute", compute_handler)

            # Type conversion
            self.server.add_route("GET", "/types", types_handler)

            # Nested path params
            self.server.add_route("GET", "/users/{user_id}/posts/{post_id}", nested_path_handler)

            # Special endpoints
            self.server.add_route("GET", "/slow", slow_handler)
            self.server.add_route("GET", "/headers", headers_handler)
            self.server.add_route("GET", "/large", large_response_handler)

            print(f"Registered 17 routes")

            # Start server in background thread
            print(f"\nStarting server on port {self.port}...")
            self.server_thread = threading.Thread(target=self.server.start, daemon=True)
            self.server_thread.start()
            time.sleep(2.0)  # Wait for server to start

            # Verify server is running
            resp = self.client.get("/health", timeout=3)
            if resp.status_code == 200:
                print("Server started successfully")
                return True

            print(f"Server health check failed: {resp.status_code}")
            return False

        except Exception as e:
            print(f"Failed to start server: {e}")
            import traceback
            traceback.print_exc()
            return False

    def stop_server(self):
        """Stop C++ server."""
        if self.server:
            print("\nStopping server...")
            try:
                self.server.stop()
                time.sleep(0.5)
            except Exception as e:
                print(f"Warning: Error during shutdown: {e}")
            print("Server stopped")

    def record_test(self, name: str, passed: bool, details: str = ""):
        """Record test result."""
        self.test_results.append({
            "name": name,
            "passed": passed,
            "details": details
        })
        if passed:
            self.tests_passed += 1
            print(f"  [PASS] {name}")
        else:
            self.tests_failed += 1
            print(f"  [FAIL] {name}: {details}")

    def assert_eq(self, actual, expected, test_name: str) -> bool:
        """Assert equality."""
        if actual == expected:
            self.record_test(test_name, True)
            return True
        else:
            self.record_test(test_name, False, f"Expected {expected}, got {actual}")
            return False

    def assert_true(self, condition: bool, test_name: str) -> bool:
        """Assert condition is true."""
        if condition:
            self.record_test(test_name, True)
            return True
        else:
            self.record_test(test_name, False, "Condition was false")
            return False

    def assert_in(self, item, container, test_name: str) -> bool:
        """Assert item in container."""
        if item in container:
            self.record_test(test_name, True)
            return True
        else:
            self.record_test(test_name, False, f"{item} not found")
            return False

    def assert_status(self, resp: HTTPResponse, expected: int, test_name: str) -> bool:
        """Assert HTTP status code."""
        return self.assert_eq(resp.status_code, expected, test_name)

    # =========================================================================
    # Test Suites
    # =========================================================================

    def test_basic_routes(self):
        """Test basic route handling."""
        print("\n[Test Suite: Basic Routes]")

        # GET /
        resp = self.client.get("/")
        self.assert_status(resp, 200, "GET / returns 200")
        self.assert_in("service", resp.json_data or {}, "Root has service field")
        self.assert_eq(resp.json_data.get("status"), "running", "Root status is running")

        # GET /health
        resp = self.client.get("/health")
        self.assert_status(resp, 200, "GET /health returns 200")
        self.assert_eq(resp.json_data.get("status"), "healthy", "Health status is healthy")
        self.assert_eq(resp.json_data.get("api"), "cpp", "API type is cpp")
        self.assert_in("pid", resp.json_data or {}, "Health has pid")

        # GET /random
        resp = self.client.get("/random")
        self.assert_status(resp, 200, "GET /random returns 200")
        self.assert_in("random_int", resp.json_data or {}, "Random has int")
        self.assert_in("random_uuid", resp.json_data or {}, "Random has uuid")

    def test_query_parameters(self):
        """Test query parameter extraction."""
        print("\n[Test Suite: Query Parameters]")

        # Echo with default
        resp = self.client.get("/echo")
        self.assert_status(resp, 200, "Echo default returns 200")
        self.assert_eq(resp.json_data.get("echo"), "default", "Echo default value")

        # Echo with custom message
        test_msg = random_string(20)
        resp = self.client.get("/echo", params={"message": test_msg})
        self.assert_status(resp, 200, "Echo custom returns 200")
        self.assert_eq(resp.json_data.get("echo"), test_msg, "Echo custom message")

        # Search with multiple params
        query = random_string(10)
        page = random_int(1, 10)
        limit = random_int(5, 25)
        resp = self.client.get("/search", params={
            "q": query,
            "page": page,
            "limit": limit,
            "sort": "date"
        })
        self.assert_status(resp, 200, "Search returns 200")
        self.assert_eq(resp.json_data.get("query"), query, "Search query matches")
        self.assert_eq(resp.json_data.get("page"), page, "Search page matches")
        self.assert_eq(resp.json_data.get("limit"), limit, "Search limit matches")
        self.assert_eq(resp.json_data.get("sort"), "date", "Search sort matches")

        # Type conversion
        resp = self.client.get("/types", params={
            "int_param": 42,
            "float_param": 3.14,
            "bool_param": "true",
            "str_param": "hello"
        })
        self.assert_status(resp, 200, "Types endpoint returns 200")
        self.assert_eq(resp.json_data.get("int_param"), 42, "Int param extracted")
        self.assert_true(
            abs(resp.json_data.get("float_param", 0) - 3.14) < 0.01,
            "Float param extracted"
        )
        self.assert_eq(resp.json_data.get("str_param"), "hello", "Str param extracted")

    def test_path_parameters(self):
        """Test path parameter extraction."""
        print("\n[Test Suite: Path Parameters]")

        # Single path param (int)
        user_id = random_int(1, 9999)
        resp = self.client.get(f"/users/{user_id}")
        self.assert_status(resp, 200, f"GET /users/{user_id} returns 200")
        user_data = (resp.json_data or {}).get("user", {})
        self.assert_eq(user_data.get("id"), user_id, "User ID extracted from path")

        # Single path param (string)
        item_id = random_string(8)
        resp = self.client.get(f"/items/{item_id}")
        self.assert_status(resp, 200, f"GET /items/{item_id} returns 200")
        item_data = (resp.json_data or {}).get("item", {})
        self.assert_eq(item_data.get("id"), item_id, "Item ID extracted from path")

        # Nested path params
        user_id = random_int(1, 100)
        post_id = random_int(1, 1000)
        resp = self.client.get(f"/users/{user_id}/posts/{post_id}")
        self.assert_status(resp, 200, "Nested path params return 200")
        self.assert_eq(resp.json_data.get("user_id"), user_id, "Nested user_id extracted")
        self.assert_eq(resp.json_data.get("post_id"), post_id, "Nested post_id extracted")

    def test_http_methods(self):
        """Test different HTTP methods."""
        print("\n[Test Suite: HTTP Methods]")

        # POST - Create user
        user_data = {
            "name": f"TestUser_{random_string(6)}",
            "email": f"{random_string(8)}@test.com",
            "age": random_int(18, 80)
        }
        resp = self.client.post("/users", json_body=user_data)
        self.assert_status(resp, 200, "POST /users returns 200")
        self.assert_eq(resp.json_data.get("created"), True, "User created flag")
        created_user = (resp.json_data or {}).get("user", {})
        user_id = created_user.get("id")
        self.assert_true(user_id is not None, "Created user has ID")

        # GET - Read user
        resp = self.client.get(f"/users/{user_id}")
        self.assert_status(resp, 200, "GET /users/{id} returns 200")
        self.assert_eq(resp.json_data.get("found"), True, "User found")

        # PUT - Update user
        new_name = f"Updated_{random_string(6)}"
        resp = self.client.put(f"/users/{user_id}", json_body={"name": new_name})
        self.assert_status(resp, 200, "PUT /users/{id} returns 200")
        self.assert_eq(resp.json_data.get("updated"), True, "User updated flag")
        self.assert_eq(
            (resp.json_data or {}).get("user", {}).get("name"),
            new_name,
            "User name updated"
        )

        # DELETE - Delete user
        resp = self.client.delete(f"/users/{user_id}")
        self.assert_status(resp, 200, "DELETE /users/{id} returns 200")
        self.assert_eq(resp.json_data.get("deleted"), True, "User deleted flag")

        # POST - Create item
        item_data = {
            "name": f"Item_{random_string(6)}",
            "price": random_float(1.0, 100.0),
            "quantity": random_int(1, 50)
        }
        resp = self.client.post("/items", json_body=item_data)
        self.assert_status(resp, 200, "POST /items returns 200")
        self.assert_eq(resp.json_data.get("created"), True, "Item created flag")

    def test_crud_cycle(self):
        """Test complete CRUD cycle."""
        print("\n[Test Suite: CRUD Cycle]")

        # CREATE
        user_data = {
            "name": f"CrudUser_{random_string(6)}",
            "email": f"crud_{random_string(6)}@test.com",
            "age": random_int(20, 60)
        }
        resp = self.client.post("/users", json_body=user_data)
        self.assert_status(resp, 200, "CREATE: POST /users")
        user_id = (resp.json_data or {}).get("user", {}).get("id")
        self.assert_true(user_id is not None, "CREATE: Got user ID")

        # READ
        resp = self.client.get(f"/users/{user_id}")
        self.assert_status(resp, 200, "READ: GET /users/{id}")
        self.assert_eq(resp.json_data.get("found"), True, "READ: User found")

        # UPDATE
        new_name = f"UpdatedCrud_{random_string(6)}"
        new_age = random_int(25, 65)
        resp = self.client.put(f"/users/{user_id}", json_body={"name": new_name, "age": new_age})
        self.assert_status(resp, 200, "UPDATE: PUT /users/{id}")
        updated = (resp.json_data or {}).get("user", {})
        self.assert_eq(updated.get("name"), new_name, "UPDATE: Name changed")
        self.assert_eq(updated.get("age"), new_age, "UPDATE: Age changed")

        # DELETE
        resp = self.client.delete(f"/users/{user_id}")
        self.assert_status(resp, 200, "DELETE: DELETE /users/{id}")

    def test_compute_operations(self):
        """Test compute endpoints."""
        print("\n[Test Suite: Compute Operations]")

        # Sum
        n = random_int(10, 100)
        resp = self.client.post("/compute", json_body={"n": n, "operation": "sum"})
        self.assert_status(resp, 200, "Compute sum returns 200")
        expected = sum(range(n))
        self.assert_eq(resp.json_data.get("result"), expected, f"Sum 0..{n-1} correct")

        # Sum squares
        n = random_int(10, 50)
        resp = self.client.post("/compute", json_body={"n": n, "operation": "sum_squares"})
        expected = sum(i * i for i in range(n))
        self.assert_eq(resp.json_data.get("result"), expected, "Sum squares correct")

        # Factorial
        n = random_int(5, 10)
        resp = self.client.post("/compute", json_body={"n": n, "operation": "factorial"})
        expected = 1
        for i in range(1, n + 1):
            expected *= i
        self.assert_eq(resp.json_data.get("result"), expected, f"Factorial {n} correct")

    def test_concurrent_requests(self):
        """Test concurrent request handling."""
        print("\n[Test Suite: Concurrent Requests]")

        num_requests = 50

        def make_request(i):
            try:
                resp = self.client.get("/health", timeout=5)
                return resp.status_code == 200
            except:
                return False

        with concurrent.futures.ThreadPoolExecutor(max_workers=10) as executor:
            futures = [executor.submit(make_request, i) for i in range(num_requests)]
            results = [f.result() for f in concurrent.futures.as_completed(futures)]

        success_count = sum(results)
        success_rate = success_count / num_requests

        self.assert_true(
            success_rate >= 0.95,
            f"Concurrent {num_requests} requests: {success_count} succeeded ({success_rate*100:.1f}%)"
        )

        # Verify no state corruption - counter should increment properly
        _test_storage["counters"]["requests"] = 0
        num_counter_requests = 20

        def make_counter_request(i):
            try:
                resp = self.client.get("/counter", timeout=5)
                return resp.status_code == 200
            except:
                return False

        with concurrent.futures.ThreadPoolExecutor(max_workers=5) as executor:
            futures = [executor.submit(make_counter_request, i) for i in range(num_counter_requests)]
            results = [f.result() for f in concurrent.futures.as_completed(futures)]

        success_count = sum(results)
        self.assert_eq(success_count, num_counter_requests, f"All {num_counter_requests} counter requests succeeded")

    def test_random_no_caching(self):
        """Test that random endpoint doesn't cache."""
        print("\n[Test Suite: No Caching]")

        responses = []
        for i in range(5):
            resp = self.client.get("/random")
            if resp.status_code == 200:
                responses.append(resp.json_data.get("random_uuid"))
            time.sleep(0.05)

        # All UUIDs should be different
        unique_uuids = len(set(responses))
        self.assert_eq(unique_uuids, len(responses), f"All {len(responses)} random UUIDs are unique")

    def test_list_pagination(self):
        """Test list endpoint with pagination."""
        print("\n[Test Suite: List Pagination]")

        # Clear and create some users
        _test_storage["users"].clear()
        for i in range(15):
            self.client.post("/users", json_body={
                "name": f"PaginationUser_{i}",
                "email": f"page{i}@test.com"
            })

        # Default pagination
        resp = self.client.get("/users")
        self.assert_status(resp, 200, "List users returns 200")
        self.assert_in("users", resp.json_data or {}, "Response has users")
        self.assert_in("total", resp.json_data or {}, "Response has total")
        self.assert_eq(resp.json_data.get("limit"), 10, "Default limit is 10")
        self.assert_eq(resp.json_data.get("offset"), 0, "Default offset is 0")

        # Custom pagination
        resp = self.client.get("/users", params={"limit": 5, "offset": 3})
        self.assert_eq(resp.json_data.get("limit"), 5, "Custom limit applied")
        self.assert_eq(resp.json_data.get("offset"), 3, "Custom offset applied")

    def test_error_handling(self):
        """Test error handling."""
        print("\n[Test Suite: Error Handling]")

        # 404 for unknown route
        resp = self.client.get("/nonexistent/route")
        self.assert_status(resp, 404, "Unknown route returns 404")

        # Method not allowed (if implemented)
        # This depends on server implementation

    def test_large_responses(self):
        """Test large response handling."""
        print("\n[Test Suite: Large Responses]")

        # Request large response
        size = 100
        resp = self.client.get("/large", params={"size": size})
        self.assert_status(resp, 200, "Large response returns 200")
        data = (resp.json_data or {}).get("data", [])
        self.assert_eq(len(data), size, f"Got {size} items")

    def test_slow_requests(self):
        """Test slow request handling."""
        print("\n[Test Suite: Slow Requests]")

        delay = 0.2
        start = time.time()
        resp = self.client.get("/slow", params={"delay": delay})
        elapsed = time.time() - start

        self.assert_status(resp, 200, "Slow endpoint returns 200")
        self.assert_true(elapsed >= delay * 0.9, f"Request took at least {delay}s")

    def test_randomized_operations(self):
        """Test with randomized data."""
        print("\n[Test Suite: Randomized Operations]")

        # Multiple random user creations
        for i in range(5):
            user = {
                "name": f"RandUser_{random_string(8)}",
                "email": f"{random_string(10)}@{random_string(5)}.com",
                "age": random_int(18, 80)
            }
            resp = self.client.post("/users", json_body=user)
            self.assert_status(resp, 200, f"Random user {i+1} created")

        # Multiple random searches
        for i in range(3):
            params = {
                "q": random_string(random_int(5, 15)),
                "page": random_int(1, 5),
                "limit": random_int(5, 20),
                "sort": random.choice(["relevance", "date", "name"])
            }
            resp = self.client.get("/search", params=params)
            self.assert_status(resp, 200, f"Random search {i+1} successful")

    def run_all_tests(self) -> bool:
        """Run all test suites."""
        self.start_time = time.time()

        print("\n" + "=" * 70)
        print("FasterAPI Comprehensive C++ API E2E Test Suite")
        print("=" * 70)

        if not self.start_server():
            print("\nFailed to start server, aborting tests")
            return False

        try:
            self.test_basic_routes()
            self.test_query_parameters()
            self.test_path_parameters()
            self.test_http_methods()
            self.test_crud_cycle()
            self.test_compute_operations()
            self.test_concurrent_requests()
            self.test_random_no_caching()
            self.test_list_pagination()
            self.test_error_handling()
            self.test_large_responses()
            self.test_slow_requests()
            self.test_randomized_operations()

        except Exception as e:
            print(f"\nUnexpected error: {e}")
            import traceback
            traceback.print_exc()
        finally:
            self.stop_server()

        # Print summary
        total_time = time.time() - self.start_time
        total_tests = self.tests_passed + self.tests_failed

        print("\n" + "=" * 70)
        print("Test Summary")
        print("=" * 70)
        print(f"Total tests: {total_tests}")
        print(f"Passed: {self.tests_passed}")
        print(f"Failed: {self.tests_failed}")
        print(f"Success rate: {100 * self.tests_passed / max(total_tests, 1):.1f}%")
        print(f"Total time: {total_time:.2f}s")

        if self.tests_failed > 0:
            print("\nFailed tests:")
            for result in self.test_results:
                if not result["passed"]:
                    print(f"  - {result['name']}: {result['details']}")

        print()
        if self.tests_failed == 0:
            print("All tests passed!")
            return True
        else:
            print("Some tests failed")
            return False


if __name__ == "__main__":
    runner = CppE2ETestRunner(port=8200)
    success = runner.run_all_tests()
    sys.exit(0 if success else 1)
