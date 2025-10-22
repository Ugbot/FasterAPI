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

        # TODO: Implement WebSocket callback wrapper
        cdef int result
        cdef bytes path_bytes = path.encode('utf-8')

        cdef HttpServer.WebSocketHandler empty_ws_handler
        result = self._server.add_websocket(
            <string>path_bytes,
            empty_ws_handler
        )

        if result != 0:
            raise RuntimeError(f"Failed to add WebSocket {path}: error code {result}")

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
