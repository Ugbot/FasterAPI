"""
HTTP Server Python Wrapper

High-performance HTTP server with multi-protocol support.
"""

import ctypes
import os
import json
import inspect
import re
from typing import Optional, Callable, Dict, Any, Union, get_type_hints
from .bindings import get_lib, _error_from_code
import sys


def _is_body_parameter(param_type) -> bool:
    """Determine if a parameter type should be treated as request body (Pydantic model or dict)."""
    if param_type is inspect.Parameter.empty:
        return False

    # Check for Pydantic BaseModel
    try:
        from pydantic import BaseModel
        if isinstance(param_type, type) and issubclass(param_type, BaseModel):
            return True
    except ImportError:
        pass

    # Check for dict type
    origin = getattr(param_type, '__origin__', None)
    if origin is dict or param_type is dict:
        return True

    return False


def _should_be_body_param(param_name: str, param_type, method: str, path_params: set) -> bool:
    """For POST/PUT/PATCH, non-path params come from body (both individual params and Pydantic)."""
    if param_name in path_params:
        return False
    if method in ('POST', 'PUT', 'PATCH'):
        return True  # All non-path params for mutation verbs come from body
    return False


def _extract_handler_metadata(handler: Callable, path: str, method: str = "GET") -> Dict[str, Any]:
    """
    Extract parameter metadata from a handler function for automatic parameter extraction.

    Args:
        handler: Handler function to introspect
        path: Route path pattern (e.g., "/user/{user_id}")
        method: HTTP method (GET, POST, etc.)

    Returns:
        Dictionary with parameter metadata in the format:
        {"parameters": [{"name": "...", "type": "...", "location": "...", "required": ...}, ...]}
    """
    # Extract path parameters from route pattern
    path_params = set(re.findall(r'\{(\w+)\}', path))

    # Get function signature
    try:
        sig = inspect.signature(handler)
    except (ValueError, TypeError):
        # Can't introspect, return empty metadata
        return {"parameters": []}

    # Try to get type hints
    try:
        type_hints = get_type_hints(handler)
    except Exception:
        type_hints = {}

    parameters = []

    for param_name, param in sig.parameters.items():
        # Determine location based on method and param type
        param_type = type_hints.get(param_name, param.annotation)

        if param_name in path_params:
            location = "path"
            required = True  # Path parameters are always required
        elif _should_be_body_param(param_name, param_type, method, path_params):
            location = "body"
            # Body parameters with defaults are optional
            required = (param.default == inspect.Parameter.empty)
        else:
            location = "query"
            # Query parameters with defaults are optional
            required = (param.default == inspect.Parameter.empty)

        # Map Python type to schema type
        param_type = type_hints.get(param_name, param.annotation)
        schema_type = "string"  # default

        if param_type == int or param_type == 'int' or param_type.__name__ == 'int' if hasattr(param_type, '__name__') else False:
            schema_type = "integer"
        elif param_type == float or param_type == 'float' or param_type.__name__ == 'float' if hasattr(param_type, '__name__') else False:
            schema_type = "number"
        elif param_type == bool or param_type == 'bool' or param_type.__name__ == 'bool' if hasattr(param_type, '__name__') else False:
            schema_type = "boolean"

        param_info = {
            "name": param_name,
            "type": schema_type,
            "location": location,
            "required": required
        }

        # Add default value if present
        if param.default != inspect.Parameter.empty and param.default is not None:
            param_info["default"] = str(param.default)

        parameters.append(param_info)

    return {"parameters": parameters}


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
        enable_webtransport: bool = False,
        http3_port: int = 443,
        enable_compression: bool = True,
        num_workers: int = 0,
        python_executable: str = None,
        **kwargs
    ):
        """
        Create a new HTTP server.

        Args:
            port: Server port (TCP for HTTP/1.1 and HTTP/2)
            host: Server host
            enable_h2: Enable HTTP/2 over TLS with ALPN
            enable_h3: Enable HTTP/3 over QUIC (UDP)
            enable_webtransport: Enable WebTransport over HTTP/3
            http3_port: UDP port for HTTP/3 (default 443)
            enable_compression: Enable zstd compression
            num_workers: Number of worker processes (0 = auto-detect CPU cores, default: 0)
            python_executable: Path to Python executable for workers (default: current Python)
            **kwargs: Additional configuration options
        """
        self.port = port
        self.host = host
        self.enable_h2 = enable_h2
        self.enable_h3 = enable_h3
        self.enable_webtransport = enable_webtransport
        self.http3_port = http3_port
        self.enable_compression = enable_compression
        self.num_workers = num_workers
        # Default to current Python executable
        self.python_executable = python_executable if python_executable else sys.executable

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

        # Initialize ProcessPoolExecutor (multiprocessing with shared memory IPC)
        # Get project directory (directory containing this package)
        project_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))

        # Initialize process pool executor
        result = self._lib.http_init_process_pool_executor(
            ctypes.c_uint32(self.num_workers),
            self.python_executable.encode('utf-8'),
            project_dir.encode('utf-8')
        )

        if result != 0:
            raise RuntimeError(f"Failed to initialize ProcessPoolExecutor: {_error_from_code(result)}")
    
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
            ctypes.c_bool(self.enable_webtransport),
            ctypes.c_uint16(self.http3_port),
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

        # Store handler
        handler_id = self._next_handler_id
        self._next_handler_id += 1
        key = f"{method}:{path}"
        self._routes[method][path] = (handler_id, handler)

        # Register route with C++ server
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

        # Register Python handler callback with PythonCallbackBridge
        # We need to pass the actual PyObject* pointer to C++
        # Keep reference to prevent GC
        if not hasattr(self, '_handler_refs'):
            self._handler_refs = []
        self._handler_refs.append(handler)

        # Get PyObject* pointer using ctypes.pythonapi
        # id(handler) gives us the memory address of the Python object
        handler_ptr = ctypes.c_void_p(id(handler))

        # Increment refcount to keep object alive (C++ will also increment)
        ctypes.pythonapi.Py_IncRef(ctypes.py_object(handler))

        # Register with C++ callback bridge
        self._lib.http_register_python_handler(
            method.encode('utf-8'),
            path.encode('utf-8'),
            handler_id,
            handler_ptr
        )

        # Extract and register parameter metadata for automatic parameter extraction
        try:
            metadata = _extract_handler_metadata(handler, path, method)
            metadata_json = json.dumps(metadata)

            result = self._lib.http_register_route_metadata(
                method.encode('utf-8'),
                path.encode('utf-8'),
                metadata_json.encode('utf-8')
            )

            if result != 0:
                print(f"Warning: Failed to register metadata for {method} {path}: {_error_from_code(result)}")
        except Exception as e:
            print(f"Warning: Failed to extract metadata for {method} {path}: {e}")
    
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

        # Register WebSocket handler metadata for ZMQ workers
        # Workers need module.function to import and call the handler
        module_name = getattr(handler, '__module__', '__main__')
        func_name = getattr(handler, '__qualname__', getattr(handler, '__name__', 'unknown'))

        # Ensure http_register_websocket_handler_metadata is available
        if not hasattr(self._lib, 'http_register_websocket_handler_metadata'):
            self._lib.http_register_websocket_handler_metadata.argtypes = [
                ctypes.c_char_p,  # path
                ctypes.c_char_p,  # module_name
                ctypes.c_char_p   # function_name
            ]
            self._lib.http_register_websocket_handler_metadata.restype = None

        self._lib.http_register_websocket_handler_metadata(
            path.encode('utf-8'),
            module_name.encode('utf-8'),
            func_name.encode('utf-8')
        )
    
    def _sync_routes_from_registry(self) -> None:
        """Sync routes from RouteRegistry to HttpServer."""
        try:
            from fasterapi._fastapi_native import get_all_routes
            import os

            # Get the RouteRegistry pointer via C API from Cython module
            # The get_route_registry_ptr() C function is exported from _fastapi_native.so
            cython_module_path = os.path.join(os.path.dirname(__file__), '..', '_fastapi_native.cpython-313-darwin.so')
            if not os.path.exists(cython_module_path):
                cython_module_path = os.path.join(os.path.dirname(__file__), '..', '_fastapi_native.so')

            if os.path.exists(cython_module_path):
                cython_lib = ctypes.CDLL(cython_module_path)
                # The C++ mangled name for get_route_registry_ptr() - returns void*
                try:
                    get_registry_fn = cython_lib._Z22get_route_registry_ptrv
                    get_registry_fn.restype = ctypes.c_void_p
                    get_registry_fn.argtypes = []
                    registry_ptr = get_registry_fn()
                except AttributeError:
                    print("Warning: Could not find get_route_registry_ptr in Cython module")
                    return
            else:
                print("Warning: Cython module not found")
                return

            if registry_ptr is None or registry_ptr == 0:
                print("Warning: RouteRegistry not initialized")
                return

            routes = get_all_routes()
            for route in routes:
                method = route['method']
                path = route['path_pattern']

                # Skip if already registered
                if method in self._routes and path in self._routes[method]:
                    continue

                # Get handler from RouteRegistry using new C API
                handler_ptr = self._lib.http_get_route_handler(
                    ctypes.c_void_p(registry_ptr),
                    method.encode('utf-8'),
                    path.encode('utf-8')
                )

                if handler_ptr is None or handler_ptr == 0:
                    print(f"Warning: No handler found for {method} {path}")
                    continue

                # Assign handler ID
                handler_id = self._next_handler_id
                self._next_handler_id += 1

                # Register handler with PythonCallbackBridge
                self._lib.http_register_python_handler(
                    method.encode('utf-8'),
                    path.encode('utf-8'),
                    ctypes.c_int(handler_id),
                    ctypes.c_void_p(handler_ptr)
                )

                # Register route with HttpServer
                error = ctypes.c_int()
                result = self._lib.http_add_route(
                    self._handle,
                    method.encode('utf-8'),
                    path.encode('utf-8'),
                    ctypes.c_uint32(handler_id),
                    ctypes.byref(error)
                )

                if result != 0:
                    print(f"Warning: Failed to add route {method} {path}: {_error_from_code(result)}")
                    continue

                # Track that we registered it
                if method not in self._routes:
                    self._routes[method] = {}
                self._routes[method][path] = (handler_id, None)

        except ImportError as e:
            # _fastapi_native not available, skip
            print(f"Debug: ImportError in _sync_routes_from_registry: {e}")
            pass

    def start(self) -> None:
        """Start the HTTP server."""
        if self._handle is None:
            self._create_server()

        # Sync any routes registered via FastAPI decorators
        self._sync_routes_from_registry()

        error = ctypes.c_int()
        result = self._lib.http_server_start(self._handle, ctypes.byref(error))

        if result != 0:
            raise RuntimeError(f"Failed to start server: {_error_from_code(result)}")

        print(f"FasterAPI HTTP server started on {self.host}:{self.port}")
        if self.enable_h2:
            print("   HTTP/2 enabled")
        if self.enable_h3:
            print(f"   HTTP/3 enabled (UDP port {self.http3_port})")
        if self.enable_webtransport:
            print("   WebTransport enabled")
        if self.enable_compression:
            print("   zstd compression enabled")
    
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
