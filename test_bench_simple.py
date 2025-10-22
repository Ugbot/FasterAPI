#!/usr/bin/env python3
"""
Simple FasterAPI performance benchmark
Tests JSON serialization and routing performance
"""

import sys
import time
import json

sys.path.insert(0, '/Users/bengamble/FasterAPI')

from fasterapi import App

# Create app
app = App(port=8000)

# Simple JSON endpoint
@app.get("/json")
def get_json(req, res):
    return {"message": "Hello, World!", "timestamp": time.time()}

# Plaintext endpoint
@app.get("/plaintext")
def get_plaintext(req, res):
    return "Hello, World!"

# Route with path parameter
@app.get("/users/{user_id}")
def get_user(req, res):
    user_id = req.path_params.get("user_id")
    return {"id": user_id, "name": f"User {user_id}"}

# Health check
@app.get("/health")
def health(req, res):
    return {"status": "ok"}

if __name__ == "__main__":
    print("="*60)
    print("FasterAPI Simple Benchmark Server")
    print("="*60)
    print()
    print("🚀 Server: http://localhost:8000")
    print()
    print("📊 Endpoints:")
    print("  GET /json          - JSON serialization")
    print("  GET /plaintext     - Plaintext response")
    print("  GET /users/{id}    - Path parameters")
    print("  GET /health        - Health check")
    print()
    print("💡 Test with:")
    print("  curl http://localhost:8000/json")
    print("  curl http://localhost:8000/users/123")
    print()
    print("="*60)
    print()

    try:
        app.run()
    except KeyboardInterrupt:
        print("\n👋 Server stopped")
