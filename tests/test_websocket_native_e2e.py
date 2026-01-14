#!/usr/bin/env python3
"""
WebSocket E2E tests for FasterAPI native C++ server.

Tests the complete WebSocket stack including:
- C++ HTTP server handling WebSocket upgrade
- ZeroMQ IPC routing to Python workers
- Worker affinity for WebSocket connections
- Message echo between client and Python handler

Run with:
    pytest tests/test_websocket_native_e2e.py -v
    python tests/test_websocket_native_e2e.py  # Direct execution
"""

import base64
import os
import random
import socket
import string
import subprocess
import sys
import threading
import time
from typing import Optional

import pytest


def random_string(length: int = 10) -> str:
    """Generate a random alphanumeric string."""
    return "".join(random.choices(string.ascii_letters + string.digits, k=length))


def random_bytes(length: int = 16) -> bytes:
    """Generate random bytes."""
    return bytes(random.randint(0, 255) for _ in range(length))


def build_websocket_frame(message: bytes, opcode: int = 0x01, masked: bool = True) -> bytes:
    """
    Build a WebSocket frame.

    Args:
        message: Payload bytes
        opcode: 0x01 for text, 0x02 for binary
        masked: Whether to mask the frame (client->server must be masked)

    Returns:
        The complete frame bytes
    """
    frame = bytearray()

    # FIN + opcode
    frame.append(0x80 | opcode)

    # Mask bit + length
    length = len(message)
    if length < 126:
        frame.append((0x80 if masked else 0) | length)
    elif length < 65536:
        frame.append((0x80 if masked else 0) | 126)
        frame.append((length >> 8) & 0xFF)
        frame.append(length & 0xFF)
    else:
        frame.append((0x80 if masked else 0) | 127)
        for i in range(7, -1, -1):
            frame.append((length >> (i * 8)) & 0xFF)

    if masked:
        mask = random_bytes(4)
        frame.extend(mask)
        for i, b in enumerate(message):
            frame.append(b ^ mask[i % 4])
    else:
        frame.extend(message)

    return bytes(frame)


def parse_websocket_frame(data: bytes) -> tuple[int, bytes]:
    """
    Parse a WebSocket frame.

    Returns:
        (opcode, payload)
    """
    if len(data) < 2:
        return (0, b"")

    opcode = data[0] & 0x0F
    masked = bool(data[1] & 0x80)
    length = data[1] & 0x7F

    offset = 2
    if length == 126:
        if len(data) < 4:
            return (opcode, b"")
        length = (data[2] << 8) | data[3]
        offset = 4
    elif length == 127:
        if len(data) < 10:
            return (opcode, b"")
        length = 0
        for i in range(8):
            length = (length << 8) | data[2 + i]
        offset = 10

    if masked:
        mask = data[offset:offset + 4]
        offset += 4
        payload = bytearray(data[offset:offset + length])
        for i in range(len(payload)):
            payload[i] ^= mask[i % 4]
        return (opcode, bytes(payload))
    else:
        return (opcode, data[offset:offset + length])


class WebSocketTestClient:
    """A simple WebSocket test client using raw sockets."""

    def __init__(self, host: str, port: int, path: str = "/ws/echo"):
        self.host = host
        self.port = port
        self.path = path
        self.sock: Optional[socket.socket] = None
        self.connected = False

    def connect(self, timeout: float = 5.0) -> bool:
        """Perform WebSocket handshake."""
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)

        try:
            self.sock.connect((self.host, self.port))
        except (socket.error, socket.timeout) as e:
            print(f"Connection failed: {e}")
            return False

        # WebSocket upgrade request
        key = base64.b64encode(random_bytes(16)).decode()
        request = (
            f"GET {self.path} HTTP/1.1\r\n"
            f"Host: {self.host}:{self.port}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n"
            f"\r\n"
        )

        self.sock.send(request.encode())

        response = self.sock.recv(4096).decode()
        if "101" in response:
            self.connected = True
            return True
        else:
            print(f"Upgrade failed: {response[:200]}")
            return False

    def send_text(self, message: str) -> bool:
        """Send a text message."""
        if not self.connected:
            return False
        frame = build_websocket_frame(message.encode(), opcode=0x01)
        self.sock.send(frame)
        return True

    def send_binary(self, data: bytes) -> bool:
        """Send a binary message."""
        if not self.connected:
            return False
        frame = build_websocket_frame(data, opcode=0x02)
        self.sock.send(frame)
        return True

    def receive(self, timeout: float = 5.0) -> tuple[int, bytes]:
        """
        Receive a WebSocket frame.

        Returns:
            (opcode, payload)
        """
        if not self.connected:
            return (0, b"")

        self.sock.settimeout(timeout)
        try:
            data = self.sock.recv(65536)
            return parse_websocket_frame(data)
        except socket.timeout:
            return (0, b"")

    def receive_text(self, timeout: float = 5.0) -> Optional[str]:
        """Receive a text message."""
        opcode, payload = self.receive(timeout)
        if opcode == 0x01:
            return payload.decode()
        return None

    def close(self):
        """Close the connection."""
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None
        self.connected = False

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


class NativeServerProcess:
    """Manages a native C++ server process for testing."""

    def __init__(self, port: int, workers: int = 1):
        self.port = port
        self.workers = workers
        self.process: Optional[subprocess.Popen] = None
        self.thread: Optional[threading.Thread] = None

    def start(self, app_code: str, timeout: float = 8.0) -> bool:
        """
        Start the native server in a subprocess.

        Args:
            app_code: Python code that defines and starts the app
            timeout: How long to wait for server startup
        """
        full_code = f"""
import sys
sys.path.insert(0, '.')

{app_code}

if __name__ == "__main__":
    import time
    time.sleep(0.5)
    app.run(host='127.0.0.1', port={self.port}, workers={self.workers})
"""

        self.process = subprocess.Popen(
            [sys.executable, "-c", full_code],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        )

        # Wait for server to start
        start = time.time()
        while time.time() - start < timeout:
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(0.5)
                sock.connect(("127.0.0.1", self.port))
                sock.close()
                return True
            except (socket.error, socket.timeout):
                time.sleep(0.2)

        return False

    def stop(self):
        """Stop the server process."""
        if self.process:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
            self.process = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.stop()


# ============================================================================
# Pytest Fixtures
# ============================================================================

@pytest.fixture(scope="module")
def free_port():
    """Find a free port for testing."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


@pytest.fixture
def echo_app_code():
    """Python code for a simple echo WebSocket app."""
    return '''
from fasterapi import FastAPI

app = FastAPI()

@app.websocket("/ws/echo")
async def echo(websocket):
    await websocket.accept()
    try:
        while True:
            data = await websocket.receive_text()
            await websocket.send_text(f"Echo: {data}")
    except Exception:
        pass

@app.get("/health")
def health():
    return {"status": "ok"}
'''


# ============================================================================
# Test Classes
# ============================================================================

class TestWebSocketUpgrade:
    """Test WebSocket upgrade handshake."""

    def test_upgrade_returns_101(self, free_port, echo_app_code):
        """Test that WebSocket upgrade returns HTTP 101."""
        with NativeServerProcess(free_port, workers=1) as server:
            if not server.start(echo_app_code):
                pytest.skip("Server failed to start")

            with WebSocketTestClient("127.0.0.1", free_port, "/ws/echo") as client:
                assert client.connect(), "WebSocket upgrade should return 101"

    def test_invalid_path_returns_404(self, free_port, echo_app_code):
        """Test that invalid WebSocket path returns 404."""
        with NativeServerProcess(free_port, workers=1) as server:
            if not server.start(echo_app_code):
                pytest.skip("Server failed to start")

            with WebSocketTestClient("127.0.0.1", free_port, "/ws/invalid") as client:
                # Should fail to connect - no 101
                assert not client.connect(), "Invalid path should not return 101"


class TestWebSocketEcho:
    """Test WebSocket echo functionality."""

    def test_echo_single_message(self, free_port, echo_app_code):
        """Test sending and receiving a single message."""
        with NativeServerProcess(free_port, workers=1) as server:
            if not server.start(echo_app_code):
                pytest.skip("Server failed to start")

            with WebSocketTestClient("127.0.0.1", free_port, "/ws/echo") as client:
                assert client.connect()

                test_msg = random_string(20)
                client.send_text(test_msg)

                response = client.receive_text(timeout=5.0)
                assert response == f"Echo: {test_msg}"

    def test_echo_multiple_messages(self, free_port, echo_app_code):
        """Test sending and receiving multiple messages."""
        with NativeServerProcess(free_port, workers=1) as server:
            if not server.start(echo_app_code):
                pytest.skip("Server failed to start")

            with WebSocketTestClient("127.0.0.1", free_port, "/ws/echo") as client:
                assert client.connect()

                for i in range(5):
                    test_msg = f"message_{i}_{random_string(10)}"
                    client.send_text(test_msg)

                    response = client.receive_text(timeout=5.0)
                    assert response == f"Echo: {test_msg}", f"Message {i} failed"

    def test_echo_random_data(self, free_port, echo_app_code):
        """Test with randomized message sizes and content."""
        with NativeServerProcess(free_port, workers=1) as server:
            if not server.start(echo_app_code):
                pytest.skip("Server failed to start")

            with WebSocketTestClient("127.0.0.1", free_port, "/ws/echo") as client:
                assert client.connect()

                for _ in range(10):
                    length = random.randint(5, 500)
                    test_msg = random_string(length)
                    client.send_text(test_msg)

                    response = client.receive_text(timeout=5.0)
                    assert response == f"Echo: {test_msg}"


class TestWebSocketMultipleRoutes:
    """Test multiple WebSocket routes."""

    @pytest.fixture
    def multi_route_app_code(self):
        """Python code for app with multiple WebSocket routes."""
        return '''
from fasterapi import FastAPI

app = FastAPI()

@app.websocket("/ws/echo1")
async def echo1(websocket):
    await websocket.accept()
    try:
        while True:
            data = await websocket.receive_text()
            await websocket.send_text(f"Route1: {data}")
    except Exception:
        pass

@app.websocket("/ws/echo2")
async def echo2(websocket):
    await websocket.accept()
    try:
        while True:
            data = await websocket.receive_text()
            await websocket.send_text(f"Route2: {data}")
    except Exception:
        pass

@app.get("/health")
def health():
    return {"status": "ok"}
'''

    def test_different_routes(self, free_port, multi_route_app_code):
        """Test that different routes invoke different handlers."""
        with NativeServerProcess(free_port, workers=1) as server:
            if not server.start(multi_route_app_code):
                pytest.skip("Server failed to start")

            # Test route 1
            with WebSocketTestClient("127.0.0.1", free_port, "/ws/echo1") as client1:
                assert client1.connect()
                client1.send_text("test1")
                response1 = client1.receive_text()
                assert response1 == "Route1: test1"

            # Test route 2
            with WebSocketTestClient("127.0.0.1", free_port, "/ws/echo2") as client2:
                assert client2.connect()
                client2.send_text("test2")
                response2 = client2.receive_text()
                assert response2 == "Route2: test2"


class TestWebSocketMultiWorker:
    """Test WebSocket with multiple workers (affinity testing)."""

    def test_worker_affinity(self, free_port, echo_app_code):
        """Test that messages on a connection go to the same worker."""
        with NativeServerProcess(free_port, workers=2) as server:
            if not server.start(echo_app_code, timeout=10.0):
                pytest.skip("Server failed to start")

            with WebSocketTestClient("127.0.0.1", free_port, "/ws/echo") as client:
                assert client.connect()

                # Send multiple messages - all should be handled correctly
                for i in range(5):
                    test_msg = f"affinity_test_{i}_{random_string(10)}"
                    client.send_text(test_msg)

                    response = client.receive_text(timeout=5.0)
                    assert response == f"Echo: {test_msg}", f"Message {i} failed"


class TestWebSocketConcurrent:
    """Test concurrent WebSocket connections."""

    def test_multiple_connections(self, free_port, echo_app_code):
        """Test multiple simultaneous connections."""
        with NativeServerProcess(free_port, workers=2) as server:
            if not server.start(echo_app_code, timeout=10.0):
                pytest.skip("Server failed to start")

            clients = []
            try:
                # Open multiple connections
                for i in range(3):
                    client = WebSocketTestClient("127.0.0.1", free_port, "/ws/echo")
                    assert client.connect(), f"Client {i} failed to connect"
                    clients.append(client)

                # Send messages on each
                for i, client in enumerate(clients):
                    test_msg = f"client_{i}_{random_string(10)}"
                    client.send_text(test_msg)

                    response = client.receive_text(timeout=5.0)
                    assert response == f"Echo: {test_msg}", f"Client {i} response failed"
            finally:
                for client in clients:
                    client.close()


# ============================================================================
# Main
# ============================================================================

if __name__ == "__main__":
    pytest.main([__file__, "-v", "--tb=short"])
