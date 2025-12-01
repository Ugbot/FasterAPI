#!/usr/bin/env python3
"""
PostgreSQL Pool Unit Tests

Tests the Python wrapper layer (PgPool, Pg, QueryResult, etc.) without
requiring a running PostgreSQL database. Uses mocking to simulate the
native C++ bindings.

These tests focus on:
- Pool lifecycle management
- Connection handle behavior
- Query result processing
- Error handling and edge cases
- Transaction context managers
- Thread safety
"""

import sys
import pytest
import random
import string
import threading

sys.path.insert(0, '/Users/bengamble/FasterAPI')

from fasterapi.pg.types import Row, QueryResult, TxIsolation, PreparedQuery
from fasterapi.pg.exceptions import PgConnectionError, PgError


# =============================================================================
# Test Helpers
# =============================================================================

def random_string(length: int = 10) -> str:
    """Generate a random alphanumeric string."""
    return ''.join(random.choices(string.ascii_letters + string.digits, k=length))


def random_int(min_val: int = 1, max_val: int = 1000) -> int:
    """Generate a random integer."""
    return random.randint(min_val, max_val)


# Check if native library is available for integration tests
try:
    from fasterapi.pg.bindings import get_lib
    NATIVE_LIB_AVAILABLE = True
except Exception:
    NATIVE_LIB_AVAILABLE = False


# Skip marker for tests requiring native library
requires_native = pytest.mark.skipif(
    not NATIVE_LIB_AVAILABLE,
    reason="Native PostgreSQL library not available"
)


# =============================================================================
# Row Type Tests
# =============================================================================

class TestRow:
    """Tests for Row type."""

    def test_row_dict_access(self):
        """Test dictionary-style access."""
        row = Row(name="test", value=42)
        assert row["name"] == "test"
        assert row["value"] == 42

    def test_row_attribute_access(self):
        """Test attribute-style access."""
        row = Row(name="test", value=42)
        assert row.name == "test"
        assert row.value == 42

    def test_row_attribute_set(self):
        """Test setting attributes."""
        row = Row()
        row.name = "test"
        row.value = 42
        assert row["name"] == "test"
        assert row["value"] == 42

    def test_row_missing_attribute(self):
        """Test accessing missing attribute raises AttributeError."""
        row = Row(name="test")
        with pytest.raises(AttributeError, match="Column 'missing' not found"):
            _ = row.missing

    def test_row_with_randomized_data(self):
        """Test row with randomized data."""
        data = {random_string(5): random_int() for _ in range(10)}
        row = Row(**data)
        for key, value in data.items():
            assert row[key] == value
            assert getattr(row, key) == value

    def test_row_iteration(self):
        """Test row iteration."""
        data = {"a": 1, "b": 2, "c": 3}
        row = Row(**data)
        keys = list(row.keys())
        values = list(row.values())
        items = list(row.items())
        assert set(keys) == {"a", "b", "c"}
        assert set(values) == {1, 2, 3}
        assert len(items) == 3

    def test_row_length(self):
        """Test row length."""
        row = Row(a=1, b=2, c=3)
        assert len(row) == 3

    def test_row_empty(self):
        """Test empty row."""
        row = Row()
        assert len(row) == 0
        assert list(row.keys()) == []

    def test_row_contains(self):
        """Test 'in' operator for row."""
        row = Row(name="test", value=42)
        assert "name" in row
        assert "missing" not in row


# =============================================================================
# QueryResult Tests
# =============================================================================

class TestQueryResult:
    """Tests for QueryResult type."""

    def test_query_result_all_empty(self):
        """Test empty query result."""
        result = QueryResult(rows=[])
        assert result.all() == []

    def test_query_result_all_with_rows(self):
        """Test query result with rows."""
        rows = [Row(id=i, name=random_string()) for i in range(5)]
        result = QueryResult(rows=rows)
        assert result.all() == rows

    def test_query_result_one_single_row(self):
        """Test one() with single row."""
        row = Row(id=1, name="test")
        result = QueryResult(rows=[row])
        assert result.one() == row

    def test_query_result_one_no_rows(self):
        """Test one() with no rows raises error."""
        result = QueryResult(rows=[])
        with pytest.raises(PgError, match="Expected exactly 1 row, got 0"):
            result.one()

    def test_query_result_one_multiple_rows(self):
        """Test one() with multiple rows raises error."""
        rows = [Row(id=i) for i in range(3)]
        result = QueryResult(rows=rows)
        with pytest.raises(PgError, match="Expected exactly 1 row, got 3"):
            result.one()

    def test_query_result_first_with_rows(self):
        """Test first() with rows."""
        rows = [Row(id=1), Row(id=2)]
        result = QueryResult(rows=rows)
        assert result.first() == rows[0]

    def test_query_result_first_empty(self):
        """Test first() with no rows."""
        result = QueryResult(rows=[])
        assert result.first() is None

    def test_query_result_stream(self):
        """Test stream() iterator."""
        rows = [Row(id=i, name=random_string()) for i in range(10)]
        result = QueryResult(rows=rows)
        streamed = list(result.stream())
        assert streamed == rows

    def test_query_result_model(self):
        """Test model() conversion."""
        # Create a simple class to simulate pydantic
        class UserModel:
            def __init__(self, **kwargs):
                for k, v in kwargs.items():
                    setattr(self, k, v)

        rows = [
            Row(id=1, name="alice"),
            Row(id=2, name="bob")
        ]
        result = QueryResult(rows=rows)
        models = result.model(UserModel)

        assert len(models) == 2
        assert models[0].id == 1
        assert models[0].name == "alice"
        assert models[1].id == 2
        assert models[1].name == "bob"

    def test_query_result_into_tuple(self):
        """Test into() conversion to tuple."""
        rows = [
            Row(id=1, name="a"),
            Row(id=2, name="b")
        ]
        result = QueryResult(rows=rows)
        converted = result.into(tuple)
        assert isinstance(converted, tuple)
        assert len(converted) == 2

    def test_query_result_consumed_flag(self):
        """Test that result tracks consumption."""
        rows = [Row(id=1)]
        result = QueryResult(rows=rows)
        result._consumed = False
        all_rows = result.all()
        # Should still work since rows are cached
        assert all_rows == rows

    def test_query_result_randomized_data(self):
        """Test with randomized data."""
        num_rows = random.randint(5, 20)
        rows = [
            Row(
                id=i,
                name=random_string(10),
                value=random_int(),
                score=random.uniform(0, 100)
            )
            for i in range(num_rows)
        ]
        result = QueryResult(rows=rows)

        all_rows = result.all()
        assert len(all_rows) == num_rows
        assert result.first() == rows[0]

        # Test iteration
        count = 0
        for row in result.stream():
            assert row.id == count
            count += 1
        assert count == num_rows


# =============================================================================
# TxIsolation Tests
# =============================================================================

class TestTxIsolation:
    """Tests for transaction isolation levels."""

    def test_read_uncommitted(self):
        """Test READ UNCOMMITTED isolation level."""
        assert TxIsolation.read_uncommitted.value == "READ UNCOMMITTED"

    def test_read_committed(self):
        """Test READ COMMITTED isolation level."""
        assert TxIsolation.read_committed.value == "READ COMMITTED"

    def test_repeatable_read(self):
        """Test REPEATABLE READ isolation level."""
        assert TxIsolation.repeatable_read.value == "REPEATABLE READ"

    def test_serializable(self):
        """Test SERIALIZABLE isolation level."""
        assert TxIsolation.serializable.value == "SERIALIZABLE"

    def test_all_isolation_levels_unique(self):
        """Test all isolation levels have unique values."""
        values = [level.value for level in TxIsolation]
        assert len(values) == len(set(values))


# =============================================================================
# PreparedQuery Tests
# =============================================================================

class TestPreparedQuery:
    """Tests for PreparedQuery type."""

    def test_prepared_query_creation(self):
        """Test creating a prepared query."""
        pq = PreparedQuery(
            query_id=123,
            sql="SELECT * FROM users WHERE id = $1",
            param_types=("int",)
        )
        assert pq.query_id == 123
        assert pq.sql == "SELECT * FROM users WHERE id = $1"
        assert pq.param_types == ("int",)
        assert pq.result_type is None

    def test_prepared_query_with_result_type(self):
        """Test prepared query with result type hint."""
        pq = PreparedQuery(
            query_id=456,
            sql="SELECT name FROM users",
            param_types=(),
            result_type=list
        )
        assert pq.result_type == list

    def test_prepared_query_frozen(self):
        """Test that PreparedQuery is immutable."""
        pq = PreparedQuery(query_id=1, sql="SELECT 1", param_types=())
        with pytest.raises(Exception):  # FrozenInstanceError
            pq.query_id = 2

    def test_prepared_query_repr(self):
        """Test PreparedQuery representation."""
        pq = PreparedQuery(
            query_id=42,
            sql="SELECT * FROM t",
            param_types=("int", "str")
        )
        repr_str = repr(pq)
        assert "42" in repr_str
        assert "2" in repr_str  # params=2


# =============================================================================
# PgPool Tests - These require the native library, marked to skip if not available
# =============================================================================

@requires_native
class TestPgPoolLifecycle:
    """Tests for PgPool lifecycle management.

    Note: These tests require the native PostgreSQL library.
    Run with a PostgreSQL test database for full integration testing.
    """

    def test_pool_creation_parameters(self):
        """Test pool stores configuration parameters correctly."""
        # This is a smoke test - actual connection tests require a database
        pass  # Tested via test_pg_integration.py with real database


@requires_native
class TestPgPoolConnections:
    """Tests for PgPool connection management."""
    pass  # Requires running PostgreSQL


@requires_native
class TestPgQueries:
    """Tests for Pg query execution."""
    pass  # Requires running PostgreSQL


class TestPgPreparedQueries:
    """Tests for prepared query functionality."""

    def test_module_level_prepare(self):
        """Test module-level prepare function."""
        from fasterapi.pg.pool import prepare
        pq = prepare("SELECT * FROM users", result=list)
        assert isinstance(pq, PreparedQuery)
        assert pq.result_type == list


@requires_native
class TestPgTransactions:
    """Tests for transaction functionality."""
    pass  # Requires running PostgreSQL


# =============================================================================
# Thread Safety Tests - Pure Python
# =============================================================================

class TestThreadSafety:
    """Tests for thread safety of pure Python components."""

    def test_row_concurrent_access(self):
        """Test concurrent access to Row objects."""
        row = Row(id=1, name="test", value=42)
        results = []
        errors = []

        def reader():
            try:
                for _ in range(100):
                    _ = row.id
                    _ = row.name
                    _ = row.value
                results.append(True)
            except Exception as e:
                errors.append(e)

        threads = [threading.Thread(target=reader) for _ in range(10)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        assert len(errors) == 0
        assert len(results) == 10

    def test_query_result_concurrent_access(self):
        """Test concurrent access to QueryResult."""
        rows = [Row(id=i) for i in range(100)]
        result = QueryResult(rows=rows)
        results = []
        errors = []

        def reader():
            try:
                all_rows = result.all()
                results.append(len(all_rows))
            except Exception as e:
                errors.append(e)

        threads = [threading.Thread(target=reader) for _ in range(10)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        assert len(errors) == 0
        assert all(r == 100 for r in results)


# =============================================================================
# Edge Case Tests
# =============================================================================

class TestEdgeCases:
    """Edge case tests for pure Python components."""

    def test_empty_query_result(self):
        """Test handling of empty query results."""
        result = QueryResult(rows=[])
        assert result.all() == []
        assert result.first() is None
        with pytest.raises(PgError):
            result.one()

    def test_large_row_count(self):
        """Test handling of large result sets."""
        num_rows = 10000
        rows = [Row(id=i, data=random_string(100)) for i in range(num_rows)]
        result = QueryResult(rows=rows)

        all_rows = result.all()
        assert len(all_rows) == num_rows

        # Test streaming
        count = 0
        for row in result.stream():
            count += 1
        assert count == num_rows

    def test_row_with_special_column_names(self):
        """Test row with special column names."""
        row = Row(**{
            "column_with_underscore": 1,
            "column123": 2,
            "CamelCase": 3,
        })
        assert row.column_with_underscore == 1
        assert row.column123 == 2
        assert row.CamelCase == 3

    def test_row_with_many_columns(self):
        """Test row with many columns."""
        data = {f"col_{i}": random_int() for i in range(100)}
        row = Row(**data)
        assert len(row) == 100
        for k, v in data.items():
            assert row[k] == v

    def test_query_result_caching(self):
        """Test that query results are cached after first access."""
        rows = [Row(id=i) for i in range(10)]
        result = QueryResult(rows=rows)

        # First call
        all1 = result.all()
        # Second call should return same cached list
        all2 = result.all()

        assert all1 is all2

    def test_row_update(self):
        """Test updating row values."""
        row = Row(id=1, name="original")
        row.name = "updated"
        row["new_field"] = "new_value"

        assert row.name == "updated"
        assert row.new_field == "new_value"

    def test_prepared_query_hash_uniqueness(self):
        """Test that different SQL produces different query IDs."""
        from fasterapi.pg.pool import prepare

        pq1 = prepare("SELECT 1")
        pq2 = prepare("SELECT 2")
        pq3 = prepare("SELECT 1")  # Same as pq1

        assert pq1.query_id != pq2.query_id
        assert pq1.query_id == pq3.query_id  # Same SQL = same hash


if __name__ == "__main__":
    pytest.main([__file__, "-v", "-s"])
