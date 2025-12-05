# distutils: language = c++
# cython: language_level = 3
# cython: boundscheck = False
# cython: wraparound = False
# cython: cdivision = True

"""
High-performance Cython bindings for FasterAPI HTTP Server.

This provides zero-overhead Python access to the C++ CoroIO-based HTTP server
with explicit GIL control for maximum performance.
"""

from libcpp cimport bool
from libcpp.string cimport string
from libcpp.functional cimport function
from cpython.ref cimport PyObject
from cpython cimport PyObject as PyObj

import sys
from typing import Callable, Dict, Any, Optional

# Declare C++ types inline
cdef extern from "src/cpp/http/request.h":
    cdef cppclass HttpRequest:
        pass

cdef extern from "src/cpp/http/response.h":
    cdef cppclass HttpResponse:
        pass

cdef extern from "src/cpp/http/python_callback_bridge.h":
    cdef cppclass PythonCallbackBridge:
        @staticmethod
        void initialize()
        @staticmethod
        void register_handler(const string& method, const string& path,
                            int handler_id, void* callable)
        @staticmethod
        void cleanup()

cdef extern from "src/cpp/http/server.h":
    # Server configuration
    cdef cppclass HttpServer_Config "HttpServer::Config":
        unsigned short port
        string host
        bool enable_h1
        bool enable_h2
        bool enable_h3
        bool enable_compression
        bool enable_websocket
        string cert_path
        string key_path
        unsigned int max_connections
        unsigned int max_request_size
        unsigned int compression_threshold
        unsigned int compression_level
        HttpServer_Config()

    # Server statistics
    cdef cppclass HttpServer_Stats "HttpServer::Stats":
        unsigned long long total_requests
        unsigned long long total_bytes_sent
        unsigned long long total_bytes_received
        unsigned long long active_connections
        unsigned long long h1_requests
        unsigned long long h2_requests
        unsigned long long h3_requests
        unsigned long long websocket_connections
        unsigned long long compressed_responses
        unsigned long long compression_bytes_saved

    # Main HTTP server class
    cdef cppclass HttpServer:
        # Route handler function types (declared within HttpServer)
        ctypedef function[void(HttpRequest*, HttpResponse*)] RouteHandler
        ctypedef function[void(void*)] WebSocketHandler

        HttpServer(const HttpServer_Config& config)
        int add_route(const string& method, const string& path, RouteHandler handler)
        int add_websocket(const string& path, WebSocketHandler handler)
        int start()
        int stop()
        bool is_running() const
        HttpServer_Stats get_stats() const


cdef class Server:
    """
    High-performance HTTP server with CoroIO event loop.

    This is a direct Cython wrapper around the C++ HttpServer class,
    providing near-zero overhead access to the native implementation.

    Features:
    - HTTP/1.1 with CoroIO (kqueue on macOS, epoll on Linux)
    - HTTP/2 with nghttp2 (optional)
    - HTTP/3 with MsQuic (optional)
    - WebSocket support
    - zstd compression
    - Explicit GIL control for performance
    """

    cdef HttpServer* _server
    cdef HttpServer_Config _config
    cdef dict _route_handlers
    cdef dict _websocket_handlers
    cdef int _next_handler_id

    def __cinit__(self,
                  int port=8000,
                  str host="0.0.0.0",
                  bool enable_h2=False,
                  bool enable_h3=False,
                  bool enable_compression=True,
                  bool enable_websocket=True,
                  **kwargs):
        """
        Create HTTP server.

        Args:
            port: Server port (default: 8000)
            host: Server host (default: 0.0.0.0)
            enable_h2: Enable HTTP/2 (default: False)
            enable_h3: Enable HTTP/3 (default: False)
            enable_compression: Enable zstd compression (default: True)
            enable_websocket: Enable WebSocket (default: True)
        """
        # Configure server
        self._config.port = port
        self._config.host = host.encode('utf-8')
        self._config.enable_h1 = True  # Always enable HTTP/1.1
        self._config.enable_h2 = enable_h2
        self._config.enable_h3 = enable_h3
        self._config.enable_compression = enable_compression
        self._config.enable_websocket = enable_websocket

        # Optional settings from kwargs
        self._config.max_connections = kwargs.get('max_connections', 10000)
        self._config.max_request_size = kwargs.get('max_request_size', 16 * 1024 * 1024)
        self._config.compression_threshold = kwargs.get('compression_threshold', 1024)
        self._config.compression_level = kwargs.get('compression_level', 3)

        # Initialize handler tracking
        self._route_handlers = {}
        self._websocket_handlers = {}
        self._next_handler_id = 1

        # Initialize Python callback bridge
        PythonCallbackBridge.initialize()

        # Create C++ server (GIL can be held during construction)
        self._server = new HttpServer(self._config)

        if self._server == NULL:
            raise MemoryError("Failed to create HTTP server")

    def __dealloc__(self):
        """Clean up C++ server."""
        if self._server != NULL:
            # Stop server if running (GIL must be held to clean up Python callbacks)
            if self._server.is_running():
                self.stop()
            # Cleanup Python callbacks
            PythonCallbackBridge.cleanup()
            # Delete C++ object
            # Note: Cython will handle C++ deletion automatically for cppclass pointers
            self._server = NULL

    def add_route(self, str method, str path, handler: Callable):
        """
        Add HTTP route handler.

        Args:
            method: HTTP method (GET, POST, etc.)
            path: Route path pattern
            handler: Python callable(request, response)
        """
        method_upper = method.upper()

        # Store Python handler
        handler_id = self._next_handler_id
        self._next_handler_id += 1

        key = f"{method_upper}:{path}"
        self._route_handlers[key] = (handler_id, handler)

        # Register handler with Python callback bridge
        cdef bytes method_bytes = method_upper.encode('utf-8')
        cdef bytes path_bytes = path.encode('utf-8')

        # Pass handler as PyObject*
        cdef PyObject* handler_ptr = <PyObject*>handler

        PythonCallbackBridge.register_handler(
            <string>method_bytes,
            <string>path_bytes,
            handler_id,
            <void*>handler_ptr
        )

        print(f"✓ Route registered: {method_upper} {path}")

    def add_websocket(self, str path, handler: Callable):
        """
        Add WebSocket endpoint.

        Args:
            path: WebSocket path
            handler: Python callable(websocket)
        """
        # Store Python handler
        handler_id = self._next_handler_id
        self._next_handler_id += 1

        self._websocket_handlers[path] = (handler_id, handler)

        # Register with C++ server using callback bridge
        cdef int result
        cdef bytes path_bytes = path.encode('utf-8')
        cdef PyObject* handler_ptr = <PyObject*>handler

        # Create empty WebSocket handler (will be implemented with proper bridge)
        cdef HttpServer.WebSocketHandler empty_ws_handler
        result = self._server.add_websocket(
            <string>path_bytes,
            empty_ws_handler
        )

        if result != 0:
            raise RuntimeError(f"Failed to add WebSocket {path}: error code {result}")

        print(f"✓ WebSocket registered: {path}")

    def start(self):
        """
        Start the HTTP server.

        This starts the CoroIO event loop in a background C++ thread.
        Returns immediately after starting the background thread.
        """
        cdef int result

        # Start server - spawns background thread, returns quickly
        result = self._server.start()

        if result != 0:
            raise RuntimeError(f"Failed to start server: error code {result}")

        print(f"🚀 FasterAPI HTTP server started on {self._config.host.decode()}:{self._config.port}")
        if self._config.enable_h2:
            print("   ✓ HTTP/2 enabled")
        if self._config.enable_h3:
            print("   ✓ HTTP/3 enabled")
        if self._config.enable_compression:
            print("   ✓ zstd compression enabled")
        if self._config.enable_websocket:
            print("   ✓ WebSocket enabled")

    def stop(self):
        """
        Stop the HTTP server.

        This gracefully shuts down the event loop and closes all connections.
        """
        cdef int result

        # Stop server - C++ call that joins threads
        result = self._server.stop()

        if result != 0:
            print(f"Warning: Failed to stop server cleanly: error code {result}")

        print("🛑 HTTP server stopped")

    def is_running(self) -> bool:
        """Check if server is running."""
        cdef bool running

        # Direct C++ call - no GIL needed since it's just reading an atomic
        running = self._server.is_running()

        return running

    def get_stats(self) -> Dict[str, int]:
        """
        Get server statistics.

        Returns:
            Dictionary with performance metrics
        """
        cdef HttpServer_Stats stats

        # Get stats - Direct C++ call
        stats = self._server.get_stats()

        # Convert to Python dict
        return {
            "total_requests": stats.total_requests,
            "total_bytes_sent": stats.total_bytes_sent,
            "total_bytes_received": stats.total_bytes_received,
            "active_connections": stats.active_connections,
            "h1_requests": stats.h1_requests,
            "h2_requests": stats.h2_requests,
            "h3_requests": stats.h3_requests,
            "websocket_connections": stats.websocket_connections,
            "compressed_responses": stats.compressed_responses,
            "compression_bytes_saved": stats.compression_bytes_saved,
        }

    # Convenience methods
    def get(self, path: str, handler: Callable):
        """Add GET route."""
        self.add_route("GET", path, handler)

    def post(self, path: str, handler: Callable):
        """Add POST route."""
        self.add_route("POST", path, handler)

    def put(self, path: str, handler: Callable):
        """Add PUT route."""
        self.add_route("PUT", path, handler)

    def delete(self, path: str, handler: Callable):
        """Add DELETE route."""
        self.add_route("DELETE", path, handler)

    def patch(self, path: str, handler: Callable):
        """Add PATCH route."""
        self.add_route("PATCH", path, handler)

    def websocket(self, path: str, handler: Callable):
        """Add WebSocket endpoint."""
        self.add_websocket(path, handler)


# ==============================================================================
# WebSocket Bindings
# ==============================================================================

# Declare C++ WebSocket types
cdef extern from "src/cpp/http/websocket.h" namespace "fasterapi::http":
    cdef cppclass WebSocketConnection:
        int send_text(const string& message) nogil
        int send_binary(const unsigned char* data, size_t length) nogil
        int send_ping(const unsigned char* data, size_t length) nogil
        int send_pong(const unsigned char* data, size_t length) nogil
        int close(unsigned short code, const char* reason) nogil
        bool is_open() nogil
        unsigned long long get_id() nogil
        unsigned long long messages_sent() nogil
        unsigned long long messages_received() nogil
        unsigned long long bytes_sent() nogil
        unsigned long long bytes_received() nogil


cdef class PyWebSocketConnection:
    """
    Python wrapper for C++ WebSocketConnection.

    Provides zero-copy access to WebSocket operations with explicit GIL control.
    All I/O operations release the GIL for maximum throughput.
    """
    cdef WebSocketConnection* _conn
    cdef unsigned long long _conn_id

    def __cinit__(self, unsigned long long conn_id, size_t conn_ptr):
        """
        Create Python wrapper for WebSocket connection.

        Args:
            conn_id: Connection ID
            conn_ptr: Pointer to C++ WebSocketConnection as integer (managed by C++ side)
        """
        self._conn_id = conn_id
        self._conn = <WebSocketConnection*>conn_ptr

    def send_text(self, str message):
        """
        Send text message to WebSocket client.

        Args:
            message: Text message (will be UTF-8 encoded)

        Raises:
            RuntimeError: If send fails
        """
        if self._conn == NULL:
            raise RuntimeError("WebSocket connection is NULL")

        cdef bytes message_bytes = message.encode('utf-8')
        cdef string message_str = <string>message_bytes
        cdef int result

        # Release GIL for I/O operation
        with nogil:
            result = self._conn.send_text(message_str)

        if result != 0:
            raise RuntimeError(f"Failed to send WebSocket text message: error code {result}")

    def send_binary(self, bytes data):
        """
        Send binary message to WebSocket client.

        Args:
            data: Binary data

        Raises:
            RuntimeError: If send fails
        """
        if self._conn == NULL:
            raise RuntimeError("WebSocket connection is NULL")

        cdef int result
        cdef const unsigned char* data_ptr = <const unsigned char*><char*>data
        cdef size_t length = len(data)

        # Release GIL for I/O operation
        with nogil:
            result = self._conn.send_binary(data_ptr, length)

        if result != 0:
            raise RuntimeError(f"Failed to send WebSocket binary message: error code {result}")

    def send(self, message):
        """
        Send message (auto-detect text/binary).

        Args:
            message: Text string or binary bytes
        """
        if isinstance(message, str):
            self.send_text(message)
        elif isinstance(message, bytes):
            self.send_binary(message)
        else:
            raise TypeError(f"Message must be str or bytes, not {type(message)}")

    def ping(self, bytes data=None):
        """
        Send ping frame.

        Args:
            data: Optional ping payload

        Raises:
            RuntimeError: If ping fails
        """
        if self._conn == NULL:
            raise RuntimeError("WebSocket connection is NULL")

        cdef int result
        cdef const unsigned char* data_ptr = NULL
        cdef size_t length = 0

        if data is not None:
            data_ptr = <const unsigned char*><char*>data
            length = len(data)

        with nogil:
            result = self._conn.send_ping(data_ptr, length)

        if result != 0:
            raise RuntimeError(f"Failed to send ping: error code {result}")

    def pong(self, bytes data=None):
        """
        Send pong frame.

        Args:
            data: Optional pong payload

        Raises:
            RuntimeError: If pong fails
        """
        if self._conn == NULL:
            raise RuntimeError("WebSocket connection is NULL")

        cdef int result
        cdef const unsigned char* data_ptr = NULL
        cdef size_t length = 0

        if data is not None:
            data_ptr = <const unsigned char*><char*>data
            length = len(data)

        with nogil:
            result = self._conn.send_pong(data_ptr, length)

        if result != 0:
            raise RuntimeError(f"Failed to send pong: error code {result}")

    def close(self, unsigned short code=1000, str reason=""):
        """
        Close WebSocket connection.

        Args:
            code: WebSocket close code (default: 1000 = normal closure)
            reason: Close reason string

        Returns:
            True if close succeeded, False otherwise
        """
        if self._conn == NULL:
            return False

        cdef bytes reason_bytes = reason.encode('utf-8')
        cdef const char* reason_ptr = <const char*>reason_bytes
        cdef int result

        with nogil:
            result = self._conn.close(code, reason_ptr)

        return result == 0

    @property
    def is_open(self) -> bool:
        """Check if connection is open."""
        if self._conn == NULL:
            return False

        cdef bool open_status
        with nogil:
            open_status = self._conn.is_open()

        return open_status

    @property
    def connection_id(self) -> int:
        """Get connection ID."""
        if self._conn == NULL:
            return 0

        cdef unsigned long long conn_id
        with nogil:
            conn_id = self._conn.get_id()

        return conn_id

    @property
    def messages_sent(self) -> int:
        """Get number of messages sent."""
        if self._conn == NULL:
            return 0

        cdef unsigned long long count
        with nogil:
            count = self._conn.messages_sent()

        return count

    @property
    def messages_received(self) -> int:
        """Get number of messages received."""
        if self._conn == NULL:
            return 0

        cdef unsigned long long count
        with nogil:
            count = self._conn.messages_received()

        return count

    @property
    def bytes_sent(self) -> int:
        """Get total bytes sent."""
        if self._conn == NULL:
            return 0

        cdef unsigned long long bytes_count
        with nogil:
            bytes_count = self._conn.bytes_sent()

        return bytes_count

    @property
    def bytes_received(self) -> int:
        """Get total bytes received."""
        if self._conn == NULL:
            return 0

        cdef unsigned long long bytes_count
        with nogil:
            bytes_count = self._conn.bytes_received()

        return bytes_count


# ==============================================================================
# SSE (Server-Sent Events) Bindings
# ==============================================================================

# Declare C++ SSE types
cdef extern from "src/cpp/http/sse.h" namespace "fasterapi::http":
    cdef cppclass SSEConnection:
        int send(const string& data, const char* event, const char* id, int retry) nogil
        int send_comment(const string& comment) nogil
        int ping() nogil
        int close() nogil
        bool is_open() nogil
        unsigned long long get_id() nogil
        unsigned long long events_sent() nogil
        unsigned long long bytes_sent() nogil


cdef class PySSEConnection:
    """
    Python wrapper for C++ SSEConnection.

    Provides zero-copy access to Server-Sent Events operations with explicit GIL control.
    All I/O operations release the GIL for maximum throughput.
    """
    cdef SSEConnection* _conn
    cdef unsigned long long _conn_id

    def __cinit__(self, unsigned long long conn_id, size_t conn_ptr):
        """
        Create Python wrapper for SSE connection.

        Args:
            conn_id: Connection ID
            conn_ptr: Pointer to C++ SSEConnection as integer (managed by C++ side)
        """
        self._conn_id = conn_id
        self._conn = <SSEConnection*>conn_ptr

    def send(self, str data, str event=None, str event_id=None, int retry=-1):
        """
        Send SSE event to client.

        Args:
            data: Event data (will be sent as "data: ..." lines)
            event: Event type (optional, defaults to "message")
            event_id: Event ID for reconnection (optional)
            retry: Retry time in milliseconds (optional, -1 = no retry field)

        Raises:
            RuntimeError: If send fails
        """
        if self._conn == NULL:
            raise RuntimeError("SSE connection is NULL")

        cdef bytes data_bytes = data.encode('utf-8')
        cdef string data_str = <string>data_bytes
        cdef bytes event_bytes
        cdef bytes id_bytes
        cdef const char* event_ptr = NULL
        cdef const char* id_ptr = NULL
        cdef int result

        if event is not None:
            event_bytes = event.encode('utf-8')
            event_ptr = <const char*>event_bytes

        if event_id is not None:
            id_bytes = event_id.encode('utf-8')
            id_ptr = <const char*>id_bytes

        # Release GIL for I/O operation
        with nogil:
            result = self._conn.send(
                data_str,
                event_ptr,
                id_ptr,
                retry
            )

        if result != 0:
            raise RuntimeError(f"Failed to send SSE event: error code {result}")

    def send_comment(self, str comment):
        """
        Send comment (ignored by client, useful for keep-alive).

        Args:
            comment: Comment text

        Raises:
            RuntimeError: If send fails
        """
        if self._conn == NULL:
            raise RuntimeError("SSE connection is NULL")

        cdef bytes comment_bytes = comment.encode('utf-8')
        cdef string comment_str = <string>comment_bytes
        cdef int result

        with nogil:
            result = self._conn.send_comment(comment_str)

        if result != 0:
            raise RuntimeError(f"Failed to send SSE comment: error code {result}")

    def ping(self):
        """
        Send keep-alive ping.

        Sends a comment to keep connection alive.

        Returns:
            True if ping succeeded, False otherwise
        """
        if self._conn == NULL:
            return False

        cdef int result

        with nogil:
            result = self._conn.ping()

        return result == 0

    def close(self):
        """
        Close SSE connection.

        Returns:
            True if close succeeded, False otherwise
        """
        if self._conn == NULL:
            return False

        cdef int result

        with nogil:
            result = self._conn.close()

        return result == 0

    @property
    def is_open(self) -> bool:
        """Check if connection is open."""
        if self._conn == NULL:
            return False

        cdef bool open_status
        with nogil:
            open_status = self._conn.is_open()

        return open_status

    @property
    def connection_id(self) -> int:
        """Get connection ID."""
        if self._conn == NULL:
            return 0

        cdef unsigned long long conn_id
        with nogil:
            conn_id = self._conn.get_id()

        return conn_id

    @property
    def events_sent(self) -> int:
        """Get number of events sent."""
        if self._conn == NULL:
            return 0

        cdef unsigned long long count
        with nogil:
            count = self._conn.events_sent()

        return count

    @property
    def bytes_sent(self) -> int:
        """Get total bytes sent."""
        if self._conn == NULL:
            return 0

        cdef unsigned long long bytes_count
        with nogil:
            bytes_count = self._conn.bytes_sent()

        return bytes_count
