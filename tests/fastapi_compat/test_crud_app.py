"""
Tests for CRUD app compatibility between FastAPI and FasterAPI.

Run with:
    TEST_FRAMEWORK=fastapi pytest tests/fastapi_compat/test_crud_app.py -v
    TEST_FRAMEWORK=fasterapi pytest tests/fastapi_compat/test_crud_app.py -v
"""

import os
import random
import string
from typing import Dict, List
from uuid import uuid4

import pytest

# Import the app
from apps.crud_app import app, clear_databases, items_db, users_db

# Try to import TestClient from appropriate framework
FRAMEWORK = os.environ.get("TEST_FRAMEWORK", "fasterapi")

if FRAMEWORK == "fastapi":
    from fastapi.testclient import TestClient
else:
    # FasterAPI should provide compatible TestClient
    try:
        from fasterapi.testclient import TestClient
    except ImportError:
        # Fall back to starlette's TestClient which works with any ASGI app
        from starlette.testclient import TestClient


@pytest.fixture(autouse=True)
def clean_db():
    """Clear databases before each test."""
    clear_databases()
    yield
    clear_databases()


@pytest.fixture
def client():
    """Create test client."""
    return TestClient(app)


def random_string(length: int = 10) -> str:
    return "".join(random.choices(string.ascii_letters, k=length))


def random_email() -> str:
    return f"{random_string(8)}@{random_string(5)}.com"


def random_price() -> float:
    return round(random.uniform(1, 1000), 2)


def generate_item_data() -> Dict:
    return {
        "name": random_string(15),
        "description": random_string(50),
        "price": random_price(),
        "tax": round(random.uniform(0, 50), 2),
        "tags": [random_string(5) for _ in range(random.randint(1, 5))],
    }


def generate_user_data() -> Dict:
    return {
        "username": random_string(10),
        "email": random_email(),
        "full_name": f"{random_string(6)} {random_string(8)}",
        "password": random_string(12),
        "is_active": True,
    }


class TestItemsCRUD:
    """Test Items CRUD operations."""

    def test_create_item(self, client):
        """Test creating an item."""
        item_data = generate_item_data()
        response = client.post("/items", json=item_data)

        assert response.status_code == 201
        data = response.json()
        assert data["name"] == item_data["name"]
        assert data["price"] == item_data["price"]
        assert "id" in data
        assert "created_at" in data
        assert "updated_at" in data

    def test_create_item_validation_error(self, client):
        """Test validation error on invalid item data."""
        # Missing required field
        response = client.post("/items", json={"description": "test"})
        assert response.status_code == 422

        # Invalid price
        response = client.post(
            "/items",
            json={
                "name": "test",
                "price": -10,  # Must be > 0
            },
        )
        assert response.status_code == 422

        # Name too long
        response = client.post(
            "/items",
            json={
                "name": "x" * 200,  # max 100
                "price": 10,
            },
        )
        assert response.status_code == 422

    def test_get_item(self, client):
        """Test getting an item by ID."""
        # Create item first
        item_data = generate_item_data()
        create_response = client.post("/items", json=item_data)
        item_id = create_response.json()["id"]

        # Get item
        response = client.get(f"/items/{item_id}")
        assert response.status_code == 200
        assert response.json()["id"] == item_id
        assert response.json()["name"] == item_data["name"]

    def test_get_item_not_found(self, client):
        """Test 404 for non-existent item."""
        response = client.get(f"/items/{uuid4()}")
        assert response.status_code == 404
        assert "not found" in response.json()["detail"].lower()

    def test_list_items_empty(self, client):
        """Test listing items when empty."""
        response = client.get("/items")
        assert response.status_code == 200
        data = response.json()
        assert data["items"] == []
        assert data["total"] == 0

    def test_list_items_with_data(self, client):
        """Test listing items with data."""
        # Create multiple items
        num_items = random.randint(5, 15)
        for _ in range(num_items):
            client.post("/items", json=generate_item_data())

        response = client.get("/items")
        assert response.status_code == 200
        data = response.json()
        assert data["total"] == num_items
        assert len(data["items"]) <= data["size"]

    def test_list_items_pagination(self, client):
        """Test pagination."""
        # Create 25 items
        for _ in range(25):
            client.post("/items", json=generate_item_data())

        # Get first page
        response = client.get("/items?page=1&size=10")
        data = response.json()
        assert len(data["items"]) == 10
        assert data["page"] == 1
        assert data["pages"] == 3

        # Get second page
        response = client.get("/items?page=2&size=10")
        data = response.json()
        assert len(data["items"]) == 10
        assert data["page"] == 2

        # Get third page
        response = client.get("/items?page=3&size=10")
        data = response.json()
        assert len(data["items"]) == 5

    def test_list_items_filter_by_price(self, client):
        """Test filtering by price range."""
        # Create items with known prices
        for price in [10, 50, 100, 500, 1000]:
            item = generate_item_data()
            item["price"] = price
            client.post("/items", json=item)

        # Filter by min_price
        response = client.get("/items?min_price=100")
        data = response.json()
        assert all(item["price"] >= 100 for item in data["items"])

        # Filter by max_price
        response = client.get("/items?max_price=100")
        data = response.json()
        assert all(item["price"] <= 100 for item in data["items"])

        # Filter by range
        response = client.get("/items?min_price=50&max_price=500")
        data = response.json()
        assert all(50 <= item["price"] <= 500 for item in data["items"])

    def test_list_items_search(self, client):
        """Test search by name."""
        # Create items with specific names
        names = ["apple", "banana", "apricot", "blueberry", "avocado"]
        for name in names:
            item = generate_item_data()
            item["name"] = name
            client.post("/items", json=item)

        # Search for "ap"
        response = client.get("/items?search=ap")
        data = response.json()
        assert data["total"] == 2  # apple, apricot
        assert all("ap" in item["name"].lower() for item in data["items"])

    def test_update_item(self, client):
        """Test updating an item."""
        # Create item
        item_data = generate_item_data()
        create_response = client.post("/items", json=item_data)
        item_id = create_response.json()["id"]

        # Update item
        new_name = random_string(20)
        new_price = random_price()
        response = client.put(
            f"/items/{item_id}",
            json={
                "name": new_name,
                "price": new_price,
            },
        )

        assert response.status_code == 200
        data = response.json()
        assert data["name"] == new_name
        assert data["price"] == new_price
        # Original fields should be preserved
        assert data["description"] == item_data["description"]

    def test_update_item_not_found(self, client):
        """Test 404 when updating non-existent item."""
        response = client.put(f"/items/{uuid4()}", json={"name": "test"})
        assert response.status_code == 404

    def test_delete_item(self, client):
        """Test deleting an item."""
        # Create item
        item_data = generate_item_data()
        create_response = client.post("/items", json=item_data)
        item_id = create_response.json()["id"]

        # Delete item
        response = client.delete(f"/items/{item_id}")
        assert response.status_code == 204

        # Verify deleted
        response = client.get(f"/items/{item_id}")
        assert response.status_code == 404

    def test_delete_item_not_found(self, client):
        """Test 404 when deleting non-existent item."""
        response = client.delete(f"/items/{uuid4()}")
        assert response.status_code == 404

    def test_add_tags(self, client):
        """Test adding tags to an item."""
        # Create item
        item_data = generate_item_data()
        item_data["tags"] = ["original"]
        create_response = client.post("/items", json=item_data)
        item_id = create_response.json()["id"]

        # Add tags
        new_tags = ["new1", "new2"]
        response = client.post(f"/items/{item_id}/tags", json={"tags": new_tags})

        assert response.status_code == 200
        data = response.json()
        assert "original" in data["tags"]
        assert "new1" in data["tags"]
        assert "new2" in data["tags"]


class TestUsersCRUD:
    """Test Users CRUD operations."""

    def test_create_user(self, client):
        """Test creating a user."""
        user_data = generate_user_data()
        response = client.post("/users", json=user_data)

        assert response.status_code == 201
        data = response.json()
        assert data["username"] == user_data["username"]
        assert data["email"] == user_data["email"]
        assert "id" in data
        assert "password" not in data  # Password should not be returned
        assert "hashed_password" not in data

    def test_create_user_validation_error(self, client):
        """Test validation errors."""
        # Username too short
        user = generate_user_data()
        user["username"] = "ab"  # min 3
        response = client.post("/users", json=user)
        assert response.status_code == 422

        # Email too short (min_length=5)
        user = generate_user_data()
        user["email"] = "a@b"  # too short
        response = client.post("/users", json=user)
        assert response.status_code == 422

        # Password too short
        user = generate_user_data()
        user["password"] = "short"  # min 8
        response = client.post("/users", json=user)
        assert response.status_code == 422

    def test_create_user_duplicate_email(self, client):
        """Test duplicate email rejection."""
        user_data = generate_user_data()
        client.post("/users", json=user_data)

        # Try to create another user with same email
        user2 = generate_user_data()
        user2["email"] = user_data["email"]
        response = client.post("/users", json=user2)
        assert response.status_code == 400
        assert "email" in response.json()["detail"].lower()

    def test_create_user_duplicate_username(self, client):
        """Test duplicate username rejection."""
        user_data = generate_user_data()
        client.post("/users", json=user_data)

        # Try to create another user with same username
        user2 = generate_user_data()
        user2["username"] = user_data["username"]
        response = client.post("/users", json=user2)
        assert response.status_code == 400
        assert "username" in response.json()["detail"].lower()

    def test_get_user(self, client):
        """Test getting a user by ID."""
        user_data = generate_user_data()
        create_response = client.post("/users", json=user_data)
        user_id = create_response.json()["id"]

        response = client.get(f"/users/{user_id}")
        assert response.status_code == 200
        assert response.json()["id"] == user_id

    def test_get_user_not_found(self, client):
        """Test 404 for non-existent user."""
        response = client.get(f"/users/{uuid4()}")
        assert response.status_code == 404

    def test_list_users(self, client):
        """Test listing users."""
        # Create multiple users
        num_users = random.randint(5, 10)
        for _ in range(num_users):
            client.post("/users", json=generate_user_data())

        response = client.get("/users")
        assert response.status_code == 200
        assert len(response.json()) == num_users

    def test_list_users_active_only(self, client):
        """Test filtering active users."""
        # Create active and inactive users
        for i in range(5):
            user = generate_user_data()
            user["is_active"] = i % 2 == 0
            client.post("/users", json=user)

        # Get only active
        response = client.get("/users?active_only=true")
        assert response.status_code == 200
        users = response.json()
        assert all(u["is_active"] for u in users)

    def test_update_user(self, client):
        """Test updating a user."""
        user_data = generate_user_data()
        create_response = client.post("/users", json=user_data)
        user_id = create_response.json()["id"]

        new_name = random_string(15)
        response = client.put(
            f"/users/{user_id}",
            json={
                "full_name": new_name,
                "is_active": False,
            },
        )

        assert response.status_code == 200
        data = response.json()
        assert data["full_name"] == new_name
        assert data["is_active"] is False
        assert data["username"] == user_data["username"]  # Preserved

    def test_delete_user(self, client):
        """Test deleting a user."""
        user_data = generate_user_data()
        create_response = client.post("/users", json=user_data)
        user_id = create_response.json()["id"]

        response = client.delete(f"/users/{user_id}")
        assert response.status_code == 204

        response = client.get(f"/users/{user_id}")
        assert response.status_code == 404


class TestSystemEndpoints:
    """Test system endpoints."""

    def test_health_check(self, client):
        """Test health check endpoint."""
        response = client.get("/health")
        assert response.status_code == 200
        data = response.json()
        assert data["status"] == "healthy"
        assert "framework" in data

    def test_error_simulation(self, client):
        """Test error simulation endpoint."""
        for status_code in [400, 401, 403, 404, 500, 503]:
            response = client.get(f"/error/{status_code}")
            assert response.status_code == status_code
            assert str(status_code) in response.json()["detail"]


class TestComplexScenarios:
    """Test complex scenarios with multiple operations."""

    def test_crud_lifecycle(self, client):
        """Test complete CRUD lifecycle."""
        # Create
        item_data = generate_item_data()
        response = client.post("/items", json=item_data)
        assert response.status_code == 201
        item_id = response.json()["id"]

        # Read
        response = client.get(f"/items/{item_id}")
        assert response.status_code == 200
        assert response.json()["name"] == item_data["name"]

        # Update
        new_name = random_string(20)
        response = client.put(f"/items/{item_id}", json={"name": new_name})
        assert response.status_code == 200
        assert response.json()["name"] == new_name

        # Delete
        response = client.delete(f"/items/{item_id}")
        assert response.status_code == 204

        # Verify deleted
        response = client.get(f"/items/{item_id}")
        assert response.status_code == 404

    def test_concurrent_operations(self, client):
        """Test many operations in sequence."""
        created_ids = []

        # Create 50 items
        for _ in range(50):
            response = client.post("/items", json=generate_item_data())
            assert response.status_code == 201
            created_ids.append(response.json()["id"])

        # Update random items
        for _ in range(20):
            item_id = random.choice(created_ids)
            response = client.put(
                f"/items/{item_id}",
                json={
                    "name": random_string(15),
                },
            )
            assert response.status_code == 200

        # Delete some items
        to_delete = random.sample(created_ids, 10)
        for item_id in to_delete:
            response = client.delete(f"/items/{item_id}")
            assert response.status_code == 204
            created_ids.remove(item_id)

        # Verify final count
        response = client.get("/items?size=100")
        assert response.json()["total"] == 40

    def test_query_params_combinations(self, client):
        """Test various query parameter combinations."""
        # Create items with diverse data
        for i in range(30):
            item = generate_item_data()
            item["price"] = (i + 1) * 10  # 10, 20, 30, ..., 300
            item["name"] = f"item_{i:02d}_{random_string(5)}"
            client.post("/items", json=item)

        # Test various combinations
        test_cases = [
            {"page": 1, "size": 5},
            {"page": 2, "size": 10},
            {"min_price": 100, "max_price": 200},
            {"search": "item_0"},
            {"page": 1, "size": 5, "min_price": 50},
        ]

        for params in test_cases:
            response = client.get("/items", params=params)
            assert response.status_code == 200
            data = response.json()
            assert "items" in data
            assert "total" in data


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
