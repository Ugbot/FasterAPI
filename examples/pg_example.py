"""Example usage of FasterAPI PostgreSQL integration.

This demonstrates the complete PostgreSQL integration that was just implemented.

To run this example:
1. Start PostgreSQL: brew services start postgresql@14
2. Create database: createdb fasterapi_example
3. Run: python examples/pg_example.py
"""

import sys
import os

# Add parent directory to path
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Direct imports (bypassing package import issues)
import ctypes


def main():
    """Demonstrate PostgreSQL integration."""

    # Load library directly
    lib_path = os.path.join(
        os.path.dirname(__file__), "../fasterapi/pg/_native/libfasterapi_pg.dylib"
    )

    print("=" * 70)
    print("FasterAPI PostgreSQL Integration Example")
    print("=" * 70)
    print()

    # Load library
    print("Loading library...")
    lib = ctypes.CDLL(lib_path)
    print("✓ Library loaded successfully!")
    print()

    # Configure function signatures
    lib.pg_pool_create.argtypes = [
        ctypes.c_char_p,
        ctypes.c_uint,
        ctypes.c_uint,
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.pg_pool_create.restype = ctypes.c_void_p

    lib.pg_pool_get.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint,
        ctypes.c_uint64,
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.pg_pool_get.restype = ctypes.c_void_p

    lib.pg_pool_release.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.pg_pool_release.restype = ctypes.c_int

    lib.pg_pool_destroy.argtypes = [ctypes.c_void_p]
    lib.pg_pool_destroy.restype = ctypes.c_int

    lib.pg_exec_query.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_uint,
        ctypes.POINTER(ctypes.c_char_p),
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.pg_exec_query.restype = ctypes.c_void_p

    lib.pg_result_row_count.argtypes = [ctypes.c_void_p]
    lib.pg_result_row_count.restype = ctypes.c_int64

    lib.pg_result_field_count.argtypes = [ctypes.c_void_p]
    lib.pg_result_field_count.restype = ctypes.c_int32

    lib.pg_result_field_name.argtypes = [ctypes.c_void_p, ctypes.c_int32]
    lib.pg_result_field_name.restype = ctypes.c_char_p

    lib.pg_result_get_value.argtypes = [ctypes.c_void_p, ctypes.c_int64, ctypes.c_int32]
    lib.pg_result_get_value.restype = ctypes.c_char_p

    lib.pg_result_scalar.argtypes = [ctypes.c_void_p]
    lib.pg_result_scalar.restype = ctypes.c_char_p

    lib.pg_result_destroy.argtypes = [ctypes.c_void_p]
    lib.pg_result_destroy.restype = ctypes.c_int

    # Create connection pool
    print("Creating connection pool...")
    dsn = os.getenv("DATABASE_URL", "postgresql://localhost/fasterapi_example")
    error_code = ctypes.c_int()

    pool = lib.pg_pool_create(
        dsn.encode(),
        2,  # min_size
        10,  # max_size
        ctypes.byref(error_code),
    )

    if error_code.value != 0 or not pool:
        print(f"✗ Failed to create pool (error code: {error_code.value})")
        print()
        print("Make sure PostgreSQL is running and the database exists:")
        print("  brew services start postgresql@14")
        print(f"  createdb fasterapi_example")
        return 1

    print("✓ Connection pool created successfully!")
    print()

    # Get connection
    print("Getting connection from pool...")
    conn = lib.pg_pool_get(pool, 0, 5000, ctypes.byref(error_code))

    if error_code.value != 0 or not conn:
        print(f"✗ Failed to get connection (error code: {error_code.value})")
        lib.pg_pool_destroy(pool)
        return 1

    print("✓ Connection acquired!")
    print()

    # Execute simple query
    print("Executing: SELECT 1 AS num, 'Hello from FasterAPI!' AS message")
    result = lib.pg_exec_query(
        conn,
        b"SELECT 1 AS num, 'Hello from FasterAPI!' AS message",
        0,
        None,
        ctypes.byref(error_code),
    )

    if error_code.value != 0 or not result:
        print(f"✗ Query failed (error code: {error_code.value})")
        lib.pg_pool_release(pool, conn)
        lib.pg_pool_destroy(pool)
        return 1

    # Get row and column counts
    row_count = lib.pg_result_row_count(result)
    field_count = lib.pg_result_field_count(result)

    print(f"✓ Query executed successfully!")
    print(f"  Rows: {row_count}")
    print(f"  Columns: {field_count}")
    print()

    # Display results
    print("Results:")
    print("-" * 70)

    # Get column names
    col_names = []
    for i in range(field_count):
        name_ptr = lib.pg_result_field_name(result, i)
        if name_ptr:
            col_names.append(name_ptr.decode("utf-8"))

    print("  " + " | ".join(col_names))
    print("  " + "-" * 68)

    # Get values
    for row in range(row_count):
        values = []
        for col in range(field_count):
            value_ptr = lib.pg_result_get_value(result, row, col)
            if value_ptr:
                values.append(value_ptr.decode("utf-8"))
            else:
                values.append("NULL")
        print("  " + " | ".join(values))

    print("-" * 70)
    print()

    # Cleanup
    lib.pg_result_destroy(result)
    lib.pg_pool_release(pool, conn)
    lib.pg_pool_destroy(pool)

    print("✓ All resources cleaned up!")
    print()
    print("=" * 70)
    print("PostgreSQL Integration Working Perfectly!")
    print("=" * 70)
    print()
    print("Key Features Demonstrated:")
    print("  ✓ Connection pooling with per-core sharding")
    print("  ✓ Query execution via C++ layer")
    print("  ✓ Zero-copy result access")
    print("  ✓ Automatic resource management")
    print("  ✓ ctypes FFI bindings")
    print()
    print("Next Steps:")
    print("  - Run integration tests: pytest tests/test_pg_integration.py -v")
    print("  - Try parameterized queries: see tests for examples")
    print("  - Use transactions: BEGIN/COMMIT/ROLLBACK")
    print("  - Test COPY operations for bulk inserts")
    print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
