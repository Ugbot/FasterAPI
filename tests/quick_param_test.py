#!/usr/bin/env python3
"""
Quick Parameter Test
Simple script to test parameter extraction with the native FastAPIServer.
Run this, then in another terminal run curl commands to test.
"""

import sys
import os
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from fasterapi.fastapi_compat import FastAPI
from fasterapi.fastapi_server import FastAPIServer, HAS_NATIVE

# Create app
app = FastAPI()

# Simple test endpoints
@app.get("/test_query")
def test_query(name: str, age: int = 25):
    """Test query parameters"""
    return {"name": name, "age": age, "test": "query"}

@app.get("/test_path/{item_id}")
def test_path(item_id: int):
    """Test path parameter"""
    return {"item_id": item_id, "test": "path"}

@app.get("/test_both/{user_id}")
def test_both(user_id: int, active: str = "yes"):
    """Test path + query"""
    return {"user_id": user_id, "active": active, "test": "both"}

if __name__ == "__main__":
    print("\n" + "="*80)
    print("Quick Parameter Test Server")
    print("="*80)

    if not HAS_NATIVE:
        print("\n❌ ERROR: Native bindings not available!")
        print("   Run: cd build && ninja fasterapi_http\n")
        sys.exit(1)

    print("\n✅ Native bindings available")
    print("\nStarting server...")

    try:
        server = FastAPIServer(
            app,
            host="127.0.0.1",
            port=8888,
            enable_h2=False,
            enable_h3=False,
            enable_compression=False  # Keep it simple
        )

        print("✅ Server object created")
        print("\nCalling server.start()...")

        server.start()

        print("\n" + "="*80)
        print("✅ Server running on http://127.0.0.1:8888")
        print("="*80)

        print("\nTest commands:")
        print('  curl "http://127.0.0.1:8888/test_query?name=alice&age=30"')
        print('  curl "http://127.0.0.1:8888/test_path/123"')
        print('  curl "http://127.0.0.1:8888/test_both/99?active=no"')
        print("\nPress Ctrl+C to stop\n")

        # Keep running
        while server.is_running():
            time.sleep(1)

    except KeyboardInterrupt:
        print("\n\n🛑 Stopping...")
        server.stop()
        print("✅ Stopped\n")
    except Exception as e:
        print(f"\n❌ ERROR: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
