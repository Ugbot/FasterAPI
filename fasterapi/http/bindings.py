"""
HTTP Server C++ Bindings

ctypes FFI bridge to the native HTTP server library.
Handles platform detection, library loading, and C function declarations.
"""

import ctypes
import platform
import os
import sys
from pathlib import Path
from typing import Optional, Dict, Any

# Platform detection
SYSTEM = platform.system().lower()
MACHINE = platform.machine().lower()

if SYSTEM == "darwin":
    if MACHINE == "arm64":
        LIB_NAME = "libfasterapi_http.dylib"
    else:
        LIB_NAME = "libfasterapi_http.dylib"
elif SYSTEM == "linux":
    LIB_NAME = "libfasterapi_http.so"
elif SYSTEM == "windows":
    LIB_NAME = "fasterapi_http.dll"
else:
    raise RuntimeError(f"Unsupported platform: {SYSTEM}")

# Library paths to try
LIB_PATHS = [
    # Development build
    Path(__file__).parent.parent.parent / "build" / "lib" / LIB_NAME,
    # Installed package
    Path(__file__).parent / "_native" / LIB_NAME,
    # System library path
    Path("/usr/local/lib") / LIB_NAME,
    Path("/opt/homebrew/lib") / LIB_NAME,
]

# Global library instance
_lib: Optional[ctypes.CDLL] = None


def _load_native_library() -> ctypes.CDLL:
    """Load the native HTTP server library."""
    global _lib
    
    if _lib is not None:
        return _lib
    
    for lib_path in LIB_PATHS:
        if lib_path.exists():
            try:
                _lib = ctypes.CDLL(str(lib_path))
                print(f"✓ Loaded HTTP library: {lib_path}")
                break
            except OSError as e:
                print(f"Failed to load {lib_path}: {e}")
                continue
    
    if _lib is None:
        # Create a mock library for development
        print("⚠️  HTTP native library not found, using mock implementation")
        _lib = _create_mock_library()
    
    return _lib


def _create_mock_library() -> ctypes.CDLL:
    """Create a mock library for development when native library is not available."""
    class MockLib:
        def __getattr__(self, name):
            def mock_function(*args, **kwargs):
                print(f"Mock HTTP function called: {name}")
                return 0
            return mock_function
    
    return MockLib()  # type: ignore


def get_lib() -> ctypes.CDLL:
    """Get the native HTTP server library."""
    return _load_native_library()


def _error_from_code(code: int) -> str:
    """Convert error code to human-readable string."""
    error_messages = {
        0: "Success",
        1: "Invalid argument",
        2: "Server already exists",
        3: "Memory allocation failed",
        4: "Server not found",
        5: "Server already running",
        6: "Server not running",
        7: "Route not found",
        8: "Handler not found",
        9: "Compression failed",
        10: "Protocol error",
    }
    return error_messages.get(code, f"Unknown error ({code})")


def declare_functions(lib: ctypes.CDLL) -> None:
    """Declare C function signatures for ctypes."""
    
    # Server Management
    lib.http_server_create.argtypes = [
        ctypes.c_uint16,  # port
        ctypes.c_char_p,  # host
        ctypes.c_bool,    # enable_h2
        ctypes.c_bool,    # enable_h3
        ctypes.c_bool,    # enable_compression
        ctypes.POINTER(ctypes.c_int),  # error_out
    ]
    lib.http_server_create.restype = ctypes.c_void_p
    
    lib.http_server_destroy.argtypes = [ctypes.c_void_p]
    lib.http_server_destroy.restype = ctypes.c_int
    
    lib.http_server_start.argtypes = [
        ctypes.c_void_p,  # server
        ctypes.POINTER(ctypes.c_int),  # error_out
    ]
    lib.http_server_start.restype = ctypes.c_int
    
    lib.http_server_stop.argtypes = [
        ctypes.c_void_p,  # server
        ctypes.POINTER(ctypes.c_int),  # error_out
    ]
    lib.http_server_stop.restype = ctypes.c_int
    
    lib.http_server_is_running.argtypes = [ctypes.c_void_p]
    lib.http_server_is_running.restype = ctypes.c_bool
    
    # Route Management
    lib.http_add_route.argtypes = [
        ctypes.c_void_p,  # server
        ctypes.c_char_p,  # method
        ctypes.c_char_p,  # path
        ctypes.c_uint32,  # handler_id
        ctypes.POINTER(ctypes.c_int),  # error_out
    ]
    lib.http_add_route.restype = ctypes.c_int
    
    lib.http_add_websocket.argtypes = [
        ctypes.c_void_p,  # server
        ctypes.c_char_p,  # path
        ctypes.c_uint32,  # handler_id
        ctypes.POINTER(ctypes.c_int),  # error_out
    ]
    lib.http_add_websocket.restype = ctypes.c_int
    
    # Statistics
    lib.http_server_stats.argtypes = [
        ctypes.c_void_p,  # server
        ctypes.c_void_p,  # out_stats
    ]
    lib.http_server_stats.restype = ctypes.c_int
    
    # Library Management
    lib.http_lib_init.argtypes = []
    lib.http_lib_init.restype = ctypes.c_int
    
    lib.http_lib_shutdown.argtypes = []
    lib.http_lib_shutdown.restype = ctypes.c_int


# Initialize library and declare functions
try:
    lib = get_lib()
    declare_functions(lib)
except Exception as e:
    print(f"Warning: Failed to initialize HTTP library: {e}")
    lib = None