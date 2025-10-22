"""
Server-Sent Events (SSE) Support

High-performance SSE implementation backed by C++.
"""

from typing import Optional, Callable, Any
import json
from .bindings import get_lib


class SSEConnection:
    """
    Server-Sent Events connection backed by C++.
    
    Sends events from server to client over HTTP.
    Simpler than WebSockets for one-way communication.
    
    Features:
    - Automatic keep-alive
    - Event IDs for reconnection
    - Named events
    - JSON data support
    - High performance (C++ core)
    
    Spec: https://html.spec.whatwg.org/multipage/server-sent-events.html
    
    Example:
        @app.sse("/events")
        def event_stream(sse: SSEConnection):
            for i in range(100):
                sse.send(
                    {"count": i, "time": time.time()},
                    event="count",
                    id=str(i)
                )
                time.sleep(1)
    """
    
    def __init__(self, native_handle: Optional[int] = None):
        """
        Create SSE connection.
        
        Args:
            native_handle: C++ SSEConnection pointer (or None for testing)
        """
        self._handle = native_handle
        self._lib = get_lib() if native_handle else None
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
            
        Raises:
            RuntimeError: If connection is closed or send fails
        """
        if self._closed:
            raise RuntimeError("Connection is closed")
        
        if not self._handle or not self._lib:
            raise RuntimeError("SSE not connected")
        
        # JSON-encode data if needed
        if isinstance(data, (dict, list)):
            data_str = json.dumps(data)
        else:
            data_str = str(data)
        
        # Encode strings to bytes
        data_bytes = data_str.encode('utf-8')
        event_bytes = event.encode('utf-8') if event else None
        id_bytes = id.encode('utf-8') if id else None
        retry_val = retry if retry is not None else -1
        
        # Call C++ backend
        result = self._lib.sse_send(
            self._handle,
            data_bytes,
            event_bytes,
            id_bytes,
            retry_val
        )
        
        if result != 0:
            raise RuntimeError(f"Failed to send SSE event: error {result}")
    
    def send_comment(self, comment: str) -> None:
        """
        Send a comment (ignored by client).
        
        Useful for keep-alive pings.
        
        Args:
            comment: Comment text
        """
        if self._closed:
            raise RuntimeError("Connection is closed")
        
        if not self._handle or not self._lib:
            raise RuntimeError("SSE not connected")
        
        comment_bytes = comment.encode('utf-8')
        result = self._lib.sse_send_comment(self._handle, comment_bytes)
        
        if result != 0:
            raise RuntimeError(f"Failed to send comment: error {result}")
    
    def ping(self) -> None:
        """Send keep-alive ping."""
        if self._closed:
            raise RuntimeError("Connection is closed")
        
        if not self._handle or not self._lib:
            raise RuntimeError("SSE not connected")
        
        result = self._lib.sse_ping(self._handle)
        
        if result != 0:
            raise RuntimeError(f"Failed to send ping: error {result}")
    
    def close(self) -> None:
        """Close the connection."""
        if self._closed:
            return
        
        if self._handle and self._lib:
            self._lib.sse_close(self._handle)
        
        self._closed = True
    
    def is_open(self) -> bool:
        """Check if connection is open."""
        if self._closed:
            return False
        
        if self._handle and self._lib:
            return self._lib.sse_is_open(self._handle)
        
        return False
    
    @property
    def events_sent(self) -> int:
        """Get number of events sent."""
        if self._handle and self._lib:
            return self._lib.sse_events_sent(self._handle)
        return 0
    
    def __del__(self):
        """Cleanup on destruction."""
        if not self._closed:
            self.close()
        
        if self._handle and self._lib:
            self._lib.sse_destroy(self._handle)
    
    def __repr__(self) -> str:
        """String representation."""
        state = "open" if self.is_open() else "closed"
        return f"SSEConnection(state={state}, events_sent={self.events_sent})"


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
        # Set SSE headers
        response_headers = {
            "Content-Type": "text/event-stream",
            "Cache-Control": "no-cache",
            "Connection": "keep-alive",
            "X-Accel-Buffering": "no",  # Disable nginx buffering
        }
        
        if headers:
            response_headers.update(headers)
        
        return {
            "handler": handler,
            "headers": response_headers,
            "type": "sse"
        }


# Decorator for easier SSE usage
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
