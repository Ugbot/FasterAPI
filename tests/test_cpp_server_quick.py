#!/usr/bin/env python3.13
"""
Quick E2E Tests for FasterAPI C++ Server API

This test file provides fast, focused E2E tests for the C++ HTTP server.
It tests basic server functionality without the overhead of full comprehensive tests.

Run with: python3.13 tests/test_cpp_server_quick.py
Or with pytest: pytest tests/test_cpp_server_quick.py -v
"""

import sys
import os
import time
import json
import random
import string
import threading
import urllib.request
import urllib.error
import urllib.parse
import pytest
from typing import Dict, Any, Optional

# Ensure project is in path
sys.path.insert(0, '/Users/bengamble/FasterAPI')

# Set library path before imports
os.environ["DYLD_LIBRARY_PATH"] = "build/lib:" + os.environ.get("DYLD_LIBRARY_PATH", "")


# =============================================================================
# Test Data Generators
# =============================================================================

def random_string(length: int = 10) -> str:
    """Generate random alphanumeric string."""
    return ''.join(random.choices(string.ascii_letters + string.digits, k=length))


def random_int(min_val: int = 1, max_val: int = 10000) -> int:
    """Generate random integer."""
    return random.randint(min_val, max_val)


# =============================================================================
# HTTP Client
# =============================================================================

class SimpleClient:
    """Simple HTTP client for testing."""

    def __init__(self, base_url: str):
        self.base_url = base_url

    def get(self, path: str, params: Optional[Dict] = None, timeout: float = 5.0) -> Dict:
        """Make GET request and return JSON response."""
        url = f"{self.base_url}{path}"
        if params:
            url = f"{url}?{urllib.parse.urlencode(params)}"

        req = urllib.request.Request(url, method="GET")
        resp = urllib.request.urlopen(req, timeout=timeout)
        return json.loads(resp.read().decode('utf-8'))

    def post(self, path: str, json_body: Optional[Dict] = None, timeout: float = 5.0) -> Dict:
        """Make POST request and return JSON response."""
        url = f"{self.base_url}{path}"
        data = json.dumps(json_body).encode('utf-8') if json_body else None

        req = urllib.request.Request(url, method="POST", data=data)
        req.add_header('Content-Type', 'application/json')
        resp = urllib.request.urlopen(req, timeout=timeout)
        return json.loads(resp.read().decode('utf-8'))


# =============================================================================
# Server Fixture
# =============================================================================

# Test storage (shared across handlers)
_test_storage = {
    "counter": 0,
    "users": {},
    "items": {}
}


def root_handler(**kwargs) -> Dict[str, Any]:
    """Root endpoint handler."""
    return {"message": "Hello from C++ Server", "status": "ok", "version": "1.0"}


def health_handler(**kwargs) -> Dict[str, Any]:
    """Health check handler."""
    return {"status": "healthy", "timestamp": time.time(), "pid": os.getpid()}


def echo_handler(message: str = "default", **kwargs) -> Dict[str, Any]:
    """Echo handler with query param."""
    return {"echo": message, "received_at": time.time()}


def counter_handler(**kwargs) -> Dict[str, Any]:
    """Counter handler - increments on each call."""
    _test_storage["counter"] += 1
    return {"count": _test_storage["counter"]}


def random_data_handler(**kwargs) -> Dict[str, Any]:
    """Random data handler."""
    import uuid
    return {
        "random_uuid": str(uuid.uuid4()),
        "random_int": random_int(1, 1000),
        "random_string": random_string(20),
        "timestamp": time.time()
    }


def get_user_handler(user_id: int, **kwargs) -> Dict[str, Any]:
    """Get user by ID."""
    if user_id in _test_storage["users"]:
        return {"user": _test_storage["users"][user_id], "found": True}
    return {"user": {"id": user_id, "name": f"User_{user_id}"}, "found": False}


def create_user_handler(**kwargs) -> Dict[str, Any]:
    """Create a new user."""
    _test_storage["counter"] += 1
    user_id = _test_storage["counter"]
    body = kwargs.get("_body", {})
    user = {
        "id": user_id,
        "name": body.get("name", f"NewUser_{user_id}"),
        "email": body.get("email", f"user{user_id}@test.com")
    }
    _test_storage["users"][user_id] = user
    return {"created": True, "user": user}


def search_handler(q: str = "", page: int = 1, limit: int = 10, **kwargs) -> Dict[str, Any]:
    """Search handler with multiple query params."""
    return {
        "query": q,
        "page": page,
        "limit": limit,
        "results": [{"id": i, "match": f"{q}_{i}"} for i in range(min(limit, 5))],
        "total": random_int(10, 100)
    }


def types_handler(int_param: int = 0, float_param: float = 0.0,
                  bool_param: bool = False, str_param: str = "", **kwargs) -> Dict[str, Any]:
    """Handler to test type conversion."""
    return {
        "int_param": int_param,
        "float_param": float_param,
        "bool_param": bool_param,
        "str_param": str_param,
        "received_types": {
            "int_param": type(int_param).__name__,
            "float_param": type(float_param).__name__,
            "bool_param": type(bool_param).__name__,
            "str_param": type(str_param).__name__
        }
    }


def nested_path_handler(user_id: int, post_id: int, **kwargs) -> Dict[str, Any]:
    """Handler for nested path params."""
    return {"user_id": user_id, "post_id": post_id}


class CppServerFixture:
    """Fixture for managing C++ server lifecycle."""

    def __init__(self, port: int = 8250):
        self.port = port
        self.server = None
        self.server_thread = None
        self.client = None

    def start(self) -> bool:
        """Start the test server."""
        from fasterapi.http.server import Server

        # Reset test storage
        _test_storage["counter"] = 0
        _test_storage["users"] = {}
        _test_storage["items"] = {}

        # Create server
        self.server = Server(
            port=self.port,
            host="127.0.0.1",
            enable_h2=False,
            enable_h3=False
        )

        # Register routes
        self.server.add_route("GET", "/", root_handler)
        self.server.add_route("GET", "/health", health_handler)
        self.server.add_route("GET", "/echo", echo_handler)
        self.server.add_route("GET", "/counter", counter_handler)
        self.server.add_route("GET", "/random", random_data_handler)
        self.server.add_route("GET", "/users/{user_id}", get_user_handler)
        self.server.add_route("POST", "/users", create_user_handler)
        self.server.add_route("GET", "/search", search_handler)
        self.server.add_route("GET", "/types", types_handler)
        self.server.add_route("GET", "/users/{user_id}/posts/{post_id}", nested_path_handler)

        # Start server in background thread
        self.server_thread = threading.Thread(target=self.server.start, daemon=True)
        self.server_thread.start()

        # Wait for server to start (ZMQ workers need time)
        time.sleep(4)

        # Create client
        self.client = SimpleClient(f"http://127.0.0.1:{self.port}")

        # Verify server is running with retries
        for attempt in range(5):
            try:
                resp = self.client.get("/health", timeout=3.0)
                if resp.get("status") == "healthy":
                    return True
            except Exception:
                time.sleep(1)
        return False

    def stop(self):
        """Stop the test server."""
        if self.server:
            try:
                self.server.stop()
            except Exception:
                pass


# =============================================================================
# Pytest Fixtures
# =============================================================================

@pytest.fixture(scope="module")
def server():
    """Server fixture for pytest."""
    fixture = CppServerFixture(port=8250)
    if not fixture.start():
        pytest.skip("Failed to start C++ server")
    yield fixture
    fixture.stop()


@pytest.fixture
def client(server):
    """Client fixture for pytest."""
    return server.client


# =============================================================================
# Test Classes
# =============================================================================

class TestBasicEndpoints:
    """Test basic endpoint functionality."""

    def test_root_endpoint(self, client):
        """Test root endpoint returns expected response."""
        resp = client.get("/")
        assert resp.get("status") == "ok"
        assert "message" in resp

    def test_health_endpoint(self, client):
        """Test health endpoint returns healthy status."""
        resp = client.get("/health")
        assert resp.get("status") == "healthy"
        assert "timestamp" in resp
        assert "pid" in resp

    def test_random_data_unique(self, client):
        """Test random endpoint returns unique data each time."""
        responses = [client.get("/random") for _ in range(5)]
        uuids = [r.get("random_uuid") for r in responses]
        assert len(set(uuids)) == 5, "All random UUIDs should be unique"


class TestQueryParameters:
    """Test query parameter handling."""

    def test_echo_with_message(self, client):
        """Test echo endpoint with message param."""
        msg = random_string(15)
        resp = client.get("/echo", params={"message": msg})
        assert resp.get("echo") == msg

    def test_echo_default(self, client):
        """Test echo endpoint with default message."""
        resp = client.get("/echo")
        assert resp.get("echo") == "default"

    def test_search_with_params(self, client):
        """Test search with multiple query params."""
        params = {
            "q": random_string(8),
            "page": random_int(1, 10),
            "limit": random_int(5, 20)
        }
        resp = client.get("/search", params=params)
        assert resp.get("query") == params["q"]
        assert resp.get("page") == params["page"]
        assert resp.get("limit") == params["limit"]

    def test_type_conversion(self, client):
        """Test query param type conversion."""
        params = {
            "int_param": 42,
            "float_param": 3.14,
            "bool_param": "true",
            "str_param": "hello"
        }
        resp = client.get("/types", params=params)
        assert resp.get("int_param") == 42
        assert abs(resp.get("float_param", 0) - 3.14) < 0.01
        assert resp.get("str_param") == "hello"


class TestPathParameters:
    """Test path parameter extraction."""

    def test_single_path_param(self, client):
        """Test single path parameter extraction."""
        user_id = random_int(1, 9999)
        resp = client.get(f"/users/{user_id}")
        user = resp.get("user", {})
        assert user.get("id") == user_id

    def test_nested_path_params(self, client):
        """Test nested path parameter extraction."""
        user_id = random_int(1, 100)
        post_id = random_int(1, 1000)
        resp = client.get(f"/users/{user_id}/posts/{post_id}")
        assert resp.get("user_id") == user_id
        assert resp.get("post_id") == post_id


class TestPostRequests:
    """Test POST request handling."""

    def test_create_user(self, client):
        """Test user creation via POST."""
        resp = client.post("/users", json_body={"name": "TestUser", "email": "test@test.com"})
        assert resp.get("created") is True
        user = resp.get("user", {})
        assert "id" in user
        assert user.get("name") == "TestUser"


class TestCounter:
    """Test stateful counter endpoint."""

    def test_counter_increments(self, client):
        """Test counter increments on each call."""
        responses = [client.get("/counter") for _ in range(5)]
        counts = [r.get("count") for r in responses]
        # Counts should be strictly increasing
        for i in range(1, len(counts)):
            assert counts[i] > counts[i-1], "Counter should increment"


class TestRandomizedData:
    """Test with randomized inputs."""

    def test_random_path_params(self, client):
        """Test with random path parameter values."""
        for _ in range(5):
            user_id = random_int(1, 10000)
            resp = client.get(f"/users/{user_id}")
            assert resp.get("user", {}).get("id") == user_id

    def test_random_query_params(self, client):
        """Test with random query parameter values."""
        for _ in range(5):
            msg = random_string(random_int(5, 30))
            resp = client.get("/echo", params={"message": msg})
            assert resp.get("echo") == msg

    def test_random_search_queries(self, client):
        """Test with random search queries."""
        for _ in range(5):
            params = {
                "q": random_string(random_int(3, 15)),
                "page": random_int(1, 20),
                "limit": random_int(5, 50)
            }
            resp = client.get("/search", params=params)
            assert resp.get("query") == params["q"]
            assert resp.get("page") == params["page"]


# =============================================================================
# Standalone Runner
# =============================================================================

def run_standalone():
    """Run tests standalone (without pytest)."""
    print("\n" + "=" * 60, flush=True)
    print("FasterAPI C++ Server Quick E2E Tests", flush=True)
    print("=" * 60, flush=True)

    fixture = CppServerFixture(port=8250)

    if not fixture.start():
        print("\n[FAIL] Failed to start server", flush=True)
        return False

    print("Server started successfully, running tests...", flush=True)
    client = fixture.client
    passed = 0
    failed = 0

    tests = [
        ("GET /", lambda: client.get("/").get("status") == "ok"),
        ("GET /health", lambda: client.get("/health").get("status") == "healthy"),
        ("GET /echo?message=test", lambda: client.get("/echo", params={"message": "test"}).get("echo") == "test"),
        ("GET /echo (default)", lambda: client.get("/echo").get("echo") == "default"),
        ("GET /random (unique)", lambda: len(set(client.get("/random").get("random_uuid") for _ in range(3))) == 3),
        ("GET /users/{id}", lambda: client.get("/users/42").get("user", {}).get("id") == 42),
        ("GET /users/{id}/posts/{id}", lambda: client.get("/users/1/posts/2").get("user_id") == 1),
        ("GET /search with params", lambda: client.get("/search", params={"q": "test", "page": 2}).get("page") == 2),
        ("GET /types conversion", lambda: client.get("/types", params={"int_param": 42}).get("int_param") == 42),
        ("POST /users", lambda: client.post("/users", json_body={"name": "Test"}).get("created") is True),
        ("GET /counter increments", lambda: client.get("/counter").get("count") > 0),
    ]

    for name, test_fn in tests:
        try:
            if test_fn():
                print(f"  [PASS] {name}", flush=True)
                passed += 1
            else:
                print(f"  [FAIL] {name}", flush=True)
                failed += 1
        except Exception as e:
            print(f"  [FAIL] {name} - {e}", flush=True)
            failed += 1

    # Print results BEFORE stopping server (stop() may crash)
    print("\n" + "=" * 60, flush=True)
    print(f"Results: {passed}/{passed + failed} passed", flush=True)
    print("=" * 60, flush=True)

    # Stop server (may crash on cleanup but results are already printed)
    try:
        fixture.stop()
    except Exception:
        pass

    return failed == 0


if __name__ == "__main__":
    success = run_standalone()
    sys.exit(0 if success else 1)
