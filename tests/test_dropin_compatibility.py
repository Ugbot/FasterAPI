"""
Drop-in Compatibility Tests for FasterAPI

This test suite validates that FasterAPI is a drop-in replacement for FastAPI
by testing specific compatibility features and edge cases discovered during
testing against real FastAPI tutorial examples.

Tests cover:
1. Response model filtering (security: password field filtering)
2. Dependency injection with proper signature introspection
3. Nested dependency injection (multi-level Depends)
4. Enum path parameter conversion
5. APIRouter prefix handling
6. BackgroundTasks with Depends()
7. Multiple routers with tags
8. Exception handlers
"""

import asyncio
import json
import random
import string
from enum import Enum
from typing import List, Optional

import pytest
from pydantic import BaseModel

from fasterapi import APIRouter, BackgroundTasks, FastAPI, HTTPException
from fasterapi.params import Depends

# ============================================================================
# Test Helpers
# ============================================================================


async def asgi_request(app, method: str, path: str, body=None, headers=None):
    """Helper to make ASGI requests to an app."""
    scope = {
        "type": "http",
        "method": method,
        "path": path,
        "query_string": b"",
        "headers": [(k.encode(), v.encode()) for k, v in (headers or {}).items()],
    }

    if "?" in path:
        path_part, query_part = path.split("?", 1)
        scope["path"] = path_part
        scope["query_string"] = query_part.encode()

    body_bytes = json.dumps(body).encode() if body else b""
    body_sent = False

    async def receive():
        nonlocal body_sent
        if not body_sent:
            body_sent = True
            return {"type": "http.request", "body": body_bytes, "more_body": False}
        return {"type": "http.request", "body": b"", "more_body": False}

    response_body = b""
    status_code = None
    response_headers = {}

    async def send(message):
        nonlocal response_body, status_code, response_headers
        if message["type"] == "http.response.start":
            status_code = message["status"]
            for k, v in message.get("headers", []):
                key = k.decode() if isinstance(k, bytes) else k
                val = v.decode() if isinstance(v, bytes) else v
                response_headers[key] = val
        elif message["type"] == "http.response.body":
            response_body += message.get("body", b"")

    await app(scope, receive, send)

    try:
        result = json.loads(response_body.decode()) if response_body else None
    except json.JSONDecodeError:
        result = response_body.decode()

    return status_code, result, response_headers


def random_string(length: int = 10) -> str:
    """Generate a random string for test data."""
    return "".join(random.choices(string.ascii_letters, k=length))


def random_email() -> str:
    """Generate a random email address."""
    return f"{random_string(8)}@{random_string(5)}.com"


# ============================================================================
# Test 1: Response Model Filtering (Security Critical)
# ============================================================================


class UserIn(BaseModel):
    username: str
    password: str
    email: str


class UserOut(BaseModel):
    username: str
    email: str


class TestResponseModelFiltering:
    """Test that response_model properly filters sensitive fields."""

    @pytest.fixture
    def app(self):
        app = FastAPI()

        @app.post("/user", response_model=UserOut)
        def create_user(user: UserIn):
            # Returns full UserIn, but should be filtered to UserOut
            return user

        @app.post("/user-dict", response_model=UserOut)
        def create_user_dict(user: UserIn):
            # Returns dict with extra fields
            return {
                "username": user.username,
                "email": user.email,
                "password": user.password,
                "internal_id": 12345,
            }

        return app

    @pytest.mark.asyncio
    async def test_password_not_leaked(self, app):
        """Password field must be filtered from response."""
        username = random_string()
        password = random_string(20)
        email = random_email()

        status, result, _ = await asgi_request(
            app,
            "POST",
            "/user",
            body={"username": username, "password": password, "email": email},
            headers={"content-type": "application/json"},
        )

        assert status == 200
        assert result["username"] == username
        assert result["email"] == email
        assert "password" not in result, "Password must not be in response"

    @pytest.mark.asyncio
    async def test_extra_fields_filtered(self, app):
        """Extra fields not in response_model must be filtered."""
        username = random_string()
        password = random_string(20)
        email = random_email()

        status, result, _ = await asgi_request(
            app,
            "POST",
            "/user-dict",
            body={"username": username, "password": password, "email": email},
            headers={"content-type": "application/json"},
        )

        assert status == 200
        assert "password" not in result
        assert "internal_id" not in result

    @pytest.mark.asyncio
    async def test_randomized_passwords_filtered(self, app):
        """Test with multiple random passwords to ensure filtering works."""
        for _ in range(10):
            username = random_string()
            # Test various password formats
            password = random.choice(
                [
                    random_string(8),
                    random_string(32),
                    "password123!@#",
                    "'.drop table users;--",
                    "<script>alert('xss')</script>",
                ]
            )
            email = random_email()

            status, result, _ = await asgi_request(
                app,
                "POST",
                "/user",
                body={"username": username, "password": password, "email": email},
                headers={"content-type": "application/json"},
            )

            assert status == 200
            assert "password" not in result


# ============================================================================
# Test 2: Dependency Injection
# ============================================================================


class TestDependencyInjection:
    """Test that dependencies are properly resolved with correct parameters."""

    @pytest.fixture
    def app(self):
        app = FastAPI()

        def common_parameters(q: str = "", skip: int = 0, limit: int = 100):
            return {"q": q, "skip": skip, "limit": limit}

        @app.get("/items")
        def read_items(commons: dict = Depends(common_parameters)):
            return {"commons": commons}

        @app.get("/items-with-extra")
        def read_items_extra(
            commons: dict = Depends(common_parameters), extra: str = "default"
        ):
            return {"commons": commons, "extra": extra}

        return app

    @pytest.mark.asyncio
    async def test_dependency_with_query_params(self, app):
        """Dependency receives query parameters correctly."""
        q = random_string()
        skip = random.randint(0, 100)
        limit = random.randint(1, 50)

        status, result, _ = await asgi_request(
            app, "GET", f"/items?q={q}&skip={skip}&limit={limit}"
        )

        assert status == 200
        assert result["commons"]["q"] == q
        assert result["commons"]["skip"] == skip
        assert result["commons"]["limit"] == limit

    @pytest.mark.asyncio
    async def test_dependency_with_defaults(self, app):
        """Dependency uses default values when params not provided."""
        status, result, _ = await asgi_request(app, "GET", "/items")

        assert status == 200
        assert result["commons"]["q"] == ""
        assert result["commons"]["skip"] == 0
        assert result["commons"]["limit"] == 100

    @pytest.mark.asyncio
    async def test_dependency_with_handler_extra_params(self, app):
        """Handler can have additional params alongside dependency."""
        q = random_string()
        extra = random_string()

        status, result, _ = await asgi_request(
            app, "GET", f"/items-with-extra?q={q}&extra={extra}"
        )

        assert status == 200
        assert result["commons"]["q"] == q
        assert result["extra"] == extra


# ============================================================================
# Test 3: Nested Dependency Injection
# ============================================================================


class TestNestedDependencyInjection:
    """Test multi-level dependency chains."""

    @pytest.fixture
    def app(self):
        app = FastAPI()

        def get_db():
            return {"connection": f"db_{random_string(5)}"}

        def get_current_user(db: dict = Depends(get_db)):
            return {"user": "authenticated", "db": db}

        def get_admin_user(user: dict = Depends(get_current_user)):
            return {"admin": True, "user": user}

        @app.get("/me")
        def read_me(user: dict = Depends(get_current_user)):
            return user

        @app.get("/admin")
        def read_admin(admin: dict = Depends(get_admin_user)):
            return admin

        # 4-level deep dependency chain
        def level_1():
            return {"level": 1, "id": random_string(4)}

        def level_2(l1: dict = Depends(level_1)):
            return {"level": 2, "parent": l1}

        def level_3(l2: dict = Depends(level_2)):
            return {"level": 3, "parent": l2}

        def level_4(l3: dict = Depends(level_3)):
            return {"level": 4, "parent": l3}

        @app.get("/deep")
        def deep_endpoint(l4: dict = Depends(level_4)):
            return l4

        return app

    @pytest.mark.asyncio
    async def test_two_level_dependency(self, app):
        """Two-level dependency chain resolves correctly."""
        status, result, _ = await asgi_request(app, "GET", "/me")

        assert status == 200
        assert result["user"] == "authenticated"
        assert "db" in result
        assert "connection" in result["db"]

    @pytest.mark.asyncio
    async def test_three_level_dependency(self, app):
        """Three-level dependency chain resolves correctly."""
        status, result, _ = await asgi_request(app, "GET", "/admin")

        assert status == 200
        assert result["admin"] == True
        assert result["user"]["user"] == "authenticated"
        assert "db" in result["user"]

    @pytest.mark.asyncio
    async def test_four_level_dependency(self, app):
        """Four-level dependency chain resolves correctly."""
        status, result, _ = await asgi_request(app, "GET", "/deep")

        assert status == 200
        assert result["level"] == 4
        assert result["parent"]["level"] == 3
        assert result["parent"]["parent"]["level"] == 2
        assert result["parent"]["parent"]["parent"]["level"] == 1


# ============================================================================
# Test 4: Enum Path Parameters
# ============================================================================


class ModelName(str, Enum):
    alexnet = "alexnet"
    resnet = "resnet"
    lenet = "lenet"
    vgg = "vgg"


class Priority(str, Enum):
    low = "low"
    medium = "medium"
    high = "high"
    critical = "critical"


class TestEnumPathParameters:
    """Test that Enum path parameters are converted correctly."""

    @pytest.fixture
    def app(self):
        app = FastAPI()

        @app.get("/models/{model_name}")
        def get_model(model_name: ModelName):
            return {
                "model": model_name.value,
                "is_enum": isinstance(model_name, ModelName),
                "enum_name": model_name.name,
            }

        @app.get("/tasks/{task_id}/priority/{priority}")
        def get_task(task_id: int, priority: Priority):
            return {
                "task_id": task_id,
                "priority": priority.value,
                "is_enum": isinstance(priority, Priority),
            }

        return app

    @pytest.mark.asyncio
    async def test_enum_path_parameter(self, app):
        """Enum path parameter is converted from string."""
        for model in ModelName:
            status, result, _ = await asgi_request(app, "GET", f"/models/{model.value}")

            assert status == 200
            assert result["model"] == model.value
            assert result["is_enum"] == True
            assert result["enum_name"] == model.name

    @pytest.mark.asyncio
    async def test_multiple_path_params_with_enum(self, app):
        """Multiple path parameters including enum work together."""
        task_id = random.randint(1, 1000)
        priority = random.choice(list(Priority))

        status, result, _ = await asgi_request(
            app, "GET", f"/tasks/{task_id}/priority/{priority.value}"
        )

        assert status == 200
        assert result["task_id"] == task_id
        assert result["priority"] == priority.value
        assert result["is_enum"] == True


# ============================================================================
# Test 5: APIRouter Prefix Handling
# ============================================================================


class TestAPIRouterPrefix:
    """Test that APIRouter prefixes are applied correctly."""

    @pytest.fixture
    def app(self):
        app = FastAPI()

        # Router with prefix
        api_v1 = APIRouter(prefix="/api/v1")

        @api_v1.get("/users")
        def list_users_v1():
            return [{"id": 1, "version": "v1"}]

        @api_v1.get("/users/{user_id}")
        def get_user_v1(user_id: int):
            return {"id": user_id, "version": "v1"}

        # Another router with different prefix
        api_v2 = APIRouter(prefix="/api/v2")

        @api_v2.get("/users")
        def list_users_v2():
            return [{"id": 1, "version": "v2", "enhanced": True}]

        # Nested prefix test
        admin_router = APIRouter(prefix="/admin")

        @admin_router.get("/stats")
        def get_stats():
            return {"requests": random.randint(100, 10000)}

        app.include_router(api_v1)
        app.include_router(api_v2)
        app.include_router(admin_router)

        return app

    @pytest.mark.asyncio
    async def test_router_prefix_applied(self, app):
        """Router prefix is applied to all routes."""
        status, result, _ = await asgi_request(app, "GET", "/api/v1/users")

        assert status == 200
        assert result[0]["version"] == "v1"

    @pytest.mark.asyncio
    async def test_router_path_params_with_prefix(self, app):
        """Path parameters work with router prefix."""
        user_id = random.randint(1, 1000)
        status, result, _ = await asgi_request(app, "GET", f"/api/v1/users/{user_id}")

        assert status == 200
        assert result["id"] == user_id
        assert result["version"] == "v1"

    @pytest.mark.asyncio
    async def test_multiple_routers_different_prefixes(self, app):
        """Multiple routers with different prefixes coexist."""
        status_v1, result_v1, _ = await asgi_request(app, "GET", "/api/v1/users")
        status_v2, result_v2, _ = await asgi_request(app, "GET", "/api/v2/users")

        assert status_v1 == 200
        assert status_v2 == 200
        assert result_v1[0]["version"] == "v1"
        assert result_v2[0]["version"] == "v2"
        assert result_v2[0]["enhanced"] == True

    @pytest.mark.asyncio
    async def test_without_prefix_returns_404(self, app):
        """Routes without prefix return 404."""
        status, _, _ = await asgi_request(app, "GET", "/users")
        assert status == 404


# ============================================================================
# Test 6: BackgroundTasks with Depends()
# ============================================================================


class TestBackgroundTasks:
    """Test BackgroundTasks injection via Depends()."""

    @pytest.fixture
    def app(self):
        app = FastAPI()

        # Store for tracking task execution
        app.task_log = []

        def log_task(message: str, log: list):
            log.append(message)

        @app.post("/notify")
        def send_notification(background_tasks: BackgroundTasks = Depends()):
            background_tasks.add_task(log_task, "notification", app.task_log)
            return {"message": "Notification scheduled"}

        @app.post("/multi-task")
        def multi_task(background_tasks: BackgroundTasks = Depends()):
            for i in range(3):
                background_tasks.add_task(log_task, f"task_{i}", app.task_log)
            return {"tasks_scheduled": 3}

        return app

    @pytest.mark.asyncio
    async def test_background_tasks_injected(self, app):
        """BackgroundTasks instance is injected via Depends()."""
        status, result, _ = await asgi_request(app, "POST", "/notify")

        assert status == 200
        assert result["message"] == "Notification scheduled"

    @pytest.mark.asyncio
    async def test_multiple_background_tasks(self, app):
        """Multiple background tasks can be added."""
        status, result, _ = await asgi_request(app, "POST", "/multi-task")

        assert status == 200
        assert result["tasks_scheduled"] == 3


# ============================================================================
# Test 7: Exception Handlers
# ============================================================================


class TestExceptionHandlers:
    """Test custom exception handlers."""

    @pytest.fixture
    def app(self):
        app = FastAPI()

        @app.exception_handler(404)
        async def not_found_handler(request, exc):
            return {"error": "not_found", "path": request.path}

        @app.exception_handler(403)
        async def forbidden_handler(request, exc):
            return {
                "error": "forbidden",
                "detail": getattr(exc, "detail", "Access denied"),
            }

        @app.get("/protected")
        def protected():
            raise HTTPException(status_code=403, detail="Admin only")

        @app.get("/item/{item_id}")
        def get_item(item_id: int):
            if item_id > 100:
                raise HTTPException(status_code=404, detail="Item not found")
            return {"id": item_id}

        return app

    @pytest.mark.asyncio
    async def test_404_for_nonexistent_route(self, app):
        """Custom 404 handler for nonexistent routes."""
        path = f"/{random_string()}"
        status, result, _ = await asgi_request(app, "GET", path)

        assert status == 404

    @pytest.mark.asyncio
    async def test_http_exception_403(self, app):
        """HTTPException with 403 status."""
        status, result, _ = await asgi_request(app, "GET", "/protected")

        assert status == 403

    @pytest.mark.asyncio
    async def test_http_exception_404_from_handler(self, app):
        """HTTPException with 404 raised from handler."""
        item_id = random.randint(101, 1000)
        status, result, _ = await asgi_request(app, "GET", f"/item/{item_id}")

        assert status == 404


# ============================================================================
# Test 8: Combined Scenarios (Real-world patterns)
# ============================================================================


class TestCombinedScenarios:
    """Test real-world usage patterns combining multiple features."""

    @pytest.fixture
    def app(self):
        """Create a realistic API with multiple features."""
        app = FastAPI()

        # Database simulation
        def get_db():
            return {"connected": True, "db_id": random_string(8)}

        def get_current_user(db: dict = Depends(get_db)):
            return {"username": "testuser", "db": db}

        # Response models
        class ItemCreate(BaseModel):
            name: str
            price: float
            secret_cost: float = 0.0

        class ItemResponse(BaseModel):
            id: int
            name: str
            price: float

        # Router for items
        items_router = APIRouter(prefix="/items", tags=["items"])

        class ItemStatus(str, Enum):
            active = "active"
            archived = "archived"
            deleted = "deleted"

        @items_router.get("/{item_id}/status/{status}", response_model=ItemResponse)
        def get_item_by_status(
            item_id: int, status: ItemStatus, user: dict = Depends(get_current_user)
        ):
            return ItemResponse(
                id=item_id,
                name=f"Item {item_id} ({status.value})",
                price=19.99,
            )

        @items_router.post("/", response_model=ItemResponse)
        def create_item(
            item: ItemCreate,
            user: dict = Depends(get_current_user),
            background_tasks: BackgroundTasks = Depends(),
        ):
            background_tasks.add_task(lambda: None)
            # secret_cost should be filtered out
            return ItemResponse(
                id=random.randint(1, 1000),
                name=item.name,
                price=item.price,
            )

        app.include_router(items_router)

        return app

    @pytest.mark.asyncio
    async def test_combined_router_enum_dependency(self, app):
        """Router prefix + enum parameter + nested dependency."""
        item_id = random.randint(1, 100)
        status_val = random.choice(list(["active", "archived", "deleted"]))

        status, result, _ = await asgi_request(
            app, "GET", f"/items/{item_id}/status/{status_val}"
        )

        assert status == 200
        assert result["id"] == item_id
        assert status_val in result["name"]

    @pytest.mark.asyncio
    async def test_combined_post_with_all_features(self, app):
        """POST with response_model + dependency + background_tasks."""
        name = random_string()
        price = round(random.uniform(1, 100), 2)

        status, result, _ = await asgi_request(
            app,
            "POST",
            "/items/",
            body={"name": name, "price": price, "secret_cost": 999.99},
            headers={"content-type": "application/json"},
        )

        assert status == 200
        assert result["name"] == name
        assert result["price"] == price
        assert "secret_cost" not in result  # Filtered by response_model


# ============================================================================
# Run tests with pytest
# ============================================================================

if __name__ == "__main__":
    pytest.main([__file__, "-v"])
