"""
FFI Bindings for FasterAPI C++ library.

Provides ctypes interface to native C++ implementations.
"""

import ctypes
import os
import sys
from ctypes import (
    c_void_p, c_char_p, c_int, c_uint64, c_size_t,
    c_uint8, c_uint16, c_uint32, c_bool, POINTER, Structure
)

# Find the native library
def _find_lib():
    """Locate the FasterAPI native library."""
    
    # Check common locations
    lib_name = "libfasterapi_http"
    
    if sys.platform == "darwin":
        ext = ".dylib"
    elif sys.platform == "win32":
        ext = ".dll"
    else:
        ext = ".so"
    
    lib_filename = lib_name + ext
    
    # Search paths
    search_paths = [
        os.path.join(os.path.dirname(__file__), "..", "..", "build", "lib"),
        os.path.join(os.path.dirname(__file__), "_native"),
        "/usr/local/lib",
        "/usr/lib",
    ]
    
    for path in search_paths:
        lib_path = os.path.join(path, lib_filename)
        if os.path.exists(lib_path):
            return lib_path
    
    # Try to load without path (system will search)
    return lib_filename


class _NativeLib:
    """Singleton for native library."""
    
    _instance = None
    _lib = None
    
    @classmethod
    def get(cls):
        if cls._instance is None:
            cls._instance = cls()
        return cls._instance
    
    def __init__(self):
        if _NativeLib._lib is not None:
            return
        
        lib_path = _find_lib()
        
        try:
            self._lib = ctypes.CDLL(lib_path)
            _NativeLib._lib = self._lib
            self._setup_functions()
        except OSError as e:
            raise RuntimeError(f"Failed to load FasterAPI native library from {lib_path}: {e}")
    
    def _setup_functions(self):
        """Define C function signatures."""

        # ====================================================================
        # HTTP Server Functions (C API)
        # ====================================================================

        # int http_lib_init()
        self._lib.http_lib_init.argtypes = []
        self._lib.http_lib_init.restype = c_int

        # HttpServerHandle http_server_create(port, host, enable_h2, enable_h3, enable_webtransport, http3_port, enable_compression, error_out)
        self._lib.http_server_create.argtypes = [c_uint16, c_char_p, c_bool, c_bool, c_bool, c_uint16, c_bool, POINTER(c_int)]
        self._lib.http_server_create.restype = c_void_p

        # int http_add_route(handle, method, path, handler_id, error_out)
        self._lib.http_add_route.argtypes = [c_void_p, c_char_p, c_char_p, ctypes.c_uint32, POINTER(c_int)]
        self._lib.http_add_route.restype = c_int

        # int http_add_websocket(handle, path, handler_id, error_out)
        self._lib.http_add_websocket.argtypes = [c_void_p, c_char_p, ctypes.c_uint32, POINTER(c_int)]
        self._lib.http_add_websocket.restype = c_int

        # int http_server_start(handle, error_out)
        self._lib.http_server_start.argtypes = [c_void_p, POINTER(c_int)]
        self._lib.http_server_start.restype = c_int

        # int http_server_stop(handle, error_out)
        self._lib.http_server_stop.argtypes = [c_void_p, POINTER(c_int)]
        self._lib.http_server_stop.restype = c_int

        # bool http_server_is_running(handle)
        self._lib.http_server_is_running.argtypes = [c_void_p]
        self._lib.http_server_is_running.restype = c_bool

        # int http_server_destroy(handle)
        self._lib.http_server_destroy.argtypes = [c_void_p]
        self._lib.http_server_destroy.restype = c_int

        # void http_register_python_handler(method, path, handler_id, py_callable)
        self._lib.http_register_python_handler.argtypes = [c_char_p, c_char_p, c_int, c_void_p]
        self._lib.http_register_python_handler.restype = None

        # void* http_get_route_handler(registry_ptr, method, path)
        self._lib.http_get_route_handler.argtypes = [c_void_p, c_char_p, c_char_p]
        self._lib.http_get_route_handler.restype = c_void_p

        # int http_connect_route_registry(registry_ptr)
        self._lib.http_connect_route_registry.argtypes = [c_void_p]
        self._lib.http_connect_route_registry.restype = c_int

        # int http_init_process_pool_executor(num_workers, python_executable, project_dir)
        self._lib.http_init_process_pool_executor.argtypes = [c_uint32, c_char_p, c_char_p]
        self._lib.http_init_process_pool_executor.restype = c_int

        # int http_register_route_metadata(method, path, param_metadata_json)
        self._lib.http_register_route_metadata.argtypes = [c_char_p, c_char_p, c_char_p]
        self._lib.http_register_route_metadata.restype = c_int

        # ====================================================================
        # WebSocket Functions
        # ====================================================================
        
        # WebSocketConnection* ws_create(uint64_t connection_id)
        self._lib.ws_create.argtypes = [c_uint64]
        self._lib.ws_create.restype = c_void_p
        
        # void ws_destroy(WebSocketConnection* ws)
        self._lib.ws_destroy.argtypes = [c_void_p]
        self._lib.ws_destroy.restype = None
        
        # int ws_send_text(WebSocketConnection* ws, const char* message)
        self._lib.ws_send_text.argtypes = [c_void_p, c_char_p]
        self._lib.ws_send_text.restype = c_int
        
        # int ws_send_binary(WebSocketConnection* ws, const uint8_t* data, size_t length)
        self._lib.ws_send_binary.argtypes = [c_void_p, POINTER(c_uint8), c_size_t]
        self._lib.ws_send_binary.restype = c_int
        
        # int ws_send_ping(WebSocketConnection* ws, const uint8_t* data, size_t length)
        self._lib.ws_send_ping.argtypes = [c_void_p, POINTER(c_uint8), c_size_t]
        self._lib.ws_send_ping.restype = c_int
        
        # int ws_send_pong(WebSocketConnection* ws, const uint8_t* data, size_t length)
        self._lib.ws_send_pong.argtypes = [c_void_p, POINTER(c_uint8), c_size_t]
        self._lib.ws_send_pong.restype = c_int
        
        # int ws_close(WebSocketConnection* ws, uint16_t code, const char* reason)
        self._lib.ws_close.argtypes = [c_void_p, c_uint16, c_char_p]
        self._lib.ws_close.restype = c_int
        
        # bool ws_is_open(WebSocketConnection* ws)
        self._lib.ws_is_open.argtypes = [c_void_p]
        self._lib.ws_is_open.restype = c_bool
        
        # uint64_t ws_messages_sent(WebSocketConnection* ws)
        self._lib.ws_messages_sent.argtypes = [c_void_p]
        self._lib.ws_messages_sent.restype = c_uint64
        
        # uint64_t ws_messages_received(WebSocketConnection* ws)
        self._lib.ws_messages_received.argtypes = [c_void_p]
        self._lib.ws_messages_received.restype = c_uint64
        
        # ====================================================================
        # SSE Functions
        # ====================================================================
        
        # SSEConnection* sse_create(uint64_t connection_id)
        self._lib.sse_create.argtypes = [c_uint64]
        self._lib.sse_create.restype = c_void_p
        
        # void sse_destroy(SSEConnection* sse)
        self._lib.sse_destroy.argtypes = [c_void_p]
        self._lib.sse_destroy.restype = None
        
        # int sse_send(SSEConnection* sse, const char* data, const char* event, const char* id, int retry)
        self._lib.sse_send.argtypes = [c_void_p, c_char_p, c_char_p, c_char_p, c_int]
        self._lib.sse_send.restype = c_int
        
        # int sse_send_comment(SSEConnection* sse, const char* comment)
        self._lib.sse_send_comment.argtypes = [c_void_p, c_char_p]
        self._lib.sse_send_comment.restype = c_int
        
        # int sse_ping(SSEConnection* sse)
        self._lib.sse_ping.argtypes = [c_void_p]
        self._lib.sse_ping.restype = c_int
        
        # int sse_close(SSEConnection* sse)
        self._lib.sse_close.argtypes = [c_void_p]
        self._lib.sse_close.restype = c_int
        
        # bool sse_is_open(SSEConnection* sse)
        self._lib.sse_is_open.argtypes = [c_void_p]
        self._lib.sse_is_open.restype = c_bool
        
        # uint64_t sse_events_sent(SSEConnection* sse)
        self._lib.sse_events_sent.argtypes = [c_void_p]
        self._lib.sse_events_sent.restype = c_uint64
    
    @property
    def lib(self):
        return self._lib


def get_lib():
    """Get the native library instance."""
    return _NativeLib.get().lib


def _error_from_code(code: int) -> str:
    """Convert error code to error message."""
    error_messages = {
        -1: "Generic error",
        -2: "Invalid argument",
        -3: "Not found",
        -4: "Connection error",
        -5: "Timeout",
        -6: "Parse error",
    }
    return error_messages.get(code, f"Unknown error ({code})")


__all__ = ['get_lib', '_error_from_code']
