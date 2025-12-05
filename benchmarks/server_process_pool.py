#!/usr/bin/env python3.13
"""
FasterAPI Benchmark Server with ProcessPoolExecutor

Unified server for benchmarking ProcessPoolExecutor performance with:
- 1MRC (1 Million Request Challenge) endpoints
- TechEmpower Framework Benchmark endpoints

Supports both Shared Memory and ZeroMQ IPC modes.

Environment Variables:
    FASTERAPI_WORKERS=N       - Number of worker processes (0=auto-detect cores)
    FASTERAPI_USE_ZMQ=1       - Enable ZeroMQ IPC (default: shared memory)
    FASTERAPI_PORT=8080       - Server port (default: 8080)
    FASTERAPI_LOG_LEVEL=ERROR - Logging level
"""

import sys
import os
import time

sys.path.insert(0, '/Users/bengamble/FasterAPI')

from fasterapi.fastapi_compat import FastAPI
from fasterapi.http.server import Server
from fasterapi._fastapi_native import connect_route_registry_to_server

# Import handlers from separate module to ensure worker processes can import them
from benchmarks import handlers_1mrc

# ============================================================================
# Configuration from Environment
# ============================================================================

NUM_WORKERS = int(os.environ.get('FASTERAPI_WORKERS', '0'))
USE_ZMQ = os.environ.get('FASTERAPI_USE_ZMQ', '0') == '1'
PORT = int(os.environ.get('FASTERAPI_PORT', '8080'))
LOG_LEVEL = os.environ.get('FASTERAPI_LOG_LEVEL', 'INFO')

# ============================================================================
# Application
# ============================================================================

app = FastAPI(
    title="FasterAPI ProcessPoolExecutor Benchmark Server",
    version="1.0.0"
)

# ============================================================================
# 1MRC Endpoints (1 Million Request Challenge)
# ============================================================================

# Register handlers directly from handlers_1mrc module (not wrapped)
app.post("/event", status_code=201)(handlers_1mrc.event)
app.get("/stats")(handlers_1mrc.get_stats)
app.post("/reset")(handlers_1mrc.reset_stats)

# ============================================================================
# TechEmpower Endpoints
# ============================================================================

app.get("/json")(handlers_1mrc.json_test)
app.get("/plaintext")(handlers_1mrc.plaintext_test)
app.get("/db")(handlers_1mrc.single_query)
app.get("/queries")(handlers_1mrc.multiple_queries)

# ============================================================================
# Additional Endpoints
# ============================================================================

app.get("/")(handlers_1mrc.root)
app.get("/health")(handlers_1mrc.health)

# ============================================================================
# Server Startup
# ============================================================================

def main():
    """Run the benchmark server."""
    print()
    print("=" * 70)
    print("FasterAPI ProcessPoolExecutor Benchmark Server")
    print("=" * 70)
    print()
    print(f"Configuration:")
    print(f"  IPC Mode:       {'ZeroMQ' if USE_ZMQ else 'Shared Memory'}")
    print(f"  Workers:        {NUM_WORKERS if NUM_WORKERS > 0 else 'auto-detect (CPU cores)'}")
    print(f"  Port:           {PORT}")
    print(f"  Log Level:      {LOG_LEVEL}")
    print()
    print(f"Benchmark Suites:")
    print(f"  1MRC:           POST /event, GET /stats, POST /reset")
    print(f"  TechEmpower:    GET /json, /plaintext, /db, /queries")
    print()

    # Connect route registry
    print("[1/2] Connecting RouteRegistry...")
    connect_route_registry_to_server()
    print("      ✅ Done")

    # Create server
    print("\n[2/2] Creating native C++ HTTP server...")
    server = Server(
        port=PORT,
        host="0.0.0.0",
        enable_h2=False,
        enable_h3=False,
        enable_compression=False  # Disable for fair benchmarking
    )
    print("      ✅ Done")
    print()
    print("=" * 70)
    print(f"🚀 Server running on http://0.0.0.0:{PORT}")
    print("=" * 70)
    print()

    if USE_ZMQ:
        print("⚡ Using ZeroMQ IPC for worker communication")
    else:
        print("⚡ Using Shared Memory IPC for worker communication")

    print()
    print("💡 Test 1MRC with:")
    print(f'   curl -X POST http://localhost:{PORT}/event \\')
    print('     -H "Content-Type: application/json" \\')
    print('     -d \'{"userId": "user_123", "value": 42.5}\'')
    print(f'   curl http://localhost:{PORT}/stats')
    print()
    print("💡 Test TechEmpower with:")
    print(f'   curl http://localhost:{PORT}/json')
    print(f'   curl http://localhost:{PORT}/plaintext')
    print(f'   curl http://localhost:{PORT}/db')
    print(f'   curl "http://localhost:{PORT}/queries?queries=20"')
    print()
    print("💡 Benchmark with wrk:")
    print(f'   wrk -t4 -c100 -d30s http://localhost:{PORT}/json')
    print(f'   wrk -t8 -c500 -d60s http://localhost:{PORT}/json')
    print()
    print("💡 Run 1MRC client:")
    print(f'   python3.13 benchmarks/1mrc/client/1mrc_client.py 1000000 1000')
    print()
    print("Press Ctrl+C to stop")
    print("=" * 70)
    print()

    # Start server
    server.start()

    try:
        while server.is_running():
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n\n🛑 Stopping server...")
        server.stop()
        print("✅ Server stopped\n")

if __name__ == "__main__":
    main()
