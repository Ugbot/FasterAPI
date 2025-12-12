"""
Tests for WebSocket app compatibility between FastAPI and FasterAPI.

Run with:
    TEST_FRAMEWORK=fastapi pytest tests/fastapi_compat/test_websocket_app.py -v
    TEST_FRAMEWORK=fasterapi pytest tests/fastapi_compat/test_websocket_app.py -v
"""

import json
import os
import random
import string

import pytest
from apps.websocket_app import app, clear_all

FRAMEWORK = os.environ.get("TEST_FRAMEWORK", "fasterapi")

if FRAMEWORK == "fastapi":
    from fastapi.testclient import TestClient
else:
    try:
        from fasterapi.testclient import TestClient
    except ImportError:
        from starlette.testclient import TestClient


@pytest.fixture(autouse=True)
def clean_state():
    """Clear all state before each test."""
    clear_all()
    yield
    clear_all()


@pytest.fixture
def client():
    """Create test client."""
    return TestClient(app)


def random_string(length: int = 10) -> str:
    return "".join(random.choices(string.ascii_letters, k=length))


class TestHealthEndpoints:
    """Test HTTP endpoints."""

    def test_health(self, client):
        """Test health check."""
        response = client.get("/health")
        assert response.status_code == 200
        assert response.json()["status"] == "healthy"

    def test_connections_empty(self, client):
        """Test connections endpoint when empty."""
        response = client.get("/connections")
        assert response.status_code == 200
        assert response.json()["count"] == 0


class TestEchoWebSocket:
    """Test basic echo WebSocket."""

    def test_echo_single_message(self, client):
        """Test sending a single message."""
        with client.websocket_connect("/ws/echo") as ws:
            ws.send_text("Hello")
            response = ws.receive_text()
            assert response == "Echo: Hello"

    def test_echo_multiple_messages(self, client):
        """Test sending multiple messages."""
        with client.websocket_connect("/ws/echo") as ws:
            for i in range(5):
                message = f"Message {i}"
                ws.send_text(message)
                response = ws.receive_text()
                assert response == f"Echo: {message}"

    def test_echo_random_messages(self, client):
        """Test with random messages."""
        with client.websocket_connect("/ws/echo") as ws:
            for _ in range(10):
                message = random_string(random.randint(5, 100))
                ws.send_text(message)
                response = ws.receive_text()
                assert response == f"Echo: {message}"


class TestClientIdWebSocket:
    """Test WebSocket with client ID path parameter."""

    def test_connect_with_client_id(self, client):
        """Test connecting with a client ID."""
        client_id = random_string(10)
        with client.websocket_connect(f"/ws/client/{client_id}") as ws:
            # Should receive welcome message
            welcome = ws.receive_text()
            assert client_id in welcome

    def test_send_message_with_client_id(self, client):
        """Test sending message with client ID."""
        client_id = random_string(10)
        with client.websocket_connect(f"/ws/client/{client_id}") as ws:
            ws.receive_text()  # Welcome message

            message = random_string(20)
            ws.send_text(message)
            response = ws.receive_text()
            assert "Sent:" in response


class TestJsonWebSocket:
    """Test JSON WebSocket."""

    def test_json_echo(self, client):
        """Test JSON echo action."""
        with client.websocket_connect("/ws/json") as ws:
            data = {"action": "echo", "value": random_string(10)}
            ws.send_json(data)
            response = ws.receive_json()
            assert response["action"] == "echo"
            assert response["data"] == data

    def test_json_ping_pong(self, client):
        """Test ping/pong action."""
        with client.websocket_connect("/ws/json") as ws:
            timestamp = random.randint(1000000, 9999999)
            ws.send_json({"action": "ping", "timestamp": timestamp})
            response = ws.receive_json()
            assert response["action"] == "pong"
            assert response["timestamp"] == timestamp

    def test_json_reverse(self, client):
        """Test reverse action."""
        with client.websocket_connect("/ws/json") as ws:
            text = random_string(20)
            ws.send_json({"action": "reverse", "text": text})
            response = ws.receive_json()
            assert response["action"] == "reversed"
            assert response["text"] == text[::-1]

    def test_json_unknown_action(self, client):
        """Test unknown action."""
        with client.websocket_connect("/ws/json") as ws:
            ws.send_json({"action": "unknown_action"})
            response = ws.receive_json()
            assert response["action"] == "unknown"


class TestBinaryWebSocket:
    """Test binary WebSocket."""

    def test_binary_echo(self, client):
        """Test binary echo."""
        with client.websocket_connect("/ws/binary") as ws:
            data = os.urandom(100)
            ws.send_bytes(data)
            response = ws.receive_bytes()
            # Response should have 4-byte length prefix
            length = int.from_bytes(response[:4], "big")
            assert length == 100
            assert response[4:] == data

    def test_binary_various_sizes(self, client):
        """Test binary with various sizes."""
        with client.websocket_connect("/ws/binary") as ws:
            for size in [1, 10, 100, 1000, 10000]:
                data = os.urandom(size)
                ws.send_bytes(data)
                response = ws.receive_bytes()
                length = int.from_bytes(response[:4], "big")
                assert length == size
                assert response[4:] == data


class TestCounterWebSocket:
    """Test stateful counter WebSocket."""

    def test_counter_increment(self, client):
        """Test counter increment."""
        with client.websocket_connect("/ws/counter") as ws:
            ws.send_text("increment")
            response = ws.receive_json()
            assert response["counter"] == 1

            ws.send_text("increment")
            response = ws.receive_json()
            assert response["counter"] == 2

    def test_counter_decrement(self, client):
        """Test counter decrement."""
        with client.websocket_connect("/ws/counter") as ws:
            ws.send_text("decrement")
            response = ws.receive_json()
            assert response["counter"] == -1

    def test_counter_reset(self, client):
        """Test counter reset."""
        with client.websocket_connect("/ws/counter") as ws:
            # Increment a few times
            for _ in range(5):
                ws.send_text("increment")
                ws.receive_json()

            # Reset
            ws.send_text("reset")
            response = ws.receive_json()
            assert response["counter"] == 0

    def test_counter_set_value(self, client):
        """Test setting counter to specific value."""
        with client.websocket_connect("/ws/counter") as ws:
            value = random.randint(1, 100)
            ws.send_text(str(value))
            response = ws.receive_json()
            assert response["counter"] == value

    def test_counter_get(self, client):
        """Test getting counter value."""
        with client.websocket_connect("/ws/counter") as ws:
            ws.send_text("42")
            ws.receive_json()

            ws.send_text("get")
            response = ws.receive_json()
            assert response["counter"] == 42


class TestChatRoomWebSocket:
    """Test chat room WebSocket."""

    def test_join_room(self, client):
        """Test joining a chat room."""
        room_id = random_string(8)
        username = random_string(10)

        with client.websocket_connect(f"/ws/chat/{room_id}?username={username}") as ws:
            # Should receive join announcement
            msg = ws.receive_text()
            data = json.loads(msg)
            assert data["type"] == "system"
            assert username in data["message"]

    def test_send_message_in_room(self, client):
        """Test sending a message in a room."""
        room_id = random_string(8)
        username = random_string(10)

        with client.websocket_connect(f"/ws/chat/{room_id}?username={username}") as ws:
            ws.receive_text()  # Join message

            message = random_string(20)
            ws.send_text(message)

            # Should receive own message broadcast
            response = ws.receive_text()
            data = json.loads(response)
            assert data["type"] == "message"
            assert data["username"] == username
            assert data["message"] == message


class TestConnectionManagement:
    """Test connection tracking."""

    def test_connection_tracked(self, client):
        """Test that connections are tracked."""
        client_id = random_string(10)

        with client.websocket_connect(f"/ws/client/{client_id}") as ws:
            ws.receive_text()  # Welcome

            # Check connection is tracked
            response = client.get("/connections")
            assert response.json()["count"] >= 1

    def test_connection_events_logged(self, client):
        """Test that connection events are logged."""
        client_id = random_string(10)

        with client.websocket_connect(f"/ws/client/{client_id}") as ws:
            ws.receive_text()

        # Check events after disconnect
        response = client.get("/connection-events")
        events = response.json()["events"]

        # Should have connect event
        connect_events = [e for e in events if e["event"] == "connect"]
        assert any(e["client_id"] == client_id for e in connect_events)


class TestMessageLogging:
    """Test message logging."""

    def test_messages_logged(self, client):
        """Test that messages are logged."""
        with client.websocket_connect("/ws/echo") as ws:
            message = random_string(15)
            ws.send_text(message)
            ws.receive_text()

        response = client.get("/message-log")
        messages = response.json()["messages"]

        assert len(messages) > 0
        assert any(m.get("received") == message for m in messages)

    def test_clear_logs(self, client):
        """Test clearing logs."""
        with client.websocket_connect("/ws/echo") as ws:
            ws.send_text("test")
            ws.receive_text()

        # Verify logs exist
        response = client.get("/message-log")
        assert len(response.json()["messages"]) > 0

        # Clear logs
        client.post("/clear-logs")

        # Verify cleared
        response = client.get("/message-log")
        assert len(response.json()["messages"]) == 0


class TestComplexScenarios:
    """Test complex WebSocket scenarios."""

    def test_rapid_messages(self, client):
        """Test sending many messages rapidly."""
        with client.websocket_connect("/ws/echo") as ws:
            messages = [random_string(20) for _ in range(50)]

            for msg in messages:
                ws.send_text(msg)
                response = ws.receive_text()
                assert response == f"Echo: {msg}"

    def test_alternating_text_json(self, client):
        """Test alternating between text and JSON endpoints."""
        # This tests switching between different WebSocket endpoints
        for _ in range(5):
            # Text echo
            with client.websocket_connect("/ws/echo") as ws:
                msg = random_string(10)
                ws.send_text(msg)
                assert ws.receive_text() == f"Echo: {msg}"

            # JSON
            with client.websocket_connect("/ws/json") as ws:
                ws.send_json({"action": "ping", "timestamp": 123})
                response = ws.receive_json()
                assert response["action"] == "pong"

    def test_counter_persistence_within_connection(self, client):
        """Test that counter state persists within a connection."""
        with client.websocket_connect("/ws/counter") as ws:
            # Perform sequence of operations
            operations = [
                ("increment", 1),
                ("increment", 2),
                ("increment", 3),
                ("decrement", 2),
                ("increment", 3),
                ("reset", 0),
                ("increment", 1),
            ]

            for op, expected in operations:
                ws.send_text(op)
                response = ws.receive_json()
                assert response["counter"] == expected


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
