#!/usr/bin/env python3
"""Simple test of lockfree HTTP server."""

from fasterapi import App, Request, Response
import time

app = App(port=8000, host="localhost")

@app.get("/")
def root(request: Request, response: Response):
    response.status = 200
    response.content_type = "text/plain"
    response.body = "Hello from lockfree server!"

@app.get("/test")
def test(request: Request, response: Response):
    response.status = 200
    response.content_type = "text/plain"
    response.body = "Keep-alive test"

if __name__ == "__main__":
    print("Starting lockfree HTTP server on http://localhost:8000")
    print("  - Lockfree handler registration via Aeron SPSC queues")
    print("  - HTTP/1.1 keep-alive support")
    print("  - Connection timeout protection")
    print("  - Graceful shutdown via atomic flags")
    print()
    print("Test with: curl http://localhost:8000/")
    print("Keep-alive test: curl -v http://localhost:8000/test")
    print()

    try:
        app.run()
    except KeyboardInterrupt:
        print("\n✓ Server shut down cleanly")
