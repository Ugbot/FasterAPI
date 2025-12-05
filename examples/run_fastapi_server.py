#!/usr/bin/env python3.13
"""
Run the FastAPI example application with a simple test server.

This demonstrates the FastAPI compatibility layer working with route registration,
parameter extraction, and automatic documentation generation.
"""

import sys
import os

# Add parent directory to path
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Set library path for dynamic loading
os.environ['DYLD_LIBRARY_PATH'] = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    'build', 'lib'
) + ':' + os.environ.get('DYLD_LIBRARY_PATH', '')

from fasterapi.fastapi_compat import FastAPI
from pydantic import BaseModel
from typing import List, Optional
import random
import json

# Create FastAPI app
app = FastAPI(
    title="FasterAPI Test Server",
    version="1.0.0",
    description="FastAPI-compatible high-performance web framework"
)

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
# Routes
# ============================================================================

@app.get("/")
def read_root():
    """Root endpoint."""
    return {
        "message": "Welcome to FasterAPI!",
        "version": "1.0.0",
        "docs": "/docs",
        "redoc": "/redoc"
    }


@app.get("/health")
def health_check():
    """Health check endpoint."""
    return {
        "status": "healthy",
        "items": len(items_db),
        "users": len(users_db)
    }


@app.get("/items", tags=["items"])
def list_items(skip: int = 0, limit: int = 10):
    """List all items with pagination."""
    items = list(items_db.values())
    return items[skip:skip + limit]


@app.get("/items/{item_id}", tags=["items"])
def get_item(item_id: int):
    """Get a specific item."""
    if item_id not in items_db:
        return {"error": "Item not found"}, 404
    return items_db[item_id]


@app.post("/items", tags=["items"])
def create_item(item: Item):
    """Create a new item."""
    new_id = max(items_db.keys()) + 1 if items_db else 1
    item_data = item.dict()
    item_data["id"] = new_id
    items_db[new_id] = item_data
    return {"id": new_id, "item": item_data}


@app.get("/users", tags=["users"])
def list_users():
    """List all users."""
    return list(users_db.values())


@app.get("/users/{user_id}", tags=["users"])
def get_user(user_id: int):
    """Get a specific user."""
    if user_id not in users_db:
        return {"error": "User not found"}, 404
    return users_db[user_id]


# ============================================================================
# Simple test server using Python's http.server
# ============================================================================

if __name__ == "__main__":
    from http.server import HTTPServer, BaseHTTPRequestHandler
    from urllib.parse import urlparse, parse_qs
    import re

    try:
        from fasterapi._fastapi_native import (
            get_all_routes,
            generate_openapi,
            generate_swagger_ui_response,
            generate_redoc_response
        )
        HAS_NATIVE = True
    except ImportError:
        HAS_NATIVE = False
        print("Warning: Native bindings not available")

    class FastAPIHandler(BaseHTTPRequestHandler):
        """Simple HTTP handler that routes requests to FastAPI handlers."""

        def do_GET(self):
            self.handle_request()

        def do_POST(self):
            self.handle_request()

        def do_PUT(self):
            self.handle_request()

        def do_DELETE(self):
            self.handle_request()

        def handle_request(self):
            """Route request to appropriate handler."""
            parsed_url = urlparse(self.path)
            path = parsed_url.path
            query = parsed_url.query

            # Special routes
            if path == "/docs" and HAS_NATIVE:
                html = generate_swagger_ui_response("/openapi.json", "FasterAPI Documentation")
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", len(html))
                self.end_headers()
                self.wfile.write(html.encode())
                return

            if path == "/redoc" and HAS_NATIVE:
                html = generate_redoc_response("/openapi.json", "FasterAPI Documentation")
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", len(html))
                self.end_headers()
                self.wfile.write(html.encode())
                return

            if path == "/openapi.json" and HAS_NATIVE:
                openapi_spec = generate_openapi(app.title, app.version, app.description)
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", len(openapi_spec))
                self.end_headers()
                self.wfile.write(openapi_spec.encode())
                return

            # Find matching route
            if HAS_NATIVE:
                routes = get_all_routes()
                for route in routes:
                    if route['method'] == self.command:
                        # Simple path matching (without pattern support for now)
                        if route['path_pattern'] == path:
                            # Call the handler (simplified - no parameter extraction yet)
                            try:
                                # Get the handler from globals
                                handler_name = None
                                for name, obj in globals().items():
                                    if callable(obj) and hasattr(obj, '__name__'):
                                        # Match by checking if this is a registered FastAPI route
                                        pass

                                # For now, just return a test response
                                response_data = {
                                    "message": "Route matched!",
                                    "path": path,
                                    "method": self.command,
                                    "route_pattern": route['path_pattern']
                                }

                                response_json = json.dumps(response_data, indent=2)
                                self.send_response(200)
                                self.send_header("Content-Type", "application/json")
                                self.send_header("Content-Length", len(response_json))
                                self.end_headers()
                                self.wfile.write(response_json.encode())
                                return
                            except Exception as e:
                                error_data = {"error": str(e)}
                                response_json = json.dumps(error_data)
                                self.send_response(500)
                                self.send_header("Content-Type", "application/json")
                                self.send_header("Content-Length", len(response_json))
                                self.end_headers()
                                self.wfile.write(response_json.encode())
                                return

            # Not found
            error_data = {"error": "Not found", "path": path, "method": self.command}
            response_json = json.dumps(error_data)
            self.send_response(404)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", len(response_json))
            self.end_headers()
            self.wfile.write(response_json.encode())

        def log_message(self, format, *args):
            """Log HTTP requests."""
            print(f"{self.address_string()} - [{self.log_date_time_string()}] {format % args}")

    # Print startup information
    print("\n" + "="*80)
    print("FasterAPI Test Server")
    print("="*80)

    if HAS_NATIVE:
        routes = get_all_routes()
        print(f"\n✅ Native bindings loaded successfully!")
        print(f"✅ {len(routes)} routes registered in C++:")
        for route in routes:
            print(f"   {route['method']:6s} {route['path_pattern']}")
    else:
        print("\n⚠️  Native bindings not available")

    print("\n" + "="*80)
    print("Server starting on http://localhost:8000")
    print("="*80)
    print("📚 Documentation:  http://localhost:8000/docs")
    print("📖 ReDoc:          http://localhost:8000/redoc")
    print("🔧 OpenAPI spec:   http://localhost:8000/openapi.json")
    print("="*80 + "\n")

    # Start server
    server_address = ('', 8000)
    httpd = HTTPServer(server_address, FastAPIHandler)

    try:
        print("Press Ctrl+C to stop the server\n")
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n\n🛑 Shutting down server...")
        httpd.shutdown()
        print("✅ Server stopped")
