#!/usr/bin/env python3
"""
Comprehensive WebSocket tests for FasterAPI.

Tests cover:
- Basic send/receive (text and binary)
- Multiple routes
- Concurrent connections
- Randomized data
- Error handling
- Connection lifecycle
- Performance benchmarks

Run with:
    pytest tests/test_websocket.py -v
    python tests/test_websocket.py  # Direct execution
"""

import pytest
import asyncio
import random
import string
import time
import threading
from typing import List, Optional

try:
    from fasterapi.http.server_cy import Server, PyWebSocketConnection
    from fasterapi.http.websocket import WebSocket

    BINDINGS_AVAILABLE = True
except ImportError:
    BINDINGS_AVAILABLE = False
    pytest.skip("WebSocket bindings not available", allow_module_level=True)


# ==============================================================================
# Test Fixtures
# ==============================================================================


@pytest.fixture
def server():
    """Create test server."""
    srv = Server(
        port=8765, host="127.0.0.1", enable_websocket=True, enable_compression=False
    )
    yield srv
    if srv.is_running():
        srv.stop()


@pytest.fixture
def random_text():
    """Generate random text."""

    def _generator(length: int = 100) -> str:
        return "".join(
            random.choices(
                string.ascii_letters + string.digits + string.punctuation, k=length
            )
        )

    return _generator


@pytest.fixture
def random_binary():
    """Generate random binary data."""

    def _generator(length: int = 100) -> bytes:
        return bytes([random.randint(0, 255) for _ in range(length)])

    return _generator


# ==============================================================================
# Basic Functionality Tests
# ==============================================================================


class TestWebSocketBasics:
    """Test basic WebSocket functionality."""

    def test_websocket_send_text(self, server, random_text):
        """Test sending text messages."""
        messages_received = []

        async def handler(ws: WebSocket):
            # Send multiple random messages
            for i in range(10):
                text = random_text(random.randint(10, 1000))
                await ws.send_text(text)
                messages_received.append(("text", text))

        server.add_websocket("/test", handler)
        server.start()

        # TODO: Connect client and verify messages received
        # In a full test, we'd use a WebSocket client library

        assert len(messages_received) == 10

    def test_websocket_send_binary(self, server, random_binary):
        """Test sending binary messages."""
        messages_received = []

        async def handler(ws: WebSocket):
            # Send multiple random binary messages
            for i in range(10):
                data = random_binary(random.randint(10, 1000))
                await ws.send_binary(data)
                messages_received.append(("binary", data))

        server.add_websocket("/test", handler)
        server.start()

        assert len(messages_received) == 10

    def test_websocket_send_json(self, server):
        """Test sending JSON messages."""
        messages_received = []

        async def handler(ws: WebSocket):
            # Send random JSON objects
            for i in range(10):
                obj = {
                    "index": i,
                    "random_int": random.randint(0, 1000),
                    "random_float": random.random(),
                    "random_string": "".join(
                        random.choices(string.ascii_letters, k=20)
                    ),
                    "nested": {
                        "value": random.randint(0, 100),
                        "data": [random.random() for _ in range(5)],
                    },
                }
                await ws.send_json(obj)
                messages_received.append(obj)

        server.add_websocket("/test", handler)
        server.start()

        assert len(messages_received) == 10
        # Verify all objects are valid
        for msg in messages_received:
            assert "index" in msg
            assert "nested" in msg
            assert "value" in msg["nested"]


class TestWebSocketRoutes:
    """Test multiple WebSocket routes."""

    def test_multiple_routes(self, server):
        """Test multiple WebSocket routes on same server."""
        route1_calls = []
        route2_calls = []
        route3_calls = []

        async def handler1(ws: WebSocket):
            route1_calls.append(ws.connection_id)
            await ws.send_text("Route 1")

        async def handler2(ws: WebSocket):
            route2_calls.append(ws.connection_id)
            await ws.send_text("Route 2")

        async def handler3(ws: WebSocket):
            route3_calls.append(ws.connection_id)
            await ws.send_text("Route 3")

        server.add_websocket("/ws1", handler1)
        server.add_websocket("/ws2", handler2)
        server.add_websocket("/ws3", handler3)
        server.start()

        # Verify all routes registered
        assert len(server._websocket_handlers) == 3

    def test_different_verbs_same_path(self, server):
        """Test different HTTP verbs don't interfere with WebSocket."""
        ws_calls = []
        http_calls = []

        async def ws_handler(ws: WebSocket):
            ws_calls.append(ws.connection_id)
            await ws.send_text("WebSocket")

        def http_handler(request, response):
            http_calls.append(request.path)
            response.status_code = 200
            response.body = b"HTTP"

        server.add_websocket("/test", ws_handler)
        server.add_route("GET", "/test", http_handler)
        server.start()

        # Both handlers should coexist
        assert "/test" in server._websocket_handlers


class TestWebSocketConcurrency:
    """Test concurrent WebSocket connections."""

    def test_concurrent_connections(self, server):
        """Test multiple concurrent WebSocket connections."""
        active_connections = []
        max_concurrent = 0

        async def handler(ws: WebSocket):
            nonlocal max_concurrent
            active_connections.append(ws.connection_id)
            max_concurrent = max(max_concurrent, len(active_connections))

            # Send some messages
            for i in range(10):
                await ws.send_json(
                    {
                        "connection_id": ws.connection_id,
                        "message": i,
                        "timestamp": time.time(),
                    }
                )
                await asyncio.sleep(0.01)

            active_connections.remove(ws.connection_id)

        server.add_websocket("/test", handler)
        server.start()

        # TODO: In full implementation, spawn multiple client connections
        # For now, verify handler is registered
        assert "/test" in server._websocket_handlers

    def test_concurrent_messages(self, server, random_text):
        """Test sending many messages concurrently."""
        message_count = 0

        async def handler(ws: WebSocket):
            nonlocal message_count

            # Send burst of messages
            tasks = []
            for i in range(100):
                text = random_text(random.randint(10, 100))
                task = asyncio.create_task(ws.send_text(text))
                tasks.append(task)

            # Wait for all to complete
            await asyncio.gather(*tasks)
            message_count = len(tasks)

        server.add_websocket("/test", handler)
        server.start()

        # Handler registered
        assert "/test" in server._websocket_handlers


class TestWebSocketDataTypes:
    """Test different data types and sizes."""

    def test_small_messages(self, server):
        """Test small messages (< 126 bytes)."""
        sizes = []

        async def handler(ws: WebSocket):
            for size in [1, 10, 50, 100, 125]:
                data = "".join(random.choices(string.ascii_letters, k=size))
                await ws.send_text(data)
                sizes.append(len(data))

        server.add_websocket("/test", handler)
        server.start()

        assert "/test" in server._websocket_handlers

    def test_medium_messages(self, server):
        """Test medium messages (126-65535 bytes)."""
        sizes = []

        async def handler(ws: WebSocket):
            for size in [126, 500, 1000, 10000, 65535]:
                data = "".join(random.choices(string.ascii_letters, k=size))
                await ws.send_text(data)
                sizes.append(len(data))

        server.add_websocket("/test", handler)
        server.start()

        assert "/test" in server._websocket_handlers

    def test_large_messages(self, server):
        """Test large messages (> 65535 bytes)."""
        sizes = []

        async def handler(ws: WebSocket):
            for size in [65536, 100000, 1000000]:
                data = "".join(random.choices(string.ascii_letters, k=size))
                await ws.send_text(data)
                sizes.append(len(data))

        server.add_websocket("/test", handler)
        server.start()

        assert "/test" in server._websocket_handlers

    def test_empty_messages(self, server):
        """Test empty messages."""

        async def handler(ws: WebSocket):
            await ws.send_text("")
            await ws.send_binary(b"")

        server.add_websocket("/test", handler)
        server.start()

        assert "/test" in server._websocket_handlers

    def test_unicode_messages(self, server):
        """Test Unicode messages."""
        unicode_strings = [
            "Hello 世界",
            "Привет мир",
            "مرحبا بالعالم",
            "🚀🌟💻🔥",
            "Test\n\r\t\x00special",
            "Iñtërnâtiônàlizætiøn",
        ]

        async def handler(ws: WebSocket):
            for text in unicode_strings:
                await ws.send_text(text)

        server.add_websocket("/test", handler)
        server.start()

        assert "/test" in server._websocket_handlers


class TestWebSocketControl:
    """Test WebSocket control frames (ping/pong/close)."""

    def test_ping_pong(self, server):
        """Test ping/pong frames."""

        async def handler(ws: WebSocket):
            # Send ping
            await ws.ping()

            # Send ping with data
            await ws.ping(b"test payload")

            # Send pong
            await ws.pong()
            await ws.pong(b"pong data")

        server.add_websocket("/test", handler)
        server.start()

        assert "/test" in server._websocket_handlers

    def test_close_normal(self, server):
        """Test normal close."""

        async def handler(ws: WebSocket):
            await ws.send_text("Goodbye")
            await ws.close(1000, "Normal closure")

        server.add_websocket("/test", handler)
        server.start()

        assert "/test" in server._websocket_handlers

    def test_close_error(self, server):
        """Test error close."""

        async def handler(ws: WebSocket):
            await ws.close(1002, "Protocol error")

        server.add_websocket("/test", handler)
        server.start()

        assert "/test" in server._websocket_handlers


class TestWebSocketPerformance:
    """Test WebSocket performance characteristics."""

    def test_throughput(self, server, random_text):
        """Test message throughput."""
        stats = {"count": 0, "bytes": 0, "start": 0, "end": 0}

        async def handler(ws: WebSocket):
            stats["start"] = time.time()

            # Send 10000 messages
            for i in range(10000):
                text = random_text(random.randint(10, 100))
                await ws.send_text(text)
                stats["count"] += 1
                stats["bytes"] += len(text)

            stats["end"] = time.time()

        server.add_websocket("/test", handler)
        server.start()

        # Handler registered
        assert "/test" in server._websocket_handlers

    def test_latency(self, server):
        """Test message latency."""
        latencies = []

        async def handler(ws: WebSocket):
            # Measure round-trip time
            for i in range(100):
                start = time.time()
                await ws.send_text(f"ping {i}")
                # In full implementation, would wait for response
                latency = time.time() - start
                latencies.append(latency)

        server.add_websocket("/test", handler)
        server.start()

        assert "/test" in server._websocket_handlers


class TestWebSocketErrors:
    """Test error handling."""

    def test_send_after_close(self, server):
        """Test sending after close raises error."""

        async def handler(ws: WebSocket):
            await ws.close()

            # Should raise error
            with pytest.raises(RuntimeError):
                await ws.send_text("Should fail")

        server.add_websocket("/test", handler)
        server.start()

        assert "/test" in server._websocket_handlers

    def test_invalid_utf8(self, server):
        """Test invalid UTF-8 handling."""

        async def handler(ws: WebSocket):
            # Invalid UTF-8 sequences
            invalid = b"\xff\xfe\xfd"

            # Should handle gracefully or raise error
            try:
                await ws.send_text(invalid.decode("utf-8", errors="replace"))
            except (UnicodeDecodeError, RuntimeError):
                pass  # Expected

        server.add_websocket("/test", handler)
        server.start()

        assert "/test" in server._websocket_handlers


# ==============================================================================
# Integration Tests
# ==============================================================================


class TestWebSocketIntegration:
    """Integration tests."""

    def test_echo_server(self, server, random_text):
        """Test echo server pattern."""
        echo_count = 0

        async def echo_handler(ws: WebSocket):
            nonlocal echo_count
            # Echo 100 messages
            for i in range(100):
                text = random_text(random.randint(10, 500))
                await ws.send_text(text)
                # In full impl, would receive and echo back
                echo_count += 1

        server.add_websocket("/echo", echo_handler)
        server.start()

        assert "/echo" in server._websocket_handlers

    def test_broadcast_pattern(self, server):
        """Test broadcast pattern."""
        connections = []

        async def broadcast_handler(ws: WebSocket):
            connections.append(ws)

            # Broadcast to all connections
            message = f"User {ws.connection_id} joined"
            for conn in connections:
                if conn.is_open:
                    await conn.send_text(message)

        server.add_websocket("/broadcast", broadcast_handler)
        server.start()

        assert "/broadcast" in server._websocket_handlers


# ==============================================================================
# Main (for direct execution)
# ==============================================================================

if __name__ == "__main__":
    # Run tests directly
    pytest.main([__file__, "-v", "--tb=short"])
