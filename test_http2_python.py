#!/usr/bin/env python3
"""
Test HTTP/2 Server with Python handlers

This tests the native HTTP/2 server with Python route handlers via Cython bindings.
"""

import sys
import json

# Import the Cython-wrapped HTTP/2 server
try:
    from fasterapi._native.http2 import PyHttp2Server
except ImportError as e:
    print(f"Error importing HTTP/2 module: {e}")
    print("Make sure to build with: cmake --build build -j12")
    sys.exit(1)


def hello_handler(request, response):
    """Simple hello world handler"""
    response['status'] = 200
    response['content_type'] = 'text/plain'
    response['body'] = 'Hello from FasterAPI HTTP/2 with Python!'


def json_handler(request, response):
    """JSON response handler"""
    data = {
        'message': 'Success',
        'method': request['method'],
        'path': request['path'],
        'server': 'FasterAPI HTTP/2'
    }
    response['status'] = 200
    response['content_type'] = 'application/json'
    response['body'] = json.dumps(data)


def echo_handler(request, response):
    """Echo the request body"""
    body = request.get('body', '')
    response['status'] = 200
    response['content_type'] = 'text/plain'
    response['body'] = f"Echo: {body}" if body else "No body received"


def headers_handler(request, response):
    """Return request headers as JSON"""
    headers = request.get('headers', {})
    data = {
        'headers': headers,
        'count': len(headers)
    }
    response['status'] = 200
    response['content_type'] = 'application/json'
    response['body'] = json.dumps(data, indent=2)


def main():
    # Create HTTP/2 server
    port = 8080
    workers = 12

    print(f"Creating HTTP/2 server on port {port} with {workers} workers...")
    server = PyHttp2Server(port=port, num_pinned_workers=workers)

    # Register routes
    print("Registering routes...")
    server.add_route("GET", "/", hello_handler)
    server.add_route("GET", "/json", json_handler)
    server.add_route("POST", "/echo", echo_handler)
    server.add_route("GET", "/headers", headers_handler)

    print(f"\nServer info: {server}")
    print("\nRoutes registered:")
    print("  GET  / - Hello world")
    print("  GET  /json - JSON response")
    print("  POST /echo - Echo request body")
    print("  GET  /headers - Show request headers")

    print(f"\nStarting HTTP/2 server on http://localhost:{port}/")
    print("Test with: curl --http2-prior-knowledge http://localhost:8080/")
    print("Press Ctrl+C to stop\n")

    try:
        # Start server (blocks)
        result = server.start(blocking=True)
        if result != 0:
            print(f"Server failed to start: {result}")
            sys.exit(1)
    except KeyboardInterrupt:
        print("\nShutting down...")
        server.stop()
        print("Server stopped.")


if __name__ == "__main__":
    main()
