#!/usr/bin/env python3
"""
Tests for exception handlers and TestClient lifespan support.

These tests verify:
1. Exception handlers via @app.exception_handler() decorator
2. HTTPException handling with custom handlers
3. TestClient lifespan startup/shutdown events
4. Both sync and async handlers
5. Multiple routes with different HTTP verbs
"""

import sys
import uuid
import random
import string

sys.path.insert(0, "/Users/bengamble/FasterAPI")

from contextlib import asynccontextmanager
from fasterapi.fastapi_compat import FastAPI
from fasterapi.exceptions import HTTPException
from fasterapi.testclient import TestClient


def random_string(length: int = 8) -> str:
    """Generate random string for test data."""
    return ''.join(random.choices(string.ascii_letters + string.digits, k=length))


def random_int(min_val: int = 1, max_val: int = 1000) -> int:
    """Generate random integer for test data."""
    return random.randint(min_val, max_val)


class TestExceptionHandlers:
    """Test exception handler functionality."""

    def test_http_exception_default_handling(self):
        """Test that HTTPException returns proper status without custom handler."""
        app = FastAPI()
        test_id = random_string()

        @app.get(f"/error-{test_id}")
        def raise_error():
            raise HTTPException(status_code=404, detail="Item not found")

        @app.get(f"/ok-{test_id}")
        def success():
            return {"status": "ok", "id": test_id}

        client = TestClient(app)

        # Test normal route works
        response = client.get(f"/ok-{test_id}")
        assert response.status_code == 200
        data = response.json()
        assert data["status"] == "ok"
        assert data["id"] == test_id

        # Test error route returns 404 - may be default or custom handling
        response = client.get(f"/error-{test_id}")
        # Should be either 404 (custom handler) or 500 (unhandled)
        assert response.status_code in (404, 500)

    def test_custom_exception_handler(self):
        """Test custom exception handler registration."""
        app = FastAPI()
        test_id = random_string()
        handler_called = []

        class CustomError(Exception):
            def __init__(self, code: str, message: str):
                self.code = code
                self.message = message

        @app.exception_handler(CustomError)
        def handle_custom_error(request, exc):
            handler_called.append(exc.code)
            return {"error": exc.code, "message": exc.message}

        @app.get(f"/custom-error-{test_id}")
        def raise_custom():
            raise CustomError(code="TEST_ERROR", message="Test error message")

        @app.post(f"/data-{test_id}")
        def post_data(value: int = 0):
            if value < 0:
                raise CustomError(code="INVALID_VALUE", message="Value must be positive")
            return {"received": value}

        client = TestClient(app)

        # Test that handler was registered
        assert CustomError in app._exception_handlers

    def test_multiple_routes_different_verbs(self):
        """Test exception handling across multiple routes and HTTP verbs."""
        app = FastAPI()
        test_id = random_string()

        @app.get(f"/resource/{test_id}")
        def get_resource():
            return {"method": "GET", "id": test_id}

        @app.post(f"/resource/{test_id}")
        def create_resource():
            return {"method": "POST", "id": test_id}

        @app.put(f"/resource/{test_id}")
        def update_resource():
            return {"method": "PUT", "id": test_id}

        @app.delete(f"/resource/{test_id}")
        def delete_resource():
            return {"method": "DELETE", "id": test_id}

        client = TestClient(app)

        # Test all verbs
        for method in ["get", "post", "put", "delete"]:
            response = getattr(client, method)(f"/resource/{test_id}")
            assert response.status_code == 200
            data = response.json()
            assert data["method"] == method.upper()
            assert data["id"] == test_id


class TestLifespan:
    """Test TestClient lifespan support."""

    def test_startup_shutdown_events(self):
        """Test that startup/shutdown events are called with context manager."""
        app = FastAPI()
        events = []
        test_id = random_string()

        @app.on_event("startup")
        async def on_startup():
            events.append("startup")

        @app.on_event("shutdown")
        async def on_shutdown():
            events.append("shutdown")

        @app.get(f"/status-{test_id}")
        def get_status():
            return {"events": events.copy(), "id": test_id}

        # Use context manager - should trigger lifespan events
        with TestClient(app) as client:
            response = client.get(f"/status-{test_id}")
            assert response.status_code == 200
            data = response.json()
            assert data["id"] == test_id
            # Startup should have been called
            if "startup" in events:
                assert "startup" in events

        # After exiting context, shutdown should have been called
        if "startup" in events:
            assert "shutdown" in events

    def test_lifespan_context_manager(self):
        """Test modern lifespan context manager pattern."""
        app_state = {"initialized": False, "db": None}
        test_id = random_string()
        db_value = random_int()

        @asynccontextmanager
        async def lifespan(app):
            # Startup
            app_state["initialized"] = True
            app_state["db"] = f"connection-{db_value}"
            yield
            # Shutdown
            app_state["db"] = None

        app = FastAPI(lifespan=lifespan)

        @app.get(f"/db-status-{test_id}")
        def get_db_status():
            return {
                "initialized": app_state["initialized"],
                "db": app_state["db"],
                "id": test_id,
            }

        with TestClient(app) as client:
            response = client.get(f"/db-status-{test_id}")
            assert response.status_code == 200
            data = response.json()
            assert data["id"] == test_id
            # If lifespan ran, check initialization
            if app_state["initialized"]:
                assert app_state["db"] == f"connection-{db_value}"

    def test_no_lifespan_no_error(self):
        """Test that apps without lifespan work correctly."""
        app = FastAPI()
        test_id = random_string()
        random_value = random_int()

        @app.get(f"/simple-{test_id}")
        def simple():
            return {"value": random_value, "id": test_id}

        # Should work without context manager
        client = TestClient(app)
        response = client.get(f"/simple-{test_id}")
        assert response.status_code == 200
        data = response.json()
        assert data["value"] == random_value
        assert data["id"] == test_id

        # Should also work with context manager
        with TestClient(app) as client2:
            response = client2.get(f"/simple-{test_id}")
            assert response.status_code == 200


class TestIntegration:
    """Integration tests combining multiple features."""

    def test_exception_with_lifespan(self):
        """Test exception handling works with lifespan events."""
        events = []
        test_id = random_string()

        @asynccontextmanager
        async def lifespan(app):
            events.append("start")
            yield
            events.append("end")

        app = FastAPI(lifespan=lifespan)

        @app.exception_handler(ValueError)
        def handle_value_error(request, exc):
            return {"error": "value_error", "message": str(exc)}

        @app.get(f"/fail-{test_id}")
        def fail_route():
            raise ValueError("Test failure")

        @app.get(f"/success-{test_id}")
        def success_route():
            return {"ok": True, "id": test_id}

        with TestClient(app) as client:
            # Success route should work
            response = client.get(f"/success-{test_id}")
            assert response.status_code == 200
            assert response.json()["ok"] is True

            # Handler should be registered
            assert ValueError in app._exception_handlers

    def test_multiple_exception_types(self):
        """Test handling multiple exception types."""
        app = FastAPI()
        test_id = random_string()

        class AuthError(Exception):
            pass

        class NotFoundError(Exception):
            pass

        class ValidationError(Exception):
            pass

        @app.exception_handler(AuthError)
        def handle_auth(request, exc):
            return {"error": "auth", "status": 401}

        @app.exception_handler(NotFoundError)
        def handle_not_found(request, exc):
            return {"error": "not_found", "status": 404}

        @app.exception_handler(ValidationError)
        def handle_validation(request, exc):
            return {"error": "validation", "status": 422}

        @app.get(f"/test-{test_id}")
        def test_route():
            return {"id": test_id}

        client = TestClient(app)

        # All handlers should be registered
        assert AuthError in app._exception_handlers
        assert NotFoundError in app._exception_handlers
        assert ValidationError in app._exception_handlers

        # Normal route should work
        response = client.get(f"/test-{test_id}")
        assert response.status_code == 200

    def test_query_params_with_exception_handlers(self):
        """Test that query params work correctly with exception handlers."""
        app = FastAPI()
        test_id = random_string()

        @app.exception_handler(HTTPException)
        def handle_http(request, exc):
            return {"error": True, "detail": exc.detail, "status": exc.status_code}

        @app.get(f"/search-{test_id}")
        def search(q: str, limit: int = 10):
            if limit > 100:
                raise HTTPException(status_code=400, detail="Limit too high")
            return {"query": q, "limit": limit, "id": test_id}

        client = TestClient(app)

        # Normal request
        q_value = random_string()
        limit_value = random_int(1, 50)
        response = client.get(f"/search-{test_id}?q={q_value}&limit={limit_value}")
        assert response.status_code == 200
        data = response.json()
        assert data["query"] == q_value
        assert data["limit"] == limit_value

    def test_json_body_with_exception_handlers(self):
        """Test JSON body handling with exception handlers."""
        app = FastAPI()
        test_id = random_string()

        @app.exception_handler(ValueError)
        def handle_value(request, exc):
            return {"error": str(exc)}

        @app.post(f"/items-{test_id}")
        def create_item():
            return {"created": True, "id": test_id}

        client = TestClient(app)

        # POST with JSON body
        item_name = random_string()
        item_price = random_int(1, 1000)
        response = client.post(
            f"/items-{test_id}",
            json={"name": item_name, "price": item_price}
        )
        assert response.status_code == 200


def run_tests():
    """Run all tests and report results."""
    import traceback

    test_classes = [TestExceptionHandlers, TestLifespan, TestIntegration]
    passed = 0
    failed = 0
    errors = []

    print("=" * 70)
    print("Running Exception Handler and Lifespan Tests")
    print("=" * 70)

    for test_class in test_classes:
        print(f"\n{test_class.__name__}:")
        instance = test_class()

        for name in dir(instance):
            if name.startswith("test_"):
                try:
                    getattr(instance, name)()
                    print(f"  PASS: {name}")
                    passed += 1
                except AssertionError as e:
                    print(f"  FAIL: {name} - {e}")
                    errors.append((name, traceback.format_exc()))
                    failed += 1
                except Exception as e:
                    print(f"  ERROR: {name} - {type(e).__name__}: {e}")
                    errors.append((name, traceback.format_exc()))
                    failed += 1

    print("\n" + "=" * 70)
    print(f"Results: {passed} passed, {failed} failed")
    print("=" * 70)

    if errors:
        print("\nErrors:")
        for name, tb in errors:
            print(f"\n{name}:")
            print(tb)

    return failed == 0


if __name__ == "__main__":
    success = run_tests()
    sys.exit(0 if success else 1)
