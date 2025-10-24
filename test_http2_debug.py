#!/usr/bin/env python3.13
"""Test HTTP/2 server with debug logging"""

import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from fasterapi._native.http2 import PyHttp2Server

def hello_handler(method, path, headers, body):
    return {
        "status": 200,
        "headers": {"content-type": "text/plain"},
        "body": "Hello from HTTP/2!\n"
    }

def main():
    server = PyHttp2Server(port=8080, num_pinned_workers=2)

    # Register handlers
    server.register_handler("GET", "/", hello_handler)

    print("[Python] Starting HTTP/2 server on 0.0.0.0:8080")
    print("[Python] Press Ctrl+C to stop")
    print("[Python] Test with: curl --http2-prior-knowledge http://localhost:8080/")

    try:
        server.start()
    except KeyboardInterrupt:
        print("\n[Python] Stopping server...")
        server.stop()

if __name__ == "__main__":
    main()
