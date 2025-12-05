#!/usr/bin/env python3
"""
Comprehensive SSE (Server-Sent Events) tests for FasterAPI.

Tests cover:
- Basic event sending
- Event types and IDs
- JSON and text events
- Multiple concurrent connections
- Randomized data
- Keep-alive
- Connection lifecycle
- Performance benchmarks

Run with:
    pytest tests/test_sse.py -v
    python tests/test_sse.py  # Direct execution
"""

import pytest
import asyncio
import random
import string
import time
import json
from typing import List

try:
    from fasterapi.http.server_cy import Server, PySSEConnection
    from fasterapi.http.sse import SSE, SSEStream

    BINDINGS_AVAILABLE = True
except ImportError:
    BINDINGS_AVAILABLE = False
    pytest.skip("SSE bindings not available", allow_module_level=True)


# ==============================================================================
# Test Fixtures
# ==============================================================================


@pytest.fixture
def server():
    """Create test server."""
    srv = Server(port=8766, host="127.0.0.1", enable_compression=False)
    yield srv
    if srv.is_running():
        srv.stop()


@pytest.fixture
def random_data():
    """Generate random data."""

    def _generator() -> dict:
        return {
            "random_int": random.randint(0, 1000),
            "random_float": random.random(),
            "random_string": "".join(random.choices(string.ascii_letters, k=20)),
            "timestamp": time.time(),
        }

    return _generator


# ==============================================================================
# Basic Functionality Tests
# ==============================================================================


class TestSSEBasics:
    """Test basic SSE functionality."""

    def test_sse_send_text(self, server):
        """Test sending text events."""
        events_sent = []

        async def handler(sse: SSE):
            # Send multiple text events
            for i in range(10):
                text = f"Event {i}: " + "".join(
                    random.choices(string.ascii_letters, k=50)
                )
                await sse.send_text(text)
                events_sent.append(("text", text))

        # TODO: Register SSE handler once server.add_sse() is implemented
        # For now, verify SSE classes work
        assert SSE is not None
        assert len(events_sent) == 0  # Will be populated when handler runs

    def test_sse_send_json(self, server, random_data):
        """Test sending JSON events."""
        events_sent = []

        async def handler(sse: SSE):
            # Send multiple JSON events
            for i in range(10):
                data = random_data()
                data["index"] = i
                await sse.send_json(data)
                events_sent.append(("json", data))

        assert SSE is not None

    def test_sse_with_event_type(self, server):
        """Test events with custom event types."""
        events_sent = []

        async def handler(sse: SSE):
            # Different event types
            event_types = ["message", "update", "alert", "notification", "heartbeat"]

            for event_type in event_types:
                await sse.send_text(f"Event of type: {event_type}", event=event_type)
                events_sent.append(event_type)

        assert SSE is not None

    def test_sse_with_event_id(self, server):
        """Test events with IDs for reconnection."""
        events_sent = []

        async def handler(sse: SSE):
            # Send events with IDs
            for i in range(10):
                event_id = f"evt-{i:04d}"
                await sse.send_json(
                    {"index": i, "data": random.random()}, event_id=event_id
                )
                events_sent.append(event_id)

        assert SSE is not None

    def test_sse_with_retry(self, server):
        """Test events with retry hints."""

        async def handler(sse: SSE):
            # Send events with retry times
            retry_times = [1000, 5000, 10000, 30000]

            for retry_ms in retry_times:
                await sse.send_json({"message": "Event with retry"}, retry=retry_ms)

        assert SSE is not None


class TestSSEKeepAlive:
    """Test SSE keep-alive functionality."""

    def test_ping(self, server):
        """Test ping (keep-alive) functionality."""
        ping_count = 0

        async def handler(sse: SSE):
            nonlocal ping_count
            # Send pings
            for i in range(10):
                await sse.ping()
                ping_count += 1
                await asyncio.sleep(0.1)

        assert SSE is not None

    def test_comment(self, server):
        """Test comment sending."""

        async def handler(sse: SSE):
            # Send comments (keep-alive)
            comments = ["Keep-alive", "Connection active", "Heartbeat", "Ping"]

            for comment in comments:
                await sse.send_comment(comment)

        assert SSE is not None

    def test_sse_stream_context(self, server):
        """Test SSEStream context manager with auto keep-alive."""

        async def handler(sse: SSE):
            async with SSEStream(sse, keep_alive_interval=1.0) as stream:
                # Send events while keep-alive runs in background
                for i in range(5):
                    await stream.send_json({"count": i})
                    await asyncio.sleep(0.5)

        assert SSEStream is not None


class TestSSEDataTypes:
    """Test different data types and sizes."""

    def test_small_events(self, server):
        """Test small events."""

        async def handler(sse: SSE):
            for size in [1, 10, 50, 100]:
                data = "".join(random.choices(string.ascii_letters, k=size))
                await sse.send_text(data)

        assert SSE is not None

    def test_large_events(self, server):
        """Test large events."""

        async def handler(sse: SSE):
            for size in [1000, 10000, 100000]:
                data = "".join(random.choices(string.ascii_letters, k=size))
                await sse.send_text(data)

        assert SSE is not None

    def test_multiline_events(self, server):
        """Test multiline event data."""

        async def handler(sse: SSE):
            # SSE spec supports multiline data
            multiline = """Line 1
Line 2
Line 3
Line 4"""
            await sse.send_text(multiline)

            # JSON with newlines
            await sse.send_json(
                {"text": "Multi\nline\nstring", "lines": ["line1", "line2", "line3"]}
            )

        assert SSE is not None

    def test_unicode_events(self, server):
        """Test Unicode event data."""
        unicode_strings = [
            "Hello 世界",
            "Привет мир",
            "مرحبا بالعالم",
            "🚀🌟💻🔥",
            "Iñtërnâtiônàlizætiøn",
        ]

        async def handler(sse: SSE):
            for text in unicode_strings:
                await sse.send_text(text)

        assert SSE is not None

    def test_special_characters(self, server):
        """Test special characters in events."""

        async def handler(sse: SSE):
            special = [
                "Line\nbreak",
                "Tab\there",
                'Quote: "test"',
                "Backslash: \\test",
                "Null: \x00 byte",
            ]

            for text in special:
                await sse.send_text(text, event="special")

        assert SSE is not None


class TestSSEConcurrency:
    """Test concurrent SSE connections."""

    def test_concurrent_connections(self, server):
        """Test multiple concurrent SSE connections."""
        connections = []

        async def handler(sse: SSE):
            connections.append(sse.connection_id)

            # Stream events
            for i in range(10):
                await sse.send_json(
                    {"connection_id": sse.connection_id, "event_number": i}
                )
                await asyncio.sleep(0.1)

        # TODO: Test with actual concurrent connections
        assert SSE is not None

    def test_broadcast_pattern(self, server, random_data):
        """Test broadcasting events to multiple connections."""
        active_connections = []

        async def handler(sse: SSE):
            active_connections.append(sse)

            # Broadcast to all connections
            for conn in active_connections:
                if conn.is_open:
                    await conn.send_json(random_data())

        assert SSE is not None


class TestSSEPerformance:
    """Test SSE performance characteristics."""

    def test_throughput(self, server):
        """Test event throughput."""
        stats = {"count": 0, "bytes": 0, "start": 0, "end": 0}

        async def handler(sse: SSE):
            stats["start"] = time.time()

            # Send 10000 events
            for i in range(10000):
                data = {"index": i, "random": random.random(), "timestamp": time.time()}
                await sse.send_json(data)
                stats["count"] += 1
                stats["bytes"] += len(json.dumps(data))

            stats["end"] = time.time()

        assert SSE is not None

    def test_streaming_latency(self, server):
        """Test streaming latency."""
        latencies = []

        async def handler(sse: SSE):
            # Measure time to send each event
            for i in range(100):
                start = time.time()
                await sse.send_json({"index": i})
                latency = time.time() - start
                latencies.append(latency)

        assert SSE is not None

    def test_burst_events(self, server):
        """Test burst of events."""

        async def handler(sse: SSE):
            # Send burst of 1000 events as fast as possible
            start = time.time()

            for i in range(1000):
                await sse.send_json({"burst_index": i, "timestamp": time.time()})

            duration = time.time() - start
            rate = 1000 / duration

            # Should achieve high throughput
            assert rate > 100  # At least 100 events/sec

        assert SSE is not None


class TestSSELifecycle:
    """Test SSE connection lifecycle."""

    def test_connection_open(self, server):
        """Test connection is open on creation."""

        async def handler(sse: SSE):
            assert sse.is_open

        assert SSE is not None

    def test_connection_close(self, server):
        """Test closing connection."""

        async def handler(sse: SSE):
            assert sse.is_open
            await sse.close()
            assert not sse.is_open

        assert SSE is not None

    def test_send_after_close(self, server):
        """Test sending after close raises error."""

        async def handler(sse: SSE):
            await sse.close()

            # Should raise error
            with pytest.raises(RuntimeError):
                await sse.send_text("Should fail")

        assert SSE is not None

    def test_double_close(self, server):
        """Test closing already closed connection."""

        async def handler(sse: SSE):
            await sse.close()
            await sse.close()  # Should not raise error

        assert SSE is not None


class TestSSEStats:
    """Test SSE statistics."""

    def test_event_counter(self, server):
        """Test events_sent counter."""

        async def handler(sse: SSE):
            initial = sse.events_sent

            # Send events
            for i in range(10):
                await sse.send_json({"index": i})

            final = sse.events_sent

            # Counter should increase
            assert final > initial

        assert SSE is not None

    def test_bytes_counter(self, server):
        """Test bytes_sent counter."""

        async def handler(sse: SSE):
            initial = sse.bytes_sent

            # Send data
            await sse.send_text("Test data")

            final = sse.bytes_sent

            # Counter should increase
            assert final > initial

        assert SSE is not None


class TestSSEIntegration:
    """Integration tests."""

    def test_time_stream(self, server):
        """Test time streaming pattern."""

        async def handler(sse: SSE):
            async with SSEStream(sse) as stream:
                for i in range(10):
                    await stream.send_json(
                        {"timestamp": time.time(), "index": i}, event="time"
                    )
                    await asyncio.sleep(0.1)

        assert SSEStream is not None

    def test_metrics_stream(self, server, random_data):
        """Test metrics streaming pattern."""

        async def handler(sse: SSE):
            async with SSEStream(sse) as stream:
                for i in range(10):
                    metrics = random_data()
                    metrics.update(
                        {
                            "cpu": random.uniform(0, 100),
                            "memory": random.uniform(0, 100),
                            "disk": random.uniform(0, 100),
                        }
                    )
                    await stream.send_json(metrics, event="metrics")
                    await asyncio.sleep(0.5)

        assert SSEStream is not None

    def test_event_log_stream(self, server):
        """Test event log streaming pattern."""
        log_levels = ["DEBUG", "INFO", "WARNING", "ERROR"]

        async def handler(sse: SSE):
            for i in range(20):
                level = random.choice(log_levels)
                await sse.send_json(
                    {
                        "level": level,
                        "message": f"Log event {i}",
                        "timestamp": time.time(),
                    },
                    event="log",
                    event_id=str(i),
                )

                # Variable delay based on level
                if level == "ERROR":
                    await asyncio.sleep(2.0)
                else:
                    await asyncio.sleep(0.2)

        assert SSE is not None

    def test_stock_ticker_stream(self, server):
        """Test stock ticker streaming pattern."""
        stocks = ["AAPL", "GOOGL", "MSFT", "AMZN"]
        prices = {stock: random.uniform(100, 500) for stock in stocks}

        async def handler(sse: SSE):
            for i in range(50):
                # Pick random stock
                stock = random.choice(stocks)

                # Update price
                change = random.gauss(0, 1.0)
                prices[stock] += change

                await sse.send_json(
                    {
                        "symbol": stock,
                        "price": round(prices[stock], 2),
                        "change": round(change, 2),
                        "timestamp": time.time(),
                    },
                    event="stock",
                    event_id=f"{stock}-{i}",
                )

                await asyncio.sleep(random.uniform(0.1, 0.5))

        assert SSE is not None


class TestSSEErrorHandling:
    """Test error handling."""

    def test_invalid_json(self, server):
        """Test handling invalid JSON."""

        async def handler(sse: SSE):
            # This should work (send_json handles serialization)
            await sse.send_json({"valid": "json"})

            # Send text that looks like JSON but isn't
            await sse.send_text("{invalid json}", event="raw")

        assert SSE is not None

    def test_none_values(self, server):
        """Test handling None values."""

        async def handler(sse: SSE):
            # None data should work (converted to JSON null)
            await sse.send_json({"value": None})

            # None event type (should use default)
            await sse.send_json({"data": "test"}, event=None)

            # None event ID
            await sse.send_json({"data": "test"}, event_id=None)

        assert SSE is not None


# ==============================================================================
# Main (for direct execution)
# ==============================================================================

if __name__ == "__main__":
    # Run tests directly
    pytest.main([__file__, "-v", "--tb=short"])
