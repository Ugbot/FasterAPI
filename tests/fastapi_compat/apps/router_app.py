"""
APIRouter application for FastAPI compatibility testing.

Tests:
- APIRouter with prefix and tags
- Nested routers
- Router-level dependencies
- include_router merging
- Multiple routers

Can be run with either FastAPI or FasterAPI by setting TEST_FRAMEWORK env var.
"""

import os
from datetime import datetime
from typing import Dict, List, Optional
from uuid import uuid4

# Import framework based on environment
FRAMEWORK = os.environ.get("TEST_FRAMEWORK", "fasterapi")

if FRAMEWORK == "fastapi":
    from fastapi import APIRouter, Depends, FastAPI, HTTPException
    from pydantic import BaseModel
else:
    from pydantic import BaseModel

    from fasterapi import APIRouter, Depends, FastAPI, HTTPException


# Pydantic Models
class Item(BaseModel):
    id: str
    name: str
    price: float


class User(BaseModel):
    id: str
    username: str
    email: str


class Order(BaseModel):
    id: str
    user_id: str
    items: List[str]
    total: float
    created_at: datetime


# In-memory storage
items_db: Dict[str, dict] = {}
users_db: Dict[str, dict] = {}
orders_db: Dict[str, dict] = {}


# Request counter for testing dependencies
request_counts: Dict[str, int] = {"items": 0, "users": 0, "orders": 0, "admin": 0}


# Dependencies
async def count_items_requests():
    """Dependency that counts item requests."""
    request_counts["items"] += 1


async def count_users_requests():
    """Dependency that counts user requests."""
    request_counts["users"] += 1


async def count_orders_requests():
    """Dependency that counts order requests."""
    request_counts["orders"] += 1


async def admin_check():
    """Dependency that simulates admin check."""
    request_counts["admin"] += 1
    return {"admin": True}


async def get_current_user_id():
    """Dependency that returns a mock user ID."""
    return "user-123"


# Items Router
items_router = APIRouter(
    prefix="/items",
    tags=["items"],
    dependencies=[Depends(count_items_requests)],
)


@items_router.get("", response_model=List[Item])
async def list_items():
    """List all items."""
    return list(items_db.values())


@items_router.get("/{item_id}", response_model=Item)
async def get_item(item_id: str):
    """Get a single item."""
    if item_id not in items_db:
        raise HTTPException(status_code=404, detail="Item not found")
    return items_db[item_id]


@items_router.post("", response_model=Item, status_code=201)
async def create_item(name: str, price: float):
    """Create a new item."""
    item_id = str(uuid4())
    item = {"id": item_id, "name": name, "price": price}
    items_db[item_id] = item
    return item


@items_router.delete("/{item_id}", status_code=204)
async def delete_item(item_id: str):
    """Delete an item."""
    if item_id not in items_db:
        raise HTTPException(status_code=404, detail="Item not found")
    del items_db[item_id]


# Users Router
users_router = APIRouter(
    prefix="/users",
    tags=["users"],
    dependencies=[Depends(count_users_requests)],
)


@users_router.get("", response_model=List[User])
async def list_users():
    """List all users."""
    return list(users_db.values())


@users_router.get("/{user_id}", response_model=User)
async def get_user(user_id: str):
    """Get a single user."""
    if user_id not in users_db:
        raise HTTPException(status_code=404, detail="User not found")
    return users_db[user_id]


@users_router.post("", response_model=User, status_code=201)
async def create_user(username: str, email: str):
    """Create a new user."""
    user_id = str(uuid4())
    user = {"id": user_id, "username": username, "email": email}
    users_db[user_id] = user
    return user


# Orders Router (depends on users and items)
orders_router = APIRouter(
    prefix="/orders",
    tags=["orders"],
    dependencies=[Depends(count_orders_requests)],
)


@orders_router.get("", response_model=List[Order])
async def list_orders():
    """List all orders."""
    return list(orders_db.values())


@orders_router.get("/{order_id}", response_model=Order)
async def get_order(order_id: str):
    """Get a single order."""
    if order_id not in orders_db:
        raise HTTPException(status_code=404, detail="Order not found")
    return orders_db[order_id]


@orders_router.post("", response_model=Order, status_code=201)
async def create_order(
    item_ids: List[str],
    user_id: str = Depends(get_current_user_id),
):
    """Create a new order."""
    # Validate items exist and calculate total
    total = 0.0
    for item_id in item_ids:
        if item_id not in items_db:
            raise HTTPException(status_code=400, detail=f"Item {item_id} not found")
        total += items_db[item_id]["price"]

    order_id = str(uuid4())
    order = {
        "id": order_id,
        "user_id": user_id,
        "items": item_ids,
        "total": total,
        "created_at": datetime.utcnow(),
    }
    orders_db[order_id] = order
    return order


# Admin Router (nested under users)
admin_router = APIRouter(
    prefix="/admin",
    tags=["admin"],
    dependencies=[Depends(admin_check)],
)


@admin_router.get("/stats")
async def get_admin_stats():
    """Get admin stats."""
    return {
        "items_count": len(items_db),
        "users_count": len(users_db),
        "orders_count": len(orders_db),
        "request_counts": request_counts,
    }


@admin_router.delete("/clear")
async def clear_all():
    """Clear all data (admin only)."""
    items_db.clear()
    users_db.clear()
    orders_db.clear()
    return {"message": "All data cleared"}


# V2 API Router (for testing versioning)
v2_router = APIRouter(prefix="/v2", tags=["v2"])


@v2_router.get("/items")
async def v2_list_items():
    """V2 items endpoint with different response format."""
    return {
        "version": 2,
        "data": list(items_db.values()),
        "count": len(items_db),
    }


@v2_router.get("/users")
async def v2_list_users():
    """V2 users endpoint with different response format."""
    return {
        "version": 2,
        "data": list(users_db.values()),
        "count": len(users_db),
    }


# Nested router test
nested_parent = APIRouter(prefix="/parent", tags=["nested"])
nested_child = APIRouter(prefix="/child", tags=["nested-child"])


@nested_child.get("/hello")
async def nested_hello():
    """Deeply nested endpoint."""
    return {"message": "Hello from nested child"}


@nested_child.get("/echo/{message}")
async def nested_echo(message: str):
    """Echo message from nested child."""
    return {"message": message, "path": "/parent/child/echo"}


@nested_parent.get("/info")
async def parent_info():
    """Parent router info."""
    return {"message": "Hello from parent"}


# Include child in parent
nested_parent.include_router(nested_child)


def create_app() -> FastAPI:
    """Create and configure the FastAPI application."""

    app = FastAPI(
        title="Router Test App",
        description="APIRouter testing application",
        version="1.0.0",
    )

    # Include all routers
    app.include_router(items_router)
    app.include_router(users_router)
    app.include_router(orders_router)
    app.include_router(admin_router, prefix="/admin", tags=["admin-extra"])
    app.include_router(v2_router)
    app.include_router(nested_parent)

    # Also include with different prefix
    app.include_router(items_router, prefix="/api/v1", tags=["api-v1"])

    # Root endpoints
    @app.get("/")
    async def root():
        """Root endpoint."""
        return {"message": "Router Test App", "framework": FRAMEWORK}

    @app.get("/health")
    async def health():
        """Health check."""
        return {"status": "healthy"}

    @app.get("/request-counts")
    async def get_request_counts():
        """Get dependency request counts."""
        return request_counts

    return app


# Create app instance
app = create_app()


def clear_all_data():
    """Clear all data and reset counters (for testing)."""
    items_db.clear()
    users_db.clear()
    orders_db.clear()
    for key in request_counts:
        request_counts[key] = 0


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=8000)
