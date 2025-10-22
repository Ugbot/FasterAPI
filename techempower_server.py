#!/usr/bin/env python3
"""
TechEmpower-style HTTP server for actual load testing
"""

import sys
import time
import random
import signal

sys.path.insert(0, '.')

from fasterapi import App

# Create app
app = App(port=8080)

print("Setting up routes...")

# Test 1: JSON Serialization
@app.get("/json")
def json_test(req, res):
    """TechEmpower Test 1: JSON serialization"""
    return {"message": "Hello, World!"}

# Test 2: Plaintext
@app.get("/plaintext")
def plaintext_test(req, res):
    """TechEmpower Test 6: Plaintext"""
    res.content_type("text/plain").text("Hello, World!").send()

# Test 3: Single Database Query (simulated)
@app.get("/db")
def db_test(req, res):
    """TechEmpower Test 2: Single database query"""
    return {
        "id": random.randint(1, 10000),
        "randomNumber": random.randint(1, 10000)
    }

# Test 4: Multiple Queries (simulated)
@app.get("/queries")
def queries_test(req, res):
    """TechEmpower Test 3: Multiple queries"""
    # Get count from query param
    count_str = req.get_query_param("queries")
    try:
        count = int(count_str) if count_str else 1
    except ValueError:
        count = 1

    # Clamp to 1-500
    count = max(1, min(500, count))

    # Generate fake data
    worlds = [
        {"id": random.randint(1, 10000), "randomNumber": random.randint(1, 10000)}
        for _ in range(count)
    ]

    return worlds

print("✅ Routes registered")
print()
print("="*80)
print("🚀 Starting FasterAPI TechEmpower Server")
print("="*80)
print()
print("Server: http://localhost:8080")
print()
print("📊 Test Endpoints:")
print("   GET /json           - JSON serialization")
print("   GET /plaintext      - Plaintext response")
print("   GET /db             - Single database query (simulated)")
print("   GET /queries?queries=N - Multiple queries (simulated)")
print()
print("💡 Test with:")
print("   curl http://localhost:8080/json")
print("   wrk -t4 -c64 -d10s http://localhost:8080/json")
print("   ab -n 10000 -c 100 http://localhost:8080/plaintext")
print()
print("Press Ctrl+C to stop")
print("="*80)
print()

# Start server
try:
    print("Calling server.start()...")
    app.server.start()
    print(f"✅ Server started (is_running: {app.server.is_running()})")
    print()

    # Keep main thread alive
    print("Server is running. Keeping main thread alive...")
    while True:
        time.sleep(1)

except KeyboardInterrupt:
    print("\n")
    print("🛑 Shutting down...")
    app.server.stop()
    print("✅ Server stopped")

except Exception as e:
    print(f"❌ Error: {e}")
    import traceback
    traceback.print_exc()
