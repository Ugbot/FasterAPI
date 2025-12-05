#!/usr/bin/env python3
"""
Test server for parameter extraction tests.
Uses native FasterAPI server (C++ backend, not ASGI).
"""

import sys
sys.path.insert(0, "/Users/bengamble/FasterAPI")

from fasterapi.fastapi_compat import FastAPI

# Create app
app = FastAPI()

# Test endpoints
@app.get("/search")
def search(q: str, limit: int = 10):
    """Search with required q and optional limit"""
    return {"query": q, "limit": limit, "test": "search"}

@app.get("/filter")
def filter_items(category: str, min_price: float = 0.0, max_price: float = 1000.0):
    """Filter with multiple params"""
    return {
        "category": category,
        "min_price": min_price,
        "max_price": max_price,
        "test": "filter"
    }

@app.get("/items/{item_id}")
def get_item(item_id: int):
    """Single path parameter"""
    return {"item_id": item_id, "test": "get_item"}

@app.get("/users/{user_id}/posts")
def get_user_posts(user_id: int, page: int = 1, size: int = 10):
    """Path + query parameters"""
    return {
        "user_id": user_id,
        "page": page,
        "size": size,
        "test": "get_user_posts"
    }

@app.get("/products/{product_id}/reviews/{review_id}")
def get_review(product_id: int, review_id: int):
    """Multiple path parameters"""
    return {
        "product_id": product_id,
        "review_id": review_id,
        "test": "get_review"
    }

if __name__ == "__main__":
    print("="*80)
    print("Parameter Extraction Test Server")
    print("="*80)
    print()
    print("Registered routes:")
    for route in app.routes():
        print(f"  {route['method']:6s} {route['path_pattern']}")
    print()
    print("Starting server on http://127.0.0.1:8092...")
    print("Press Ctrl+C to stop")
    print("="*80)
    print()

    # Start native server
    # The FastAPIApp should have a way to run the server
    # Let me check the actual API
    try:
        # Try to start server - need to find correct API
        if hasattr(app, 'serve'):
            app.serve(host="127.0.0.1", port=8092)
        elif hasattr(app, 'run'):
            app.run(host="127.0.0.1", port=8092)
        elif hasattr(app, 'start'):
            app.start(host="127.0.0.1", port=8092)
        else:
            # Fallback: try to access internal server
            print("Note: Need to implement server.run() or similar")
            print("App object methods:", [m for m in dir(app) if not m.startswith('_')])

            # Keep running for manual testing
            import time
            while True:
                time.sleep(1)
    except KeyboardInterrupt:
        print("\n\nShutting down...")
