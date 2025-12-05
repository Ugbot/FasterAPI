#!/usr/bin/env python3.13
"""
TechEmpower Framework Benchmarks for FasterAPI
Using FastAPI-compatible syntax on native C++ server

Implements standard TechEmpower test types:
1. JSON Serialization - Return a JSON object
2. Single Database Query - Fetch a single row (simulated)
3. Multiple Database Queries - Fetch N rows (simulated)
4. Plaintext - Return plaintext response

Reference: https://github.com/TechEmpower/FrameworkBenchmarks
"""

import sys
import random
import time
sys.path.insert(0, '/Users/bengamble/FasterAPI')

from fasterapi.fastapi_compat import FastAPI
from fasterapi.http.server import Server
from fasterapi._fastapi_native import connect_route_registry_to_server

# ============================================================================
# TechEmpower Test App
# ============================================================================

app = FastAPI(
    title="FasterAPI TechEmpower Benchmarks",
    version="1.0.0"
)

# Simulated database
class FakeDB:
    """Simulates database for benchmarking."""

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

# ============================================================================
# Test 1: JSON Serialization
# ============================================================================

@app.get("/json")
def json_test():
    """
    Test type 1: JSON serialization

    Returns a simple JSON object.
    Tests framework's JSON serialization performance.
    """
    return {"message": "Hello, World!"}


# ============================================================================
# Test 2: Single Database Query
# ============================================================================

@app.get("/db")
def single_query():
    """
    Test type 2: Single database query

    Fetches a single row from database (simulated).
    Tests framework's database integration.
    """
    world = db.get_world(random.randint(1, 10000))
    return world


# ============================================================================
# Test 3: Multiple Database Queries
# ============================================================================

@app.get("/queries")
def multiple_queries(queries: int = 1):
    """
    Test type 3: Multiple database queries

    Fetches N rows from database (N from query param, 1-500).
    Tests framework's ability to handle multiple queries.
    """
    # Clamp to spec range
    count = max(1, min(500, queries))

    # Fetch worlds
    worlds = db.get_worlds(count)

    return worlds


# ============================================================================
# Test 6: Plaintext
# ============================================================================

@app.get("/plaintext")
def plaintext_test():
    """
    Test type 6: Plaintext

    Returns simple plaintext response.
    Tests framework's absolute minimum overhead.
    """
    return "Hello, World!"


# ============================================================================
# Additional endpoints for testing
# ============================================================================

@app.get("/")
def root():
    """Root endpoint with benchmark info"""
    return {
        "name": "FasterAPI TechEmpower Benchmarks",
        "version": "1.0.0",
        "backend": "Native C++ HTTP Server",
        "endpoints": {
            "/json": "JSON serialization test",
            "/db": "Single database query",
            "/queries?queries=N": "Multiple queries (N=1-500)",
            "/plaintext": "Plaintext response"
        }
    }


# ============================================================================
# Server Startup
# ============================================================================

def main():
    """Run TechEmpower benchmark server."""
    print()
    print("╔══════════════════════════════════════════════════════════╗")
    print("║   FasterAPI - TechEmpower Benchmark Server (FastAPI)    ║")
    print("╚══════════════════════════════════════════════════════════╝")
    print()
    print("Backend: Native C++ HTTP Server with FastAPI compatibility")
    print()

    # Connect route registry
    print("[1/3] Connecting RouteRegistry...")
    connect_route_registry_to_server()
    print("      ✅ Done")

    # Create server
    print("\n[2/3] Creating native C++ HTTP server...")
    server = Server(
        port=8080,
        host="0.0.0.0",
        enable_h2=False,
        enable_h3=False,
        enable_compression=False  # Disable for fair benchmarking
    )
    print("      ✅ Done")

    # Start server
    print("\n[3/3] Starting server...")
    server.start()

    print()
    print("╔══════════════════════════════════════════════════════════╗")
    print("║  🚀 Server running on http://0.0.0.0:8080                ║")
    print("╚══════════════════════════════════════════════════════════╝")
    print()
    print("📊 TechEmpower Test Endpoints:")
    print("   GET /json              - JSON serialization")
    print("   GET /db                - Single query")
    print("   GET /queries?queries=N - Multiple queries (N=1-500)")
    print("   GET /plaintext         - Plaintext")
    print()
    print("💡 Test with curl:")
    print('   curl "http://localhost:8080/json"')
    print('   curl "http://localhost:8080/db"')
    print('   curl "http://localhost:8080/queries?queries=20"')
    print('   curl "http://localhost:8080/plaintext"')
    print()
    print("💡 Benchmark with wrk:")
    print("   wrk -t4 -c64 -d10s http://localhost:8080/json")
    print("   wrk -t4 -c64 -d10s http://localhost:8080/plaintext")
    print()
    print("Press Ctrl+C to stop")
    print("╚══════════════════════════════════════════════════════════╝")
    print()

    try:
        while server.is_running():
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n\n🛑 Stopping server...")
        server.stop()
        print("✅ Server stopped\n")


if __name__ == "__main__":
    main()
