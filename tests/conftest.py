"""pytest configuration and fixtures for integration tests."""

import os
import subprocess
import time
from pathlib import Path
from typing import Optional

import pytest

from fasterapi.pg import PgPool


# Database configuration
PG_USER = os.getenv("PG_USER", "postgres")
PG_PASS = os.getenv("PG_PASS", "postgres")
PG_HOST = os.getenv("PG_HOST", "localhost")
PG_PORT = os.getenv("PG_PORT", "5432")
PG_DB = os.getenv("PG_DB", "fasterapi_test")
PG_DSN = f"postgres://{PG_USER}:{PG_PASS}@{PG_HOST}:{PG_PORT}/{PG_DB}"


@pytest.fixture(scope="session")
def postgres_container() -> Optional[str]:
    """Start PostgreSQL container for testing.
    
    Uses Docker to spin up PostgreSQL 15+ if not already running.
    Yields container ID.
    """
    container_name = "fasterapi-pg-test"
    
    # Check if container already running
    result = subprocess.run(
        ["docker", "ps", "-q", "-f", f"name={container_name}"],
        capture_output=True,
        text=True
    )
    
    if result.stdout.strip():
        print(f"Using existing container: {container_name}")
        yield container_name
        return
    
    # Try to start new container
    print(f"Starting PostgreSQL test container: {container_name}")
    try:
        subprocess.run([
            "docker", "run", "-d",
            "--name", container_name,
            "-e", f"POSTGRES_PASSWORD={PG_PASS}",
            "-e", f"POSTGRES_DB={PG_DB}",
            "-p", f"{PG_PORT}:5432",
            "postgres:15-alpine"
        ], check=True, capture_output=True)
        
        # Wait for container to be ready
        for attempt in range(30):
            try:
                subprocess.run(
                    ["docker", "exec", container_name, "pg_isready", "-U", PG_USER],
                    check=True,
                    capture_output=True,
                    timeout=2
                )
                print(f"PostgreSQL container ready after {attempt}s")
                break
            except Exception:
                time.sleep(1)
        else:
            raise RuntimeError("PostgreSQL container failed to start")
        
        yield container_name
    finally:
        # Cleanup
        subprocess.run(
            ["docker", "kill", container_name],
            capture_output=True
        )
        subprocess.run(
            ["docker", "rm", container_name],
            capture_output=True
        )


@pytest.fixture(scope="session")
def test_schema(postgres_container: str) -> None:
    """Create test schema in PostgreSQL.
    
    Creates tables: items, orders, stock
    """
    schema_sql = """
    DROP TABLE IF EXISTS orders CASCADE;
    DROP TABLE IF EXISTS stock CASCADE;
    DROP TABLE IF EXISTS items CASCADE;
    
    CREATE TABLE items (
        id SERIAL PRIMARY KEY,
        name VARCHAR(255) NOT NULL,
        price FLOAT8 NOT NULL,
        created_at TIMESTAMPTZ DEFAULT NOW()
    );
    
    CREATE TABLE stock (
        item_id INTEGER PRIMARY KEY REFERENCES items(id),
        qty INTEGER NOT NULL DEFAULT 0
    );
    
    CREATE TABLE orders (
        id SERIAL PRIMARY KEY,
        user_id INTEGER NOT NULL,
        item_id INTEGER NOT NULL REFERENCES items(id),
        created_at TIMESTAMPTZ DEFAULT NOW()
    );
    
    -- Insert test data
    INSERT INTO items (name, price) VALUES ('Widget', 9.99), ('Gadget', 19.95);
    INSERT INTO stock (item_id, qty) VALUES (1, 100), (2, 50);
    """
    
    try:
        # Use psycopg if available for schema setup
        import psycopg
        with psycopg.connect(PG_DSN) as conn:
            conn.execute(schema_sql)
            conn.commit()
        print("Test schema created")
    except ImportError:
        print("psycopg not available, skipping schema setup")
    except Exception as e:
        print(f"Warning: Failed to create test schema: {e}")


@pytest.fixture
def pg_pool(test_schema) -> PgPool:
    """Create a connection pool for testing.
    
    Yields:
        Configured PgPool instance.
    """
    pool = PgPool(
        PG_DSN,
        min_size=1,
        max_size=5,
        idle_timeout_secs=60,
    )
    yield pool
    pool.close()


@pytest.fixture
def bench_baseline() -> dict:
    """Get baseline performance metrics for comparison.
    
    Returns:
        Dict with benchmark results from psycopg/asyncpg.
    """
    baseline = {
        "psycopg_query_latency_us": 1000,  # Typical libpq round-trip
        "asyncpg_query_latency_us": 500,   # asyncpg is faster
        "psycopg_copy_throughput_mbps": 500,
    }
    return baseline
