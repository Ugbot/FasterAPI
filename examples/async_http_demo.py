"""
Async HTTP + Database Demo

Demonstrates future chaining with HTTP requests and database queries.
"""

import asyncio
from fasterapi import App, Depends, when_all
from fasterapi.core import Future
from fasterapi.http import Request, Response


# Create app
app = App(port=8000)

# Simulated database (would be real PgPool in production)
class FakeDB:
    """Simulated async database."""
    
    async def get_user(self, user_id: int):
        """Get user by ID."""
        await asyncio.sleep(0.01)  # Simulate I/O
        return {
            "id": user_id,
            "name": f"User {user_id}",
            "email": f"user{user_id}@example.com"
        }
    
    async def get_orders(self, user_id: int):
        """Get orders for user."""
        await asyncio.sleep(0.01)  # Simulate I/O
        return [
            {"id": 1, "user_id": user_id, "total": 99.99},
            {"id": 2, "user_id": user_id, "total": 149.99},
        ]
    
    async def get_products(self, order_id: int):
        """Get products in order."""
        await asyncio.sleep(0.01)  # Simulate I/O
        return [
            {"id": 1, "name": "Product A", "price": 49.99},
            {"id": 2, "name": "Product B", "price": 50.00},
        ]

db = FakeDB()


# Example 1: Basic async handler
@app.get("/user/{user_id}")
async def get_user(req: Request, res: Response):
    """Get user information (async/await style)."""
    user_id = int(req.get_path().split("/")[-1])
    
    # Async database query
    user = await db.get_user(user_id)
    
    return {"user": user}


# Example 2: Parallel queries
@app.get("/user/{user_id}/summary")
async def get_user_summary(req: Request, res: Response):
    """Get user with orders in parallel."""
    user_id = int(req.get_path().split("/")[-1])
    
    # Execute queries in parallel
    user, orders = await asyncio.gather(
        db.get_user(user_id),
        db.get_orders(user_id)
    )
    
    return {
        "user": user,
        "orders": orders,
        "order_count": len(orders)
    }


# Example 3: Explicit future chaining (power user)
@app.get("/user/{user_id}/fast")
def get_user_fast(req: Request, res: Response):
    """
    Get user with explicit future chaining (no await).
    
    This style can be faster by avoiding async/await overhead.
    """
    user_id = int(req.get_path().split("/")[-1])
    
    # Create future chain
    # In a real implementation, db methods would return Future objects
    # For now, we simulate with ready futures
    user_future = Future.make_ready({
        "id": user_id,
        "name": f"User {user_id}",
        "email": f"user{user_id}@example.com"
    })
    
    # Chain transformations
    result = (user_future
              .then(lambda user: {"user": user, "cached": True})
              .get())  # Blocking get (in real impl, would return future)
    
    return result


# Example 4: Composed operations
@app.get("/user/{user_id}/full")
async def get_user_full(req: Request, res: Response):
    """Get user with all related data."""
    user_id = int(req.get_path().split("/")[-1])
    
    # Step 1: Get user and orders in parallel
    user, orders = await asyncio.gather(
        db.get_user(user_id),
        db.get_orders(user_id)
    )
    
    # Step 2: Get products for all orders in parallel
    product_lists = await asyncio.gather(
        *[db.get_products(order["id"]) for order in orders]
    )
    
    # Step 3: Combine results
    orders_with_products = [
        {**order, "products": products}
        for order, products in zip(orders, product_lists)
    ]
    
    return {
        "user": user,
        "orders": orders_with_products,
        "total_spent": sum(order["total"] for order in orders)
    }


# Example 5: Error handling
@app.get("/user/{user_id}/safe")
async def get_user_safe(req: Request, res: Response):
    """Get user with error handling."""
    user_id = int(req.get_path().split("/")[-1])
    
    try:
        user = await db.get_user(user_id)
        return {"user": user}
    except Exception as e:
        return {
            "error": str(e),
            "user_id": user_id,
            "fallback": True
        }


# Health check
@app.get("/health")
def health(req: Request, res: Response):
    """Health check endpoint."""
    return {
        "status": "healthy",
        "async_enabled": True
    }


def main():
    """Run the demo server."""
    print("╔══════════════════════════════════════════╗")
    print("║  FasterAPI Async HTTP + DB Demo         ║")
    print("╚══════════════════════════════════════════╝")
    print()
    print("Server starting on http://localhost:8000")
    print()
    print("Try these endpoints:")
    print("  GET /user/123")
    print("  GET /user/123/summary")
    print("  GET /user/123/fast")
    print("  GET /user/123/full")
    print("  GET /health")
    print()
    
    app.run()


if __name__ == "__main__":
    main()

