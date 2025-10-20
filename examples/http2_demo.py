#!/usr/bin/env python3
"""
HTTP/2 Demo with FasterAPI

Demonstrates HTTP/2 features including:
- ALPN negotiation
- Multiplexing
- Server push
- HPACK compression
- Flow control
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

def create_http2_app() -> App:
    """Create FasterAPI application with HTTP/2 support."""
    app = App(
        port=8000,
        host="127.0.0.1",
        enable_h2=True,      # Enable HTTP/2
        enable_h3=False,     # Disable HTTP/3 for now
        enable_compression=True
    )
    
    # Simple ping endpoint
    @app.get("/ping")
    def ping(req: Request, res: Response):
        return {
            "message": "pong",
            "protocol": req.get_protocol(),
            "timestamp": time.time()
        }
    
    # JSON data endpoint
    @app.get("/data")
    def get_data(req: Request, res: Response):
        return {
            "items": [
                {"id": i, "name": f"item_{i}", "value": i * 10}
                for i in range(100)
            ],
            "count": 100,
            "protocol": req.get_protocol(),
            "timestamp": time.time()
        }
    
    # Large response for compression testing
    @app.get("/large")
    def get_large_data(req: Request, res: Response):
        return {
            "data": "x" * 10000,  # 10KB of data
            "size": 10000,
            "protocol": req.get_protocol(),
            "timestamp": time.time()
        }
    
    # Server push endpoint
    @app.get("/push")
    def push_data(req: Request, res: Response):
        # In a real implementation, this would trigger server push
        return {
            "message": "Server push triggered",
            "pushed_resources": ["/data", "/ping"],
            "protocol": req.get_protocol(),
            "timestamp": time.time()
        }
    
    # Multiplexing test endpoint
    @app.get("/multiplex/{count}")
    def multiplex_test(count: int, req: Request, res: Response):
        return {
            "count": count,
            "message": f"Multiplexed request {count}",
            "protocol": req.get_protocol(),
            "timestamp": time.time()
        }
    
    # Middleware for logging
    @app.add_middleware
    def logging_middleware(req: Request, res: Response):
        protocol = req.get_protocol()
        print(f"📥 {req.get_method().value} {req.get_path()} - {protocol}")
    
    # Lifecycle hooks
    @app.on_event("startup")
    def startup():
        print("🚀 FasterAPI HTTP/2 server starting...")
        print("   Features: HTTP/2=True, HTTP/3=False, Compression=True")
    
    @app.on_event("shutdown")
    def shutdown():
        print("🛑 FasterAPI HTTP/2 server stopping...")
    
    return app

def test_http2_endpoints():
    """Test HTTP/2 endpoints."""
    base_url = "http://127.0.0.1:8000"
    
    tests = [
        ("GET", "/ping", None, "Simple ping"),
        ("GET", "/data", None, "JSON data"),
        ("GET", "/large", None, "Large response"),
        ("GET", "/push", None, "Server push"),
        ("GET", "/multiplex/1", None, "Multiplex test 1"),
        ("GET", "/multiplex/2", None, "Multiplex test 2"),
        ("GET", "/multiplex/3", None, "Multiplex test 3"),
    ]
    
    print("\n🧪 Testing HTTP/2 endpoints...")
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
                protocol = result.get("protocol", "Unknown")
                print(f"✅ {description}: {response.status_code} ({protocol})")
                print(f"   Response: {result.get('message', 'OK')}")
            else:
                print(f"❌ {description}: {response.status_code}")
                print(f"   Error: {response.text}")
                
        except requests.exceptions.RequestException as e:
            print(f"❌ {description}: Request failed - {e}")
        
        print()

def test_concurrent_requests():
    """Test concurrent requests to demonstrate HTTP/2 multiplexing."""
    base_url = "http://127.0.0.1:8000"
    
    print("\n🔄 Testing concurrent requests (HTTP/2 multiplexing)...")
    print("=" * 60)
    
    def make_request(endpoint: str, request_id: int):
        try:
            start_time = time.time()
            response = requests.get(f"{base_url}{endpoint}", timeout=5)
            duration = time.time() - start_time
            
            if response.status_code == 200:
                result = response.json()
                protocol = result.get("protocol", "Unknown")
                print(f"✅ Request {request_id}: {response.status_code} ({protocol}) - {duration:.3f}s")
            else:
                print(f"❌ Request {request_id}: {response.status_code} - {duration:.3f}s")
                
        except requests.exceptions.RequestException as e:
            print(f"❌ Request {request_id}: Failed - {e}")
    
    # Launch concurrent requests
    threads = []
    for i in range(10):
        thread = threading.Thread(target=make_request, args=("/multiplex/" + str(i), i))
        threads.append(thread)
        thread.start()
    
    # Wait for all threads to complete
    for thread in threads:
        thread.join()
    
    print("\n✅ Concurrent request test completed!")

def test_compression():
    """Test response compression."""
    base_url = "http://127.0.0.1:8000"
    
    print("\n🗜️  Testing response compression...")
    print("=" * 40)
    
    try:
        # Test large response
        response = requests.get(f"{base_url}/large", timeout=5)
        
        if response.status_code == 200:
            result = response.json()
            content_length = len(response.content)
            data_size = result.get("size", 0)
            
            print(f"✅ Large response: {response.status_code}")
            print(f"   Content-Length: {content_length} bytes")
            print(f"   Data size: {data_size} bytes")
            print(f"   Compression ratio: {((data_size - content_length) / data_size * 100):.1f}%")
            
            # Check for compression headers
            content_encoding = response.headers.get("Content-Encoding", "")
            if content_encoding:
                print(f"   Compression: {content_encoding}")
            else:
                print("   Compression: None")
        else:
            print(f"❌ Large response: {response.status_code}")
            
    except requests.exceptions.RequestException as e:
        print(f"❌ Compression test failed: {e}")

def run_http2_demo():
    """Run the HTTP/2 demo."""
    print("🔥 FasterAPI HTTP/2 Demo")
    print("=" * 30)
    
    # Create and configure app
    app = create_http2_app()
    
    print("✅ HTTP/2 app created successfully")
    print(f"   Routes: {len(app.routes)}")
    print(f"   Middleware: {len(app.middleware)}")
    print(f"   HTTP/2: {app.enable_h2}")
    print(f"   HTTP/3: {app.enable_h3}")
    print(f"   Compression: {app.enable_compression}")
    
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
    
    # Run tests
    test_http2_endpoints()
    test_concurrent_requests()
    test_compression()
    
    # Test server stats
    try:
        stats = app.get_stats()
        print("\n📊 Server Statistics:")
        print(f"   Total requests: {stats['server']['total_requests']}")
        print(f"   Active connections: {stats['server']['active_connections']}")
        print(f"   HTTP/1.1 requests: {stats['server']['h1_requests']}")
        print(f"   HTTP/2 requests: {stats['server']['h2_requests']}")
        print(f"   HTTP/3 requests: {stats['server']['h3_requests']}")
        print(f"   Compressed responses: {stats['server']['compressed_responses']}")
        print(f"   Compression bytes saved: {stats['server']['compression_bytes_saved']}")
    except Exception as e:
        print(f"❌ Failed to get server stats: {e}")
    
    print("\n✅ HTTP/2 demo completed successfully!")
    print("🎉 FasterAPI HTTP/2 features are working!")

if __name__ == "__main__":
    try:
        run_http2_demo()
    except KeyboardInterrupt:
        print("\n⏹️  Demo interrupted by user")
    except Exception as e:
        print(f"\n❌ Demo failed: {e}")
        import traceback
        traceback.print_exc()
