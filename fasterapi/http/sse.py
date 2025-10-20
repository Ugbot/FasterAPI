"""
Server-Sent Events (SSE) Support

Provides SSE connections for real-time server-to-client streaming.
"""

import ctypes
from ctypes import c_void_p, c_char_p, c_uint64, c_int
from typing import Optional, Callable, Any
import json
import time


class SSEConnection:
    """
    Server-Sent Events connection.
    
    Sends events from server to client over HTTP.
    Simpler than WebSockets for one-way communication.
    
    Features:
    - Automatic keep-alive
    - Event IDs for reconnection
    - Named events
    - JSON data support
    
    Spec: https://html.spec.whatwg.org/multipage/server-sent-events.html
    """
    
    def __init__(self, handle: Optional[int] = None):
        """
        Create SSE connection.
        
        Args:
            handle: C++ connection handle (opaque pointer)
        """
        self._handle = handle
        self._events_sent = 0
        self._last_event_id: Optional[str] = None
        self._closed = False
    
    def send(
        self,
        data: Any,
        event: Optional[str] = None,
        id: Optional[str] = None,
        retry: Optional[int] = None
    ) -> None:
        """
        Send an event to the client.
        
        Args:
            data: Event data (will be JSON-encoded if dict/list)
            event: Event type (optional, defaults to "message")
            id: Event ID (optional, for reconnection)
            retry: Retry time in milliseconds (optional)
        
        Examples:
            sse.send("Hello World")
            sse.send({"message": "hi"}, event="chat", id="123")
            sse.send([1, 2, 3], event="update")
        """
        if self._closed:
            raise RuntimeError("Connection is closed")
        
        # JSON-encode data if needed
        if isinstance(data, (dict, list)):
            data_str = json.dumps(data)
        else:
            data_str = str(data)
        
        # Format SSE message
        message = self._format_message(data_str, event, id, retry)
        
        # Send (in real implementation, would call C++ function)
        # For now, just track
        self._events_sent += 1
        if id:
            self._last_event_id = id
        
        return message  # Return for testing/inspection
    
    def send_comment(self, comment: str) -> None:
        """
        Send a comment (ignored by client).
        
        Useful for keep-alive pings.
        
        Args:
            comment: Comment text
        """
        if self._closed:
            raise RuntimeError("Connection is closed")
        
        return f": {comment}\n\n"
    
    def ping(self) -> None:
        """Send keep-alive ping."""
        return self.send_comment("ping")
    
    def close(self) -> None:
        """Close the connection."""
        if not self._closed:
            self._closed = True
            # In real implementation, would call C++ close
    
    def is_open(self) -> bool:
        """Check if connection is open."""
        return not self._closed
    
    def events_sent(self) -> int:
        """Get number of events sent."""
        return self._events_sent
    
    @property
    def last_event_id(self) -> Optional[str]:
        """Get last event ID."""
        return self._last_event_id
    
    def set_last_event_id(self, id: str) -> None:
        """Set last event ID (for reconnection)."""
        self._last_event_id = id
    
    def _format_message(
        self,
        data: str,
        event: Optional[str],
        id: Optional[str],
        retry: Optional[int]
    ) -> str:
        """
        Format message according to SSE spec.
        
        SSE format:
            event: <event_type>
            id: <event_id>
            retry: <milliseconds>
            data: <line1>
            data: <line2>
            <blank line>
        """
        lines = []
        
        # Event type
        if event:
            lines.append(f"event: {event}")
        
        # Event ID
        if id:
            lines.append(f"id: {id}")
        
        # Retry
        if retry is not None and retry >= 0:
            lines.append(f"retry: {retry}")
        
        # Data (split by newlines)
        for line in data.split('\n'):
            lines.append(f"data: {line}")
        
        # Add blank line to signal end of event
        lines.append('')
        
        return '\n'.join(lines) + '\n'


class SSEResponse:
    """
    SSE response helper.
    
    Sets proper headers and creates SSE connection.
    """
    
    @staticmethod
    def create(
        handler: Callable[[SSEConnection], None],
        headers: Optional[dict] = None
    ) -> dict:
        """
        Create SSE response.
        
        Args:
            handler: Function that handles the SSE connection
            headers: Additional headers
        
        Returns:
            Response dict with proper SSE headers
        
        Example:
            @app.get("/events")
            def event_stream(req, res):
                def handle_sse(sse):
                    for i in range(10):
                        sse.send({"count": i}, event="count", id=str(i))
                        time.sleep(1)
                
                return SSEResponse.create(handle_sse)
        """
        # Create connection
        conn = SSEConnection()
        
        # Set SSE headers
        response_headers = {
            "Content-Type": "text/event-stream",
            "Cache-Control": "no-cache",
            "Connection": "keep-alive",
            "X-Accel-Buffering": "no",  # Disable nginx buffering
        }
        
        if headers:
            response_headers.update(headers)
        
        # Call handler
        handler(conn)
        
        return {
            "connection": conn,
            "headers": response_headers,
            "type": "sse"
        }


# Decorators for easier SSE usage
def sse_endpoint(func: Callable) -> Callable:
    """
    Decorator for SSE endpoints.
    
    Automatically sets up SSE headers and connection.
    
    Example:
        @app.get("/events")
        @sse_endpoint
        def event_stream(sse: SSEConnection):
            while sse.is_open():
                sse.send({"time": time.time()}, event="time")
                time.sleep(1)
    """
    def wrapper(*args, **kwargs):
        def sse_handler(conn: SSEConnection):
            func(conn, *args, **kwargs)
        
        return SSEResponse.create(sse_handler)
    
    return wrapper


__all__ = [
    'SSEConnection',
    'SSEResponse',
    'sse_endpoint',
]

