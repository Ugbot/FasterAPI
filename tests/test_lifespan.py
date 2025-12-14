"""
Lifespan Context Manager Tests

Tests for the modern FastAPI lifespan pattern.
"""

import asyncio
from contextlib import asynccontextmanager

from fasterapi import FastAPI


class TestLifespan:
    """Tests for lifespan context manager."""

    def _run_lifespan(self, app):
        """Helper to simulate ASGI lifespan cycle."""
        events = []

        async def run():
            messages = [
                {"type": "lifespan.startup"},
                {"type": "lifespan.shutdown"},
            ]
            msg_index = 0

            async def receive():
                nonlocal msg_index
                msg = messages[msg_index]
                msg_index += 1
                return msg

            sent = []

            async def send(msg):
                sent.append(msg)
                events.append(msg["type"])

            scope = {"type": "lifespan"}
            await app(scope, receive, send)
            return sent

        return asyncio.run(run()), events

    def test_basic_lifespan(self):
        """Test basic lifespan context manager."""
        startup_called = []
        shutdown_called = []

        @asynccontextmanager
        async def lifespan(app):
            startup_called.append(True)
            yield
            shutdown_called.append(True)

        app = FastAPI(lifespan=lifespan)

        sent, events = self._run_lifespan(app)

        assert startup_called == [True], "Startup not called"
        assert shutdown_called == [True], "Shutdown not called"
        assert "lifespan.startup.complete" in events
        assert "lifespan.shutdown.complete" in events

    def test_lifespan_with_state(self):
        """Test lifespan sets app state."""

        @asynccontextmanager
        async def lifespan(app):
            app.state.db = "connected"
            app.state.cache = {"items": []}
            yield
            app.state.db = None

        app = FastAPI(lifespan=lifespan)
        self._run_lifespan(app)

        # After shutdown, db should be None
        assert app.state.db is None

    def test_lifespan_async_operations(self):
        """Test lifespan with async operations."""
        operations = []

        @asynccontextmanager
        async def lifespan(app):
            await asyncio.sleep(0.01)
            operations.append("startup_complete")
            yield
            await asyncio.sleep(0.01)
            operations.append("shutdown_complete")

        app = FastAPI(lifespan=lifespan)
        self._run_lifespan(app)

        assert operations == ["startup_complete", "shutdown_complete"]

    def test_lifespan_with_old_handlers(self):
        """Test lifespan works alongside old on_startup/on_shutdown."""
        events = []

        @asynccontextmanager
        async def lifespan(app):
            events.append("lifespan_startup")
            yield
            events.append("lifespan_shutdown")

        app = FastAPI(lifespan=lifespan)

        @app.on_event("startup")
        def old_startup():
            events.append("old_startup")

        @app.on_event("shutdown")
        def old_shutdown():
            events.append("old_shutdown")

        self._run_lifespan(app)

        # Lifespan runs first, then old handlers
        assert "lifespan_startup" in events
        assert "old_startup" in events
        assert "old_shutdown" in events
        assert "lifespan_shutdown" in events

    def test_lifespan_exception_handling(self):
        """Test lifespan handles startup exceptions."""

        @asynccontextmanager
        async def lifespan(app):
            raise RuntimeError("Startup failed")
            yield

        app = FastAPI(lifespan=lifespan)

        async def run():
            messages = [{"type": "lifespan.startup"}]
            msg_index = 0

            async def receive():
                nonlocal msg_index
                if msg_index < len(messages):
                    msg = messages[msg_index]
                    msg_index += 1
                    return msg
                # Return shutdown to exit loop
                return {"type": "lifespan.shutdown"}

            sent = []

            async def send(msg):
                sent.append(msg)

            scope = {"type": "lifespan"}
            await app(scope, receive, send)
            return sent

        sent = asyncio.run(run())
        # Should have startup.failed message
        assert any("failed" in msg.get("type", "") for msg in sent)

    def test_app_state_accessible(self):
        """Test app.state is accessible."""
        app = FastAPI()

        # State should be accessible
        app.state.custom_value = 42
        assert app.state.custom_value == 42

        # State supports attribute access
        app.state.nested = {"key": "value"}
        assert app.state.nested["key"] == "value"

    def test_no_lifespan_still_works(self):
        """Test app works without lifespan."""
        app = FastAPI()  # No lifespan

        startup_called = []

        @app.on_event("startup")
        def on_startup():
            startup_called.append(True)

        sent, events = self._run_lifespan(app)

        assert startup_called == [True]
        assert "lifespan.startup.complete" in events


def run_all_tests():
    """Run all test classes."""
    test_classes = [
        TestLifespan,
    ]

    total_passed = 0
    total_failed = 0
    failures = []

    for test_class in test_classes:
        print(f"\n{test_class.__name__}:")
        instance = test_class()
        for name in dir(instance):
            if name.startswith("test_"):
                try:
                    getattr(instance, name)()
                    print(f"  ✓ {name}")
                    total_passed += 1
                except Exception as e:
                    print(f"  ✗ {name}: {e}")
                    total_failed += 1
                    failures.append((test_class.__name__, name, str(e)))

    print(f"\n{'=' * 60}")
    print(f"Results: {total_passed} passed, {total_failed} failed")

    if failures:
        print("\nFailures:")
        for cls, name, error in failures:
            print(f"  {cls}.{name}: {error}")
        return False

    return True


if __name__ == "__main__":
    import sys

    success = run_all_tests()
    sys.exit(0 if success else 1)
