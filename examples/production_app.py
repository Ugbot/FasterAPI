"""
Production-Ready FasterAPI Application

Demonstrates real-world usage of Seastar-style futures in a production app.
Includes:
- Database connection pooling
- Async request handling
- Error handling & retry logic
- Parallel query execution
- Caching patterns
- Monitoring & metrics
"""

import asyncio
from typing import Optional, List, Dict, Any
from fasterapi import (
    App, Depends, Future, when_all, Reactor,
    retry_async, timeout_async, Pipeline
)
from fasterapi.http import Request, Response


# ============================================================================
# Configuration & Setup
# ============================================================================

class Config:
    """Application configuration."""
    DATABASE_URL = "postgres://localhost:5432/app"
    CACHE_TTL = 300  # 5 minutes
    REQUEST_TIMEOUT = 5.0  # 5 seconds
    MAX_RETRIES = 3
    RETRY_DELAY = 0.1


# ============================================================================
# Database Layer (Simulated)
# ============================================================================

class Database:
    """Simulated async database with connection pooling."""
    
    def __init__(self, url: str, pool_size: int = 10):
        self.url = url
        self.pool_size = pool_size
        print(f"📦 Database pool initialized: {pool_size} connections")
    
    async def get_user(self, user_id: int) -> Optional[Dict[str, Any]]:
        """Get user by ID."""
        await asyncio.sleep(0.01)  # Simulate I/O
        return {
            "id": user_id,
            "name": f"User {user_id}",
            "email": f"user{user_id}@example.com",
            "credits": 100
        }
    
    async def get_orders(self, user_id: int) -> List[Dict[str, Any]]:
        """Get orders for user."""
        await asyncio.sleep(0.02)  # Simulate I/O
        return [
            {"id": 1, "user_id": user_id, "total": 99.99, "status": "shipped"},
            {"id": 2, "user_id": user_id, "total": 149.99, "status": "pending"},
        ]
    
    async def get_products(self, order_id: int) -> List[Dict[str, Any]]:
        """Get products in order."""
        await asyncio.sleep(0.01)  # Simulate I/O
        return [
            {"id": 1, "name": "Widget", "price": 49.99, "qty": 1},
            {"id": 2, "name": "Gadget", "price": 50.00, "qty": 1},
        ]
    
    async def update_credits(self, user_id: int, delta: int) -> int:
        """Update user credits."""
        await asyncio.sleep(0.01)  # Simulate I/O
        return 100 + delta


class Cache:
    """Simulated in-memory cache."""
    
    def __init__(self):
        self.data: Dict[str, Any] = {}
        print("💾 Cache initialized")
    
    async def get(self, key: str) -> Optional[Any]:
        """Get cached value."""
        return self.data.get(key)
    
    async def set(self, key: str, value: Any, ttl: int = 300):
        """Set cached value."""
        self.data[key] = value
    
    async def delete(self, key: str):
        """Delete cached value."""
        self.data.pop(key, None)


# ============================================================================
# Application Setup
# ============================================================================

app = App(port=8000, enable_compression=True)

# Initialize services
db = Database(Config.DATABASE_URL, pool_size=10)
cache = Cache()

# Initialize reactor
Reactor.initialize()


# Dependencies
def get_db() -> Database:
    """Get database connection."""
    return db


def get_cache() -> Cache:
    """Get cache instance."""
    return cache


# ============================================================================
# Metrics & Monitoring
# ============================================================================

class Metrics:
    """Simple metrics collector."""
    
    def __init__(self):
        self.request_count = 0
        self.error_count = 0
        self.cache_hits = 0
        self.cache_misses = 0
    
    def incr_requests(self):
        self.request_count += 1
    
    def incr_errors(self):
        self.error_count += 1
    
    def incr_cache_hits(self):
        self.cache_hits += 1
    
    def incr_cache_misses(self):
        self.cache_misses += 1
    
    def get_stats(self) -> Dict[str, int]:
        return {
            "requests": self.request_count,
            "errors": self.error_count,
            "cache_hits": self.cache_hits,
            "cache_misses": self.cache_misses,
            "cache_hit_rate": (
                self.cache_hits / (self.cache_hits + self.cache_misses)
                if (self.cache_hits + self.cache_misses) > 0
                else 0.0
            )
        }


metrics = Metrics()


# ============================================================================
# Route Handlers
# ============================================================================

@app.get("/")
async def index(req: Request, res: Response):
    """Health check endpoint."""
    return {
        "status": "healthy",
        "service": "FasterAPI Production App",
        "reactor_cores": Reactor.num_cores()
    }


@app.get("/user/{user_id}")
async def get_user(
    req: Request,
    res: Response,
    db: Database = Depends(get_db),
    cache: Cache = Depends(get_cache)
):
    """
    Get user with caching.
    
    Demonstrates:
    - Cache-aside pattern
    - Async database queries
    - Error handling
    """
    metrics.incr_requests()
    
    # Extract user_id from path
    path_parts = req.get_path().split('/')
    user_id = int(path_parts[-1])
    
    # Check cache first
    cache_key = f"user:{user_id}"
    cached = await cache.get(cache_key)
    
    if cached:
        metrics.incr_cache_hits()
        return {"user": cached, "cached": True}
    
    # Cache miss - fetch from database
    metrics.incr_cache_misses()
    
    try:
        user = await timeout_async(
            Future.make_ready(await db.get_user(user_id)),
            timeout_seconds=Config.REQUEST_TIMEOUT
        )
        
        # Cache the result
        await cache.set(cache_key, user, ttl=Config.CACHE_TTL)
        
        return {"user": user, "cached": False}
    
    except asyncio.TimeoutError:
        metrics.incr_errors()
        return {"error": "Request timeout"}, 504
    except Exception as e:
        metrics.incr_errors()
        return {"error": str(e)}, 500


@app.get("/user/{user_id}/orders")
async def get_user_orders(
    req: Request,
    res: Response,
    db: Database = Depends(get_db)
):
    """
    Get user with all orders.
    
    Demonstrates:
    - Parallel query execution
    - Data composition
    - when_all combinator
    """
    metrics.incr_requests()
    
    path_parts = req.get_path().split('/')
    user_id = int(path_parts[2])
    
    try:
        # Execute queries in parallel
        user_future = Future.make_ready(await db.get_user(user_id))
        orders_future = Future.make_ready(await db.get_orders(user_id))
        
        user, orders = await when_all([user_future, orders_future])
        
        return {
            "user": user,
            "orders": orders,
            "order_count": len(orders),
            "total_spent": sum(o["total"] for o in orders)
        }
    
    except Exception as e:
        metrics.incr_errors()
        return {"error": str(e)}, 500


@app.get("/user/{user_id}/full")
async def get_user_full(
    req: Request,
    res: Response,
    db: Database = Depends(get_db)
):
    """
    Get user with orders and products.
    
    Demonstrates:
    - Multi-level parallel execution
    - Complex data composition
    - Nested async operations
    """
    metrics.incr_requests()
    
    path_parts = req.get_path().split('/')
    user_id = int(path_parts[2])
    
    try:
        # Step 1: Get user and orders in parallel
        user = await db.get_user(user_id)
        orders = await db.get_orders(user_id)
        
        # Step 2: Get products for all orders in parallel
        product_futures = [
            Future.make_ready(await db.get_products(order["id"]))
            for order in orders
        ]
        product_lists = await when_all(product_futures)
        
        # Step 3: Combine everything
        orders_with_products = [
            {**order, "products": products}
            for order, products in zip(orders, product_lists)
        ]
        
        return {
            "user": user,
            "orders": orders_with_products,
            "stats": {
                "total_orders": len(orders),
                "total_products": sum(len(p) for p in product_lists),
                "total_spent": sum(o["total"] for o in orders)
            }
        }
    
    except Exception as e:
        metrics.incr_errors()
        return {"error": str(e)}, 500


@app.post("/user/{user_id}/credits")
async def update_user_credits(
    req: Request,
    res: Response,
    db: Database = Depends(get_db),
    cache: Cache = Depends(get_cache)
):
    """
    Update user credits with retry.
    
    Demonstrates:
    - Retry logic with exponential backoff
    - Cache invalidation
    - Error recovery
    """
    metrics.incr_requests()
    
    path_parts = req.get_path().split('/')
    user_id = int(path_parts[2])
    
    # Parse request body (simulated)
    delta = 10  # In real app, parse from req.get_body()
    
    try:
        # Update with retry
        async def update_operation():
            new_credits = await db.update_credits(user_id, delta)
            return Future.make_ready(new_credits)
        
        new_credits = await retry_async(
            update_operation,
            max_retries=Config.MAX_RETRIES,
            delay=Config.RETRY_DELAY,
            backoff=2.0
        )
        
        # Invalidate cache
        await cache.delete(f"user:{user_id}")
        
        return {
            "user_id": user_id,
            "credits": new_credits,
            "delta": delta
        }
    
    except Exception as e:
        metrics.incr_errors()
        return {"error": str(e)}, 500


@app.get("/pipeline/demo")
async def pipeline_demo(req: Request, res: Response):
    """
    Demonstrate pipeline pattern.
    
    Shows how to compose multiple async operations.
    """
    metrics.incr_requests()
    
    # Create a data processing pipeline
    pipeline = (
        Pipeline()
        .add(lambda: "  hello world  ")
        .add(lambda x: x.strip())
        .add(lambda x: x.upper())
        .add(lambda x: x.split())
        .add(lambda x: {"words": x, "count": len(x)})
    )
    
    result = await pipeline.execute()
    return result


@app.get("/metrics")
def get_metrics(req: Request, res: Response):
    """
    Get application metrics.
    
    Demonstrates:
    - Synchronous endpoint (no async needed)
    - Metrics collection
    """
    return {
        "metrics": metrics.get_stats(),
        "reactor": {
            "cores": Reactor.num_cores(),
            "current_core": Reactor.current_core()
        }
    }


# ============================================================================
# Lifecycle Hooks
# ============================================================================

@app.on_event("startup")
def startup():
    """Application startup."""
    print()
    print("╔══════════════════════════════════════════════════════════╗")
    print("║     FasterAPI Production Application                    ║")
    print("╚══════════════════════════════════════════════════════════╝")
    print()
    print(f"🚀 Starting server on http://localhost:{app.port}")
    print(f"⚡ Reactor: {Reactor.num_cores()} cores initialized")
    print(f"📦 Database: Connection pool ready")
    print(f"💾 Cache: In-memory cache ready")
    print()
    print("📍 Available endpoints:")
    print("   GET  / - Health check")
    print("   GET  /user/{id} - Get user (with caching)")
    print("   GET  /user/{id}/orders - Get user with orders")
    print("   GET  /user/{id}/full - Get full user data")
    print("   POST /user/{id}/credits - Update credits (with retry)")
    print("   GET  /pipeline/demo - Pipeline demonstration")
    print("   GET  /metrics - Application metrics")
    print()


@app.on_event("shutdown")
def shutdown():
    """Application shutdown."""
    print()
    print("🛑 Shutting down...")
    print(f"📊 Final metrics: {metrics.get_stats()}")
    Reactor.shutdown()
    print("✅ Shutdown complete")


# ============================================================================
# Main
# ============================================================================

def main():
    """Run the production application."""
    app.run()


if __name__ == "__main__":
    main()

