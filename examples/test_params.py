#!/usr/bin/env python3.13
"""
Test server with path and query parameters.
"""

import sys
import os
import time

# Add parent directory to path
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Set library path for dynamic loading
os.environ['DYLD_LIBRARY_PATH'] = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    'build', 'lib'
) + ':' + os.environ.get('DYLD_LIBRARY_PATH', '')

from fasterapi.fastapi_compat import FastAPI
from fasterapi.http.server import Server
from fasterapi._fastapi_native import connect_route_registry_to_server
from typing import Optional

# Create FastAPI app
app = FastAPI(title="Parameter Test API", version="1.0.0")

# Simple route without parameters
@app.get("/")
def read_root():
    return {"message": "Parameter test server"}

# Route with path parameter
@app.get("/items/{item_id}")
def get_item(item_id: int):
    return {"item_id": item_id, "type": type(item_id).__name__}

# Route with query parameter
@app.get("/search")
def search(q: str, limit: int = 10):
    return {"query": q, "limit": limit, "limit_type": type(limit).__name__}

# Route with both path and query parameters
@app.get("/users/{user_id}/posts")
def get_user_posts(user_id: int, page: int = 1, size: int = 20):
    return {
        "user_id": user_id,
        "page": page,
        "size": size,
        "types": {
            "user_id": type(user_id).__name__,
            "page": type(page).__name__,
            "size": type(size).__name__
        }
    }

if __name__ == "__main__":
    print("="*80)
    print("Parameter Test Server")
    print("="*80)

    # Connect RouteRegistry to server for parameter extraction
    connect_route_registry_to_server()

    # Create server
    server = Server(
        port=8000,
        host="0.0.0.0",
        enable_h2=False,
        enable_h3=False
    )

    print(f"\n✅ Server initialized")
    print(f"✅ Routes registered with parameters")
    print(f"✅ RouteRegistry connected")

    # Start server
    server.start()

    print(f"\n🚀 Server running on http://0.0.0.0:8000")
    print("\nTest URLs:")
    print("  GET /")
    print("  GET /items/123")
    print("  GET /search?q=test&limit=5")
    print("  GET /users/42/posts?page=2&size=10")
    print("="*80)
    print("Press Ctrl+C to stop\n")

    try:
        while server.is_running():
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n\n🛑 Shutting down...")
        server.stop()
        print("✅ Stopped")
