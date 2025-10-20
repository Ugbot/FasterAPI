"""
HTTP Server Python Wrapper

High-performance HTTP server with multi-protocol support.
"""

import ctypes
from typing import Optional, Callable, Dict, Any, Union
from .bindings import get_lib, _error_from_code


class Server:
    """
    High-performance HTTP server with multi-protocol support.
    
    Features:
    - HTTP/1.1 (uWebSockets)
    - HTTP/2 (nghttp2 + ALPN)
    - HTTP/3 (MsQuic, optional)
    - WebSocket support
    - zstd compression
    - Per-core event loops
    """
    
    def __init__(
        self,
        port: int = 8000,
        host: str = "0.0.0.0",
        enable_h2: bool = False,
        enable_h3: bool = False,
        enable_compression: bool = True,
        **kwargs
    ):
        """
        Create a new HTTP server.
        
        Args:
            port: Server port
            host: Server host
            enable_h2: Enable HTTP/2 support
            enable_h3: Enable HTTP/3 support
            enable_compression: Enable zstd compression
            **kwargs: Additional configuration options
        """
        self.port = port
        self.host = host
        self.enable_h2 = enable_h2
        self.enable_h3 = enable_h3
        self.enable_compression = enable_compression
        
        # Server handle from C++ library
        self._handle: Optional[ctypes.c_void_p] = None
        
        # Route handlers
        self._routes: Dict[str, Dict[str, Callable]] = {}
        self._websocket_handlers: Dict[str, Callable] = {}
        
        # Handler ID counter
        self._next_handler_id = 1
        
        # Initialize C++ library
        self._lib = get_lib()
        if self._lib is None:
            raise RuntimeError("Failed to load HTTP server library")
        
        # Initialize library
        error = ctypes.c_int()
        result = self._lib.http_lib_init()
        if result != 0:
            raise RuntimeError(f"Failed to initialize HTTP library: {_error_from_code(result)}")
    
    def __del__(self):
        """Cleanup server resources."""
        if self._handle is not None:
            self.stop()
    
    def _create_server(self) -> None:
        """Create the C++ server instance."""
        if self._handle is not None:
            raise RuntimeError("Server already created")
        
        error = ctypes.c_int()
        self._handle = self._lib.http_server_create(
            ctypes.c_uint16(self.port),
            self.host.encode('utf-8'),
            ctypes.c_bool(self.enable_h2),
            ctypes.c_bool(self.enable_h3),
            ctypes.c_bool(self.enable_compression),
            ctypes.byref(error)
        )
        
        if error.value != 0:
            raise RuntimeError(f"Failed to create server: {_error_from_code(error.value)}")
    
    def add_route(
        self,
        method: str,
        path: str,
        handler: Callable
    ) -> None:
        """
        Add a route handler.
        
        Args:
            method: HTTP method (GET, POST, etc.)
            path: Route path pattern
            handler: Handler function
        """
        if self._handle is None:
            self._create_server()
        
        method = method.upper()
        if method not in self._routes:
            self._routes[method] = {}
        
        self._routes[method][path] = handler
        
        # Register with C++ library
        handler_id = self._next_handler_id
        self._next_handler_id += 1
        
        error = ctypes.c_int()
        result = self._lib.http_add_route(
            self._handle,
            method.encode('utf-8'),
            path.encode('utf-8'),
            ctypes.c_uint32(handler_id),
            ctypes.byref(error)
        )
        
        if result != 0:
            raise RuntimeError(f"Failed to add route: {_error_from_code(result)}")
    
    def add_websocket(self, path: str, handler: Callable) -> None:
        """
        Add a WebSocket endpoint.
        
        Args:
            path: WebSocket path
            handler: WebSocket handler function
        """
        if self._handle is None:
            self._create_server()
        
        self._websocket_handlers[path] = handler
        
        # Register with C++ library
        handler_id = self._next_handler_id
        self._next_handler_id += 1
        
        error = ctypes.c_int()
        result = self._lib.http_add_websocket(
            self._handle,
            path.encode('utf-8'),
            ctypes.c_uint32(handler_id),
            ctypes.byref(error)
        )
        
        if result != 0:
            raise RuntimeError(f"Failed to add WebSocket: {_error_from_code(result)}")
    
    def start(self) -> None:
        """Start the HTTP server."""
        if self._handle is None:
            self._create_server()
        
        error = ctypes.c_int()
        result = self._lib.http_server_start(self._handle, ctypes.byref(error))
        
        if result != 0:
            raise RuntimeError(f"Failed to start server: {_error_from_code(result)}")
        
        print(f"🚀 FasterAPI HTTP server started on {self.host}:{self.port}")
        if self.enable_h2:
            print("   ✓ HTTP/2 enabled")
        if self.enable_h3:
            print("   ✓ HTTP/3 enabled")
        if self.enable_compression:
            print("   ✓ zstd compression enabled")
    
    def stop(self) -> None:
        """Stop the HTTP server."""
        if self._handle is None:
            return
        
        error = ctypes.c_int()
        result = self._lib.http_server_stop(self._handle, ctypes.byref(error))
        
        if result != 0:
            print(f"Warning: Failed to stop server: {_error_from_code(result)}")
        
        # Destroy server
        result = self._lib.http_server_destroy(self._handle)
        if result != 0:
            print(f"Warning: Failed to destroy server: {_error_from_code(result)}")
        
        self._handle = None
        print("🛑 HTTP server stopped")
    
    def is_running(self) -> bool:
        """Check if server is running."""
        if self._handle is None:
            return False
        
        return self._lib.http_server_is_running(self._handle)
    
    def get_stats(self) -> Dict[str, Any]:
        """
        Get server statistics.
        
        Returns:
            Dictionary with server statistics
        """
        if self._handle is None:
            return {}
        
        # This would be implemented to get real stats from C++
        return {
            "total_requests": 0,
            "total_bytes_sent": 0,
            "total_bytes_received": 0,
            "active_connections": 0,
            "h1_requests": 0,
            "h2_requests": 0,
            "h3_requests": 0,
            "websocket_connections": 0,
            "compressed_responses": 0,
            "compression_bytes_saved": 0,
        }
    
    # Convenience methods for common HTTP methods
    def get(self, path: str, handler: Callable) -> None:
        """Add a GET route."""
        self.add_route("GET", path, handler)
    
    def post(self, path: str, handler: Callable) -> None:
        """Add a POST route."""
        self.add_route("POST", path, handler)
    
    def put(self, path: str, handler: Callable) -> None:
        """Add a PUT route."""
        self.add_route("PUT", path, handler)
    
    def delete(self, path: str, handler: Callable) -> None:
        """Add a DELETE route."""
        self.add_route("DELETE", path, handler)
    
    def patch(self, path: str, handler: Callable) -> None:
        """Add a PATCH route."""
        self.add_route("PATCH", path, handler)
    
    def websocket(self, path: str, handler: Callable) -> None:
        """Add a WebSocket endpoint."""
        self.add_websocket(path, handler)
