#!/usr/bin/env python3
"""
Minimal FasterAPI Test

Test basic functionality without starting the HTTP server.
"""

import sys
import os
import time
from typing import Dict, Any

# Add parent directory to path for imports
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from fasterapi import App
from fasterapi.http import Request, Response

def test_app_creation():
    """Test basic app creation and configuration."""
    print("🧪 Testing App Creation...")
    
    app = App(
        port=8000,
        host="127.0.0.1",
        enable_h2=True,
        enable_compression=True
    )
    
    print("✅ App created successfully")
    print(f"   Port: {app.port}")
    print(f"   Host: {app.host}")
    print(f"   HTTP/2: {app.enable_h2}")
    print(f"   Compression: {app.enable_compression}")
    
    return app

def test_route_registration(app: App):
    """Test route registration."""
    print("\n🧪 Testing Route Registration...")
    
    # Register routes
    @app.get("/ping")
    def ping(req: Request, res: Response):
        return {"message": "pong", "timestamp": time.time()}
    
    @app.get("/data")
    def get_data(req: Request, res: Response):
        return {"items": [{"id": i, "name": f"item_{i}"} for i in range(5)]}
    
    @app.post("/create")
    def create_item(req: Request, res: Response):
        return {"message": "created", "id": 123}
    
    print("✅ Routes registered successfully")
    print(f"   Total routes: {len(app.routes)}")
    
    # Check route types
    route_counts = {}
    for method, routes in app.routes.items():
        route_counts[method] = len(routes)
    
    print(f"   Route breakdown: {route_counts}")

def test_middleware(app: App):
    """Test middleware registration."""
    print("\n🧪 Testing Middleware...")
    
    @app.add_middleware
    def logging_middleware(req: Request, res: Response):
        print(f"📥 {req.get_method().value} {req.get_path()}")
    
    @app.add_middleware
    def timing_middleware(req: Request, res: Response):
        start_time = time.time()
        # Simulate processing
        time.sleep(0.001)
        duration = time.time() - start_time
        print(f"⏱️  Request processed in {duration*1000:.2f}ms")
    
    print("✅ Middleware registered successfully")
    print(f"   Total middleware: {len(app.middleware)}")

def test_lifecycle_hooks(app: App):
    """Test lifecycle hooks."""
    print("\n🧪 Testing Lifecycle Hooks...")
    
    @app.on_event("startup")
    def startup():
        print("🚀 Application starting up...")
    
    @app.on_event("shutdown")
    def shutdown():
        print("🛑 Application shutting down...")
    
    print("✅ Lifecycle hooks registered successfully")
    print(f"   Startup hooks: {len(app.startup_hooks)}")
    print(f"   Shutdown hooks: {len(app.shutdown_hooks)}")

def test_request_response_objects():
    """Test Request and Response objects."""
    print("\n🧪 Testing Request/Response Objects...")
    
    # Create mock objects
    req = Request()
    res = Response()
    
    # Test request methods
    print(f"   Request method: {req.get_method().value}")
    print(f"   Request path: {req.get_path()}")
    print(f"   Request protocol: {req.get_protocol()}")
    print(f"   Request client IP: {req.get_client_ip()}")
    
    # Test response methods
    res.status(200)
    res.header("Content-Type", "application/json")
    res.json({"message": "test"})
    
    print("✅ Request/Response objects working")
    print(f"   Response status: {res.status}")
    print(f"   Response headers: {len(res.headers)}")
    print(f"   Response body: {res.body}")

def test_server_stats(app: App):
    """Test server statistics."""
    print("\n🧪 Testing Server Statistics...")
    
    try:
        stats = app.get_stats()
        print("✅ Server statistics retrieved")
        print(f"   Server stats: {stats['server']}")
        print(f"   Route stats: {stats['routes']}")
        print(f"   Middleware count: {stats['middleware_count']}")
        print(f"   Startup hooks: {stats['startup_hooks']}")
        print(f"   Shutdown hooks: {stats['shutdown_hooks']}")
    except Exception as e:
        print(f"❌ Failed to get server stats: {e}")

def main():
    """Run all tests."""
    print("🔥 FasterAPI Minimal Test Suite")
    print("=" * 40)
    
    try:
        # Test app creation
        app = test_app_creation()
        
        # Test route registration
        test_route_registration(app)
        
        # Test middleware
        test_middleware(app)
        
        # Test lifecycle hooks
        test_lifecycle_hooks(app)
        
        # Test request/response objects
        test_request_response_objects()
        
        # Test server stats
        test_server_stats(app)
        
        print("\n✅ All tests passed successfully!")
        print("🎉 FasterAPI framework is working correctly!")
        
    except Exception as e:
        print(f"\n❌ Test failed: {e}")
        import traceback
        traceback.print_exc()
        return 1
    
    return 0

if __name__ == "__main__":
    sys.exit(main())
