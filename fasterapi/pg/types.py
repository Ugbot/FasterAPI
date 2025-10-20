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


T = TypeVar('T')


class QueryResult(Generic[T]):
    """Query result set with multiple consumption methods.
    
    Supports eager loading (.all()), single row (.one()), streaming (.stream()),
    and various result conversions (.model(), .into()).
    """
    
    def __init__(self, rows: List[Row] | None = None, iterator: Iterator[Row] | None = None):
        """Initialize result.
        
        Args:
            rows: Pre-loaded list of rows (eager mode)
            iterator: Iterator of rows (streaming mode)
        """
        self._rows = rows
        self._iterator = iterator
        self._consumed = False
    
    def all(self) -> List[Row]:
        """Get all rows as a list.
        
        Returns:
            List of Row objects.
            
        Raises:
            PgError: If result is in streaming mode and already consumed.
        """
        raise NotImplementedError("Stub: implement in C++ layer")
    
    def one(self) -> Row:
        """Get exactly one row.
        
        Returns:
            Single Row object.
            
        Raises:
            PgError: If result has 0 or more than 1 row.
        """
        raise NotImplementedError("Stub: implement in C++ layer")
    
    def first(self) -> Row | None:
        """Get first row or None.
        
        Returns:
            First Row or None if empty.
        """
        raise NotImplementedError("Stub: implement in C++ layer")
    
    def scalar(self) -> Any:
        """Get first column of first row.
        
        Returns:
            Value of first column in first row.
            
        Raises:
            PgError: If result is empty.
        """
        raise NotImplementedError("Stub: implement in C++ layer")
    
    def stream(self, chunk_size: int = 1000) -> Iterator[Row]:
        """Stream rows without buffering all in memory.
        
        Args:
            chunk_size: Number of rows to fetch per batch.
            
        Yields:
            Row objects one at a time.
        """
        raise NotImplementedError("Stub: implement in C++ layer")
    
    def model(self, model_type: Type[T]) -> List[T]:
        """Convert rows to pydantic models using pydantic-core (fast path).
        
        Args:
            model_type: Pydantic BaseModel class.
            
        Returns:
            List of model instances.
        """
        raise NotImplementedError("Stub: implement in C++ layer")
    
    def into(self, target_type: Type[T]) -> T:
        """Convert rows into typed container (list, tuple, etc.).
        
        Args:
            target_type: Target type (e.g., list[tuple[int, str]]).
            
        Returns:
            Converted result.
        """
        raise NotImplementedError("Stub: implement in C++ layer")


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
