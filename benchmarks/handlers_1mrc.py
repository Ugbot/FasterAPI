#!/usr/bin/env python3.13
"""
1MRC and TechEmpower benchmark handlers.

Separate module to ensure workers can import handlers correctly.
"""

import random
from threading import Lock

# ============================================================================
# 1MRC Shared State
# ============================================================================

# Thread-safe stats storage
stats_lock = Lock()
stats = {
    'total': 0,
    'sum': 0.0,
    'users': set()
}

# ============================================================================
# 1MRC Handlers
# ============================================================================

def event(userId: str, value: float):
    """
    1MRC Event Endpoint

    Accepts event data and aggregates statistics.
    POST /event with JSON body: {"userId": "user_12345", "value": 499.5}
    """
    with stats_lock:
        stats['total'] += 1
        stats['sum'] += value
        stats['users'].add(userId)

    return {"status": "ok"}

def get_stats():
    """
    1MRC Stats Endpoint

    Returns aggregated statistics from all received events.
    """
    with stats_lock:
        return {
            "totalRequests": stats['total'],
            "uniqueUsers": len(stats['users']),
            "sum": stats['sum'],
            "avg": stats['sum'] / stats['total'] if stats['total'] > 0 else 0.0
        }

def reset_stats():
    """
    1MRC Reset Endpoint

    Resets all statistics to zero.
    """
    with stats_lock:
        stats['total'] = 0
        stats['sum'] = 0.0
        stats['users'].clear()

    return {"status": "reset"}

# ============================================================================
# TechEmpower Handlers
# ============================================================================

class FakeDB:
    """Simulates database for TechEmpower benchmarks."""

    def get_world(self, id: int):
        """Get a single world row."""
        return {
            "id": id,
            "randomNumber": random.randint(1, 10000)
        }

    def get_worlds(self, count: int):
        """Get multiple world rows."""
        return [self.get_world(random.randint(1, 10000)) for _ in range(count)]

db = FakeDB()

def json_test():
    """
    TechEmpower Test 1: JSON Serialization

    Returns a simple JSON object.
    Tests framework's JSON serialization performance.
    """
    return {"message": "Hello, World!"}

def plaintext_test():
    """
    TechEmpower Test 6: Plaintext

    Returns simple plaintext response.
    Tests framework's absolute minimum overhead.
    """
    return "Hello, World!"

def single_query():
    """
    TechEmpower Test 2: Single Database Query

    Fetches a single row from database (simulated).
    Tests framework's database integration.
    """
    world = db.get_world(random.randint(1, 10000))
    return world

def multiple_queries(queries: int = 1):
    """
    TechEmpower Test 3: Multiple Database Queries

    Fetches N rows from database (N from query param, 1-500).
    Tests framework's ability to handle multiple queries.
    """
    # Clamp to spec range
    count = max(1, min(500, queries))
    worlds = db.get_worlds(count)
    return worlds

# ============================================================================
# Utility Handlers
# ============================================================================

def root():
    """Root endpoint with server information."""
    import os
    num_workers = int(os.environ.get('FASTERAPI_WORKERS', '0'))
    use_zmq = os.environ.get('FASTERAPI_USE_ZMQ', '0') == '1'

    return {
        "name": "FasterAPI ProcessPoolExecutor Benchmark Server",
        "version": "1.0.0",
        "backend": "Native C++ HTTP Server",
        "executor": "ProcessPoolExecutor",
        "ipc_mode": "ZeroMQ" if use_zmq else "Shared Memory",
        "workers": num_workers if num_workers > 0 else "auto-detect",
        "endpoints": {
            "1mrc": {
                "POST /event": "Accept event data",
                "GET /stats": "Get statistics",
                "POST /reset": "Reset statistics"
            },
            "techempower": {
                "GET /json": "JSON serialization test",
                "GET /plaintext": "Plaintext response",
                "GET /db": "Single database query",
                "GET /queries?queries=N": "Multiple queries (N=1-500)"
            }
        }
    }

def health():
    """Health check endpoint."""
    import os
    num_workers = int(os.environ.get('FASTERAPI_WORKERS', '0'))
    return {"status": "ok", "workers": num_workers}
