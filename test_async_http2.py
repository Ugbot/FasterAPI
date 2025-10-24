#!/usr/bin/env python3.13
"""
Test HTTP/2 server with async sub-interpreter execution
"""

import sys
sys.path.insert(0, 'fasterapi')

from fasterapi._native.http2 import PyHttp2Server
import time

def hello_handler(req, res):
    """Simple handler for testing"""
    print(f"Handler called: {req['method']} {req['path']}")
    res['status'] = 200
    res['content_type'] = 'text/plain'
    res['body'] = f"Hello from {req['path']}"

# Create server with sub-interpreter configuration
server = PyHttp2Server(
    port=8080,
    num_pinned_workers=2,      # 2 dedicated sub-interpreters
    num_pooled_workers=2,      # 2 additional pooled workers
    num_pooled_interpreters=1  # Sharing 1 interpreter
)

# Register routes
server.add_route("GET", "/", hello_handler)
server.add_route("GET", "/test", hello_handler)

print("Starting HTTP/2 server with async sub-interpreter execution...")
print("  - Pinned workers: 2 (dedicated sub-interpreters)")
print("  - Pooled workers: 2 (sharing 1 sub-interpreter)")
print("  - Total parallelism: Up to 3 concurrent Python executions")
print()
print("Test with:")
print("  curl --http2-prior-knowledge http://localhost:8080/")
print()

# Start server (non-blocking)
server.start(blocking=False)

# Keep running for 30 seconds
time.sleep(30)

print("\nShutting down...")
server.stop()
print("Done!")
