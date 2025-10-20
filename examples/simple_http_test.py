#!/usr/bin/env python3
"""
Simple HTTP Server Test

Test basic HTTP functionality of FasterAPI without external dependencies.
"""

import sys
import os
import time
import threading
import requests
from typing import Dict, Any

# Add parent directory to path for imports
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from fasterapi import App
from fasterapi.http import Request, Response

def create_test_app() -> App:
    """Create a test FasterAPI application."""
    app = App(
        port=8000,
        host="127.0.0.1",
        enable_h2=True,
        enable_compression=True
    )
    
    # Simple ping endpoint
    @app.get("/ping")
    def ping(req: Request, res: Response):
        return {"message": "pong", "timestamp": time.time()}
    
    # JSON data endpoint
    @app.get("/data")
    def get_data(req: Request, res: Response):
        return {
            "items": [
                {"id": i, "name": f"item_{i}", "value": i * 10}
                for i in range(10)
            ],
            "count": 10,
            "timestamp": time.time()
        }
    
    # Path parameter endpoint
    @app.get("/user/{user_id}")
    def get_user(user_id: int, req: Request, res: Response):
        return {
            "id": user_id,
            "name": f"user_{user_id}",
            "email": f"user_{user_id}@example.com",
            "timestamp": time.time()
        }
    
    # Query parameter endpoint
    @app.get("/search")
    def search(req: Request, res: Response):
        query = req.get_query_param("q")
        limit = req.get_query_param("limit")
        
        return {
            "query": query,
            "limit": limit,
            "results": [
                {"id": i, "title": f"Result {i} for {query}"}
                for i in range(int(limit) if limit else 5)
            ],
            "timestamp": time.time()
        }
    
    # POST endpoint
    @app.post("/create")
    def create_item(req: Request, res: Response):
        try:
            data = req.json()
            return {
                "message": "Item created successfully",
                "data": data,
                "id": 123,
                "timestamp": time.time()
            }
        except Exception as e:
            return {"error": str(e)}, 400
    
    # Middleware for logging
    @app.add_middleware
    def logging_middleware(req: Request, res: Response):
        print(f"📥 {req.get_method().value} {req.get_path()} - {req.get_client_ip()}")
    
    # Lifecycle hooks
    @app.on_event("startup")
    def startup():
        print("🚀 FasterAPI HTTP server starting...")
    
    @app.on_event("shutdown")
    def shutdown():
        print("🛑 FasterAPI HTTP server stopping...")
    
    return app

def test_endpoints():
    """Test all endpoints with HTTP requests."""
    base_url = "http://127.0.0.1:8000"
    
    tests = [
        ("GET", "/ping", None, "Simple ping"),
        ("GET", "/data", None, "JSON data"),
        ("GET", "/user/123", None, "Path parameter"),
        ("GET", "/search?q=test&limit=3", None, "Query parameters"),
        ("POST", "/create", {"name": "test", "value": 42}, "POST with JSON"),
    ]
    
    print("\n🧪 Testing HTTP endpoints...")
    print("=" * 50)
    
    for method, endpoint, data, description in tests:
        try:
            if method == "GET":
                response = requests.get(f"{base_url}{endpoint}", timeout=5)
            elif method == "POST":
                response = requests.post(
                    f"{base_url}{endpoint}", 
                    json=data, 
                    timeout=5
                )
            
            if response.status_code == 200:
                result = response.json()
                print(f"✅ {description}: {response.status_code}")
                print(f"   Response: {result}")
            else:
                print(f"❌ {description}: {response.status_code}")
                print(f"   Error: {response.text}")
                
        except requests.exceptions.RequestException as e:
            print(f"❌ {description}: Request failed - {e}")
        
        print()

def run_server_test():
    """Run the HTTP server test."""
    print("🔥 FasterAPI HTTP Server Test")
    print("=" * 40)
    
    # Create and configure app
    app = create_test_app()
    
    print("✅ App created successfully")
    print(f"   Routes: {len(app.routes)}")
    print(f"   Middleware: {len(app.middleware)}")
    print(f"   Startup hooks: {len(app.startup_hooks)}")
    print(f"   Shutdown hooks: {len(app.shutdown_hooks)}")
    
    # Start server in background thread
    server_thread = threading.Thread(target=app.run, daemon=True)
    server_thread.start()
    
    # Wait for server to start
    print("\n⏳ Waiting for server to start...")
    time.sleep(3)
    
    # Test server health
    try:
        response = requests.get("http://127.0.0.1:8000/ping", timeout=5)
        if response.status_code == 200:
            print("✅ Server is responding")
        else:
            print(f"❌ Server responded with status {response.status_code}")
            return
    except requests.exceptions.RequestException as e:
        print(f"❌ Server is not responding: {e}")
        return
    
    # Run endpoint tests
    test_endpoints()
    
    # Test server stats
    try:
        stats = app.get_stats()
        print("📊 Server Statistics:")
        print(f"   Total requests: {stats['server']['total_requests']}")
        print(f"   Active connections: {stats['server']['active_connections']}")
        print(f"   Routes: {stats['routes']}")
        print(f"   Middleware count: {stats['middleware_count']}")
    except Exception as e:
        print(f"❌ Failed to get server stats: {e}")
    
    print("\n✅ HTTP server test completed successfully!")

if __name__ == "__main__":
    try:
        run_server_test()
    except KeyboardInterrupt:
        print("\n⏹️  Test interrupted by user")
    except Exception as e:
        print(f"\n❌ Test failed: {e}")
        import traceback
        traceback.print_exc()
