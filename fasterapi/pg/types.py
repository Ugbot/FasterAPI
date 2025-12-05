"""Core types for PostgreSQL integration."""

from enum import Enum
from dataclasses import dataclass
from typing import Any, Generic, TypeVar, Iterator, Type, Optional, Mapping, List
from datetime import datetime
from decimal import Decimal
from uuid import UUID


class TxIsolation(Enum):
    """Transaction isolation levels."""

    read_uncommitted = "READ UNCOMMITTED"
    read_committed = "READ COMMITTED"
    repeatable_read = "REPEATABLE READ"
    serializable = "SERIALIZABLE"


class Row(dict):
    """Mapping-like row result with zero-copy view support.

    Supports both dict-like access and attribute access.
    """

    def __getattr__(self, name: str) -> Any:
        try:
            return self[name]
        except KeyError:
            raise AttributeError(f"Column '{name}' not found") from None

    def __setattr__(self, name: str, value: Any) -> None:
        self[name] = value


T = TypeVar("T")


class QueryResult(Generic[T]):
    """Query result set with multiple consumption methods.

    Supports eager loading (.all()), single row (.one()), streaming (.stream()),
    and various result conversions (.model(), .into()).
    """

    def __init__(
        self,
        result_handle: Any = None,
        rows: List[Row] | None = None,
        iterator: Iterator[Row] | None = None,
    ):
        """Initialize result.

        Args:
            result_handle: C++ result handle (from pg_exec_query)
            rows: Pre-loaded list of rows (eager mode)
            iterator: Iterator of rows (streaming mode)
        """
        self._handle = result_handle
        self._rows = rows
        self._iterator = iterator
        self._consumed = False

    def __del__(self):
        """Cleanup result handle when garbage collected."""
        if self._handle:
            try:
                from .bindings import get_lib

                lib = get_lib()
                lib.pg_result_destroy(self._handle)
            except:
                pass  # Ignore errors during cleanup

    def all(self) -> List[Row]:
        """Get all rows as a list.

        Returns:
            List of Row objects.

        Raises:
            PgError: If result is in streaming mode and already consumed.
        """
        from .bindings import get_lib
        from .exceptions import PgError

        if self._rows is not None:
            return self._rows

        if self._consumed:
            raise PgError("Result already consumed")

        if not self._handle:
            return []

        lib = get_lib()

        # Get row and column counts
        row_count = lib.pg_result_row_count(self._handle)
        if row_count < 0:
            raise PgError("Failed to get row count")

        field_count = lib.pg_result_field_count(self._handle)
        if field_count < 0:
            raise PgError("Failed to get field count")

        # Get column names
        col_names = []
        for col_idx in range(field_count):
            name_ptr = lib.pg_result_field_name(self._handle, col_idx)
            if name_ptr:
                col_names.append(name_ptr.decode("utf-8"))
            else:
                col_names.append(f"col_{col_idx}")

        # Build rows
        rows = []
        for row_idx in range(row_count):
            row = Row()
            for col_idx in range(field_count):
                col_name = col_names[col_idx]

                # Check if NULL
                is_null = lib.pg_result_is_null(self._handle, row_idx, col_idx)
                if is_null == 1:
                    row[col_name] = None
                else:
                    # Get value
                    value_ptr = lib.pg_result_get_value(self._handle, row_idx, col_idx)
                    if value_ptr:
                        row[col_name] = value_ptr.decode("utf-8")
                    else:
                        row[col_name] = None

            rows.append(row)

        self._rows = rows
        self._consumed = True
        return rows

    def one(self) -> Row:
        """Get exactly one row.

        Returns:
            Single Row object.

        Raises:
            PgError: If result has 0 or more than 1 row.
        """
        from .exceptions import PgError

        rows = self.all()
        if len(rows) == 0:
            raise PgError("Expected exactly 1 row, got 0")
        elif len(rows) > 1:
            raise PgError(f"Expected exactly 1 row, got {len(rows)}")
        return rows[0]

    def first(self) -> Row | None:
        """Get first row or None.

        Returns:
            First Row or None if empty.
        """
        rows = self.all()
        return rows[0] if rows else None

    def scalar(self) -> Any:
        """Get first column of first row.

        Returns:
            Value of first column in first row.

        Raises:
            PgError: If result is empty.
        """
        from .bindings import get_lib
        from .exceptions import PgError

        if not self._handle:
            raise PgError("Result is empty")

        lib = get_lib()

        # Use optimized scalar method if available
        value_ptr = lib.pg_result_scalar(self._handle)
        if value_ptr:
            return value_ptr.decode("utf-8")

        # Fallback to getting first row
        row = self.first()
        if not row:
            raise PgError("Result is empty")

        # Get first value from row
        for value in row.values():
            return value

        raise PgError("Result is empty")

    def stream(self, chunk_size: int = 1000) -> Iterator[Row]:
        """Stream rows without buffering all in memory.

        Args:
            chunk_size: Number of rows to fetch per batch.

        Yields:
            Row objects one at a time.
        """
        # For now, just iterate over all() results
        # TODO: Implement true streaming with cursor/COPY
        for row in self.all():
            yield row

    def model(self, model_type: Type[T]) -> List[T]:
        """Convert rows to pydantic models using pydantic-core (fast path).

        Args:
            model_type: Pydantic BaseModel class.

        Returns:
            List of model instances.
        """
        rows = self.all()
        return [model_type(**row) for row in rows]

    def into(self, target_type: Type[T]) -> T:
        """Convert rows into typed container (list, tuple, etc.).

        Args:
            target_type: Target type (e.g., list[tuple[int, str]]).

        Returns:
            Converted result.
        """
        rows = self.all()

        # Simple conversion for common types
        if target_type == list:
            return rows  # type: ignore
        elif target_type == tuple or str(target_type).startswith("typing.Tuple"):
            return tuple(tuple(row.values()) for row in rows)  # type: ignore
        else:
            # Default: return as-is
            return rows  # type: ignore


@dataclass(frozen=True)
class PreparedQuery:
    """Pre-compiled prepared statement metadata.

    Stores query text, bind parameter info, and result type hints for zero-copy
    optimizations.
    """

    query_id: int
    sql: str
    param_types: tuple[str, ...]
    result_type: Optional[Type] = None

    def __repr__(self) -> str:
        return f"PreparedQuery(id={self.query_id}, params={len(self.param_types)})"
