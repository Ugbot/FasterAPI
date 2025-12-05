#!/usr/bin/env python3.13
"""
E2E Test Server - FasterAPI REST API Server
This server demonstrates a complete Python REST API using FasterAPI.
"""

import sys
import os
import time
import random

sys.path.insert(0, '/Users/bengamble/FasterAPI')

from fasterapi.fastapi_compat import FastAPI
from fasterapi.http.server import Server
from fasterapi._fastapi_native import connect_route_registry_to_server

# Create FastAPI app
app = FastAPI(
    title="FasterAPI E2E Test Server",
    version="1.0.0",
    description="Comprehensive REST API for testing FasterAPI"
)

# ========================================
# Test Data (in-memory storage)
# ========================================
users_db = {}
items_db = {}
next_user_id = 1
next_item_id = 1

# ========================================
# API Endpoints
# ========================================

@app.get("/")
async def root():
    """Root endpoint"""
    return {
        "service": "FasterAPI E2E Test Server",
        "version": "1.0.0",
        "status": "running",
        "architecture": "C++ HTTP Server + ZMQ IPC + Python Workers"
    }

@app.get("/health")
async def health():
    """Health check endpoint"""
    return {
        "status": "healthy",
        "timestamp": time.time(),
        "pid": os.getpid()
    }

# User CRUD endpoints
@app.post("/users")
async def create_user(name: str, email: str, age: int = 0):
    """Create a new user"""
    global next_user_id
    user_id = next_user_id
    next_user_id += 1

    user = {
        "id": user_id,
        "name": name,
        "email": email,
        "age": age,
        "created_at": time.time()
    }
    users_db[user_id] = user

    return {
        "created": True,
        "user": user
    }

@app.get("/users/{user_id}")
async def get_user(user_id: int):
    """Get user by ID"""
    if user_id not in users_db:
        return {
            "error": "User not found",
            "user_id": user_id
        }

    return {
        "user": users_db[user_id]
    }

@app.put("/users/{user_id}")
async def update_user(user_id: int, name: str = None, email: str = None, age: int = None):
    """Update user"""
    if user_id not in users_db:
        return {
            "error": "User not found",
            "user_id": user_id
        }

    user = users_db[user_id]
    if name is not None:
        user["name"] = name
    if email is not None:
        user["email"] = email
    if age is not None:
        user["age"] = age

    user["updated_at"] = time.time()

    return {
        "updated": True,
        "user": user
    }

@app.delete("/users/{user_id}")
async def delete_user(user_id: int):
    """Delete user"""
    if user_id not in users_db:
        return {
            "error": "User not found",
            "user_id": user_id
        }

    del users_db[user_id]

    return {
        "deleted": True,
        "user_id": user_id
    }

@app.get("/users")
async def list_users(limit: int = 10, offset: int = 0):
    """List all users with pagination"""
    all_users = list(users_db.values())
    paginated = all_users[offset:offset + limit]

    return {
        "users": paginated,
        "total": len(all_users),
        "limit": limit,
        "offset": offset
    }

# Item CRUD endpoints
@app.post("/items")
async def create_item(name: str, price: float, in_stock: bool = True):
    """Create a new item"""
    global next_item_id
    item_id = next_item_id
    next_item_id += 1

    item = {
        "id": item_id,
        "name": name,
        "price": price,
        "in_stock": in_stock,
        "created_at": time.time()
    }
    items_db[item_id] = item

    return {
        "created": True,
        "item": item
    }

@app.get("/items/{item_id}")
async def get_item(item_id: int):
    """Get item by ID"""
    if item_id not in items_db:
        return {
            "error": "Item not found",
            "item_id": item_id
        }

    return {
        "item": items_db[item_id]
    }

@app.get("/items")
async def list_items(in_stock: bool = None, min_price: float = 0.0, max_price: float = 999999.0):
    """List items with filters"""
    filtered_items = []

    for item in items_db.values():
        if in_stock is not None and item["in_stock"] != in_stock:
            continue
        if item["price"] < min_price or item["price"] > max_price:
            continue
        filtered_items.append(item)

    return {
        "items": filtered_items,
        "total": len(filtered_items)
    }

# Test endpoints for various parameter types
@app.get("/search")
async def search(q: str, page: int = 1, limit: int = 10, sort: str = "relevance"):
    """Search with multiple query parameters"""
    # Randomized results for testing
    num_results = random.randint(0, 100)
    results = [
        {
            "id": i,
            "title": f"Result {i} for '{q}'",
            "score": random.random()
        }
        for i in range(min(num_results, limit))
    ]

    return {
        "query": q,
        "page": page,
        "limit": limit,
        "sort": sort,
        "total": num_results,
        "results": results
    }

@app.post("/compute")
async def compute(n: int, operation: str = "sum_squares"):
    """CPU-bound computation to test worker pool"""
    if operation == "sum_squares":
        result = sum(i * i for i in range(n))
    elif operation == "factorial":
        result = 1
        for i in range(1, n + 1):
            result *= i
    else:
        result = n

    return {
        "n": n,
        "operation": operation,
        "result": result,
        "worker_pid": os.getpid()
    }

@app.get("/random")
async def random_data():
    """Generate random data (testing requirement)"""
    return {
        "random_int": random.randint(1, 1000),
        "random_float": random.random() * 100,
        "random_bool": random.choice([True, False]),
        "random_string": ''.join(random.choices('abcdefghijklmnopqrstuvwxyz', k=10)),
        "random_list": [random.randint(1, 100) for _ in range(random.randint(1, 10))]
    }

# ========================================
# Server Startup
# ========================================

if __name__ == "__main__":
    print("=" * 70)
    print("FasterAPI E2E Test Server")
    print("=" * 70)
    print()
    print("Architecture:")
    print("  HTTP/1.1 Request → C++ HTTP Server (coroio)")
    print("                  → C++ Radix Tree Router (zero-copy routing)")
    print("                  → C++ Parameter Extraction (zero-copy)")
    print("                  → C++ JSON Parsing (simdjson)")
    print("                  → ZeroMQ IPC")
    print("                  → Python Worker Process")
    print("                  → Handler Execution")
    print("                  → ZeroMQ IPC")
    print("                  → C++ Server")
    print("                  → HTTP Response")
    print()

    # Connect routes to C++ router
    print("Connecting routes to C++ router...")
    connect_route_registry_to_server()
    print(f"Registered {len(app.routes())} routes")
    print()

    # Create C++ HTTP server
    print("Creating C++ HTTP server...")
    server = Server(
        port=8000,
        host="0.0.0.0",
        enable_h2=False,  # HTTP/1.1 for simplicity
        enable_h3=False,
        enable_compression=False
    )

    print()
    print("=" * 70)
    print("Server started on http://0.0.0.0:8000")
    print("=" * 70)
    print()
    print("Available endpoints:")
    print("  GET    /")
    print("  GET    /health")
    print("  POST   /users (JSON: name, email, age)")
    print("  GET    /users/{user_id}")
    print("  PUT    /users/{user_id} (JSON: name, email, age)")
    print("  DELETE /users/{user_id}")
    print("  GET    /users?limit=10&offset=0")
    print("  POST   /items (JSON: name, price, in_stock)")
    print("  GET    /items/{item_id}")
    print("  GET    /items?in_stock=true&min_price=0&max_price=100")
    print("  GET    /search?q=term&page=1&limit=10&sort=relevance")
    print("  POST   /compute (JSON: n, operation)")
    print("  GET    /random")
    print()
    print("Ready for e2e tests")
    print()

    server.start()

    try:
        while server.is_running():
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nStopping server...")
        server.stop()
        print("Server stopped")
