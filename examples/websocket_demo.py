#!/usr/bin/env python3
"""
WebSocket Demo for FasterAPI

Demonstrates WebSocket functionality with randomized test data:
- Echo endpoint (text and binary)
- Chat room with broadcast
- Streaming data endpoint
- JSON message handling

Run with:
    python examples/websocket_demo.py

Test with websocat:
    websocat ws://localhost:8000/ws/echo
    websocat ws://localhost:8000/ws/chat
    websocat ws://localhost:8000/ws/stream
"""

import asyncio
import random
import string
import time
import sys
from pathlib import Path

# Add parent directory to path
sys.path.insert(0, str(Path(__file__).parent.parent))

from fasterapi.http.server_cy import Server
from fasterapi.http.websocket import WebSocket


# ==============================================================================
# WebSocket Endpoints
# ==============================================================================


async def websocket_echo(ws: WebSocket):
    """
    Echo endpoint: Echoes back any message received.

    Handles both text and binary messages.
    """
    print(f"WebSocket connected: {ws.connection_id}")

    try:
        await ws.send_text("Welcome to echo server! Send me a message.")

        while ws.is_open:
            try:
                # Receive message
                message = await ws.receive()

                # Echo it back
                if isinstance(message, str):
                    await ws.send_text(f"Echo: {message}")
                else:
                    await ws.send_binary(b"Echo (binary): " + message)

                print(
                    f"[Echo] Connection {ws.connection_id}: echoed {len(str(message))} chars"
                )

            except RuntimeError as e:
                print(f"[Echo] Connection {ws.connection_id} error: {e}")
                break

    finally:
        print(f"[Echo] Connection {ws.connection_id} closed")
        if ws.is_open:
            await ws.close()


async def websocket_chat(ws: WebSocket):
    """
    Chat room endpoint: Broadcasts messages to all connected clients.

    In a real implementation, we'd maintain a list of active connections.
    This is a simplified version that just echoes with a chat prefix.
    """
    print(f"[Chat] User joined: {ws.connection_id}")

    try:
        # Generate random username
        username = "".join(random.choices(string.ascii_lowercase, k=8))
        await ws.send_json(
            {
                "type": "welcome",
                "username": username,
                "message": f"Welcome {username}! Type a message to chat.",
            }
        )

        message_count = 0

        while ws.is_open:
            try:
                # Receive message
                message = await ws.receive_text()
                message_count += 1

                # Broadcast (simplified - just echo with username)
                await ws.send_json(
                    {
                        "type": "message",
                        "username": username,
                        "message": message,
                        "timestamp": time.time(),
                        "message_number": message_count,
                    }
                )

                print(f"[Chat] {username}: {message}")

            except RuntimeError as e:
                print(f"[Chat] {username} error: {e}")
                break

    finally:
        print(f"[Chat] {username} left ({message_count} messages sent)")
        if ws.is_open:
            await ws.close()


async def websocket_stream(ws: WebSocket):
    """
    Streaming endpoint: Streams randomized data to client.

    Demonstrates:
    - Continuous data streaming
    - Randomized test data
    - Different message types
    - Ping/pong keep-alive
    """
    print(f"[Stream] Client connected: {ws.connection_id}")

    try:
        await ws.send_text("Starting data stream...")

        count = 0
        last_ping = time.time()

        while ws.is_open and count < 1000:  # Stream 1000 messages
            count += 1

            # Randomize message type
            msg_type = random.choice(["text", "json", "binary"])

            if msg_type == "text":
                # Random text message
                data = "".join(
                    random.choices(
                        string.ascii_letters + string.digits, k=random.randint(10, 100)
                    )
                )
                await ws.send_text(f"Message {count}: {data}")

            elif msg_type == "json":
                # Random JSON data
                await ws.send_json(
                    {
                        "count": count,
                        "timestamp": time.time(),
                        "random_value": random.random(),
                        "random_int": random.randint(0, 1000),
                        "random_data": "".join(random.choices(string.hexdigits, k=16)),
                    }
                )

            else:  # binary
                # Random binary data
                data = bytes(
                    [random.randint(0, 255) for _ in range(random.randint(10, 100))]
                )
                await ws.send_binary(data)

            # Send ping every 30 seconds
            if time.time() - last_ping > 30:
                await ws.ping()
                last_ping = time.time()
                print(f"[Stream] Sent ping to {ws.connection_id}")

            # Random delay
            await asyncio.sleep(random.uniform(0.01, 0.1))

            # Log progress every 100 messages
            if count % 100 == 0:
                print(f"[Stream] Connection {ws.connection_id}: {count} messages sent")

        # Stream complete
        await ws.send_json(
            {
                "type": "complete",
                "total_messages": count,
                "messages_sent": ws.messages_sent,
                "bytes_sent": ws.bytes_sent,
            }
        )

        print(
            f"[Stream] Connection {ws.connection_id} stream complete: {count} messages"
        )

    except Exception as e:
        print(f"[Stream] Connection {ws.connection_id} error: {e}")

    finally:
        if ws.is_open:
            await ws.close()


async def websocket_benchmark(ws: WebSocket):
    """
    Benchmark endpoint: High-throughput message testing.

    Sends bursts of randomized messages to test performance.
    """
    print(f"[Benchmark] Client connected: {ws.connection_id}")

    try:
        await ws.send_json(
            {"type": "benchmark_start", "message": "Benchmark starting..."}
        )

        # Parameters
        num_messages = 10000
        batch_size = 100

        start_time = time.time()

        for batch in range(num_messages // batch_size):
            for i in range(batch_size):
                # Randomize message size
                size = random.randint(10, 1000)
                data = "".join(random.choices(string.ascii_letters, k=size))

                await ws.send_text(data)

            # Brief pause between batches
            await asyncio.sleep(0.001)

            # Progress update
            if (batch + 1) % 10 == 0:
                messages_sent = (batch + 1) * batch_size
                elapsed = time.time() - start_time
                rate = messages_sent / elapsed
                print(f"[Benchmark] {messages_sent} messages, {rate:.0f} msg/s")

        # Final stats
        elapsed = time.time() - start_time
        rate = num_messages / elapsed

        await ws.send_json(
            {
                "type": "benchmark_complete",
                "total_messages": num_messages,
                "elapsed_seconds": elapsed,
                "messages_per_second": rate,
                "bytes_sent": ws.bytes_sent,
            }
        )

        print(
            f"[Benchmark] Complete: {num_messages} messages in {elapsed:.2f}s ({rate:.0f} msg/s)"
        )

    except Exception as e:
        print(f"[Benchmark] Error: {e}")

    finally:
        if ws.is_open:
            await ws.close()


# ==============================================================================
# Server Setup
# ==============================================================================


def main():
    """Start WebSocket demo server."""
    # Create server
    server = Server(
        port=8000,
        host="0.0.0.0",
        enable_h2=False,
        enable_h3=False,
        enable_compression=True,
        enable_websocket=True,
    )

    # Register WebSocket endpoints
    print("Registering WebSocket endpoints...")
    server.add_websocket("/ws/echo", websocket_echo)
    server.add_websocket("/ws/chat", websocket_chat)
    server.add_websocket("/ws/stream", websocket_stream)
    server.add_websocket("/ws/benchmark", websocket_benchmark)

    # Add a simple HTTP endpoint for testing
    from fasterapi.http.server_cy import PyWebSocketConnection  # For type hints

    def http_index(request, response):
        """HTTP index page."""
        html = """
        <html>
        <head><title>FasterAPI WebSocket Demo</title></head>
        <body>
            <h1>FasterAPI WebSocket Demo</h1>
            <p>WebSocket endpoints:</p>
            <ul>
                <li><code>ws://localhost:8000/ws/echo</code> - Echo server</li>
                <li><code>ws://localhost:8000/ws/chat</code> - Chat room</li>
                <li><code>ws://localhost:8000/ws/stream</code> - Data streaming</li>
                <li><code>ws://localhost:8000/ws/benchmark</code> - Performance test</li>
            </ul>
            <p>Test with: <code>websocat ws://localhost:8000/ws/echo</code></p>
        </body>
        </html>
        """
        response.status_code = 200
        response.body = html.encode("utf-8")
        response.content_type = "text/html"

    server.add_route("GET", "/", http_index)

    # Start server
    print("\nStarting FasterAPI WebSocket demo server...")
    print("Server listening on http://0.0.0.0:8000")
    print("\nAvailable endpoints:")
    print("  - ws://localhost:8000/ws/echo (echo server)")
    print("  - ws://localhost:8000/ws/chat (chat room)")
    print("  - ws://localhost:8000/ws/stream (data streaming)")
    print("  - ws://localhost:8000/ws/benchmark (performance test)")
    print("\nPress Ctrl+C to stop\n")

    server.start()

    try:
        # Keep server running
        while server.is_running():
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nShutting down...")
        server.stop()
        print("Server stopped")


if __name__ == "__main__":
    main()
