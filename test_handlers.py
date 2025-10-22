#!/usr/bin/env python3.13
"""Test HTTP server with real Python handlers"""
import importlib.util
import time

# Load Cython server module
spec = importlib.util.spec_from_file_location("server_cy",
    "fasterapi/http/server_cy.cpython-313-darwin.so")
server_cy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(server_cy)

print("=" * 70)
print("FasterAPI HTTP Server - Python Handler Test")
print("=" * 70)
print()

# Define Python handlers
def home_handler(request, response):
    """Handle GET /"""
    print(f"🐍 Python handler called: {request['method']} {request['path']}")
    response['status'] = 200
    response['content_type'] = 'application/json'
    response['body'] = '{"message": "Hello from Python!", "path": "/"}'

def health_handler(request, response):
    """Handle GET /health"""
    print(f"🐍 Python handler called: {request['method']} {request['path']}")
    response['status'] = 200
    response['content_type'] = 'application/json'
    response['body'] = '{"status": "healthy", "server": "FasterAPI + CoroIO + Cython"}'

def echo_handler(request, response):
    """Handle POST /echo - echo back the request body"""
    print(f"🐍 Python handler called: {request['method']} {request['path']}")
    body = request.get('body', '')
    response['status'] = 200
    response['content_type'] = 'application/json'
    response['body'] = f'{{"echo": "{body}", "length": {len(body)}}}'

# Create server
print("Creating HTTP server on port 8000...")
server = server_cy.Server(port=8000, host="0.0.0.0")
print("✓ Server created!")
print()

# Add routes with Python handlers
print("Registering Python handlers...")
server.add_route("GET", "/", home_handler)
server.add_route("GET", "/health", health_handler)
server.add_route("POST", "/echo", echo_handler)
print()

# Start server
print("Starting server...")
server.start()
print(f"✓ Server running: {server.is_running()}")
print()

print("=" * 70)
print("Server is ready at http://0.0.0.0:8000")
print()
print("Try these curl commands:")
print("  curl http://localhost:8000/")
print("  curl http://localhost:8000/health")
print("  curl -X POST http://localhost:8000/echo -d 'Hello, World!'")
print()
print("Press Ctrl+C to stop...")
print("=" * 70)
print()

try:
    while server.is_running():
        time.sleep(1)
except KeyboardInterrupt:
    print("\n\nShutting down...")
    server.stop()
    print("✓ Server stopped")
