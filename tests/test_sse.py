"""
Server-Sent Events (SSE) Tests

Comprehensive test suite for SSE functionality.
"""

import sys
sys.path.insert(0, '/Users/bengamble/FasterAPI')

from fasterapi.http.sse import SSEConnection, SSEResponse, sse_endpoint
import time


class TestSSEConnection:
    """Test SSE connection functionality."""
    
    def test_create_connection(self):
        """Test creating an SSE connection."""
        conn = SSEConnection()
        assert conn.is_open()
        assert conn.events_sent() == 0
        print("✅ Create connection")
    
    def test_send_simple_message(self):
        """Test sending a simple text message."""
        conn = SSEConnection()
        message = conn.send("Hello World")
        
        # Verify format
        assert "data: Hello World\n" in message
        assert message.endswith("\n\n")
        assert conn.events_sent() == 1
        print("✅ Send simple message")
    
    def test_send_with_event_type(self):
        """Test sending message with event type."""
        conn = SSEConnection()
        message = conn.send("Hello", event="chat")
        
        # Verify format
        assert "event: chat\n" in message
        assert "data: Hello\n" in message
        print("✅ Send with event type")
    
    def test_send_with_id(self):
        """Test sending message with event ID."""
        conn = SSEConnection()
        message = conn.send("Test", id="123")
        
        # Verify format
        assert "id: 123\n" in message
        assert "data: Test\n" in message
        assert conn.last_event_id == "123"
        print("✅ Send with event ID")
    
    def test_send_with_retry(self):
        """Test sending message with retry time."""
        conn = SSEConnection()
        message = conn.send("Test", retry=5000)
        
        # Verify format
        assert "retry: 5000\n" in message
        assert "data: Test\n" in message
        print("✅ Send with retry")
    
    def test_send_multiline_data(self):
        """Test sending multiline data."""
        conn = SSEConnection()
        message = conn.send("Line 1\nLine 2\nLine 3")
        
        # Each line should be prefixed with "data: "
        assert "data: Line 1\n" in message
        assert "data: Line 2\n" in message
        assert "data: Line 3\n" in message
        print("✅ Send multiline data")
    
    def test_send_json_data(self):
        """Test sending JSON data."""
        conn = SSEConnection()
        message = conn.send({"message": "hello", "count": 42})
        
        # Should be JSON-encoded
        assert '"message"' in message
        assert '"hello"' in message
        assert '"count"' in message
        assert '42' in message
        print("✅ Send JSON data")
    
    def test_send_comment(self):
        """Test sending a comment."""
        conn = SSEConnection()
        message = conn.send_comment("This is a comment")
        
        # Comments start with ':'
        assert message.startswith(": ")
        assert "This is a comment" in message
        print("✅ Send comment")
    
    def test_ping(self):
        """Test ping (keep-alive)."""
        conn = SSEConnection()
        message = conn.ping()
        
        # Ping is a comment
        assert message.startswith(": ")
        assert "ping" in message
        print("✅ Ping")
    
    def test_close_connection(self):
        """Test closing connection."""
        conn = SSEConnection()
        assert conn.is_open()
        
        conn.close()
        assert not conn.is_open()
        
        # Sending after close should raise
        try:
            conn.send("Test")
            assert False, "Should raise after close"
        except RuntimeError:
            pass
        
        print("✅ Close connection")
    
    def test_multiple_events(self):
        """Test sending multiple events."""
        conn = SSEConnection()
        
        for i in range(10):
            conn.send(f"Event {i}", event="update", id=str(i))
        
        assert conn.events_sent() == 10
        assert conn.last_event_id == "9"
        print("✅ Multiple events")


class TestSSEResponse:
    """Test SSE response helper."""
    
    def test_create_response(self):
        """Test creating SSE response."""
        events = []
        
        def handler(sse):
            events.append("handler called")
            sse.send("test")
        
        response = SSEResponse.create(handler)
        
        # Verify headers
        assert response["headers"]["Content-Type"] == "text/event-stream"
        assert response["headers"]["Cache-Control"] == "no-cache"
        assert response["headers"]["Connection"] == "keep-alive"
        assert response["type"] == "sse"
        
        # Verify handler was called
        assert len(events) == 1
        print("✅ Create SSE response")
    
    def test_custom_headers(self):
        """Test SSE response with custom headers."""
        def handler(sse):
            pass
        
        response = SSEResponse.create(
            handler,
            headers={"X-Custom": "value"}
        )
        
        assert response["headers"]["X-Custom"] == "value"
        assert response["headers"]["Content-Type"] == "text/event-stream"
        print("✅ Custom headers")


class TestSSEDecorator:
    """Test SSE decorator."""
    
    def test_decorator_basic(self):
        """Test basic decorator usage."""
        @sse_endpoint
        def event_stream(sse):
            sse.send("test")
        
        result = event_stream()
        
        assert result["type"] == "sse"
        assert result["headers"]["Content-Type"] == "text/event-stream"
        print("✅ SSE decorator")


class TestSSEFormats:
    """Test SSE message formatting."""
    
    def test_format_simple(self):
        """Test simple message format."""
        conn = SSEConnection()
        msg = conn.send("Hello")
        
        expected = "data: Hello\n\n"
        assert msg == expected
        print("✅ Format simple message")
    
    def test_format_with_all_fields(self):
        """Test message with all fields."""
        conn = SSEConnection()
        msg = conn.send("Test", event="chat", id="123", retry=5000)
        
        lines = msg.split('\n')
        assert "event: chat" in lines
        assert "id: 123" in lines
        assert "retry: 5000" in lines
        assert "data: Test" in lines
        assert lines[-1] == ""  # Ends with blank line
        print("✅ Format complete message")
    
    def test_format_multiline(self):
        """Test multiline data format."""
        conn = SSEConnection()
        msg = conn.send("Line 1\nLine 2\nLine 3")
        
        assert msg.count("data: ") == 3
        assert "data: Line 1\n" in msg
        assert "data: Line 2\n" in msg
        assert "data: Line 3\n" in msg
        print("✅ Format multiline data")


class TestSSEReconnection:
    """Test reconnection scenarios."""
    
    def test_last_event_id_tracking(self):
        """Test last event ID is tracked."""
        conn = SSEConnection()
        
        conn.send("Event 1", id="1")
        conn.send("Event 2", id="2")
        conn.send("Event 3", id="3")
        
        assert conn.last_event_id == "3"
        print("✅ Last event ID tracking")
    
    def test_reconnection_resume(self):
        """Test client can resume from last event ID."""
        # First connection
        conn1 = SSEConnection()
        conn1.send("Event 1", id="1")
        conn1.send("Event 2", id="2")
        last_id = conn1.last_event_id
        conn1.close()
        
        # Reconnect with last ID
        conn2 = SSEConnection()
        conn2.set_last_event_id(last_id)
        
        # Can resume from event 3
        conn2.send("Event 3", id="3")
        
        assert conn2.last_event_id == "3"
        print("✅ Reconnection resume")


class TestSSEUseCases:
    """Test real-world use cases."""
    
    def test_chat_messages(self):
        """Test chat message streaming."""
        conn = SSEConnection()
        
        messages = [
            {"user": "Alice", "text": "Hello!"},
            {"user": "Bob", "text": "Hi there!"},
            {"user": "Alice", "text": "How are you?"},
        ]
        
        for i, msg in enumerate(messages):
            conn.send(msg, event="chat", id=str(i))
        
        assert conn.events_sent() == 3
        print("✅ Chat messages")
    
    def test_progress_updates(self):
        """Test progress update streaming."""
        conn = SSEConnection()
        
        for progress in [0, 25, 50, 75, 100]:
            conn.send(
                {"progress": progress, "status": "processing"},
                event="progress"
            )
        
        assert conn.events_sent() == 5
        print("✅ Progress updates")
    
    def test_live_metrics(self):
        """Test live metrics streaming."""
        conn = SSEConnection()
        
        # Send metrics every second (simulated)
        for i in range(5):
            conn.send({
                "cpu": 45 + i,
                "memory": 60 + i,
                "requests": 1000 + i * 10
            }, event="metrics", id=f"m{i}")
        
        assert conn.events_sent() == 5
        print("✅ Live metrics")
    
    def test_notifications(self):
        """Test notification streaming."""
        conn = SSEConnection()
        
        notifications = [
            {"type": "info", "message": "Task started"},
            {"type": "warning", "message": "High CPU usage"},
            {"type": "success", "message": "Task completed"},
        ]
        
        for notif in notifications:
            conn.send(notif, event="notification")
        
        assert conn.events_sent() == 3
        print("✅ Notifications")


def run_tests():
    """Run all tests."""
    print("╔══════════════════════════════════════════════════════════╗")
    print("║          SSE Comprehensive Test Suite                   ║")
    print("╚══════════════════════════════════════════════════════════╝")
    print()
    
    # Connection tests
    print("=== SSEConnection Tests ===")
    test = TestSSEConnection()
    test.test_create_connection()
    test.test_send_simple_message()
    test.test_send_with_event_type()
    test.test_send_with_id()
    test.test_send_with_retry()
    test.test_send_multiline_data()
    test.test_send_json_data()
    test.test_send_comment()
    test.test_ping()
    test.test_close_connection()
    test.test_multiple_events()
    print()
    
    # Response tests
    print("=== SSEResponse Tests ===")
    test = TestSSEResponse()
    test.test_create_response()
    test.test_custom_headers()
    print()
    
    # Decorator tests
    print("=== SSE Decorator Tests ===")
    test = TestSSEDecorator()
    test.test_decorator_basic()
    print()
    
    # Format tests
    print("=== SSE Format Tests ===")
    test = TestSSEFormats()
    test.test_format_simple()
    test.test_format_with_all_fields()
    test.test_format_multiline()
    print()
    
    # Reconnection tests
    print("=== Reconnection Tests ===")
    test = TestSSEReconnection()
    test.test_last_event_id_tracking()
    test.test_reconnection_resume()
    print()
    
    # Use case tests
    print("=== Real-World Use Cases ===")
    test = TestSSEUseCases()
    test.test_chat_messages()
    test.test_progress_updates()
    test.test_live_metrics()
    test.test_notifications()
    print()
    
    print("============================================================")
    print("All SSE tests passed! 🎉")
    print("Total: 24 tests")


if __name__ == "__main__":
    run_tests()

