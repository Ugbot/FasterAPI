#!/usr/bin/env python3
"""
Test server for parameter extraction validation.
Uses FastAPIServer with native C++ backend.
"""

import sys
import os

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from fasterapi.fastapi_compat import FastAPI
from fasterapi.fastapi_server import FastAPIServer, HAS_NATIVE
import time

# Create test app
app = FastAPI(
    title="Parameter Test Server",
    version="1.0.0"
)

# Test endpoints with query parameters
@app.get("/search")
def search(q: str, limit: int = 10):
    """Search with required q and optional limit"""
    return {"query": q, "limit": limit, "endpoint": "search"}

@app.get("/filter")
def filter_items(category: str, min_price: float = 0.0, max_price: float = 1000.0):
    """Filter with multiple params"""
    return {
        "category": category,
        "min_price": min_price,
        "max_price": max_price,
        "endpoint": "filter"
    }

# Test endpoints with path parameters
@app.get("/items/{item_id}")
def get_item(item_id: int):
    """Single path parameter"""
    return {"item_id": item_id, "endpoint": "get_item"}

@app.get("/products/{product_id}/reviews/{review_id}")
def get_review(product_id: int, review_id: int):
    """Multiple path parameters"""
    return {
        "product_id": product_id,
        "review_id": review_id,
        "endpoint": "get_review"
    }

# Test endpoints with combined path + query
@app.get("/users/{user_id}/posts")
def get_user_posts(user_id: int, page: int = 1, size: int = 10):
    """Path + query parameters"""
    return {
        "user_id": user_id,
        "page": page,
        "size": size,
        "endpoint": "get_user_posts"
    }

if __name__ == "__main__":
    print("="*80)
    print("Parameter Extraction Test Server")
    print("="*80)

    if not HAS_NATIVE:
        print("\n❌ ERROR: Native bindings not available!")
        print("   Build required: cd build && ninja fasterapi_http")
        sys.exit(1)

    try:
        # Create server
        server = FastAPIServer(
            app,
            host="127.0.0.1",
            port=8092,
            enable_h2=False,
            enable_h3=False,
            enable_compression=True
        )

        print("\n✅ Server initialized")
        print("\nRegistered test routes:")
        for route in app.routes():
            print(f"  {route['method']:6s} {route['path_pattern']}")

        # Start server
        server.start()

        print("\n" + "="*80)
        print("Server started on http://127.0.0.1:8092")
        print("="*80)
        print("\nTest URLs:")
        print("  curl 'http://127.0.0.1:8092/search?q=test&limit=50'")
        print("  curl 'http://127.0.0.1:8092/filter?category=electronics&min_price=10&max_price=100'")
        print("  curl 'http://127.0.0.1:8092/items/123'")
        print("  curl 'http://127.0.0.1:8092/users/42/posts?page=5&size=20'")
        print("  curl 'http://127.0.0.1:8092/products/1/reviews/99'")
        print("="*80)
        print("\nPress Ctrl+C to stop\n")

        # Keep running
        try:
            while server.is_running():
                time.sleep(1)
        except KeyboardInterrupt:
            print("\n\n🛑 Shutting down...")
            server.stop()
            print("✅ Stopped")

    except Exception as e:
        print(f"\n❌ ERROR: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
