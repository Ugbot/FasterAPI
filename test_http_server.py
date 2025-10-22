#!/usr/bin/env python3
"""
Simple test script to verify HTTP server is working with CoroIO
"""
import sys
import time
sys.path.insert(0, '/Users/bengamble/FasterAPI')

from fasterapi import App

app = App(port=8000, host="0.0.0.0")

@app.get("/")
def read_root(req, res):
    return {"message": "Hello from FasterAPI with CoroIO!"}

@app.get("/health")
def health_check(req, res):
    return {"status": "healthy", "timestamp": time.time()}

@app.get("/benchmark")
def benchmark(req, res):
    """Minimal benchmark endpoint"""
    return {"hello": "world"}

if __name__ == "__main__":
    print("=" * 60)
    print("Starting FasterAPI server with CoroIO on http://0.0.0.0:8000")
    print("=" * 60)
    print("Test endpoints:")
    print("  curl http://localhost:8000/")
    print("  curl http://localhost:8000/health")
    print("  curl http://localhost:8000/benchmark")
    print("=" * 60)
    print()

    # This will start the actual CoroIO-based HTTP server
    app.run()
