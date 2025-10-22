#!/usr/bin/env python3.13
"""
Test Cython-based CoroIO HTTP server.

This uses the high-performance Cython bindings directly.
"""

from fasterapi.http.server_cy import Server
import time
import signal
import sys

# Create server using Cython bindings
print("Creating server with Cython bindings...")
server = Server(
    port=8000,
    host="0.0.0.0",  # CoroIO requires IP address, not hostname
    enable_h2=False,
    enable_h3=False,
    enable_compression=True
)

# Define request handler
def hello_handler(request, response):
    """Simple handler that returns Hello World."""
    print(f"Handler called: {request}")
    response['status'] = 200
    response['content_type'] = 'text/plain'
    response['body'] = 'Hello from Cython CoroIO server!\n'

def test_handler(request, response):
    """Test handler for keep-alive testing."""
    response['status'] = 200
    response['content_type'] = 'text/plain'
    response['body'] = f"Request path: {request.get('path', 'unknown')}\n"

# Register routes
print("Registering routes...")
server.add_route("GET", "/", hello_handler)
server.add_route("GET", "/test", test_handler)

# Start server
print("Starting Cython HTTP server...")
server.start()

print(f"""
✓ Server started on http://localhost:8000

Test with:
  curl http://localhost:8000/
  curl http://localhost:8000/test
  curl -v http://localhost:8000/  # See keep-alive header

Benchmark:
  wrk -t4 -c100 -d10s http://localhost:8000/

Press Ctrl+C to stop
""")

# Signal handler for graceful shutdown
def signal_handler(sig, frame):
    print("\n🛑 Shutting down...")
    server.stop()
    sys.exit(0)

signal.signal(signal.SIGINT, signal_handler)

# Keep server running
try:
    while server.is_running():
        time.sleep(0.1)
except KeyboardInterrupt:
    print("\n🛑 Shutting down...")
    server.stop()
