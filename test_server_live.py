#!/usr/bin/env python3.13
"""Test the Cython HTTP server with actual requests"""
import sys
import time
import importlib.util

# Load Cython server module directly
spec = importlib.util.spec_from_file_location("server_cy", "fasterapi/http/server_cy.cpython-313-darwin.so")
server_cy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(server_cy)

print("=" * 70)
print("FasterAPI + CoroIO + Cython HTTP Server - Live Test")
print("=" * 70)
print()

# Create server
print("Creating HTTP server on port 8000...")
server = server_cy.Server(port=8000, host="0.0.0.0")
print(f"✓ Server created!")
print()

# Add routes (note: callbacks not yet implemented, but we can test the server starts)
print("Adding routes...")
server.add_route("GET", "/", lambda req, res: None)
server.add_route("GET", "/health", lambda req, res: None)
print("✓ Routes added")
print()

# Start server
print("Starting server...")
server.start()
print(f"✓ Server started!")
print(f"  Running: {server.is_running()}")
print()

print("=" * 70)
print("Server is running on http://0.0.0.0:8000")
print("Test with: curl http://localhost:8000/")
print("Press Ctrl+C to stop...")
print("=" * 70)
print()

try:
    while server.is_running():
        time.sleep(0.5)
        stats = server.get_stats()
        if stats['total_requests'] > 0:
            print(f"\rRequests: {stats['total_requests']}, "
                  f"Connections: {stats['active_connections']}, "
                  f"Bytes sent: {stats['total_bytes_sent']}", end='', flush=True)
except KeyboardInterrupt:
    print("\n\nShutting down...")
    server.stop()
    print("✓ Server stopped")
