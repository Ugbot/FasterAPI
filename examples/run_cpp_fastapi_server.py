#!/usr/bin/env python3.13
"""
Run FastAPI example with the C++ HTTP server backend.

This demonstrates FasterAPI with the high-performance C++ HTTP server,
providing 10-50x better performance than standard FastAPI.

NOTE: When run as __main__, handlers must be at module level so workers
can import them. Don't define them inside if __name__ == "__main__".
"""

import sys
import os
import signal

# Add parent directory to path
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Set library path for dynamic loading
os.environ['DYLD_LIBRARY_PATH'] = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    'build', 'lib'
) + ':' + os.environ.get('DYLD_LIBRARY_PATH', '')

from fasterapi.fastapi_compat import FastAPI
from fasterapi.http.server import Server
from pydantic import BaseModel
from typing import List, Optional
import json
import time

# Import native bindings
try:
    from fasterapi._fastapi_native import connect_route_registry_to_server
    HAS_NATIVE = True
except ImportError:
    HAS_NATIVE = False
    connect_route_registry_to_server = None

# ============================================================================
# Models
# ============================================================================

class Item(BaseModel):
    """Item model."""
    name: str
    description: Optional[str] = None
    price: float
    tax: Optional[float] = None


class User(BaseModel):
    """User model."""
    id: int
    username: str
    email: str


# ============================================================================
# In-memory data
# ============================================================================

items_db = {
    1: {"id": 1, "name": "Widget", "description": "A useful widget", "price": 29.99, "tax": 2.99},
    2: {"id": 2, "name": "Gadget", "description": "An amazing gadget", "price": 49.99, "tax": 4.99},
    3: {"id": 3, "name": "Doohickey", "description": "Essential doohickey", "price": 19.99, "tax": 1.99},
}

users_db = {
    1: {"id": 1, "username": "alice", "email": "alice@example.com"},
    2: {"id": 2, "username": "bob", "email": "bob@example.com"},
    3: {"id": 3, "username": "charlie", "email": "charlie@example.com"},
}


# ============================================================================
# Create FastAPI app BEFORE defining routes
# This must be at module level so it's accessible when imported
# ============================================================================

app = FastAPI(
    title="FasterAPI Test Server",
    version="1.0.0",
    description="FastAPI-compatible high-performance web framework",
    # OpenAPI spec works, but Swagger UI/ReDoc require module-level handler definitions
    # to work with the ZMQ worker architecture. Disabled for now.
    docs_url=None,
    redoc_url=None
)


# ============================================================================
# Routes - Defined at module level so workers can import them
# ============================================================================

@app.get("/")
def root():
    """Root endpoint."""
    print(f"[HANDLER] root() called!", flush=True)
    return {
        "message": "Welcome to FasterAPI!",
        "version": "1.0.0",
        "docs": "/docs",
        "redoc": "/redoc"
    }


@app.get("/health")
def health():
    """Health check endpoint."""
    return {
        "status": "healthy",
        "items": len(items_db),
        "users": len(users_db)
    }


@app.get("/items", tags=["items"])
def items_list(skip: int = 0, limit: int = 10):
    """List all items with pagination."""
    items = list(items_db.values())
    return items[skip:skip + limit]


@app.get("/items/{item_id}", tags=["items"])
def item_get(item_id: int):
    """Get a specific item."""
    if item_id not in items_db:
        return {"error": "Item not found"}, 404
    return items_db[item_id]


@app.post("/items", tags=["items"])
def item_create(item: Item):
    """Create a new item."""
    new_id = max(items_db.keys()) + 1 if items_db else 1
    item_data = item.dict()
    item_data["id"] = new_id
    items_db[new_id] = item_data
    return {"id": new_id, "item": item_data}


@app.get("/users", tags=["users"])
def users_list():
    """List all users."""
    return list(users_db.values())


@app.get("/users/{user_id}", tags=["users"])
def user_get(user_id: int):
    """Get a specific user."""
    if user_id not in users_db:
        return {"error": "User not found"}, 404
    return users_db[user_id]


# ============================================================================
# Server startup
# ============================================================================

def run_server():
    """Run the FasterAPI server."""
    print("\n" + "="*80)
    print("FasterAPI Test Server (C++ Backend)")
    print("="*80)

    if not HAS_NATIVE:
        print("\n❌ ERROR: Native bindings not available!")
        print("   Please build the C++ libraries first:")
        print("   cd build && ninja fasterapi_http")
        sys.exit(1)

    try:
        # Step 1: Connect RouteRegistry to server (CRITICAL - do this FIRST)
        print("\n[1/3] Connecting RouteRegistry to server...")
        connect_route_registry_to_server()
        print("      ✅ Done")

        # Step 2: Create native C++ HTTP server
        print("\n[2/3] Creating native C++ HTTP server...")
        server = Server(
            host="0.0.0.0",
            port=8000,
            enable_h2=False,  # Disable HTTP/2 for simpler testing
            enable_h3=False,
            enable_compression=True
        )
        print("      ✅ Done")

        # Step 3: Start the server
        print("\n[3/3] Starting server...")
        server.start()

        print("\n" + "="*80)
        print("✅ Server running on http://0.0.0.0:8000")
        print("="*80)
        print("\n📚 Available Endpoints:")
        print("  • GET  /                     - Root endpoint")
        print("  • GET  /health               - Health check")
        print("  • GET  /items                - List items")
        print("  • GET  /items/{item_id}      - Get specific item")
        print("  • POST /items                - Create new item")
        print("  • GET  /users                - List users")
        print("  • GET  /users/{user_id}      - Get specific user")
        print("\n" + "="*80)
        print("Press Ctrl+C to stop the server")
        print("="*80 + "\n")

        # Keep server running
        try:
            while server.is_running():
                time.sleep(1)
        except KeyboardInterrupt:
            print("\n\n🛑 Shutting down server...")
            server.stop()
            print("✅ Server stopped")

    except Exception as e:
        print(f"\n❌ ERROR: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)


if __name__ == "__main__":
    run_server()
