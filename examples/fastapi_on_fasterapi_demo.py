#!/usr/bin/env python3.13
"""
Demo: Running FastAPI Applications on FasterAPI's Native C++ Server

This demonstrates how to run standard FastAPI code on FasterAPI's
high-performance native C++ HTTP server instead of Uvicorn.

Features:
- Standard FastAPI decorator syntax (@app.get, @app.post, etc.)
- Path parameters with type conversion
- Query parameters with defaults
- Native C++ HTTP/1.1 server with CoroIO event loop
- Full parameter extraction and validation
"""

from fasterapi.fastapi_compat import FastAPI
from fasterapi.http.server import Server
from fasterapi._fastapi_native import connect_route_registry_to_server
import time

# Create a FastAPI app (standard FastAPI code!)
app = FastAPI(
    title="FastAPI on FasterAPI Demo",
    version="1.0.0",
    description="FastAPI app running on native C++ server"
)

# ============================================================================
# Define Routes - Standard FastAPI Syntax
# ============================================================================

@app.get("/")
def root():
    """Root endpoint"""
    return {
        "message": "Welcome to FastAPI on FasterAPI!",
        "backend": "C++ native HTTP server",
        "framework": "FastAPI compatible"
    }

@app.get("/health")
def health():
    """Health check endpoint"""
    return {"status": "healthy", "server": "FasterAPI"}

@app.get("/users/{user_id}")
def get_user(user_id: int):
    """
    Get user by ID
    
    Path Parameters:
    - user_id: Integer user ID
    """
    return {
        "id": user_id,
        "name": f"User {user_id}",
        "email": f"user{user_id}@example.com"
    }

@app.get("/items/{item_id}")
def get_item(item_id: int, detailed: bool = False):
    """
    Get item by ID with optional details
    
    Path Parameters:
    - item_id: Integer item ID
    
    Query Parameters:
    - detailed: Include detailed information (default: False)
    """
    item = {
        "id": item_id,
        "name": f"Item {item_id}",
        "price": 19.99
    }
    
    if detailed:
        item["description"] = f"Detailed information about item {item_id}"
        item["stock"] = 100
        item["category"] = "general"
    
    return item

@app.get("/products/{product_id}")
def get_product(product_id: int, include_reviews: bool = False, format: str = "json"):
    """
    Get product with multiple query parameters
    
    Path Parameters:
    - product_id: Integer product ID
    
    Query Parameters:
    - include_reviews: Include product reviews (default: False)
    - format: Response format (default: "json")
    """
    product = {
        "id": product_id,
        "name": f"Product {product_id}",
        "format": format
    }
    
    if include_reviews:
        product["reviews"] = [
            {"rating": 5, "comment": "Great product!"},
            {"rating": 4, "comment": "Good value"}
        ]
    
    return product

# ============================================================================
# Server Startup
# ============================================================================

if __name__ == "__main__":
    print("\n" + "="*70)
    print("🚀 Starting FastAPI App on FasterAPI Native C++ Server")
    print("="*70)
    
    # Step 1: Connect RouteRegistry for parameter extraction
    print("\n[1/3] Connecting RouteRegistry to server...")
    connect_route_registry_to_server()
    print("      ✅ Done")
    
    # Step 2: Create native C++ HTTP server
    print("\n[2/3] Creating native C++ HTTP server...")
    server = Server(
        port=8000,
        host="127.0.0.1",
        enable_h2=False,  # HTTP/1.1 only for this demo
        enable_h3=False,
        enable_compression=True
    )
    print("      ✅ Done")
    
    # Step 3: Start the server (non-blocking)
    print("\n[3/3] Starting server...")
    server.start()
    
    print("\n" + "="*70)
    print("✅ Server running on http://127.0.0.1:8000")
    print("="*70)
    
    print("\n📚 Available Endpoints:")
    print("  • GET  /                              - Root endpoint")
    print("  • GET  /health                        - Health check")
    print("  • GET  /users/{user_id}               - Get user by ID")
    print("  • GET  /items/{item_id}               - Get item")
    print("  • GET  /items/{item_id}?detailed=true - Get item with details")
    print("  • GET  /products/{id}?include_reviews=true - Get product with reviews")
    
    print("\n📖 Example Commands:")
    print('  curl "http://127.0.0.1:8000/"')
    print('  curl "http://127.0.0.1:8000/users/42"')
    print('  curl "http://127.0.0.1:8000/items/123?detailed=true"')
    print('  curl "http://127.0.0.1:8000/products/999?include_reviews=true"')
    
    print("\n💡 Features:")
    print("  ✓ Standard FastAPI decorators")
    print("  ✓ Path parameters with type conversion")
    print("  ✓ Query parameters with defaults")
    print("  ✓ Native C++ HTTP server (no Uvicorn needed)")
    print("  ✓ High-performance CoroIO event loop")
    
    print("\n" + "="*70)
    print("Press Ctrl+C to stop the server")
    print("="*70 + "\n")
    
    # Keep server running
    try:
        while server.is_running():
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n\n🛑 Stopping server...")
        server.stop()
        print("✅ Server stopped\n")
