#!/usr/bin/env python3
"""
Benchmark native FasterAPI server vs FastAPI/uvicorn
"""
import sys
sys.path.insert(0, '.')

from fasterapi import App
import json

# Create native app
app = App(port=8080)

# Simple counter for 1MRC-style benchmark
total_requests = 0
total_sum = 0.0
users = {}

@app.get("/json")
def json_test(req, res):
    return {"message": "Hello, World!"}

@app.get("/plaintext")
def plaintext_test(req, res):
    res.content_type("text/plain").text("Hello, World!").send()

@app.get("/health")
def health(req, res):
    return {"status": "healthy", "framework": "fasterapi-native"}

@app.post("/event")
def post_event(req, res):
    global total_requests, total_sum, users
    try:
        data = req.json()
        user_id = data.get("userId", "")
        value = float(data.get("value", 0))
        total_requests += 1
        total_sum += value
        users[user_id] = True
        res.status(201).json({"status": "ok"})
    except Exception as e:
        res.status(400).json({"error": str(e)})

@app.get("/stats")
def get_stats(req, res):
    global total_requests, total_sum, users
    avg = total_sum / total_requests if total_requests > 0 else 0.0
    return {
        "totalRequests": total_requests,
        "uniqueUsers": len(users),
        "sum": total_sum,
        "avg": avg
    }

@app.post("/reset")
def reset_stats(req, res):
    global total_requests, total_sum, users
    total_requests = 0
    total_sum = 0.0
    users = {}
    return {"status": "reset"}

if __name__ == "__main__":
    print("=" * 60)
    print("FasterAPI Native Benchmark Server")
    print("=" * 60)
    print("Starting on http://0.0.0.0:8080")
    print("")
    print("Endpoints:")
    print("  GET  /json      - JSON response")
    print("  GET  /plaintext - Plain text response")
    print("  GET  /health    - Health check")
    print("  POST /event     - 1MRC event")
    print("  GET  /stats     - 1MRC stats")
    print("=" * 60)
    
    app.server.start()
    
    # Keep server running
    import time
    try:
        while app.server.is_running():
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nShutting down...")
        app.server.stop()
