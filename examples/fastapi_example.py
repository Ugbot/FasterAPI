"""
FastAPI-compatible example application using FasterAPI.

This demonstrates the drop-in replacement API with:
- Multiple routes with different HTTP verbs
- Path and query parameters
- Pydantic models for validation
- Automatic OpenAPI generation
- Interactive documentation at /docs and /redoc
"""

import sys
import random
from typing import List, Optional
from pydantic import BaseModel

# Import FasterAPI with FastAPI-compatible API
from fasterapi.fastapi_compat import FastAPI

# Create FastAPI-compatible app
app = FastAPI(
    title="FasterAPI Example",
    version="1.0.0",
    description="High-performance FastAPI drop-in replacement",
    docs_url="/docs",
    redoc_url="/redoc",
    openapi_url="/openapi.json"
)


# ============================================================================
# Pydantic Models
# ============================================================================

class Item(BaseModel):
    """Item model with validation."""
    name: str
    description: Optional[str] = None
    price: float
    tax: Optional[float] = None
    tags: List[str] = []


class User(BaseModel):
    """User model with validation."""
    id: int
    username: str
    email: str
    full_name: Optional[str] = None
    disabled: Optional[bool] = False


class CreateUserRequest(BaseModel):
    """Request model for creating a user."""
    username: str
    email: str
    full_name: Optional[str] = None


class ItemStats(BaseModel):
    """Statistics for items."""
    total_items: int
    average_price: float
    most_common_tag: str


# ============================================================================
# In-memory data storage (for demo)
# ============================================================================

# Generate randomized test data
items_db = {}
users_db = {}

def generate_random_items(count: int = 10):
    """Generate random items for testing."""
    item_names = ["Widget", "Gadget", "Doohickey", "Thingamajig", "Gizmo",
                  "Contraption", "Device", "Tool", "Appliance", "Instrument"]
    tags_pool = ["electronics", "tools", "home", "office", "outdoor", "premium", "sale"]

    for i in range(count):
        item_id = i + 1
        items_db[item_id] = {
            "id": item_id,
            "name": f"{random.choice(item_names)} #{item_id}",
            "description": f"A high-quality item with random price ${random.uniform(10, 500):.2f}",
            "price": round(random.uniform(10, 500), 2),
            "tax": round(random.uniform(0, 50), 2) if random.random() > 0.3 else None,
            "tags": random.sample(tags_pool, k=random.randint(1, 3))
        }

def generate_random_users(count: int = 5):
    """Generate random users for testing."""
    first_names = ["Alice", "Bob", "Charlie", "Diana", "Eve", "Frank", "Grace", "Hank"]
    last_names = ["Smith", "Johnson", "Williams", "Brown", "Jones", "Garcia", "Miller"]

    for i in range(count):
        user_id = i + 1
        first = random.choice(first_names)
        last = random.choice(last_names)
        users_db[user_id] = {
            "id": user_id,
            "username": f"{first.lower()}{user_id}",
            "email": f"{first.lower()}.{last.lower()}@example.com",
            "full_name": f"{first} {last}",
            "disabled": random.random() < 0.2  # 20% chance of being disabled
        }

# Initialize with random data
generate_random_items(20)
generate_random_users(10)


# ============================================================================
# Routes
# ============================================================================

@app.get("/")
def read_root():
    """Root endpoint - health check."""
    return {
        "message": "Welcome to FasterAPI!",
        "version": "1.0.0",
        "docs": "/docs",
        "redoc": "/redoc",
        "openapi": "/openapi.json"
    }


@app.get("/health")
def health_check():
    """Health check endpoint."""
    return {
        "status": "healthy",
        "items_count": len(items_db),
        "users_count": len(users_db)
    }


# ============================================================================
# Items Endpoints
# ============================================================================

@app.get("/items", response_model=None, tags=["items"])
def list_items(skip: int = 0, limit: int = 10, tag: Optional[str] = None):
    """
    List all items with pagination.

    - **skip**: Number of items to skip
    - **limit**: Maximum number of items to return
    - **tag**: Optional tag filter
    """
    items = list(items_db.values())

    # Filter by tag if provided
    if tag:
        items = [item for item in items if tag in item.get("tags", [])]

    # Apply pagination
    return items[skip:skip + limit]


@app.get("/items/{item_id}", response_model=None, tags=["items"])
def get_item(item_id: int, include_stats: bool = False):
    """
    Get a specific item by ID.

    - **item_id**: The ID of the item to retrieve
    - **include_stats**: Whether to include item statistics
    """
    if item_id not in items_db:
        return {"error": "Item not found"}, 404

    item = items_db[item_id]

    if include_stats:
        # Calculate some stats
        all_prices = [i["price"] for i in items_db.values()]
        return {
            "item": item,
            "stats": {
                "position": sorted(all_prices, reverse=True).index(item["price"]) + 1,
                "total_items": len(items_db),
                "avg_price": sum(all_prices) / len(all_prices)
            }
        }

    return item


@app.post("/items", response_model=None, tags=["items"])
def create_item(item: Item):
    """
    Create a new item.

    Accepts an Item model with validation.
    """
    # Generate new ID
    new_id = max(items_db.keys()) + 1 if items_db else 1

    item_data = item.dict()
    item_data["id"] = new_id
    items_db[new_id] = item_data

    return {"id": new_id, "item": item_data}


@app.put("/items/{item_id}", response_model=None, tags=["items"])
def update_item(item_id: int, item: Item):
    """
    Update an existing item.

    - **item_id**: The ID of the item to update
    """
    if item_id not in items_db:
        return {"error": "Item not found"}, 404

    item_data = item.dict()
    item_data["id"] = item_id
    items_db[item_id] = item_data

    return {"id": item_id, "item": item_data}


@app.delete("/items/{item_id}", tags=["items"])
def delete_item(item_id: int):
    """
    Delete an item.

    - **item_id**: The ID of the item to delete
    """
    if item_id not in items_db:
        return {"error": "Item not found"}, 404

    deleted_item = items_db.pop(item_id)
    return {"deleted": True, "item": deleted_item}


@app.get("/items/stats/summary", tags=["items"])
def get_items_stats():
    """Get aggregate statistics for all items."""
    if not items_db:
        return {"error": "No items found"}

    prices = [item["price"] for item in items_db.values()]
    all_tags = []
    for item in items_db.values():
        all_tags.extend(item.get("tags", []))

    from collections import Counter
    tag_counts = Counter(all_tags)
    most_common_tag = tag_counts.most_common(1)[0][0] if tag_counts else "none"

    return {
        "total_items": len(items_db),
        "total_value": sum(prices),
        "average_price": sum(prices) / len(prices),
        "min_price": min(prices),
        "max_price": max(prices),
        "most_common_tag": most_common_tag,
        "tag_distribution": dict(tag_counts)
    }


# ============================================================================
# Users Endpoints
# ============================================================================

@app.get("/users", tags=["users"])
def list_users(skip: int = 0, limit: int = 10, active_only: bool = False):
    """
    List all users with pagination.

    - **skip**: Number of users to skip
    - **limit**: Maximum number of users to return
    - **active_only**: Only return active (non-disabled) users
    """
    users = list(users_db.values())

    # Filter by active status if requested
    if active_only:
        users = [u for u in users if not u.get("disabled", False)]

    # Apply pagination
    return users[skip:skip + limit]


@app.get("/users/{user_id}", tags=["users"])
def get_user(user_id: int):
    """
    Get a specific user by ID.

    - **user_id**: The ID of the user to retrieve
    """
    if user_id not in users_db:
        return {"error": "User not found"}, 404

    return users_db[user_id]


@app.post("/users", tags=["users"])
def create_user(user: CreateUserRequest):
    """
    Create a new user.

    Accepts a CreateUserRequest model with validation.
    """
    # Generate new ID
    new_id = max(users_db.keys()) + 1 if users_db else 1

    user_data = {
        "id": new_id,
        "username": user.username,
        "email": user.email,
        "full_name": user.full_name,
        "disabled": False
    }
    users_db[new_id] = user_data

    return {"id": new_id, "user": user_data}


@app.patch("/users/{user_id}/disable", tags=["users"])
def disable_user(user_id: int):
    """
    Disable a user account.

    - **user_id**: The ID of the user to disable
    """
    if user_id not in users_db:
        return {"error": "User not found"}, 404

    users_db[user_id]["disabled"] = True
    return {"disabled": True, "user": users_db[user_id]}


# ============================================================================
# Main
# ============================================================================

if __name__ == "__main__":
    print("=" * 80)
    print("FasterAPI Example Application")
    print("=" * 80)
    print(f"Items in database: {len(items_db)}")
    print(f"Users in database: {len(users_db)}")
    print()
    print("Registered routes:")
    for route in app.routes():
        print(f"  {route['method']:6s} {route['path_pattern']}")
    print()
    print("Documentation endpoints:")
    print("  GET    /docs       - Swagger UI")
    print("  GET    /redoc      - ReDoc")
    print("  GET    /openapi.json - OpenAPI spec")
    print("=" * 80)

    # Note: Actual server integration would go here
    # For now, this demonstrates the API registration
