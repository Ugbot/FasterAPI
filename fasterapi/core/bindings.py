"""
C++ Future Bindings

ctypes bindings for C++ future/reactor functionality.
"""

import ctypes
from ctypes import c_void_p, c_uint32, c_uint64, c_int, c_char_p, CFUNCTYPE
from typing import Optional
import os
import sys

# Find library
def find_library():
    """Find the fasterapi library."""
    possible_paths = [
        "build/lib/libfasterapi_pg.dylib",
        "build/lib/libfasterapi_pg.so",
        "build/lib/libfasterapi_http.dylib",
        "build/lib/libfasterapi_http.so",
    ]
    
    for path in possible_paths:
        if os.path.exists(path):
            return path
    
    return None

# Load library
_lib_path = find_library()
if _lib_path:
    _lib = ctypes.CDLL(_lib_path)
else:
    _lib = None
    print("Warning: FasterAPI C++ library not found", file=sys.stderr)

# Callback type for future resolution
FutureCallback = CFUNCTYPE(None, c_void_p, c_int, c_void_p)

# Reactor functions (to be implemented in C++)
if _lib:
    # Reactor initialization
    # int reactor_initialize(uint32_t num_cores);
    reactor_initialize = _lib.reactor_initialize if hasattr(_lib, 'reactor_initialize') else None
    if reactor_initialize:
        reactor_initialize.argtypes = [c_uint32]
        reactor_initialize.restype = c_int
    
    # int reactor_shutdown();
    reactor_shutdown = _lib.reactor_shutdown if hasattr(_lib, 'reactor_shutdown') else None
    if reactor_shutdown:
        reactor_shutdown.argtypes = []
        reactor_shutdown.restype = c_int
    
    # uint32_t reactor_current_core();
    reactor_current_core = _lib.reactor_current_core if hasattr(_lib, 'reactor_current_core') else None
    if reactor_current_core:
        reactor_current_core.argtypes = []
        reactor_current_core.restype = c_uint32
    
    # uint32_t reactor_num_cores();
    reactor_num_cores = _lib.reactor_num_cores if hasattr(_lib, 'reactor_num_cores') else None
    if reactor_num_cores:
        reactor_num_cores.argtypes = []
        reactor_num_cores.restype = c_uint32
else:
    reactor_initialize = None
    reactor_shutdown = None
    reactor_current_core = None
    reactor_num_cores = None

# Future functions (to be implemented in C++)
# These would be exported from the C++ library
# For now, they're placeholders

def future_add_callback(future_handle: int, callback: FutureCallback) -> int:
    """
    Add a callback to be invoked when future resolves.
    
    Args:
        future_handle: Opaque future handle from C++
        callback: Callback function to invoke
        
    Returns:
        0 on success, error code otherwise
    """
    # TODO: Implement C++ export
    return 0

def future_then(future_handle: int, func) -> int:
    """
    Chain a continuation to a future.
    
    Args:
        future_handle: Opaque future handle from C++
        func: Continuation function
        
    Returns:
        New future handle
    """
    # TODO: Implement C++ export
    return 0

def future_get(future_handle: int) -> tuple:
    """
    Get future value (blocking).
    
    Args:
        future_handle: Opaque future handle from C++
        
    Returns:
        (success: bool, value: any)
    """
    # TODO: Implement C++ export
    return (False, None)

def future_is_ready(future_handle: int) -> bool:
    """
    Check if future is ready.
    
    Args:
        future_handle: Opaque future handle from C++
        
    Returns:
        True if ready, False otherwise
    """
    # TODO: Implement C++ export
    return False

