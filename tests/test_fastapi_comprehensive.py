"""
Comprehensive FastAPI Compatibility Test Suite

This test suite validates near-identical FastAPI compatibility:
- Query parameter extraction and type coercion
- Request body validation with Pydantic models
- Combined path + query + body parameters
- 422 error handling for validation failures
- All HTTP verbs (GET, POST, PUT, DELETE, PATCH)
- Randomized fuzzing and edge cases

These tests ensure FasterAPI is a drop-in replacement for FastAPI.
"""

import pytest
import requests
import random
import string
import json
from fasterapi.fastapi_compat import FastAPI
from fasterapi.fastapi_server import FastAPIServer
from pydantic import BaseModel, Field
from typing import Optional, List
import time
import threading


# ============================================================================
# Pydantic Models for Testing
# ============================================================================

class User(BaseModel):
    """User model for testing request/response validation."""
    name: str
    age: int
    email: str


class Product(BaseModel):
    """Product model with optional fields."""
    id: int
    name: str
    price: float
    description: Optional[str] = None
    tags: List[str] = []


class NestedAddress(BaseModel):
    """Nested model for complex validation."""
    street: str
    city: str
    zipcode: str


class UserWithAddress(BaseModel):
    """User with nested address."""
    name: str
    age: int
    address: NestedAddress


# ============================================================================
# Test: Query Parameters
# ============================================================================

class TestQueryParameters:
    """Test query parameter extraction, defaults, and optionals."""

    @pytest.fixture(scope="class")
    def app_and_server(self):
        app = FastAPI()

        @app.get("/search")
        def search(q: str, limit: int = 10, offset: int = 0, active: bool = True):
            """Search with query parameters."""
            return {
                "q": q,
                "limit": limit,
                "offset": offset,
                "active": active,
                "types": [type(q).__name__, type(limit).__name__, type(offset).__name__, type(active).__name__]
            }

        @app.get("/filter")
        def filter_items(min_price: float, max_price: float, category: str = "all"):
            """Filter with float params."""
            return {
                "min_price": min_price,
                "max_price": max_price,
                "category": category
            }

        server = FastAPIServer(app, port=8010, host="127.0.0.1")
        server.start()
        time.sleep(1.0)

        yield app, server
        server.stop()

    def test_query_with_defaults(self, app_and_server):
        """Test query parameters with default values."""
        test_cases = [
            ("?q=test", {"q": "test", "limit": 10, "offset": 0, "active": True}),
            ("?q=hello&limit=50", {"q": "hello", "limit": 50, "offset": 0, "active": True}),
            ("?q=world&limit=100&offset=20", {"q": "world", "limit": 100, "offset": 20, "active": True}),
            ("?q=python&active=false", {"q": "python", "limit": 10, "offset": 0, "active": False}),
        ]

        for query, expected in test_cases:
            response = requests.get(f"http://127.0.0.1:8010/search{query}")
            assert response.status_code == 200
            data = response.json()
            assert data["q"] == expected["q"]
            assert data["limit"] == expected["limit"]
            assert data["offset"] == expected["offset"]
            assert data["active"] == expected["active"]

    def test_float_query_params(self, app_and_server):
        """Test float type coercion in query params."""
        for _ in range(10):
            min_price = round(random.uniform(0, 100), 2)
            max_price = round(random.uniform(100, 1000), 2)

            response = requests.get(
                f"http://127.0.0.1:8010/filter?min_price={min_price}&max_price={max_price}"
            )

            assert response.status_code == 200
            data = response.json()
            assert abs(data["min_price"] - min_price) < 0.01
            assert abs(data["max_price"] - max_price) < 0.01
            assert data["category"] == "all"


# ============================================================================
# Test: Request Body Validation
# ============================================================================

class TestBodyValidation:
    """Test Pydantic model validation for request bodies."""

    @pytest.fixture(scope="class")
    def app_and_server(self):
        app = FastAPI()

        @app.post("/users", response_model=User)
        def create_user(user: User):
            """Create user with Pydantic validation."""
            return user.model_dump()

        @app.post("/products", response_model=Product)
        def create_product(product: Product):
            """Create product with optional fields."""
            return product.model_dump()

        @app.post("/users_with_address", response_model=UserWithAddress)
        def create_user_with_address(user: UserWithAddress):
            """Create user with nested model."""
            return user.model_dump()

        server = FastAPIServer(app, port=8011, host="127.0.0.1")
        server.start()
        time.sleep(1.0)

        yield app, server
        server.stop()

    def test_valid_user_creation(self, app_and_server):
        """Test valid user creation with Pydantic."""
        response = requests.post(
            "http://127.0.0.1:8011/users",
            json={"name": "John Doe", "age": 30, "email": "john@example.com"}
        )

        assert response.status_code == 200
        data = response.json()
        assert data["name"] == "John Doe"
        assert data["age"] == 30
        assert data["email"] == "john@example.com"

    def test_invalid_user_missing_field(self, app_and_server):
        """Test 422 error for missing required field."""
        response = requests.post(
            "http://127.0.0.1:8011/users",
            json={"name": "Jane", "age": 25}  # Missing email
        )

        assert response.status_code == 422
        data = response.json()
        assert "detail" in data  # FastAPI format

    def test_invalid_user_wrong_type(self, app_and_server):
        """Test 422 error for wrong type."""
        response = requests.post(
            "http://127.0.0.1:8011/users",
            json={"name": "Bob", "age": "not_a_number", "email": "bob@example.com"}
        )

        assert response.status_code == 422

    def test_optional_fields(self, app_and_server):
        """Test optional fields in Pydantic models."""
        # With optional fields
        response1 = requests.post(
            "http://127.0.0.1:8011/products",
            json={
                "id": 1,
                "name": "Widget",
                "price": 19.99,
                "description": "A useful widget",
                "tags": ["useful", "cheap"]
            }
        )
        assert response1.status_code == 200

        # Without optional fields
        response2 = requests.post(
            "http://127.0.0.1:8011/products",
            json={"id": 2, "name": "Gadget", "price": 29.99}
        )
        assert response2.status_code == 200

    def test_nested_model(self, app_and_server):
        """Test nested Pydantic models."""
        response = requests.post(
            "http://127.0.0.1:8011/users_with_address",
            json={
                "name": "Alice",
                "age": 28,
                "address": {
                    "street": "123 Main St",
                    "city": "Springfield",
                    "zipcode": "12345"
                }
            }
        )

        assert response.status_code == 200
        data = response.json()
        assert data["name"] == "Alice"
        assert data["address"]["city"] == "Springfield"


# ============================================================================
# Test: Combined Parameters (Path + Query + Body)
# ============================================================================

class TestCombinedParameters:
    """Test combinations of path, query, and body parameters."""

    @pytest.fixture(scope="class")
    def app_and_server(self):
        app = FastAPI()

        @app.post("/users/{user_id}/posts")
        def create_post(user_id: int, title: str, content: str, draft: bool = False):
            """Create post with path param, query params, and body."""
            return {
                "user_id": user_id,
                "title": title,
                "content": content,
                "draft": draft
            }

        @app.put("/items/{item_id}")
        def update_item(item_id: str, product: Product, notify: bool = True):
            """Update item with path, query, and body."""
            return {
                "item_id": item_id,
                "product": product.model_dump(),
                "notify": notify
            }

        server = FastAPIServer(app, port=8012, host="127.0.0.1")
        server.start()
        time.sleep(1.0)

        yield app, server
        server.stop()

    def test_path_and_query_and_body(self, app_and_server):
        """Test all parameter types together."""
        for _ in range(5):
            user_id = random.randint(1, 1000)
            title = f"Post {random.randint(1, 100)}"
            content = f"Content {random.randint(1, 100)}"

            response = requests.post(
                f"http://127.0.0.1:8012/users/{user_id}/posts?draft=true",
                json={"title": title, "content": content}
            )

            assert response.status_code == 200
            data = response.json()
            assert data["user_id"] == user_id
            assert data["title"] == title
            assert data["content"] == content
            assert data["draft"] is True


# ============================================================================
# Test: HTTP Verbs
# ============================================================================

class TestHTTPVerbs:
    """Test all HTTP verbs work correctly."""

    @pytest.fixture(scope="class")
    def app_and_server(self):
        app = FastAPI()

        @app.get("/resource/{id}")
        def get_resource(id: int):
            return {"method": "GET", "id": id}

        @app.post("/resource")
        def create_resource(name: str):
            return {"method": "POST", "name": name}

        @app.put("/resource/{id}")
        def update_resource(id: int, name: str):
            return {"method": "PUT", "id": id, "name": name}

        @app.delete("/resource/{id}")
        def delete_resource(id: int):
            return {"method": "DELETE", "id": id}

        @app.patch("/resource/{id}")
        def patch_resource(id: int, field: str):
            return {"method": "PATCH", "id": id, "field": field}

        server = FastAPIServer(app, port=8013, host="127.0.0.1")
        server.start()
        time.sleep(1.0)

        yield app, server
        server.stop()

    def test_all_http_verbs(self, app_and_server):
        """Test GET, POST, PUT, DELETE, PATCH."""
        # GET
        r1 = requests.get("http://127.0.0.1:8013/resource/1")
        assert r1.status_code == 200
        assert r1.json()["method"] == "GET"

        # POST
        r2 = requests.post("http://127.0.0.1:8013/resource", json={"name": "test"})
        assert r2.status_code == 200
        assert r2.json()["method"] == "POST"

        # PUT
        r3 = requests.put("http://127.0.0.1:8013/resource/1", json={"name": "updated"})
        assert r3.status_code == 200
        assert r3.json()["method"] == "PUT"

        # DELETE
        r4 = requests.delete("http://127.0.0.1:8013/resource/1")
        assert r4.status_code == 200
        assert r4.json()["method"] == "DELETE"

        # PATCH
        r5 = requests.patch("http://127.0.0.1:8013/resource/1", json={"field": "value"})
        assert r5.status_code == 200
        assert r5.json()["method"] == "PATCH"


# ============================================================================
# Test: Randomized Fuzzing
# ============================================================================

class TestRandomized:
    """Randomized tests for robustness."""

    @pytest.fixture(scope="class")
    def app_and_server(self):
        app = FastAPI()

        @app.get("/echo/{value}")
        def echo(value: str, count: int = 1, reverse: bool = False):
            result = value * count
            if reverse:
                result = result[::-1]
            return {"result": result, "length": len(result)}

        server = FastAPIServer(app, port=8014, host="127.0.0.1")
        server.start()
        time.sleep(1.0)

        yield app, server
        server.stop()

    def test_random_strings(self, app_and_server):
        """Test with 100 random strings."""
        for _ in range(100):
            # Generate random string
            length = random.randint(1, 20)
            value = ''.join(random.choices(string.ascii_letters + string.digits, k=length))
            count = random.randint(1, 5)
            reverse = random.choice([True, False])

            import urllib.parse
            encoded = urllib.parse.quote(value, safe='')

            response = requests.get(
                f"http://127.0.0.1:8014/echo/{encoded}?count={count}&reverse={reverse}"
            )

            assert response.status_code == 200
            data = response.json()

            expected = value * count
            if reverse:
                expected = expected[::-1]

            assert data["result"] == expected
            assert data["length"] == len(expected)


if __name__ == "__main__":
    pytest.main([__file__, "-v", "-s"])
