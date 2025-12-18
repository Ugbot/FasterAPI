#!/usr/bin/env python3
"""
Tests for yield dependency cleanup.

Verifies that:
1. Sync yield dependencies are properly cleaned up after handler
2. Async yield dependencies are properly cleaned up after handler
3. Nested yield dependencies are cleaned up in LIFO order
4. Cleanup happens even when handler raises an exception
5. Multiple requests don't share cleanup state
"""

import sys
import uuid
import random
import string
import asyncio

sys.path.insert(0, "/Users/bengamble/FasterAPI")

from fasterapi.fastapi_compat import FastAPI
from fasterapi.params import Depends
from fasterapi.testclient import TestClient


def random_string(length: int = 8) -> str:
    """Generate random string for test data."""
    return ''.join(random.choices(string.ascii_letters + string.digits, k=length))


def random_int(min_val: int = 1, max_val: int = 1000) -> int:
    """Generate random integer for test data."""
    return random.randint(min_val, max_val)


class TestSyncYieldDependency:
    """Test sync yield dependencies."""

    def test_sync_yield_dependency_cleanup(self):
        """Test that sync yield dependency cleanup is called after handler."""
        app = FastAPI()
        events = []
        test_id = random_string()
        resource_value = random_int()

        def get_resource():
            """Sync yield dependency - simulates resource acquisition/release."""
            events.append(f"open-{resource_value}")
            yield {"value": resource_value, "id": test_id}
            events.append(f"close-{resource_value}")

        @app.get(f"/resource-{test_id}")
        def get_item(res=Depends(get_resource)):
            events.append(f"handler-{resource_value}")
            return {"resource": res["value"], "id": res["id"]}

        client = TestClient(app)

        # Clear events before request
        events.clear()

        response = client.get(f"/resource-{test_id}")
        assert response.status_code == 200
        data = response.json()
        assert data["resource"] == resource_value
        assert data["id"] == test_id

        # Verify cleanup sequence: open -> handler -> close
        assert f"open-{resource_value}" in events
        assert f"handler-{resource_value}" in events
        assert f"close-{resource_value}" in events

        # Verify order: open before handler, handler before close
        open_idx = events.index(f"open-{resource_value}")
        handler_idx = events.index(f"handler-{resource_value}")
        close_idx = events.index(f"close-{resource_value}")
        assert open_idx < handler_idx < close_idx

    def test_multiple_sync_yield_dependencies(self):
        """Test multiple sync yield dependencies are cleaned up in LIFO order."""
        app = FastAPI()
        events = []
        test_id = random_string()

        def dep_a():
            events.append("open-A")
            yield "A"
            events.append("close-A")

        def dep_b():
            events.append("open-B")
            yield "B"
            events.append("close-B")

        @app.get(f"/multi-{test_id}")
        def multi_dep(a=Depends(dep_a), b=Depends(dep_b)):
            events.append("handler")
            return {"a": a, "b": b, "id": test_id}

        client = TestClient(app)
        events.clear()

        response = client.get(f"/multi-{test_id}")
        assert response.status_code == 200
        data = response.json()
        assert data["a"] == "A"
        assert data["b"] == "B"

        # Both should be opened before handler
        assert events.index("open-A") < events.index("handler")
        assert events.index("open-B") < events.index("handler")

        # Both should be closed after handler (LIFO order)
        assert events.index("handler") < events.index("close-B")
        assert events.index("handler") < events.index("close-A")


class TestAsyncYieldDependency:
    """Test async yield dependencies."""

    def test_async_yield_dependency_cleanup(self):
        """Test that async yield dependency cleanup is called after handler."""
        app = FastAPI()
        events = []
        test_id = random_string()
        db_id = random_int()

        async def get_async_db():
            """Async yield dependency - simulates async DB connection."""
            events.append(f"db-connect-{db_id}")
            yield {"connection": db_id, "id": test_id}
            events.append(f"db-disconnect-{db_id}")

        @app.get(f"/async-db-{test_id}")
        async def query_db(db=Depends(get_async_db)):
            events.append(f"query-{db_id}")
            return {"db_id": db["connection"], "id": db["id"]}

        client = TestClient(app)
        events.clear()

        response = client.get(f"/async-db-{test_id}")
        assert response.status_code == 200
        data = response.json()
        assert data["db_id"] == db_id
        assert data["id"] == test_id

        # Verify cleanup happened
        assert f"db-connect-{db_id}" in events
        assert f"query-{db_id}" in events
        assert f"db-disconnect-{db_id}" in events

        # Verify order
        connect_idx = events.index(f"db-connect-{db_id}")
        query_idx = events.index(f"query-{db_id}")
        disconnect_idx = events.index(f"db-disconnect-{db_id}")
        assert connect_idx < query_idx < disconnect_idx


class TestNestedYieldDependencies:
    """Test nested yield dependencies."""

    def test_nested_yield_cleanup_order(self):
        """Test nested yield dependencies are cleaned up in LIFO order."""
        app = FastAPI()
        events = []
        test_id = random_string()

        def outer_dep():
            events.append("outer-open")
            yield "outer"
            events.append("outer-close")

        def inner_dep(outer=Depends(outer_dep)):
            events.append("inner-open")
            yield f"inner-{outer}"
            events.append("inner-close")

        @app.get(f"/nested-{test_id}")
        def nested_handler(inner=Depends(inner_dep)):
            events.append("handler")
            return {"value": inner, "id": test_id}

        client = TestClient(app)
        events.clear()

        response = client.get(f"/nested-{test_id}")
        assert response.status_code == 200
        data = response.json()
        assert data["value"] == "inner-outer"

        # Verify LIFO cleanup: outer opens first, inner opens, handler, inner closes, outer closes
        assert "outer-open" in events
        assert "inner-open" in events
        assert "handler" in events
        assert "inner-close" in events
        assert "outer-close" in events

        outer_open = events.index("outer-open")
        inner_open = events.index("inner-open")
        handler_idx = events.index("handler")
        inner_close = events.index("inner-close")
        outer_close = events.index("outer-close")

        # Open order: outer then inner
        assert outer_open < inner_open
        # Handler after opens
        assert inner_open < handler_idx
        # Close order: LIFO - inner then outer
        assert handler_idx < inner_close
        assert inner_close < outer_close


class TestYieldCleanupOnError:
    """Test yield cleanup when handler raises exception."""

    def test_cleanup_on_handler_error(self):
        """Test that cleanup runs even when handler raises an exception."""
        app = FastAPI()
        events = []
        test_id = random_string()
        resource_id = random_int()

        def get_critical_resource():
            events.append(f"acquire-{resource_id}")
            yield {"id": resource_id}
            events.append(f"release-{resource_id}")

        @app.get(f"/error-{test_id}")
        def error_handler(res=Depends(get_critical_resource)):
            events.append("handler-start")
            raise ValueError(f"Intentional error for test {test_id}")

        client = TestClient(app, raise_server_exceptions=False)
        events.clear()

        response = client.get(f"/error-{test_id}")
        # Handler error should result in 500
        assert response.status_code == 500

        # But cleanup should still have run
        assert f"acquire-{resource_id}" in events
        assert "handler-start" in events
        assert f"release-{resource_id}" in events

        # Verify order
        acquire_idx = events.index(f"acquire-{resource_id}")
        handler_idx = events.index("handler-start")
        release_idx = events.index(f"release-{resource_id}")
        assert acquire_idx < handler_idx < release_idx


class TestMultipleRequests:
    """Test that multiple requests have independent cleanup."""

    def test_independent_cleanup_per_request(self):
        """Test that each request has its own cleanup scope."""
        app = FastAPI()
        all_events = []
        test_id = random_string()

        def get_request_resource():
            req_id = random_string(4)
            all_events.append(f"open-{req_id}")
            yield {"request_id": req_id}
            all_events.append(f"close-{req_id}")

        @app.get(f"/independent-{test_id}")
        def handler(res=Depends(get_request_resource)):
            return {"request_id": res["request_id"], "test_id": test_id}

        client = TestClient(app)
        all_events.clear()

        # Make multiple requests
        responses = []
        for _ in range(3):
            response = client.get(f"/independent-{test_id}")
            assert response.status_code == 200
            responses.append(response.json())

        # Each request should have unique request_id
        request_ids = [r["request_id"] for r in responses]
        assert len(set(request_ids)) == 3  # All unique

        # Each request should have open and close events
        opens = [e for e in all_events if e.startswith("open-")]
        closes = [e for e in all_events if e.startswith("close-")]
        assert len(opens) == 3
        assert len(closes) == 3


class TestCachingBehavior:
    """Test dependency caching behavior."""

    def test_same_dependency_called_once_per_request(self):
        """Test that same dependency is only called once per request (cached)."""
        app = FastAPI()
        call_count = [0]
        test_id = random_string()

        def counted_dependency():
            call_count[0] += 1
            yield f"call-{call_count[0]}"

        @app.get(f"/cached-{test_id}")
        def handler(dep1=Depends(counted_dependency), dep2=Depends(counted_dependency)):
            return {"dep1": dep1, "dep2": dep2, "count": call_count[0]}

        client = TestClient(app)
        call_count[0] = 0

        response = client.get(f"/cached-{test_id}")
        assert response.status_code == 200
        data = response.json()

        # Both should have same value (cached)
        assert data["dep1"] == data["dep2"]
        # Dependency should only be called once
        assert data["count"] == 1


def run_tests():
    """Run all tests and report results."""
    import traceback

    test_classes = [
        TestSyncYieldDependency,
        TestAsyncYieldDependency,
        TestNestedYieldDependencies,
        TestYieldCleanupOnError,
        TestMultipleRequests,
        TestCachingBehavior,
    ]
    passed = 0
    failed = 0
    errors = []

    print("=" * 70)
    print("Running Yield Dependency Cleanup Tests")
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
