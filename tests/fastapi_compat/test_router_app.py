"""
Tests for APIRouter app compatibility between FastAPI and FasterAPI.

Run with:
    TEST_FRAMEWORK=fastapi pytest tests/fastapi_compat/test_router_app.py -v
    TEST_FRAMEWORK=fasterapi pytest tests/fastapi_compat/test_router_app.py -v
"""

import os
import random
import string

import pytest
from apps.router_app import (
    app,
    clear_all_data,
    items_db,
    orders_db,
    request_counts,
    users_db,
)

FRAMEWORK = os.environ.get("TEST_FRAMEWORK", "fasterapi")

if FRAMEWORK == "fastapi":
    from fastapi.testclient import TestClient
else:
    try:
        from fasterapi.testclient import TestClient
    except ImportError:
        from starlette.testclient import TestClient


@pytest.fixture(autouse=True)
def clean_data():
    """Clear all data before each test."""
    clear_all_data()
    yield
    clear_all_data()


@pytest.fixture
def client():
    """Create test client."""
    return TestClient(app)


def random_string(length: int = 10) -> str:
    return "".join(random.choices(string.ascii_letters, k=length))


def random_price() -> float:
    return round(random.uniform(1, 1000), 2)


def random_email() -> str:
    return f"{random_string(8)}@{random_string(5)}.com"


class TestRootEndpoints:
    """Test root app endpoints."""

    def test_root(self, client):
        """Test root endpoint."""
        response = client.get("/")
        assert response.status_code == 200
        assert "message" in response.json()

    def test_health(self, client):
        """Test health endpoint."""
        response = client.get("/health")
        assert response.status_code == 200
        assert response.json()["status"] == "healthy"


class TestItemsRouter:
    """Test items router."""

    def test_list_items_empty(self, client):
        """Test listing items when empty."""
        response = client.get("/items")
        assert response.status_code == 200
        assert response.json() == []

    def test_create_item(self, client):
        """Test creating an item."""
        name = random_string(15)
        price = random_price()

        response = client.post(f"/items?name={name}&price={price}")
        assert response.status_code == 201
        data = response.json()
        assert data["name"] == name
        assert data["price"] == price
        assert "id" in data

    def test_get_item(self, client):
        """Test getting an item."""
        # Create item
        name = random_string(15)
        create_response = client.post(f"/items?name={name}&price=10.0")
        item_id = create_response.json()["id"]

        # Get item
        response = client.get(f"/items/{item_id}")
        assert response.status_code == 200
        assert response.json()["name"] == name

    def test_get_item_not_found(self, client):
        """Test 404 for non-existent item."""
        response = client.get("/items/nonexistent")
        assert response.status_code == 404

    def test_delete_item(self, client):
        """Test deleting an item."""
        # Create item
        create_response = client.post("/items?name=test&price=10.0")
        item_id = create_response.json()["id"]

        # Delete item
        response = client.delete(f"/items/{item_id}")
        assert response.status_code == 204

        # Verify deleted
        response = client.get(f"/items/{item_id}")
        assert response.status_code == 404

    def test_list_items_with_data(self, client):
        """Test listing items with data."""
        # Create multiple items
        for i in range(5):
            client.post(f"/items?name=item{i}&price={i * 10}")

        response = client.get("/items")
        assert response.status_code == 200
        assert len(response.json()) == 5


class TestUsersRouter:
    """Test users router."""

    def test_list_users_empty(self, client):
        """Test listing users when empty."""
        response = client.get("/users")
        assert response.status_code == 200
        assert response.json() == []

    def test_create_user(self, client):
        """Test creating a user."""
        username = random_string(10)
        email = random_email()

        response = client.post(f"/users?username={username}&email={email}")
        assert response.status_code == 201
        data = response.json()
        assert data["username"] == username
        assert data["email"] == email

    def test_get_user(self, client):
        """Test getting a user."""
        username = random_string(10)
        email = random_email()
        create_response = client.post(f"/users?username={username}&email={email}")
        user_id = create_response.json()["id"]

        response = client.get(f"/users/{user_id}")
        assert response.status_code == 200
        assert response.json()["username"] == username


class TestOrdersRouter:
    """Test orders router."""

    def test_create_order(self, client):
        """Test creating an order."""
        # Create items first
        item_ids = []
        for i in range(3):
            response = client.post(f"/items?name=item{i}&price={10 * (i + 1)}")
            item_ids.append(response.json()["id"])

        # Create order
        response = client.post("/orders", json=item_ids)
        assert response.status_code == 201
        data = response.json()
        assert data["user_id"] == "user-123"  # From dependency
        assert data["items"] == item_ids
        assert data["total"] == 60.0  # 10 + 20 + 30

    def test_create_order_invalid_item(self, client):
        """Test creating order with invalid item."""
        response = client.post("/orders", json=["invalid-item-id"])
        assert response.status_code == 400

    def test_list_orders(self, client):
        """Test listing orders."""
        # Create item and order
        item_response = client.post("/items?name=test&price=50.0")
        item_id = item_response.json()["id"]
        client.post("/orders", json=[item_id])

        response = client.get("/orders")
        assert response.status_code == 200
        assert len(response.json()) == 1


class TestAdminRouter:
    """Test admin router (nested with extra prefix)."""

    def test_admin_stats(self, client):
        """Test admin stats endpoint."""
        # Create some data
        client.post("/items?name=test&price=10.0")
        client.post("/users?username=user1&email=user1@test.com")

        response = client.get("/admin/admin/stats")
        assert response.status_code == 200
        data = response.json()
        assert data["items_count"] == 1
        assert data["users_count"] == 1

    def test_admin_clear(self, client):
        """Test admin clear endpoint."""
        # Create some data
        client.post("/items?name=test&price=10.0")

        # Clear
        response = client.delete("/admin/admin/clear")
        assert response.status_code == 200

        # Verify cleared
        assert len(items_db) == 0


class TestV2Router:
    """Test V2 API router (versioning)."""

    def test_v2_items(self, client):
        """Test V2 items endpoint."""
        # Create items
        client.post("/items?name=test1&price=10.0")
        client.post("/items?name=test2&price=20.0")

        response = client.get("/v2/items")
        assert response.status_code == 200
        data = response.json()
        assert data["version"] == 2
        assert data["count"] == 2
        assert len(data["data"]) == 2

    def test_v2_users(self, client):
        """Test V2 users endpoint."""
        client.post("/users?username=user1&email=user1@test.com")

        response = client.get("/v2/users")
        assert response.status_code == 200
        data = response.json()
        assert data["version"] == 2
        assert data["count"] == 1


class TestNestedRouters:
    """Test nested routers."""

    def test_parent_endpoint(self, client):
        """Test parent router endpoint."""
        response = client.get("/parent/info")
        assert response.status_code == 200
        assert "parent" in response.json()["message"].lower()

    def test_nested_child_endpoint(self, client):
        """Test nested child endpoint."""
        response = client.get("/parent/child/hello")
        assert response.status_code == 200
        assert "nested child" in response.json()["message"].lower()

    def test_nested_echo(self, client):
        """Test nested echo endpoint."""
        message = random_string(20)
        response = client.get(f"/parent/child/echo/{message}")
        assert response.status_code == 200
        assert response.json()["message"] == message


class TestRouterWithPrefix:
    """Test router included with different prefix."""

    def test_items_at_api_v1_prefix(self, client):
        """Test items router at /api/v1 prefix."""
        # Create via original prefix
        create_response = client.post("/items?name=test&price=10.0")
        item_id = create_response.json()["id"]

        # Access via /api/v1 prefix
        response = client.get(f"/api/v1/items/{item_id}")
        assert response.status_code == 200
        assert response.json()["name"] == "test"

    def test_list_items_at_both_prefixes(self, client):
        """Test that both prefixes share the same data."""
        # Create via /items
        client.post("/items?name=item1&price=10.0")

        # Access via /api/v1/items
        response = client.get("/api/v1/items")
        assert response.status_code == 200
        assert len(response.json()) == 1


class TestRouterDependencies:
    """Test router-level dependencies."""

    def test_items_dependency_called(self, client):
        """Test that items router dependency is called."""
        initial_count = request_counts["items"]

        client.get("/items")
        client.get("/items")
        client.get("/items")

        assert request_counts["items"] == initial_count + 3

    def test_users_dependency_called(self, client):
        """Test that users router dependency is called."""
        initial_count = request_counts["users"]

        client.get("/users")

        assert request_counts["users"] == initial_count + 1

    def test_admin_dependency_called(self, client):
        """Test that admin router dependency is called."""
        initial_count = request_counts["admin"]

        client.get("/admin/admin/stats")

        assert request_counts["admin"] == initial_count + 1

    def test_request_counts_endpoint(self, client):
        """Test request counts endpoint."""
        # Make some requests
        client.get("/items")
        client.get("/items")
        client.get("/users")

        response = client.get("/request-counts")
        assert response.status_code == 200
        data = response.json()
        assert data["items"] >= 2
        assert data["users"] >= 1


class TestComplexScenarios:
    """Test complex router scenarios."""

    def test_full_order_workflow(self, client):
        """Test complete order workflow across routers."""
        # Create user (users router)
        user_response = client.post("/users?username=buyer&email=buyer@test.com")
        assert user_response.status_code == 201

        # Create items (items router)
        item_ids = []
        for i in range(3):
            response = client.post(f"/items?name=product{i}&price={(i + 1) * 25}")
            item_ids.append(response.json()["id"])

        # Create order (orders router)
        order_response = client.post("/orders", json=item_ids)
        assert order_response.status_code == 201
        assert order_response.json()["total"] == 150.0  # 25 + 50 + 75

        # Check stats (admin router)
        stats_response = client.get("/admin/admin/stats")
        assert stats_response.json()["items_count"] == 3
        assert stats_response.json()["orders_count"] == 1

    def test_many_operations(self, client):
        """Test many operations across all routers."""
        # Create multiple items
        for i in range(10):
            client.post(f"/items?name=item{i}&price={i}")

        # Create multiple users
        for i in range(5):
            client.post(f"/users?username=user{i}&email=user{i}@test.com")

        # Verify counts
        items_response = client.get("/items")
        assert len(items_response.json()) == 10

        users_response = client.get("/users")
        assert len(users_response.json()) == 5

        v2_items = client.get("/v2/items")
        assert v2_items.json()["count"] == 10


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
