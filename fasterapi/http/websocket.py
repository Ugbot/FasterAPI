"""
High-level WebSocket API for FasterAPI.

Provides async-friendly WebSocket interface built on top of Cython bindings.
"""

from typing import Union, Optional
import asyncio
import json
from collections import deque

try:
    from fasterapi.http.server_cy import PyWebSocketConnection
except ImportError:
    # Fallback for development/testing
    PyWebSocketConnection = None


class WebSocket:
    """
    High-level WebSocket API.

    Wraps PyWebSocketConnection with async interface and message queuing.

    Example:
        @app.websocket("/ws")
        async def websocket_handler(ws: WebSocket):
            await ws.send("Hello!")
            message = await ws.receive()
            await ws.close()
    """

    def __init__(self, connection: "PyWebSocketConnection"):
        """
        Create WebSocket wrapper.

        Args:
            connection: Cython WebSocket connection object
        """
        self._conn = connection
        self._receive_queue = deque()
        self._closed = False

    async def send(self, message: Union[str, bytes, dict, list]) -> None:
        """
        Send message (auto-detect type).

        Args:
            message: Text string, binary bytes, or JSON-serializable object

        Raises:
            RuntimeError: If connection is closed or send fails
            TypeError: If message type is not supported
        """
        if self._closed:
            raise RuntimeError("WebSocket connection is closed")

        if isinstance(message, str):
            await self.send_text(message)
        elif isinstance(message, bytes):
            await self.send_binary(message)
        elif isinstance(message, (dict, list)):
            await self.send_json(message)
        else:
            raise TypeError(f"Unsupported message type: {type(message)}")

    async def send_text(self, text: str) -> None:
        """
        Send text message.

        Args:
            text: Text message

        Raises:
            RuntimeError: If connection is closed or send fails
        """
        if self._closed:
            raise RuntimeError("WebSocket connection is closed")

        # Call Cython binding (releases GIL)
        await asyncio.get_event_loop().run_in_executor(None, self._conn.send_text, text)

    async def send_binary(self, data: bytes) -> None:
        """
        Send binary message.

        Args:
            data: Binary data

        Raises:
            RuntimeError: If connection is closed or send fails
        """
        if self._closed:
            raise RuntimeError("WebSocket connection is closed")

        # Call Cython binding (releases GIL)
        await asyncio.get_event_loop().run_in_executor(
            None, self._conn.send_binary, data
        )

    async def send_json(self, obj: Union[dict, list]) -> None:
        """
        Send JSON message.

        Args:
            obj: JSON-serializable object (dict or list)

        Raises:
            RuntimeError: If connection is closed or send fails
            TypeError: If object is not JSON-serializable
        """
        text = json.dumps(obj, ensure_ascii=False)
        await self.send_text(text)

    async def receive(self) -> Union[str, bytes]:
        """
        Receive message from client.

        This is a simplified implementation. In a real implementation,
        we would have a background thread reading from the socket and
        populating the queue.

        Returns:
            Message data (str for text, bytes for binary)

        Raises:
            RuntimeError: If connection is closed
        """
        if self._closed:
            raise RuntimeError("WebSocket connection is closed")

        # Wait for message in queue
        while not self._receive_queue:
            if not self.is_open:
                raise RuntimeError(
                    "WebSocket connection closed while waiting for message"
                )
            await asyncio.sleep(0.01)  # Poll for messages

        return self._receive_queue.popleft()

    async def receive_text(self) -> str:
        """
        Receive text message.

        Returns:
            Text message

        Raises:
            RuntimeError: If connection is closed
            TypeError: If received message is not text
        """
        message = await self.receive()
        if not isinstance(message, str):
            raise TypeError(f"Expected text message, got {type(message)}")
        return message

    async def receive_binary(self) -> bytes:
        """
        Receive binary message.

        Returns:
            Binary message

        Raises:
            RuntimeError: If connection is closed
            TypeError: If received message is not binary
        """
        message = await self.receive()
        if not isinstance(message, bytes):
            raise TypeError(f"Expected binary message, got {type(message)}")
        return message

    async def receive_json(self) -> Union[dict, list]:
        """
        Receive JSON message.

        Returns:
            Parsed JSON object

        Raises:
            RuntimeError: If connection is closed
            json.JSONDecodeError: If message is not valid JSON
        """
        text = await self.receive_text()
        return json.loads(text)

    async def ping(self, data: Optional[bytes] = None) -> None:
        """
        Send ping frame.

        Args:
            data: Optional ping payload

        Raises:
            RuntimeError: If ping fails
        """
        if self._closed:
            raise RuntimeError("WebSocket connection is closed")

        await asyncio.get_event_loop().run_in_executor(None, self._conn.ping, data)

    async def pong(self, data: Optional[bytes] = None) -> None:
        """
        Send pong frame.

        Args:
            data: Optional pong payload

        Raises:
            RuntimeError: If pong fails
        """
        if self._closed:
            raise RuntimeError("WebSocket connection is closed")

        await asyncio.get_event_loop().run_in_executor(None, self._conn.pong, data)

    async def close(self, code: int = 1000, reason: str = "") -> None:
        """
        Close WebSocket connection.

        Args:
            code: WebSocket close code (default: 1000 = normal closure)
            reason: Close reason string
        """
        if self._closed:
            return

        self._closed = True
        await asyncio.get_event_loop().run_in_executor(
            None, self._conn.close, code, reason
        )

    @property
    def is_open(self) -> bool:
        """Check if connection is open."""
        if self._closed:
            return False
        return self._conn.is_open

    @property
    def connection_id(self) -> int:
        """Get connection ID."""
        return self._conn.connection_id

    @property
    def messages_sent(self) -> int:
        """Get number of messages sent."""
        return self._conn.messages_sent

    @property
    def messages_received(self) -> int:
        """Get number of messages received."""
        return self._conn.messages_received

    @property
    def bytes_sent(self) -> int:
        """Get total bytes sent."""
        return self._conn.bytes_sent

    @property
    def bytes_received(self) -> int:
        """Get total bytes received."""
        return self._conn.bytes_received

    def _on_message_received(self, message: Union[str, bytes]) -> None:
        """
        Internal: Called by C++ layer when message is received.

        Args:
            message: Received message
        """
        self._receive_queue.append(message)


__all__ = ["WebSocket"]
