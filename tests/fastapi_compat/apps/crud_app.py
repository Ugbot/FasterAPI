"""
Basic CRUD application for FastAPI compatibility testing.

This app implements a typical REST API with:
- GET, POST, PUT, DELETE endpoints
- Pydantic model validation
- Query parameters with validation
- Path parameters
- Response models
- HTTPException handling

Can be run with either FastAPI or FasterAPI by setting TEST_FRAMEWORK env var.
"""

import os
from datetime import datetime
from typing import Dict, List, Optional
from uuid import uuid4

# Import framework based on environment
FRAMEWORK = os.environ.get("TEST_FRAMEWORK", "fasterapi")

if FRAMEWORK == "fastapi":
    from fastapi import Body, FastAPI, HTTPException, Path, Query
    from fastapi.responses import JSONResponse
    from pydantic import BaseModel, Field
else:
    from pydantic import BaseModel, Field

    from fasterapi import Body, FastAPI, HTTPException, Path, Query
    from fasterapi.responses import JSONResponse


# Pydantic Models
class ItemBase(BaseModel):
    name: str = Field(..., min_length=1, max_length=100)
    description: Optional[str] = Field(None, max_length=500)
    price: float = Field(..., gt=0)
    tax: Optional[float] = Field(None, ge=0)
    tags: List[str] = Field(default_factory=list)


class ItemCreate(ItemBase):
    pass


class ItemUpdate(BaseModel):
    name: Optional[str] = Field(None, min_length=1, max_length=100)
    description: Optional[str] = Field(None, max_length=500)
    price: Optional[float] = Field(None, gt=0)
    tax: Optional[float] = Field(None, ge=0)
    tags: Optional[List[str]] = None


class Item(ItemBase):
    id: str
    created_at: datetime
    updated_at: datetime

    class Config:
        from_attributes = True


class UserBase(BaseModel):
    username: str = Field(..., min_length=3, max_length=50)
    email: str = Field(
        ..., min_length=5
    )  # Simple string validation instead of EmailStr
    full_name: Optional[str] = None
    is_active: bool = True


class UserCreate(UserBase):
    password: str = Field(..., min_length=8)


class UserUpdate(BaseModel):
    username: Optional[str] = Field(None, min_length=3, max_length=50)
    email: Optional[str] = Field(None, min_length=5)
    full_name: Optional[str] = None
    is_active: Optional[bool] = None


class User(UserBase):
    id: str
    created_at: datetime

    class Config:
        from_attributes = True


class PaginatedResponse(BaseModel):
    items: List[Item]
    total: int
    page: int
    size: int
    pages: int


# In-memory storage
items_db: Dict[str, dict] = {}
users_db: Dict[str, dict] = {}


def create_app() -> FastAPI:
    """Create and configure the FastAPI application."""

    app = FastAPI(
        title="CRUD Test App",
        description="A test application for FastAPI compatibility testing",
        version="1.0.0",
    )

    # Items CRUD endpoints

    @app.post("/items", response_model=Item, status_code=201, tags=["items"])
    async def create_item(item: ItemCreate):
        """Create a new item."""
        now = datetime.utcnow()
        item_id = str(uuid4())
        item_data = {
            **item.model_dump(),
            "id": item_id,
            "created_at": now,
            "updated_at": now,
        }
        items_db[item_id] = item_data
        return item_data

    @app.get("/items", response_model=PaginatedResponse, tags=["items"])
    async def list_items(
        page: int = Query(1, ge=1, description="Page number"),
        size: int = Query(10, ge=1, le=100, description="Items per page"),
        search: Optional[str] = Query(None, description="Search in name"),
        min_price: Optional[float] = Query(None, ge=0, description="Minimum price"),
        max_price: Optional[float] = Query(None, ge=0, description="Maximum price"),
        tags: Optional[List[str]] = Query(None, description="Filter by tags"),
    ):
        """List items with pagination and filtering."""
        filtered = list(items_db.values())

        # Apply filters
        if search:
            filtered = [i for i in filtered if search.lower() in i["name"].lower()]
        if min_price is not None:
            filtered = [i for i in filtered if i["price"] >= min_price]
        if max_price is not None:
            filtered = [i for i in filtered if i["price"] <= max_price]
        if tags:
            filtered = [i for i in filtered if any(t in i["tags"] for t in tags)]

        total = len(filtered)
        pages = (total + size - 1) // size
        start = (page - 1) * size
        end = start + size

        return {
            "items": filtered[start:end],
            "total": total,
            "page": page,
            "size": size,
            "pages": pages,
        }

    @app.get("/items/{item_id}", response_model=Item, tags=["items"])
    async def get_item(
        item_id: str = Path(..., description="The item ID"),
    ):
        """Get a single item by ID."""
        if item_id not in items_db:
            raise HTTPException(status_code=404, detail="Item not found")
        return items_db[item_id]

    @app.put("/items/{item_id}", response_model=Item, tags=["items"])
    async def update_item(
        item_id: str = Path(..., description="The item ID"),
        item: ItemUpdate = Body(...),
    ):
        """Update an existing item."""
        if item_id not in items_db:
            raise HTTPException(status_code=404, detail="Item not found")

        stored = items_db[item_id]
        update_data = item.model_dump(exclude_unset=True)

        for key, value in update_data.items():
            stored[key] = value

        stored["updated_at"] = datetime.utcnow()
        items_db[item_id] = stored
        return stored

    @app.delete("/items/{item_id}", status_code=204, tags=["items"])
    async def delete_item(
        item_id: str = Path(..., description="The item ID"),
    ):
        """Delete an item."""
        if item_id not in items_db:
            raise HTTPException(status_code=404, detail="Item not found")
        del items_db[item_id]
        return None

    @app.post("/items/{item_id}/tags", response_model=Item, tags=["items"])
    async def add_item_tags(
        item_id: str = Path(..., description="The item ID"),
        tags: List[str] = Body(..., embed=True),
    ):
        """Add tags to an item."""
        if item_id not in items_db:
            raise HTTPException(status_code=404, detail="Item not found")

        stored = items_db[item_id]
        existing_tags = set(stored["tags"])
        existing_tags.update(tags)
        stored["tags"] = list(existing_tags)
        stored["updated_at"] = datetime.utcnow()
        return stored

    # Users CRUD endpoints

    @app.post("/users", response_model=User, status_code=201, tags=["users"])
    async def create_user(user: UserCreate):
        """Create a new user."""
        # Check for duplicate email
        for existing in users_db.values():
            if existing["email"] == user.email:
                raise HTTPException(status_code=400, detail="Email already registered")
            if existing["username"] == user.username:
                raise HTTPException(status_code=400, detail="Username already taken")

        user_id = str(uuid4())
        user_data = {
            **user.model_dump(exclude={"password"}),
            "id": user_id,
            "created_at": datetime.utcnow(),
            "hashed_password": f"hashed_{user.password}",  # Fake hash
        }
        users_db[user_id] = user_data
        return user_data

    @app.get("/users", response_model=List[User], tags=["users"])
    async def list_users(
        skip: int = Query(0, ge=0),
        limit: int = Query(100, ge=1, le=100),
        active_only: bool = Query(False),
    ):
        """List all users with pagination."""
        users = list(users_db.values())
        if active_only:
            users = [u for u in users if u["is_active"]]
        return users[skip : skip + limit]

    @app.get("/users/{user_id}", response_model=User, tags=["users"])
    async def get_user(user_id: str = Path(..., description="The user ID")):
        """Get a single user by ID."""
        if user_id not in users_db:
            raise HTTPException(status_code=404, detail="User not found")
        return users_db[user_id]

    @app.put("/users/{user_id}", response_model=User, tags=["users"])
    async def update_user(
        user_id: str = Path(...),
        user: UserUpdate = Body(...),
    ):
        """Update an existing user."""
        if user_id not in users_db:
            raise HTTPException(status_code=404, detail="User not found")

        stored = users_db[user_id]
        update_data = user.model_dump(exclude_unset=True)

        # Check for duplicate email/username
        if "email" in update_data:
            for uid, existing in users_db.items():
                if uid != user_id and existing["email"] == update_data["email"]:
                    raise HTTPException(
                        status_code=400, detail="Email already registered"
                    )

        if "username" in update_data:
            for uid, existing in users_db.items():
                if uid != user_id and existing["username"] == update_data["username"]:
                    raise HTTPException(
                        status_code=400, detail="Username already taken"
                    )

        for key, value in update_data.items():
            stored[key] = value

        users_db[user_id] = stored
        return stored

    @app.delete("/users/{user_id}", status_code=204, tags=["users"])
    async def delete_user(user_id: str = Path(...)):
        """Delete a user."""
        if user_id not in users_db:
            raise HTTPException(status_code=404, detail="User not found")
        del users_db[user_id]
        return None

    # Health check
    @app.get("/health", tags=["system"])
    async def health_check():
        """Health check endpoint."""
        return {
            "status": "healthy",
            "framework": FRAMEWORK,
            "items_count": len(items_db),
            "users_count": len(users_db),
        }

    # Error simulation for testing
    @app.get("/error/{status_code}", tags=["system"])
    async def simulate_error(status_code: int = Path(..., ge=400, le=599)):
        """Simulate an HTTP error for testing."""
        raise HTTPException(
            status_code=status_code, detail=f"Simulated {status_code} error"
        )

    return app


# Create app instance
app = create_app()


def clear_databases():
    """Clear all in-memory databases (for testing)."""
    items_db.clear()
    users_db.clear()


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=8000)
