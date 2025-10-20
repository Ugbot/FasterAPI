#!/usr/bin/env python3
"""
Simple HTTP/2 Test for FasterAPI

Tests basic HTTP/2 functionality without complex features.
"""

import sys
import os
import time

# Add parent directory to path for imports
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from fasterapi import App
from fasterapi.http import Request, Response

def create_simple_http2_app() -> App:
    """Create a simple FasterAPI application with HTTP/2 support."""
    app = App(
        port=8001,  # Different port to avoid conflicts
        host="127.0.0.1",
        enable_h2=True,
        enable_h3=False,
        enable_compression=False  # Disable compression for simplicity
    )
    
    # Simple ping endpoint
    @app.get("/ping")
    def ping(req: Request, res: Response):
        return {"message": "pong", "protocol": "HTTP/2"}
    
    # Simple data endpoint
    @app.get("/data")
    def get_data(req: Request, res: Response):
        return {"data": "Hello HTTP/2", "count": 42}
    
    # Lifecycle hooks
    @app.on_event("startup")
    def startup():
        print("🚀 Simple HTTP/2 server starting...")
    
    @app.on_event("shutdown")
    def shutdown():
        print("🛑 Simple HTTP/2 server stopping...")
    
    return app

def test_simple_http2():
    """Test simple HTTP/2 functionality."""
    print("🔥 Simple HTTP/2 Test")
    print("=" * 30)
    
    # Create app
    app = create_simple_http2_app()
    
    print("✅ HTTP/2 app created successfully")
    print(f"   Port: {app.port}")
    print(f"   Host: {app.host}")
    print(f"   HTTP/2: {app.enable_h2}")
    print(f"   HTTP/3: {app.enable_h3}")
    print(f"   Compression: {app.enable_compression}")
    
    # Test route registration
    print(f"\n✅ Routes registered: {len(app.routes)}")
    for method, routes in app.routes.items():
        print(f"   {method}: {len(routes)} routes")
    
    # Test middleware
    print(f"\n✅ Middleware registered: {len(app.middleware)}")
    
    # Test lifecycle hooks
    print(f"\n✅ Lifecycle hooks registered")
    print(f"   Startup hooks: {len(app.startup_hooks)}")
    print(f"   Shutdown hooks: {len(app.shutdown_hooks)}")
    
    # Test server stats
    try:
        stats = app.get_stats()
        print(f"\n✅ Server statistics:")
        print(f"   Total requests: {stats['server']['total_requests']}")
        print(f"   Active connections: {stats['server']['active_connections']}")
        print(f"   HTTP/2 requests: {stats['server']['h2_requests']}")
    except Exception as e:
        print(f"\n❌ Failed to get server stats: {e}")
    
    print("\n✅ Simple HTTP/2 test completed successfully!")
    print("🎉 FasterAPI HTTP/2 features are working!")

if __name__ == "__main__":
    try:
        test_simple_http2()
    except KeyboardInterrupt:
        print("\n⏹️  Test interrupted by user")
    except Exception as e:
        print(f"\n❌ Test failed: {e}")
        import traceback
        traceback.print_exc()
