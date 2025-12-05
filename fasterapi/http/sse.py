"""
High-level Server-Sent Events (SSE) API for FasterAPI.

Provides async-friendly SSE interface built on top of Cython bindings.
"""

from typing import Union, Optional, Any
import asyncio
import json

try:
    from fasterapi.http.server_cy import PySSEConnection
except ImportError:
    # Fallback for development/testing
    PySSEConnection = None


class SSE:
    """
    High-level Server-Sent Events API.

    Wraps PySSEConnection with async interface for streaming events to clients.

    Example:
        @app.sse("/sse/events")
        async def sse_handler(sse: SSE):
            for i in range(10):
                await sse.send({"count": i}, event="counter")
                await asyncio.sleep(1)
    """

    def __init__(self, connection: "PySSEConnection"):
        """
        Create SSE wrapper.

        Args:
            connection: Cython SSE connection object
        """
        self._conn = connection
        self._closed = False

    async def send(
        self,
        data: Union[str, dict, list, Any],
        event: Optional[str] = None,
        event_id: Optional[str] = None,
        retry: Optional[int] = None,
    ) -> None:
        """
        Send SSE event to client.

        Args:
            data: Event data (str, dict, list, or JSON-serializable object)
            event: Event type (optional, defaults to "message")
            event_id: Event ID for reconnection (optional)
            retry: Retry time in milliseconds (optional)

        Raises:
            RuntimeError: If connection is closed or send fails
        """
        if self._closed:
            raise RuntimeError("SSE connection is closed")

        # Convert data to string if needed
        if isinstance(data, str):
            data_str = data
        else:
            # Serialize to JSON
            data_str = json.dumps(data, ensure_ascii=False)

        # Determine retry value
        retry_ms = retry if retry is not None else -1

        # Call Cython binding (releases GIL)
        await asyncio.get_event_loop().run_in_executor(
            None, self._conn.send, data_str, event, event_id, retry_ms
        )

    async def send_json(
        self,
        obj: Union[dict, list],
        event: Optional[str] = None,
        event_id: Optional[str] = None,
    ) -> None:
        """
        Send JSON event.

        Args:
            obj: JSON-serializable object
            event: Event type (optional)
            event_id: Event ID (optional)

        Raises:
            RuntimeError: If connection is closed or send fails
        """
        await self.send(obj, event=event, event_id=event_id)

    async def send_text(
        self, text: str, event: Optional[str] = None, event_id: Optional[str] = None
    ) -> None:
        """
        Send text event.

        Args:
            text: Text data
            event: Event type (optional)
            event_id: Event ID (optional)

        Raises:
            RuntimeError: If connection is closed or send fails
        """
        await self.send(text, event=event, event_id=event_id)

    async def send_comment(self, comment: str) -> None:
        """
        Send comment (ignored by client, useful for keep-alive).

        Args:
            comment: Comment text

        Raises:
            RuntimeError: If connection is closed or send fails
        """
        if self._closed:
            raise RuntimeError("SSE connection is closed")

        await asyncio.get_event_loop().run_in_executor(
            None, self._conn.send_comment, comment
        )

    async def ping(self) -> None:
        """
        Send keep-alive ping.

        Sends a comment to keep connection alive and prevent timeouts.

        Raises:
            RuntimeError: If ping fails
        """
        if self._closed:
            raise RuntimeError("SSE connection is closed")

        success = await asyncio.get_event_loop().run_in_executor(None, self._conn.ping)

        if not success:
            raise RuntimeError("Failed to send SSE ping")

    async def close(self) -> None:
        """
        Close SSE connection.

        After calling close(), no more events can be sent.
        """
        if self._closed:
            return

        self._closed = True
        await asyncio.get_event_loop().run_in_executor(None, self._conn.close)

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
    def events_sent(self) -> int:
        """Get number of events sent."""
        return self._conn.events_sent

    @property
    def bytes_sent(self) -> int:
        """Get total bytes sent."""
        return self._conn.bytes_sent

    async def keep_alive(self, interval: float = 30.0) -> None:
        """
        Start automatic keep-alive loop.

        Sends ping comments at regular intervals to prevent connection timeout.
        Runs until connection is closed.

        Args:
            interval: Interval in seconds between pings (default: 30)
        """
        while self.is_open and not self._closed:
            try:
                await self.ping()
            except RuntimeError:
                # Connection closed
                break

            await asyncio.sleep(interval)


class SSEStream:
    """
    Context manager for SSE streaming.

    Provides automatic keep-alive and clean connection closing.

    Example:
        @app.sse("/sse/events")
        async def sse_handler(sse: SSE):
            async with SSEStream(sse) as stream:
                for i in range(100):
                    await stream.send({"count": i})
                    await asyncio.sleep(1)
    """

    def __init__(self, sse: SSE, keep_alive_interval: float = 30.0):
        """
        Create SSE stream manager.

        Args:
            sse: SSE connection
            keep_alive_interval: Keep-alive ping interval in seconds
        """
        self.sse = sse
        self.keep_alive_interval = keep_alive_interval
        self._keep_alive_task = None

    async def __aenter__(self) -> SSE:
        """Enter context: Start keep-alive."""
        # Start background keep-alive task
        self._keep_alive_task = asyncio.create_task(
            self.sse.keep_alive(self.keep_alive_interval)
        )
        return self.sse

    async def __aexit__(self, exc_type, exc_val, exc_tb):
        """Exit context: Stop keep-alive and close connection."""
        # Cancel keep-alive task
        if self._keep_alive_task:
            self._keep_alive_task.cancel()
            try:
                await self._keep_alive_task
            except asyncio.CancelledError:
                pass

        # Close connection
        await self.sse.close()

        return False  # Don't suppress exceptions


__all__ = ["SSE", "SSEStream"]
