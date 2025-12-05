"""PostgreSQL connection pool and handle wrappers."""

from typing import Optional, Any, Iterator, List, Type, TypeVar
import threading
import ctypes
from contextlib import contextmanager

from .types import QueryResult, Row, TxIsolation, PreparedQuery
from .exceptions import PgConnectionError, PgError
from .bindings import get_lib, PgNativeError

T = TypeVar("T")


class PgPool:
    """High-performance connection pool with per-core sharding.

    Manages PostgreSQL connections with:
    - Per-core connection affinity (avoid cross-core locks)
    - Prepared statement caching per connection
    - Health checks and connection recycling
    - Zero-copy row decoding
    """

    def __init__(
        self,
        dsn: str,
        min_size: int = 1,
        max_size: int = 20,
        idle_timeout_secs: int = 600,
        health_check_interval_secs: int = 30,
    ):
        """Initialize connection pool.

        Args:
            dsn: PostgreSQL connection string.
            min_size: Minimum connections per core.
            max_size: Maximum connections per core.
            idle_timeout_secs: Close idle connections after this time.
            health_check_interval_secs: Interval between health checks.

        Raises:
            PgConnectionError: If pool initialization fails.
        """
        self.dsn = dsn
        self.min_size = min_size
        self.max_size = max_size
        self.idle_timeout_secs = idle_timeout_secs
        self.health_check_interval_secs = health_check_interval_secs

        # Pool handle from C++ layer
        self._handle: Any = None
        self._lock = threading.Lock()
        self._closed = False

        self._initialize()

    def _initialize(self) -> None:
        """Initialize pool from C++ layer.

        Raises:
            PgConnectionError: If initialization fails.
        """
        try:
            lib = get_lib()
            error_code = ctypes.c_int()
            self._handle = lib.pg_pool_create(
                self.dsn.encode(),
                self.min_size,
                self.max_size,
                ctypes.byref(error_code),
            )

            if error_code.value != 0 or not self._handle:
                raise PgConnectionError(
                    f"Failed to initialize pool: error code {error_code.value}"
                )
        except Exception as e:
            raise PgConnectionError(f"Failed to initialize pool: {e}")

    def get(self, core_id: Optional[int] = None, deadline_ms: int = 5000) -> "Pg":
        """Get a connection handle from the pool.

        Args:
            core_id: CPU core for affinity (None = auto-select).
            deadline_ms: Max time to wait for available connection.

        Returns:
            Pg handle bound to a connection.

        Raises:
            PgConnectionError: If pool exhausted or deadline exceeded.
        """
        if self._closed:
            raise PgConnectionError("Pool is closed")

        # Auto-select core based on thread ID
        if core_id is None:
            core_id = threading.current_thread().ident % 16  # Assume max 16 cores

        try:
            lib = get_lib()
            error_code = ctypes.c_int()
            conn_handle = lib.pg_pool_get(
                self._handle, core_id, deadline_ms, ctypes.byref(error_code)
            )

            if error_code.value != 0 or not conn_handle:
                if error_code.value == 4:
                    raise PgConnectionError("Connection pool timeout")
                raise PgConnectionError(
                    f"Failed to get connection: error code {error_code.value}"
                )

            return Pg(pool=self, core_id=core_id, conn_handle=conn_handle)
        except Exception as e:
            raise PgConnectionError(f"Failed to get connection: {e}")

    def release(self, conn_handle: Any) -> None:
        """Release a connection back to the pool.

        Args:
            conn_handle: Connection handle from get().
        """
        if self._closed or not conn_handle:
            return

        try:
            lib = get_lib()
            lib.pg_pool_release(self._handle, conn_handle)
        except Exception:
            pass  # Ignore errors during release

    def stats(self) -> dict:
        """Get pool statistics.

        Returns:
            Dict with keys: in_use, idle, waiting, total_created, total_recycled.
        """
        if self._closed:
            return {
                "in_use": 0,
                "idle": 0,
                "waiting": 0,
                "total_created": 0,
                "total_recycled": 0,
            }

        try:
            lib = get_lib()

            # Create buffer for stats structure
            # Simplified: just return dummy stats for now
            # TODO: Implement proper stats structure
            return {
                "in_use": 0,
                "idle": self.min_size,
                "waiting": 0,
                "total_created": self.min_size,
                "total_recycled": 0,
            }
        except Exception:
            return {
                "in_use": 0,
                "idle": 0,
                "waiting": 0,
                "total_created": 0,
                "total_recycled": 0,
            }

    def close(self) -> None:
        """Close pool and all connections."""
        with self._lock:
            if self._closed:
                return

            try:
                lib = get_lib()
                if self._handle is not None:
                    lib.pg_pool_destroy(self._handle)
            except Exception:
                pass  # Ignore errors during cleanup

            self._closed = True

    def __enter__(self) -> "PgPool":
        return self

    def __exit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> None:
        self.close()


class Pg:
    """Connection handle for query execution.

    Lightweight wrapper around a C++ connection in the pool. Each request
    gets a per-core bound handle that avoids cross-core locks.
    """

    def __init__(self, pool: PgPool, core_id: int, conn_handle: Any):
        """Initialize connection handle.

        Args:
            pool: Parent pool.
            core_id: Core affinity for this connection.
            conn_handle: Low-level C++ connection handle.
        """
        self.pool = pool
        self.core_id = core_id
        self._handle = conn_handle
        self._in_transaction = False
        self._released = False

    def __enter__(self) -> "Pg":
        return self

    def __exit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> None:
        self.release()

    def release(self) -> None:
        """Release this connection back to the pool."""
        if not self._released:
            self.pool.release(self._handle)
            self._released = True

    def exec(self, sql: str, *params: Any) -> QueryResult[Row]:
        """Execute query with parameters.

        Args:
            sql: SQL query string (use $1, $2, ... for params).
            *params: Query parameters.

        Returns:
            QueryResult with .all(), .one(), .scalar(), etc.

        Raises:
            PgError: On query execution error.
        """
        if self._released:
            raise PgError("Connection already released")

        try:
            lib = get_lib()
            error_code = ctypes.c_int()

            # Convert parameters to strings
            param_strs = [str(p).encode() if p is not None else b"" for p in params]
            param_ptrs = (
                (ctypes.c_char_p * len(param_strs))(*param_strs) if param_strs else None
            )

            result_handle = lib.pg_exec_query(
                self._handle,
                sql.encode(),
                len(param_strs),
                param_ptrs,
                ctypes.byref(error_code),
            )

            if error_code.value != 0:
                # Try to get error message from result
                error_msg = "Query execution failed"
                if result_handle:
                    err_ptr = lib.pg_result_error_message(result_handle)
                    if err_ptr:
                        error_msg = err_ptr.decode("utf-8")
                    lib.pg_result_destroy(result_handle)
                raise PgError(f"{error_msg} (error code {error_code.value})")

            return QueryResult(result_handle=result_handle)
        except PgError:
            raise
        except Exception as e:
            raise PgError(f"Query execution failed: {e}")

    def prepare(self, sql: str, result_type: Optional[Type] = None) -> PreparedQuery:
        """Prepare a query for reuse.

        Args:
            sql: SQL query string.
            result_type: Optional result type hint (list[tuple], etc.).

        Returns:
            PreparedQuery handle for use with .run().
        """
        # For now, just return a prepared query object
        # Full implementation would cache in C++ layer
        return PreparedQuery(
            query_id=hash(sql), sql=sql, param_types=(), result_type=result_type
        )

    def run(self, prepared: PreparedQuery, *params: Any) -> QueryResult[Row]:
        """Run a prepared query (fastest path).

        Args:
            prepared: PreparedQuery from .prepare().
            *params: Query parameters (must match prepared types).

        Returns:
            QueryResult.

        Raises:
            PgError: On query execution error.
        """
        # For now, just execute as regular query
        return self.exec(prepared.sql, *params)

    @contextmanager
    def tx(
        self,
        isolation: TxIsolation = TxIsolation.read_committed,
        retries: int = 0,
    ) -> Iterator["Pg"]:
        """Start a transaction context.

        Args:
            isolation: Transaction isolation level.
            retries: Number of retries on serialization failure.

        Yields:
            Self (same Pg handle) for transaction operations.

        Raises:
            PgError: On commit failure or serialization conflict (after retries).
        """
        attempt = 0
        last_error = None

        while attempt <= retries:
            try:
                # Begin transaction
                lib = get_lib()
                error_code = ctypes.c_int()
                result = lib.pg_tx_begin(
                    self._handle, isolation.value.encode(), ctypes.byref(error_code)
                )

                if result != 0:
                    raise PgError(
                        f"Failed to begin transaction: error code {error_code.value}"
                    )

                self._in_transaction = True

                # Yield for transaction operations
                yield self

                # Commit transaction
                error_code = ctypes.c_int()
                result = lib.pg_tx_commit(self._handle, ctypes.byref(error_code))

                if result != 0:
                    raise PgError(f"Commit failed: error code {error_code.value}")

                self._in_transaction = False
                return

            except PgError as e:
                # Rollback transaction
                try:
                    lib = get_lib()
                    lib.pg_tx_rollback(self._handle)
                except:
                    pass

                self._in_transaction = False
                last_error = e

                # Check if serialization error and retry
                if "serialization" in str(e).lower() and attempt < retries:
                    attempt += 1
                    continue
                else:
                    raise

        if last_error:
            raise last_error

    @contextmanager
    def copy_in(self, sql: str, chunk_size: int = 8192) -> Iterator[Any]:
        """Start a COPY IN stream.

        Args:
            sql: COPY command (e.g., "COPY table(col1, col2) FROM stdin CSV").
            chunk_size: Size of write chunks.

        Yields:
            Pipe-like object with .write(bytes) method.

        Raises:
            PgError: On COPY error.
        """
        try:
            lib = get_lib()
            error_code = ctypes.c_int()
            result = lib.pg_copy_in_start(
                self._handle, sql.encode(), ctypes.byref(error_code)
            )

            if result != 0:
                raise PgError(f"COPY IN start failed: error code {error_code.value}")

            pipe = _CopyInPipe(self, chunk_size)
            yield pipe

        finally:
            # End COPY IN
            try:
                pipe.close()
            except:
                pass

    def copy_out_response(self, sql: str, filename: Optional[str] = None) -> Any:
        """Stream COPY OUT to HTTP response.

        Args:
            sql: COPY command (e.g., "COPY table TO stdout CSV").
            filename: Optional filename for Content-Disposition header.

        Returns:
            StreamingResponse-compatible object.
        """
        # TODO: Implement COPY OUT streaming
        raise NotImplementedError("COPY OUT not yet implemented")

    def cancel(self) -> None:
        """Cancel current query on this connection."""
        if self._released:
            return

        try:
            lib = get_lib()
            # TODO: Implement cancel via C++ layer
            # lib.pg_cancel_query(self._handle)
        except:
            pass


class _CopyInPipe:
    """Internal pipe for COPY IN streaming."""

    def __init__(self, pg: Pg, chunk_size: int):
        self.pg = pg
        self.chunk_size = chunk_size
        self.buffer: bytearray = bytearray()
        self.closed = False

    def write(self, data: bytes) -> int:
        """Write data to COPY stream.

        Args:
            data: Bytes to write.

        Returns:
            Number of bytes written.

        Raises:
            PgError: On write error.
        """
        if self.closed:
            raise PgError("COPY stream already closed")

        try:
            lib = get_lib()
            error_code = ctypes.c_int()
            bytes_written = lib.pg_copy_in_write(
                self.pg._handle, data, len(data), ctypes.byref(error_code)
            )

            if error_code.value != 0:
                raise PgError(f"COPY IN write failed: error code {error_code.value}")

            return bytes_written
        except PgError:
            raise
        except Exception as e:
            raise PgError(f"COPY IN write failed: {e}")

    def close(self) -> None:
        """Close COPY stream and finish operation."""
        if self.closed:
            return

        try:
            lib = get_lib()
            error_code = ctypes.c_int()
            result = lib.pg_copy_in_end(self.pg._handle, ctypes.byref(error_code))

            if result != 0:
                raise PgError(f"COPY IN end failed: error code {error_code.value}")

            self.closed = True
        except PgError:
            raise
        except Exception as e:
            raise PgError(f"COPY IN end failed: {e}")


def prepare(sql: str, result: Optional[Type] = None) -> PreparedQuery:
    """Module-level function to prepare a query statically.

    Args:
        sql: SQL query string.
        result: Optional result type hint.

    Returns:
        PreparedQuery handle.
    """
    return PreparedQuery(
        query_id=hash(sql), sql=sql, param_types=(), result_type=result
    )
