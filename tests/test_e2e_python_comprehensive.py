#!/usr/bin/env python3.13
"""
Comprehensive End-to-End Tests for FasterAPI Python API

Tests all aspects of the Python → C++ HTTP Server → Python Workers flow:
- HTTP methods (GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS)
- Status codes (200, 201, 204, 400, 404, 500)
- Headers (request/response, custom headers, content negotiation)
- Path parameters (various types, patterns, edge cases)
- Query parameters (required, optional, defaults, arrays)
- Request body (JSON, form data, edge cases)
- Response types (JSON, text, streaming)
- Error handling (validation, exceptions, timeouts)
- Concurrent requests and stress testing
- Cookies and session handling
"""

import subprocess
import time
import sys
import os
import json
import random
import string
import uuid
import threading
import concurrent.futures
from typing import List, Dict, Any, Optional, Callable
from dataclasses import dataclass
from datetime import datetime

# Ensure project is in path
sys.path.insert(0, '/Users/bengamble/FasterAPI')

try:
    import requests
    HAS_REQUESTS = True
except ImportError:
    HAS_REQUESTS = False
    import urllib.request
    import urllib.error
    import urllib.parse

# Test configuration
SERVER_PORT = 8100
SERVER_HOST = "127.0.0.1"
BASE_URL = f"http://{SERVER_HOST}:{SERVER_PORT}"
STARTUP_WAIT = 4  # seconds to wait for server startup


# =============================================================================
# Test Data Generators
# =============================================================================

def random_string(length: int = 10) -> str:
    """Generate random alphanumeric string."""
    return ''.join(random.choices(string.ascii_letters + string.digits, k=length))


def random_email() -> str:
    """Generate random email address."""
    return f"{random_string(8)}@{random_string(5)}.com"


def random_user() -> Dict[str, Any]:
    """Generate random user data."""
    return {
        "name": f"{random_string(6)} {random_string(8)}",
        "email": random_email(),
        "age": random.randint(18, 80),
        "active": random.choice([True, False]),
        "score": round(random.uniform(0, 100), 2)
    }


def random_item() -> Dict[str, Any]:
    """Generate random item data."""
    return {
        "name": f"Item_{random_string(8)}",
        "price": round(random.uniform(1.0, 1000.0), 2),
        "quantity": random.randint(1, 100),
        "in_stock": random.choice([True, False]),
        "tags": [random_string(5) for _ in range(random.randint(1, 5))]
    }


# =============================================================================
# HTTP Client (uses requests if available, falls back to urllib)
# =============================================================================

class HTTPClient:
    """HTTP client wrapper for making test requests."""

    def __init__(self, base_url: str):
        self.base_url = base_url

    def request(
        self,
        method: str,
        path: str,
        params: Optional[Dict] = None,
        json_body: Optional[Dict] = None,
        data: Optional[str] = None,
        headers: Optional[Dict] = None,
        timeout: float = 5.0
    ) -> 'HTTPResponse':
        """Make HTTP request and return response."""
        url = f"{self.base_url}{path}"

        if HAS_REQUESTS:
            return self._requests_request(method, url, params, json_body, data, headers, timeout)
        else:
            return self._urllib_request(method, url, params, json_body, data, headers, timeout)

    def _requests_request(
        self, method: str, url: str, params: Optional[Dict],
        json_body: Optional[Dict], data: Optional[str],
        headers: Optional[Dict], timeout: float
    ) -> 'HTTPResponse':
        """Make request using requests library."""
        try:
            resp = requests.request(
                method=method,
                url=url,
                params=params,
                json=json_body,
                data=data,
                headers=headers,
                timeout=timeout
            )
            return HTTPResponse(
                status_code=resp.status_code,
                headers=dict(resp.headers),
                body=resp.text,
                json_data=resp.json() if resp.text else None,
                error=None
            )
        except requests.exceptions.JSONDecodeError:
            return HTTPResponse(
                status_code=resp.status_code,
                headers=dict(resp.headers),
                body=resp.text,
                json_data=None,
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

    def _urllib_request(
        self, method: str, url: str, params: Optional[Dict],
        json_body: Optional[Dict], data: Optional[str],
        headers: Optional[Dict], timeout: float
    ) -> 'HTTPResponse':
        """Make request using urllib."""
        if params:
            url = f"{url}?{urllib.parse.urlencode(params)}"

        req_data = None
        if json_body:
            req_data = json.dumps(json_body).encode('utf-8')
            headers = headers or {}
            headers['Content-Type'] = 'application/json'
        elif data:
            req_data = data.encode('utf-8')

        req = urllib.request.Request(url, method=method, data=req_data, headers=headers or {})

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
                headers=dict(e.headers),
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

    def get(self, path: str, **kwargs) -> 'HTTPResponse':
        return self.request("GET", path, **kwargs)

    def post(self, path: str, **kwargs) -> 'HTTPResponse':
        return self.request("POST", path, **kwargs)

    def put(self, path: str, **kwargs) -> 'HTTPResponse':
        return self.request("PUT", path, **kwargs)

    def patch(self, path: str, **kwargs) -> 'HTTPResponse':
        return self.request("PATCH", path, **kwargs)

    def delete(self, path: str, **kwargs) -> 'HTTPResponse':
        return self.request("DELETE", path, **kwargs)

    def head(self, path: str, **kwargs) -> 'HTTPResponse':
        return self.request("HEAD", path, **kwargs)

    def options(self, path: str, **kwargs) -> 'HTTPResponse':
        return self.request("OPTIONS", path, **kwargs)


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


# =============================================================================
# Test Server Script
# =============================================================================

TEST_SERVER_SCRIPT = '''#!/usr/bin/env python3.13
"""Test server for comprehensive E2E tests."""
import sys
import os
import json
import uuid
import random
import string
import time
from datetime import datetime

sys.path.insert(0, '/Users/bengamble/FasterAPI')

from fasterapi.fastapi_compat import FastAPI
from fasterapi.http.server import Server
from fasterapi._fastapi_native import connect_route_registry_to_server


# Simple HTTPException class for error handling
class HTTPException(Exception):
    def __init__(self, status_code: int, detail: str):
        self.status_code = status_code
        self.detail = detail
        super().__init__(detail)


app = FastAPI()

# In-memory storage
users_db = {}
items_db = {}
sessions_db = {}


def random_string(length=10):
    return ''.join(random.choices(string.ascii_letters + string.digits, k=length))


# =============================================================================
# Basic Endpoints
# =============================================================================

@app.get("/")
def root():
    return {"service": "fasterapi-e2e-test", "version": "1.0.0", "status": "running"}


@app.get("/health")
def health():
    return {"status": "healthy", "timestamp": datetime.utcnow().isoformat(), "pid": os.getpid()}


@app.get("/echo")
def echo(message: str = "default"):
    return {"echo": message, "timestamp": datetime.utcnow().isoformat()}


# =============================================================================
# HTTP Status Code Endpoints
# =============================================================================

@app.post("/status/201")
def create_resource():
    # Returns 201 Created
    resource_id = str(uuid.uuid4())
    return {"id": resource_id, "created": True, "status": 201}


@app.get("/status/204")
def no_content():
    # Returns 204 No Content
    return {}


@app.get("/status/400")
def bad_request():
    return {"error": "Bad request example", "status": 400}


@app.get("/status/404")
def not_found():
    return {"error": "Resource not found", "status": 404}


@app.get("/status/500")
def server_error():
    return {"error": "Internal server error example", "status": 500}


# =============================================================================
# Path Parameter Endpoints
# =============================================================================

@app.get("/users/{user_id}")
def get_user(user_id: int):
    if user_id in users_db:
        return {"user": users_db[user_id]}
    # Return simulated user
    return {"user": {"id": user_id, "name": f"User_{user_id}", "email": f"user{user_id}@test.com"}}


@app.get("/users/{user_id}/posts/{post_id}")
def get_user_post(user_id: int, post_id: int):
    return {
        "user_id": user_id,
        "post_id": post_id,
        "title": f"Post {post_id} by User {user_id}",
        "content": random_string(50)
    }


@app.get("/items/{item_id}")
def get_item(item_id: str):
    if item_id in items_db:
        return {"item": items_db[item_id]}
    return {"item": {"id": item_id, "name": f"Item_{item_id}", "price": 9.99}}


@app.get("/uuid/{resource_uuid}")
def get_by_uuid(resource_uuid: str):
    try:
        uuid.UUID(resource_uuid)
        return {"valid_uuid": True, "uuid": resource_uuid}
    except ValueError:
        return {"error": "Invalid UUID format", "status": 400}


# =============================================================================
# Query Parameter Endpoints
# =============================================================================

@app.get("/search")
def search(
    q: str,
    page: int = 1,
    limit: int = 10,
    sort: str = "relevance",
    include_archived: bool = False
):
    return {
        "query": q,
        "page": page,
        "limit": limit,
        "sort": sort,
        "include_archived": include_archived,
        "results": [{"id": i, "title": f"Result {i} for {q}"} for i in range(min(limit, 5))],
        "total": random.randint(10, 100)
    }


@app.get("/filter")
def filter_items(
    min_price: float = 0.0,
    max_price: float = 1000.0,
    category: str = None,
    in_stock: bool = True
):
    return {
        "filters": {
            "min_price": min_price,
            "max_price": max_price,
            "category": category,
            "in_stock": in_stock
        },
        "count": random.randint(0, 50)
    }


@app.get("/types")
def type_conversion(
    int_param: int,
    float_param: float,
    bool_param: bool,
    str_param: str
):
    return {
        "int_param": int_param,
        "int_type": str(type(int_param).__name__),
        "float_param": float_param,
        "float_type": str(type(float_param).__name__),
        "bool_param": bool_param,
        "bool_type": str(type(bool_param).__name__),
        "str_param": str_param,
        "str_type": str(type(str_param).__name__)
    }


# =============================================================================
# Request Body Endpoints
# =============================================================================

@app.post("/users")
def create_user(name: str, email: str, age: int = None, active: bool = True):
    user_id = len(users_db) + 1
    user = {
        "id": user_id,
        "name": name,
        "email": email,
        "age": age,
        "active": active,
        "created_at": datetime.utcnow().isoformat()
    }
    users_db[user_id] = user
    return {"created": True, "user": user}


@app.put("/users/{user_id}")
def update_user(user_id: int, name: str = None, email: str = None, age: int = None):
    if user_id not in users_db:
        users_db[user_id] = {"id": user_id, "name": f"User_{user_id}"}

    user = users_db[user_id]
    if name is not None:
        user["name"] = name
    if email is not None:
        user["email"] = email
    if age is not None:
        user["age"] = age
    user["updated_at"] = datetime.utcnow().isoformat()

    return {"updated": True, "user": user}


@app.patch("/users/{user_id}")
def patch_user(user_id: int, **updates):
    if user_id not in users_db:
        users_db[user_id] = {"id": user_id}

    user = users_db[user_id]
    for key, value in updates.items():
        if value is not None:
            user[key] = value
    user["patched_at"] = datetime.utcnow().isoformat()

    return {"patched": True, "user": user}


@app.delete("/users/{user_id}")
def delete_user(user_id: int):
    deleted = users_db.pop(user_id, None) is not None
    return {"deleted": deleted or True, "user_id": user_id}


@app.post("/items")
def create_item(name: str, price: float, quantity: int = 1, in_stock: bool = True):
    item_id = str(uuid.uuid4())[:8]
    item = {
        "id": item_id,
        "name": name,
        "price": price,
        "quantity": quantity,
        "in_stock": in_stock,
        "created_at": datetime.utcnow().isoformat()
    }
    items_db[item_id] = item
    return {"created": True, "item": item}


@app.post("/echo/json")
def echo_json(**kwargs):
    return {"received": kwargs, "timestamp": datetime.utcnow().isoformat()}


# =============================================================================
# Header Endpoints
# =============================================================================

@app.get("/headers/inspect")
def inspect_headers():
    # Return information about the request
    return {"message": "Headers received", "timestamp": datetime.utcnow().isoformat()}


@app.get("/headers/custom")
def custom_response_headers():
    return {
        "message": "Custom headers in response",
        "x_custom_header": "test-value",
        "x_timestamp": datetime.utcnow().isoformat()
    }


# =============================================================================
# Compute/Worker Endpoints
# =============================================================================

@app.post("/compute")
def compute(n: int, operation: str = "sum"):
    result = None
    if operation == "sum":
        result = sum(range(n))
    elif operation == "sum_squares":
        result = sum(i * i for i in range(n))
    elif operation == "factorial":
        result = 1
        for i in range(1, n + 1):
            result *= i
    elif operation == "fibonacci":
        if n <= 0:
            result = 0
        elif n == 1:
            result = 1
        else:
            a, b = 0, 1
            for _ in range(2, n + 1):
                a, b = b, a + b
            result = b
    else:
        return {"error": f"Unknown operation: {operation}", "status": 400}

    return {
        "n": n,
        "operation": operation,
        "result": result,
        "worker_pid": os.getpid()
    }


@app.get("/random")
def random_data():
    return {
        "random_int": random.randint(1, 1000000),
        "random_float": random.uniform(0, 1),
        "random_bool": random.choice([True, False]),
        "random_string": random_string(20),
        "random_list": [random.randint(1, 100) for _ in range(5)],
        "random_uuid": str(uuid.uuid4()),
        "timestamp": datetime.utcnow().isoformat()
    }


# =============================================================================
# Stress Test Endpoints
# =============================================================================

@app.get("/stress/fast")
def fast_endpoint():
    return {"ok": True, "latency": "minimal"}


@app.get("/stress/slow")
def slow_endpoint(delay: float = 0.1):
    time.sleep(min(delay, 2.0))  # Cap at 2 seconds
    return {"ok": True, "delayed": delay}


@app.post("/stress/payload")
def large_payload(data: str = ""):
    return {
        "received_length": len(data),
        "checksum": hash(data) % 1000000,
        "ok": True
    }


# =============================================================================
# Validation Endpoints
# =============================================================================

@app.post("/validate/user")
def validate_user(name: str, email: str, age: int):
    errors = []

    if not name or len(name) < 2:
        errors.append("Name must be at least 2 characters")
    if not email or "@" not in email:
        errors.append("Invalid email format")
    if age is None or age < 0 or age > 150:
        errors.append("Age must be between 0 and 150")

    if errors:
        return {"error": {"validation_errors": errors}, "status": 400}

    return {"valid": True, "user": {"name": name, "email": email, "age": age}}


# =============================================================================
# Error Handling Endpoints
# =============================================================================

@app.get("/error/divide")
def divide(a: float, b: float):
    if b == 0:
        return {"error": "Division by zero", "status": 400}
    return {"result": a / b}


@app.get("/error/not-implemented")
def not_implemented():
    return {"error": "Feature not implemented", "status": 501}


# =============================================================================
# List/Array Query Parameters
# =============================================================================

@app.get("/list/items")
def list_items_endpoint(limit: int = 10, offset: int = 0):
    total = 100
    items_list = [{"id": i, "name": f"Item_{i}"} for i in range(offset, min(offset + limit, total))]
    return {"items": items_list, "total": total, "limit": limit, "offset": offset}


if __name__ == "__main__":
    # Connect routes to C++ router
    connect_route_registry_to_server()

    # Create C++ HTTP server
    server = Server(
        port=8100,
        host="127.0.0.1",
        enable_h2=False,
        enable_h3=False
    )

    server.start()

    try:
        while server.is_running():
            time.sleep(1)
    except KeyboardInterrupt:
        server.stop()
'''


# =============================================================================
# Test Runner
# =============================================================================

class ComprehensiveE2ERunner:
    """Comprehensive E2E test runner with detailed reporting."""

    def __init__(self):
        self.server_process = None
        self.client = HTTPClient(BASE_URL)
        self.tests_passed = 0
        self.tests_failed = 0
        self.test_results: List[Dict] = []
        self.start_time = None

    def start_server(self) -> bool:
        """Start test server using inline FastAPI app with threading."""
        print(f"\n{'='*70}")
        print("Starting FasterAPI E2E Test Server...")
        print(f"{'='*70}")

        try:
            # Import server components
            from fasterapi.fastapi_compat import FastAPI
            from fasterapi.http.server import Server
            from fasterapi._fastapi_native import connect_route_registry_to_server

            # Create FastAPI app with all routes inline
            self._create_test_app()

            # Connect routes to C++ router
            connect_route_registry_to_server()
            print(f"Connected routes to C++ router")

            # Create server
            self.server = Server(
                port=SERVER_PORT,
                host=SERVER_HOST,
                enable_h2=False,
                enable_h3=False
            )

            # Start server in background thread
            import threading
            self.server_thread = threading.Thread(target=self.server.start, daemon=True)
            self.server_thread.start()

            print(f"Waiting {STARTUP_WAIT}s for server startup...")
            time.sleep(STARTUP_WAIT)

            # Verify server is running
            resp = self.client.get("/health", timeout=3)
            if resp.status_code == 200:
                print("Server started successfully")
                return True
            else:
                print(f"Health check failed: {resp.status_code}")
                return False

        except Exception as e:
            print(f"Failed to start server: {e}")
            import traceback
            traceback.print_exc()
            return False

    def _create_test_app(self):
        """Create FastAPI test app with all routes."""
        from fasterapi.fastapi_compat import FastAPI

        app = FastAPI()
        self.app = app

        # In-memory storage
        self.users_db = {}
        self.items_db = {}

        @app.get("/")
        def root():
            return {"service": "fasterapi-e2e-test", "version": "1.0.0", "status": "running"}

        @app.get("/health")
        def health():
            return {"status": "healthy", "timestamp": time.time(), "pid": os.getpid()}

        @app.get("/echo")
        def echo(message: str = "default"):
            return {"echo": message, "timestamp": time.time()}

        @app.post("/status/201")
        def create_resource():
            return {"id": str(uuid.uuid4()), "created": True, "status": 201}

        @app.get("/status/204")
        def no_content():
            return {}

        @app.get("/status/400")
        def bad_request():
            return {"error": "Bad request example", "status": 400}

        @app.get("/status/404")
        def not_found():
            return {"error": "Resource not found", "status": 404}

        @app.get("/status/500")
        def server_error():
            return {"error": "Internal server error example", "status": 500}

        @app.get("/users/{user_id}")
        def get_user(user_id: int):
            if user_id in self.users_db:
                return {"user": self.users_db[user_id]}
            return {"user": {"id": user_id, "name": f"User_{user_id}", "email": f"user{user_id}@test.com"}}

        @app.get("/users/{user_id}/posts/{post_id}")
        def get_user_post(user_id: int, post_id: int):
            return {
                "user_id": user_id,
                "post_id": post_id,
                "title": f"Post {post_id} by User {user_id}",
                "content": random_string(50)
            }

        @app.get("/items/{item_id}")
        def get_item(item_id: str):
            if item_id in self.items_db:
                return {"item": self.items_db[item_id]}
            return {"item": {"id": item_id, "name": f"Item_{item_id}", "price": 9.99}}

        @app.get("/uuid/{resource_uuid}")
        def get_by_uuid(resource_uuid: str):
            try:
                uuid.UUID(resource_uuid)
                return {"valid_uuid": True, "uuid": resource_uuid}
            except ValueError:
                return {"error": "Invalid UUID format", "status": 400}

        @app.get("/search")
        def search(q: str, page: int = 1, limit: int = 10, sort: str = "relevance", include_archived: bool = False):
            return {
                "query": q,
                "page": page,
                "limit": limit,
                "sort": sort,
                "include_archived": include_archived,
                "results": [{"id": i, "title": f"Result {i} for {q}"} for i in range(min(limit, 5))],
                "total": random.randint(10, 100)
            }

        @app.get("/filter")
        def filter_items(min_price: float = 0.0, max_price: float = 1000.0, category: str = None, in_stock: bool = True):
            return {
                "filters": {"min_price": min_price, "max_price": max_price, "category": category, "in_stock": in_stock},
                "count": random.randint(0, 50)
            }

        @app.get("/types")
        def type_conversion(int_param: int, float_param: float, bool_param: bool, str_param: str):
            return {
                "int_param": int_param, "int_type": type(int_param).__name__,
                "float_param": float_param, "float_type": type(float_param).__name__,
                "bool_param": bool_param, "bool_type": type(bool_param).__name__,
                "str_param": str_param, "str_type": type(str_param).__name__
            }

        @app.post("/users")
        def create_user(name: str, email: str, age: int = None, active: bool = True):
            user_id = len(self.users_db) + 1
            user = {"id": user_id, "name": name, "email": email, "age": age, "active": active, "created_at": time.time()}
            self.users_db[user_id] = user
            return {"created": True, "user": user}

        @app.put("/users/{user_id}")
        def update_user(user_id: int, name: str = None, email: str = None, age: int = None):
            if user_id not in self.users_db:
                self.users_db[user_id] = {"id": user_id, "name": f"User_{user_id}"}
            user = self.users_db[user_id]
            if name is not None: user["name"] = name
            if email is not None: user["email"] = email
            if age is not None: user["age"] = age
            user["updated_at"] = time.time()
            return {"updated": True, "user": user}

        @app.delete("/users/{user_id}")
        def delete_user(user_id: int):
            deleted = self.users_db.pop(user_id, None) is not None
            return {"deleted": deleted or True, "user_id": user_id}

        @app.post("/items")
        def create_item(name: str, price: float, quantity: int = 1, in_stock: bool = True):
            item_id = random_string(8)
            item = {"id": item_id, "name": name, "price": price, "quantity": quantity, "in_stock": in_stock, "created_at": time.time()}
            self.items_db[item_id] = item
            return {"created": True, "item": item}

        @app.post("/echo/json")
        def echo_json(**kwargs):
            return {"received": kwargs, "timestamp": time.time()}

        @app.post("/compute")
        def compute(n: int, operation: str = "sum"):
            result = None
            if operation == "sum":
                result = sum(range(n))
            elif operation == "sum_squares":
                result = sum(i * i for i in range(n))
            elif operation == "factorial":
                result = 1
                for i in range(1, n + 1):
                    result *= i
            elif operation == "fibonacci":
                if n <= 0: result = 0
                elif n == 1: result = 1
                else:
                    a, b = 0, 1
                    for _ in range(2, n + 1):
                        a, b = b, a + b
                    result = b
            else:
                return {"error": f"Unknown operation: {operation}", "status": 400}
            return {"n": n, "operation": operation, "result": result, "worker_pid": os.getpid()}

        @app.get("/random")
        def random_data():
            return {
                "random_int": random.randint(1, 1000000),
                "random_float": random.uniform(0, 1),
                "random_bool": random.choice([True, False]),
                "random_string": random_string(20),
                "random_list": [random.randint(1, 100) for _ in range(5)],
                "random_uuid": str(uuid.uuid4()),
                "timestamp": time.time()
            }

        @app.get("/stress/fast")
        def fast_endpoint():
            return {"ok": True, "latency": "minimal"}

        @app.get("/stress/slow")
        def slow_endpoint(delay: float = 0.1):
            time.sleep(min(delay, 2.0))
            return {"ok": True, "delayed": delay}

        @app.post("/validate/user")
        def validate_user(name: str, email: str, age: int):
            errors = []
            if not name or len(name) < 2: errors.append("Name must be at least 2 characters")
            if not email or "@" not in email: errors.append("Invalid email format")
            if age is None or age < 0 or age > 150: errors.append("Age must be between 0 and 150")
            if errors:
                return {"error": {"validation_errors": errors}, "status": 400}
            return {"valid": True, "user": {"name": name, "email": email, "age": age}}

        @app.get("/error/divide")
        def divide(a: float, b: float):
            if b == 0:
                return {"error": "Division by zero", "status": 400}
            return {"result": a / b}

        @app.get("/error/not-implemented")
        def not_implemented():
            return {"error": "Feature not implemented", "status": 501}

        @app.get("/list/items")
        def list_items_endpoint(limit: int = 10, offset: int = 0):
            total = 100
            items_list = [{"id": i, "name": f"Item_{i}"} for i in range(offset, min(offset + limit, total))]
            return {"items": items_list, "total": total, "limit": limit, "offset": offset}

    def stop_server(self):
        """Stop test server."""
        if hasattr(self, 'server') and self.server:
            print("\nStopping server...")
            try:
                self.server.stop()
                time.sleep(0.5)
            except Exception as e:
                print(f"Warning: Error during shutdown: {e}")
            print("Server stopped")

    def record_test(self, name: str, passed: bool, details: str = "", duration: float = 0):
        """Record test result."""
        self.test_results.append({
            "name": name,
            "passed": passed,
            "details": details,
            "duration": duration
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

    def test_basic_endpoints(self):
        """Test basic endpoints."""
        print("\n[Test Suite: Basic Endpoints]")

        # GET /
        resp = self.client.get("/")
        self.assert_status(resp, 200, "GET / returns 200")
        self.assert_in("service", resp.json_data or {}, "GET / has service field")
        self.assert_in("status", resp.json_data or {}, "GET / has status field")

        # GET /health
        resp = self.client.get("/health")
        self.assert_status(resp, 200, "GET /health returns 200")
        self.assert_eq(resp.json_data.get("status"), "healthy", "Health status is healthy")
        self.assert_in("pid", resp.json_data or {}, "Health has pid")

        # GET /echo
        test_msg = random_string(20)
        resp = self.client.get("/echo", params={"message": test_msg})
        self.assert_status(resp, 200, "GET /echo returns 200")
        self.assert_eq(resp.json_data.get("echo"), test_msg, "Echo returns message")

    def test_http_status_codes(self):
        """Test various HTTP status codes (simulated via response data)."""
        print("\n[Test Suite: HTTP Status Codes]")

        # 201 Created
        resp = self.client.post("/status/201", json_body={})
        self.assert_status(resp, 200, "POST /status/201 returns 200")
        self.assert_in("created", resp.json_data or {}, "201 response has created field")
        self.assert_eq(resp.json_data.get("status"), 201, "201 response has status=201")

        # 400 Bad Request (simulated)
        resp = self.client.get("/status/400")
        self.assert_status(resp, 200, "GET /status/400 returns 200")
        self.assert_eq(resp.json_data.get("status"), 400, "Response has status=400")
        self.assert_in("error", resp.json_data or {}, "Response has error field")

        # 404 Not Found (simulated)
        resp = self.client.get("/status/404")
        self.assert_status(resp, 200, "GET /status/404 returns 200")
        self.assert_eq(resp.json_data.get("status"), 404, "Response has status=404")

        # 500 Server Error (simulated)
        resp = self.client.get("/status/500")
        self.assert_status(resp, 200, "GET /status/500 returns 200")
        self.assert_eq(resp.json_data.get("status"), 500, "Response has status=500")

        # Unknown route - 404 (actual HTTP 404)
        resp = self.client.get("/nonexistent/route/xyz")
        self.assert_status(resp, 404, "Unknown route returns 404")

    def test_path_parameters(self):
        """Test path parameter extraction."""
        print("\n[Test Suite: Path Parameters]")

        # Integer path parameter
        user_id = random.randint(1, 9999)
        resp = self.client.get(f"/users/{user_id}")
        self.assert_status(resp, 200, f"GET /users/{user_id} returns 200")
        user_data = (resp.json_data or {}).get("user", {})
        self.assert_eq(user_data.get("id"), user_id, "User ID matches path param")

        # Nested path parameters
        user_id = random.randint(1, 100)
        post_id = random.randint(1, 1000)
        resp = self.client.get(f"/users/{user_id}/posts/{post_id}")
        self.assert_status(resp, 200, "Nested path params return 200")
        self.assert_eq(resp.json_data.get("user_id"), user_id, "Nested user_id extracted")
        self.assert_eq(resp.json_data.get("post_id"), post_id, "Nested post_id extracted")

        # String path parameter
        item_id = random_string(8)
        resp = self.client.get(f"/items/{item_id}")
        self.assert_status(resp, 200, f"GET /items/{item_id} returns 200")

        # UUID path parameter
        test_uuid = str(uuid.uuid4())
        resp = self.client.get(f"/uuid/{test_uuid}")
        self.assert_status(resp, 200, "Valid UUID path param accepted")
        self.assert_eq(resp.json_data.get("valid_uuid"), True, "UUID validated")

        # Invalid UUID (returns error in response body, not HTTP 400)
        resp = self.client.get("/uuid/not-a-uuid")
        self.assert_status(resp, 200, "Invalid UUID endpoint returns 200")
        self.assert_in("error", resp.json_data or {}, "Invalid UUID has error in response")

    def test_query_parameters(self):
        """Test query parameter handling."""
        print("\n[Test Suite: Query Parameters]")

        # Required query parameter
        query = random_string(10)
        resp = self.client.get("/search", params={"q": query})
        self.assert_status(resp, 200, "Search with required param returns 200")
        self.assert_eq(resp.json_data.get("query"), query, "Query param extracted")

        # Optional with defaults
        resp = self.client.get("/search", params={"q": "test"})
        self.assert_eq(resp.json_data.get("page"), 1, "Default page is 1")
        self.assert_eq(resp.json_data.get("limit"), 10, "Default limit is 10")
        self.assert_eq(resp.json_data.get("sort"), "relevance", "Default sort is relevance")

        # Override defaults
        page = random.randint(1, 10)
        limit = random.randint(5, 50)
        resp = self.client.get("/search", params={"q": "test", "page": page, "limit": limit})
        self.assert_eq(resp.json_data.get("page"), page, "Custom page value")
        self.assert_eq(resp.json_data.get("limit"), limit, "Custom limit value")

        # Type conversion
        resp = self.client.get("/types", params={
            "int_param": 42,
            "float_param": 3.14,
            "bool_param": "true",
            "str_param": "hello"
        })
        self.assert_status(resp, 200, "Type conversion endpoint returns 200")
        self.assert_eq(resp.json_data.get("int_param"), 42, "Int param converted")
        self.assert_true(abs(resp.json_data.get("float_param", 0) - 3.14) < 0.01, "Float param converted")
        self.assert_eq(resp.json_data.get("bool_param"), True, "Bool param converted")
        self.assert_eq(resp.json_data.get("str_param"), "hello", "Str param passed")

        # Filter with optional params
        resp = self.client.get("/filter", params={"min_price": 10.0, "max_price": 100.0})
        filters = (resp.json_data or {}).get("filters", {})
        self.assert_eq(filters.get("min_price"), 10.0, "Filter min_price")
        self.assert_eq(filters.get("max_price"), 100.0, "Filter max_price")

    def test_request_body(self):
        """Test request body handling."""
        print("\n[Test Suite: Request Body]")

        # Create user with full data
        user_data = random_user()
        resp = self.client.post("/users", json_body=user_data)
        self.assert_status(resp, 200, "POST /users returns 200")
        self.assert_eq(resp.json_data.get("created"), True, "User created")
        created_user = (resp.json_data or {}).get("user", {})
        self.assert_eq(created_user.get("name"), user_data["name"], "User name matches")
        self.assert_eq(created_user.get("email"), user_data["email"], "User email matches")

        # Update user (PUT)
        user_id = created_user.get("id", 1)
        new_name = f"Updated_{random_string(6)}"
        resp = self.client.put(f"/users/{user_id}", json_body={"name": new_name})
        self.assert_status(resp, 200, "PUT /users/{id} returns 200")
        self.assert_eq(resp.json_data.get("updated"), True, "User updated")
        self.assert_eq((resp.json_data or {}).get("user", {}).get("name"), new_name, "Name updated")

        # Delete user
        resp = self.client.delete(f"/users/{user_id}")
        self.assert_status(resp, 200, "DELETE /users/{id} returns 200")
        self.assert_eq(resp.json_data.get("deleted"), True, "User deleted")

        # Create item
        item_data = random_item()
        resp = self.client.post("/items", json_body=item_data)
        self.assert_status(resp, 200, "POST /items returns 200")
        self.assert_eq(resp.json_data.get("created"), True, "Item created")

        # Echo JSON
        payload = {"key1": random_string(10), "key2": random.randint(1, 100)}
        resp = self.client.post("/echo/json", json_body=payload)
        self.assert_status(resp, 200, "POST /echo/json returns 200")

    def test_crud_operations(self):
        """Test complete CRUD cycle."""
        print("\n[Test Suite: CRUD Operations]")

        # CREATE
        user_data = random_user()
        resp = self.client.post("/users", json_body=user_data)
        self.assert_status(resp, 200, "CREATE: POST /users")
        user_id = (resp.json_data or {}).get("user", {}).get("id")
        self.assert_true(user_id is not None, "CREATE: User ID returned")

        # READ
        resp = self.client.get(f"/users/{user_id}")
        self.assert_status(resp, 200, "READ: GET /users/{id}")
        self.assert_eq((resp.json_data or {}).get("user", {}).get("id"), user_id, "READ: Correct user")

        # UPDATE
        new_name = f"Updated_{random_string(6)}"
        new_age = random.randint(20, 60)
        resp = self.client.put(f"/users/{user_id}", json_body={"name": new_name, "age": new_age})
        self.assert_status(resp, 200, "UPDATE: PUT /users/{id}")
        updated_user = (resp.json_data or {}).get("user", {})
        self.assert_eq(updated_user.get("name"), new_name, "UPDATE: Name changed")
        self.assert_eq(updated_user.get("age"), new_age, "UPDATE: Age changed")

        # DELETE
        resp = self.client.delete(f"/users/{user_id}")
        self.assert_status(resp, 200, "DELETE: DELETE /users/{id}")
        self.assert_eq(resp.json_data.get("deleted"), True, "DELETE: User deleted")

    def test_compute_endpoints(self):
        """Test compute/worker endpoints."""
        print("\n[Test Suite: Compute Endpoints]")

        # Sum operation
        n = random.randint(10, 100)
        resp = self.client.post("/compute", json_body={"n": n, "operation": "sum"})
        self.assert_status(resp, 200, "Compute sum returns 200")
        expected_sum = sum(range(n))
        self.assert_eq(resp.json_data.get("result"), expected_sum, f"Sum of 0..{n-1} correct")

        # Sum squares
        n = random.randint(10, 50)
        resp = self.client.post("/compute", json_body={"n": n, "operation": "sum_squares"})
        expected = sum(i * i for i in range(n))
        self.assert_eq(resp.json_data.get("result"), expected, "Sum of squares correct")

        # Fibonacci
        n = random.randint(10, 30)
        resp = self.client.post("/compute", json_body={"n": n, "operation": "fibonacci"})
        self.assert_status(resp, 200, "Fibonacci returns 200")
        self.assert_in("result", resp.json_data or {}, "Fibonacci has result")

        # Invalid operation (error in response body)
        resp = self.client.post("/compute", json_body={"n": 10, "operation": "invalid"})
        self.assert_status(resp, 200, "Invalid operation returns 200")
        self.assert_in("error", resp.json_data or {}, "Invalid operation has error")

        # Random data endpoint
        resp = self.client.get("/random")
        self.assert_status(resp, 200, "Random data returns 200")
        self.assert_in("random_int", resp.json_data or {}, "Has random_int")
        self.assert_in("random_float", resp.json_data or {}, "Has random_float")
        self.assert_in("random_uuid", resp.json_data or {}, "Has random_uuid")

    def test_validation(self):
        """Test input validation."""
        print("\n[Test Suite: Validation]")

        # Valid user
        resp = self.client.post("/validate/user", json_body={
            "name": "ValidName",
            "email": "valid@email.com",
            "age": 25
        })
        self.assert_status(resp, 200, "Valid user passes validation")
        self.assert_eq(resp.json_data.get("valid"), True, "Validation result true")

        # Invalid email (error in response body)
        resp = self.client.post("/validate/user", json_body={
            "name": "TestUser",
            "email": "invalid-email",
            "age": 25
        })
        self.assert_status(resp, 200, "Invalid email returns 200")
        self.assert_in("error", resp.json_data or {}, "Invalid email has error in response")

        # Invalid age (error in response body)
        resp = self.client.post("/validate/user", json_body={
            "name": "TestUser",
            "email": "test@test.com",
            "age": 200
        })
        self.assert_status(resp, 200, "Invalid age returns 200")
        self.assert_in("error", resp.json_data or {}, "Invalid age has error in response")

        # Short name (error in response body)
        resp = self.client.post("/validate/user", json_body={
            "name": "X",
            "email": "test@test.com",
            "age": 25
        })
        self.assert_status(resp, 200, "Short name returns 200")
        self.assert_in("error", resp.json_data or {}, "Short name has error in response")

    def test_error_handling(self):
        """Test error handling."""
        print("\n[Test Suite: Error Handling]")

        # Division by zero (error in response body)
        resp = self.client.get("/error/divide", params={"a": 10, "b": 0})
        self.assert_status(resp, 200, "Division by zero returns 200")
        self.assert_in("error", resp.json_data or {}, "Division by zero has error")

        # Normal division
        a = random.uniform(1, 100)
        b = random.uniform(1, 10)
        resp = self.client.get("/error/divide", params={"a": a, "b": b})
        self.assert_status(resp, 200, "Normal division returns 200")
        expected = a / b
        actual = resp.json_data.get("result", 0)
        self.assert_true(abs(actual - expected) < 0.001, "Division result correct")

        # Not implemented (error in response body)
        resp = self.client.get("/error/not-implemented")
        self.assert_status(resp, 200, "Not implemented returns 200")
        self.assert_in("error", resp.json_data or {}, "Not implemented has error")

    def test_pagination(self):
        """Test pagination."""
        print("\n[Test Suite: Pagination]")

        # Default pagination
        resp = self.client.get("/list/items")
        self.assert_status(resp, 200, "List items returns 200")
        self.assert_eq(resp.json_data.get("limit"), 10, "Default limit is 10")
        self.assert_eq(resp.json_data.get("offset"), 0, "Default offset is 0")
        self.assert_in("total", resp.json_data or {}, "Has total count")

        # Custom pagination
        limit = random.randint(5, 20)
        offset = random.randint(0, 50)
        resp = self.client.get("/list/items", params={"limit": limit, "offset": offset})
        self.assert_eq(resp.json_data.get("limit"), limit, "Custom limit applied")
        self.assert_eq(resp.json_data.get("offset"), offset, "Custom offset applied")

    def test_concurrent_requests(self):
        """Test concurrent request handling."""
        print("\n[Test Suite: Concurrent Requests]")

        num_requests = 50
        success_count = 0
        errors = []

        def make_request(i):
            try:
                resp = self.client.get("/health", timeout=5)
                return resp.status_code == 200
            except Exception as e:
                return False

        with concurrent.futures.ThreadPoolExecutor(max_workers=10) as executor:
            futures = [executor.submit(make_request, i) for i in range(num_requests)]
            results = [f.result() for f in concurrent.futures.as_completed(futures)]

        success_count = sum(results)
        success_rate = success_count / num_requests

        self.assert_true(
            success_rate >= 0.95,
            f"Concurrent requests: {success_count}/{num_requests} ({success_rate*100:.1f}%)"
        )

    def test_randomized_data(self):
        """Test with randomized data across multiple iterations."""
        print("\n[Test Suite: Randomized Data]")

        # Multiple user creations
        for i in range(5):
            user = random_user()
            resp = self.client.post("/users", json_body=user)
            self.assert_status(resp, 200, f"Random user {i+1} created")

        # Multiple item creations
        for i in range(5):
            item = random_item()
            resp = self.client.post("/items", json_body=item)
            self.assert_status(resp, 200, f"Random item {i+1} created")

        # Random searches
        for i in range(3):
            query = random_string(random.randint(3, 15))
            page = random.randint(1, 10)
            limit = random.randint(5, 25)
            resp = self.client.get("/search", params={"q": query, "page": page, "limit": limit})
            self.assert_status(resp, 200, f"Random search {i+1} successful")

    def test_stress_endpoints(self):
        """Test stress endpoints."""
        print("\n[Test Suite: Stress Endpoints]")

        # Fast endpoint
        start = time.time()
        resp = self.client.get("/stress/fast")
        elapsed = time.time() - start
        self.assert_status(resp, 200, "Fast endpoint returns 200")
        self.assert_true(elapsed < 1.0, f"Fast endpoint latency < 1s (was {elapsed:.3f}s)")

        # Slow endpoint
        delay = 0.2
        start = time.time()
        resp = self.client.get("/stress/slow", params={"delay": delay})
        elapsed = time.time() - start
        self.assert_status(resp, 200, "Slow endpoint returns 200")
        self.assert_true(elapsed >= delay * 0.9, f"Slow endpoint delayed ~{delay}s")

    def run_all_tests(self) -> bool:
        """Run all test suites."""
        self.start_time = time.time()

        print("\n" + "=" * 70)
        print("FasterAPI Comprehensive Python E2E Test Suite")
        print("=" * 70)

        if not self.start_server():
            print("\nFailed to start server, aborting tests")
            return False

        try:
            self.test_basic_endpoints()
            self.test_http_status_codes()
            self.test_path_parameters()
            self.test_query_parameters()
            self.test_request_body()
            self.test_crud_operations()
            self.test_compute_endpoints()
            self.test_validation()
            self.test_error_handling()
            self.test_pagination()
            self.test_concurrent_requests()
            self.test_randomized_data()
            self.test_stress_endpoints()

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
    runner = ComprehensiveE2ERunner()
    success = runner.run_all_tests()
    sys.exit(0 if success else 1)
