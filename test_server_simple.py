#!/usr/bin/env python3
"""Simple server test to verify basic functionality"""

import sys
sys.path.insert(0, '.')

print("Testing FasterAPI server...")

from fasterapi import App

app = App(port=8080)

@app.get("/json")
def json_test(req, res):
    return {"message": "Hello, World!"}

@app.get("/plaintext")
def plaintext_test(req, res):
    res.content_type("text/plain").text("Hello, World!").send()

print("✅ Routes registered successfully")
print("🚀 Starting server on http://localhost:8080")
print()
print("Test endpoints:")
print("  http://localhost:8080/json")
print("  http://localhost:8080/plaintext")
print()

# Check if server has necessary methods
print(f"Server has start(): {hasattr(app.server, 'start')}")
print(f"Server has stop(): {hasattr(app.server, 'stop')}")
print(f"Server has is_running(): {hasattr(app.server, 'is_running')}")
print()

# Try to start
try:
    print("Calling app.server.start()...")
    app.server.start()
    print("✅ Server started")

    import time
    print("Waiting to check if server is running...")
    time.sleep(1)

    if app.server.is_running():
        print("✅ Server is running!")
    else:
        print("❌ Server is not running")

    print("\nStopping server...")
    app.server.stop()
    print("✅ Server stopped")

except Exception as e:
    print(f"❌ Error: {e}")
    import traceback
    traceback.print_exc()
