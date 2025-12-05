"""ctypes FFI bindings to native PostgreSQL library.

Handles platform detection, library loading, and function declarations.
"""

import ctypes
import sys
import os
from pathlib import Path
from typing import Callable, Optional
import logging

logger = logging.getLogger(__name__)


class PgNativeError(Exception):
    """Error loading or calling native library."""

    pass


# Platform detection
PLATFORM = sys.platform
if PLATFORM.startswith("linux"):
    LIB_NAME = "libfasterapi_pg.so"
elif PLATFORM == "darwin":
    LIB_NAME = "libfasterapi_pg.dylib"
elif PLATFORM == "win32":
    LIB_NAME = "fasterapi_pg.dll"
else:
    raise PgNativeError(f"Unsupported platform: {PLATFORM}")


def _load_native_library() -> ctypes.CDLL:
    """Load native library with fallback paths.

    Searches in:
    1. Package _native/ directory
    2. build/ directory (during development)
    3. System library path

    Returns:
        Loaded ctypes.CDLL instance.

    Raises:
        PgNativeError: If library cannot be found or loaded.
    """
    search_paths = [
        Path(__file__).parent / "_native" / LIB_NAME,
        Path(__file__).parent.parent.parent / "build" / LIB_NAME,
        Path(LIB_NAME),  # System path
    ]

    for path in search_paths:
        if path.exists() or not str(path).startswith("/"):
            try:
                lib = ctypes.CDLL(str(path))
                logger.debug(f"Loaded native library from {path}")
                return lib
            except OSError as e:
                logger.debug(f"Failed to load from {path}: {e}")

    raise PgNativeError(
        f"Could not load {LIB_NAME}. Searched: {search_paths}. "
        "Run: python setup.py build_ext --inplace"
    )


# Load library (lazy if needed)
_lib: Optional[ctypes.CDLL] = None


def get_lib() -> ctypes.CDLL:
    """Get loaded native library (lazy initialization)."""
    global _lib
    if _lib is None:
        _lib = _load_native_library()
    return _lib


# Type aliases for C callbacks
PgPoolHandle = ctypes.c_void_p
PgConnectionHandle = ctypes.c_void_p
PgResultHandle = ctypes.c_void_p


# Error code to exception mapping
def _error_from_code(code: int, message: str) -> Exception:
    """Convert error code to appropriate exception type.

    Args:
        code: Error code from native library.
        message: Error message string.

    Returns:
        Appropriate exception instance.
    """
    from .exceptions import (
        PgError,
        PgConnectionError,
        PgTimeout,
        PgCanceled,
        PgIntegrityError,
        PgDataError,
    )

    # Stub: error code mapping will be expanded as implementation progresses
    if code == 1:
        return PgConnectionError(message, code)
    elif code == 2:
        return PgTimeout(message, code)
    elif code == 3:
        return PgCanceled(message, code)
    elif code == 4:
        return PgIntegrityError(message, code)
    elif code == 5:
        return PgDataError(message, code)
    else:
        return PgError(message, code)


# ==============================================================================
# Native Function Declarations (Stubs)
# ==============================================================================


def declare_functions() -> None:
    """Declare ctypes function signatures for native library.

    This is called during module initialization to set up the FFI.
    Functions will be actual ctypes declarations as implementation progresses.
    """
    lib = get_lib()

    # ---- Pool Management ----

    # pool_create(dsn: *const char, min_size: uint, max_size: uint, error_out: *mut int) -> PgPoolHandle
    # Creates a new connection pool. Error code returned via error_out pointer.
    lib.pg_pool_create.argtypes = [
        ctypes.c_char_p,
        ctypes.c_uint,
        ctypes.c_uint,
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.pg_pool_create.restype = PgPoolHandle

    # pool_destroy(pool: PgPoolHandle) -> int
    # Destroys pool and closes all connections. Returns error code.
    lib.pg_pool_destroy.argtypes = [PgPoolHandle]
    lib.pg_pool_destroy.restype = ctypes.c_int

    # pool_get(pool: PgPoolHandle, core_id: uint, deadline_ms: uint64, error_out: *mut int) -> PgConnectionHandle
    # Get connection from pool for given core. Blocks up to deadline_ms.
    lib.pg_pool_get.argtypes = [
        PgPoolHandle,
        ctypes.c_uint,
        ctypes.c_uint64,
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.pg_pool_get.restype = PgConnectionHandle

    # pool_release(pool: PgPoolHandle, conn: PgConnectionHandle) -> int
    # Release connection back to pool.
    lib.pg_pool_release.argtypes = [PgPoolHandle, PgConnectionHandle]
    lib.pg_pool_release.restype = ctypes.c_int

    # ---- Query Execution ----

    # exec_query(conn: PgConnectionHandle, sql: *const char, param_count: uint, params: *const *const char, error_out: *mut int) -> PgResultHandle
    # Execute query with parameters. Returns result handle.
    lib.pg_exec_query.argtypes = [
        PgConnectionHandle,
        ctypes.c_char_p,
        ctypes.c_uint,
        ctypes.POINTER(ctypes.c_char_p),
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.pg_exec_query.restype = PgResultHandle

    # result_row_count(result: PgResultHandle) -> int64
    # Get number of rows in result.
    lib.pg_result_row_count.argtypes = [PgResultHandle]
    lib.pg_result_row_count.restype = ctypes.c_int64

    # result_field_count(result: PgResultHandle) -> int32
    # Get number of columns in result.
    lib.pg_result_field_count.argtypes = [PgResultHandle]
    lib.pg_result_field_count.restype = ctypes.c_int32

    # result_field_name(result: PgResultHandle, col_index: int32) -> *const char
    # Get column name.
    lib.pg_result_field_name.argtypes = [PgResultHandle, ctypes.c_int32]
    lib.pg_result_field_name.restype = ctypes.c_char_p

    # result_get_value(result: PgResultHandle, row_index: int64, col_index: int32) -> *const char
    # Get value from result.
    lib.pg_result_get_value.argtypes = [PgResultHandle, ctypes.c_int64, ctypes.c_int32]
    lib.pg_result_get_value.restype = ctypes.c_char_p

    # result_is_null(result: PgResultHandle, row_index: int64, col_index: int32) -> int
    # Check if value is NULL.
    lib.pg_result_is_null.argtypes = [PgResultHandle, ctypes.c_int64, ctypes.c_int32]
    lib.pg_result_is_null.restype = ctypes.c_int

    # result_get_length(result: PgResultHandle, row_index: int64, col_index: int32) -> int32
    # Get value length.
    lib.pg_result_get_length.argtypes = [PgResultHandle, ctypes.c_int64, ctypes.c_int32]
    lib.pg_result_get_length.restype = ctypes.c_int32

    # result_scalar(result: PgResultHandle) -> *const char
    # Get scalar value (single row, single column).
    lib.pg_result_scalar.argtypes = [PgResultHandle]
    lib.pg_result_scalar.restype = ctypes.c_char_p

    # result_error_message(result: PgResultHandle) -> *const char
    # Get error message from result.
    lib.pg_result_error_message.argtypes = [PgResultHandle]
    lib.pg_result_error_message.restype = ctypes.c_char_p

    # result_destroy(result: PgResultHandle) -> int
    # Free result handle.
    lib.pg_result_destroy.argtypes = [PgResultHandle]
    lib.pg_result_destroy.restype = ctypes.c_int

    # ---- Transactions ----

    # tx_begin(conn: PgConnectionHandle, isolation: *const char, error_out: *mut int) -> int
    # Start transaction with isolation level.
    lib.pg_tx_begin.argtypes = [
        PgConnectionHandle,
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.pg_tx_begin.restype = ctypes.c_int

    # tx_commit(conn: PgConnectionHandle, error_out: *mut int) -> int
    # Commit transaction.
    lib.pg_tx_commit.argtypes = [PgConnectionHandle, ctypes.POINTER(ctypes.c_int)]
    lib.pg_tx_commit.restype = ctypes.c_int

    # tx_rollback(conn: PgConnectionHandle) -> int
    # Rollback transaction.
    lib.pg_tx_rollback.argtypes = [PgConnectionHandle]
    lib.pg_tx_rollback.restype = ctypes.c_int

    # ---- COPY Operations ----

    # copy_in_start(conn: PgConnectionHandle, sql: *const char, error_out: *mut int) -> int
    # Start COPY IN operation.
    lib.pg_copy_in_start.argtypes = [
        PgConnectionHandle,
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.pg_copy_in_start.restype = ctypes.c_int

    # copy_in_write(conn: PgConnectionHandle, data: *const char, len: uint64, error_out: *mut int) -> uint64
    # Write data to COPY IN. Returns bytes written.
    lib.pg_copy_in_write.argtypes = [
        PgConnectionHandle,
        ctypes.c_char_p,
        ctypes.c_uint64,
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.pg_copy_in_write.restype = ctypes.c_uint64

    # copy_in_end(conn: PgConnectionHandle, error_out: *mut int) -> int
    # End COPY IN operation.
    lib.pg_copy_in_end.argtypes = [PgConnectionHandle, ctypes.POINTER(ctypes.c_int)]
    lib.pg_copy_in_end.restype = ctypes.c_int

    # ---- Pool Statistics ----

    # pool_stats_get(pool: PgPoolHandle, out: *mut PgPoolStats) -> int
    # Get current pool statistics (stubs for now, structure TBD).
    lib.pg_pool_stats_get.argtypes = [PgPoolHandle, ctypes.c_void_p]
    lib.pg_pool_stats_get.restype = ctypes.c_int


# Initialize function declarations on module load
try:
    declare_functions()
except PgNativeError as e:
    logger.warning(f"Native library not available during development: {e}")
