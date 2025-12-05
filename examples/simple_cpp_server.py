#!/usr/bin/env python3.13
"""
Simple test server using just FastAPI decorators and C++ server.
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
from pydantic import BaseModel
from typing import Optional

# Create FastAPI app
app = FastAPI(title="Test API", version="1.0.0")

# Test data
items_db = {
    1: {"id": 1, "name": "Widget", "price": 29.99},
    2: {"id": 2, "name": "Gadget", "price": 49.99},
    3: {"id": 3, "name": "Doohickey", "price": 19.99},
}

# Define routes
@app.get("/")
def read_root():
    return {"message": "Welcome to FasterAPI!", "version": "1.0.0"}

@app.get("/health")
def health_check():
    return {"status": "healthy", "items": len(items_db)}

@app.get("/items")
def list_items():
    return list(items_db.values())

if __name__ == "__main__":
    print("="*80)
    print("Simple FasterAPI Test Server")
    print("="*80)

    # Connect RouteRegistry to server for parameter extraction
    connect_route_registry_to_server()

    # Create plain HTTP server (not FastAPIServer)
    server = Server(
        port=8000,
        host="0.0.0.0",
        enable_h2=False,
        enable_h3=False
    )

    print(f"\n✅ Server initialized")
    print(f"✅ Routes registered via decorators")
    print(f"✅ RouteRegistry connected")

    # Start server
    server.start()

    print(f"\n🚀 Server running on http://0.0.0.0:8000")
    print("="*80)
    print("Press Ctrl+C to stop\n")

    try:
        while server.is_running():
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n\n🛑 Shutting down...")
        server.stop()
        print("✅ Stopped")
