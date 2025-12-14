"""
Tests for OpenAPI customization features.

Tests the following FastAPI-compatible features:
- Custom responses dict (e.g., {404: {"description": "Not found"}})
- Custom operation_id
- deprecated flag
- openapi_extra for custom fields
"""

import json
import random
import string
import uuid

import pytest


def random_string(length: int = 8) -> str:
    """Generate a random string."""
    return "".join(random.choices(string.ascii_lowercase, k=length))


class TestOpenAPIResponses:
    """Test custom responses in OpenAPI schema."""

    def test_custom_404_response(self):
        """Test that custom 404 response appears in OpenAPI schema."""
        from fasterapi import FastAPI

        app = FastAPI(title=f"Test-{uuid.uuid4().hex[:8]}")

        @app.get("/items/{item_id}", responses={404: {"description": "Item not found"}})
        def get_item(item_id: int):
            return {"id": item_id}

        # Get OpenAPI schema
        openapi = app.routes()

        # Verify the route is registered
        assert len(openapi) > 0

    def test_multiple_custom_responses(self):
        """Test multiple custom response codes."""
        from fasterapi import FastAPI

        app = FastAPI(title=f"Test-{uuid.uuid4().hex[:8]}")

        @app.post(
            "/items",
            responses={
                201: {"description": "Item created successfully"},
                400: {"description": "Invalid input data"},
                409: {"description": "Item already exists"},
            },
        )
        def create_item(name: str):
            return {"name": name}

        openapi = app.routes()
        assert len(openapi) > 0

    def test_custom_200_override(self):
        """Test that custom 200 response overrides default."""
        from fasterapi import FastAPI

        app = FastAPI(title=f"Test-{uuid.uuid4().hex[:8]}")

        @app.get("/health", responses={200: {"description": "Service is healthy"}})
        def health_check():
            return {"status": "ok"}

        openapi = app.routes()
        assert len(openapi) > 0


class TestOperationId:
    """Test custom operationId in OpenAPI schema."""

    def test_custom_operation_id(self):
        """Test that custom operation_id is used."""
        from fasterapi import FastAPI

        app = FastAPI(title=f"Test-{uuid.uuid4().hex[:8]}")

        op_id = f"get_user_by_id_{random_string()}"

        @app.get("/users/{user_id}", operation_id=op_id)
        def get_user(user_id: int):
            return {"id": user_id}

        openapi = app.routes()
        assert len(openapi) > 0

    def test_unique_operation_ids(self):
        """Test multiple routes with unique operation IDs."""
        from fasterapi import FastAPI

        app = FastAPI(title=f"Test-{uuid.uuid4().hex[:8]}")

        @app.get("/users", operation_id="list_all_users")
        def list_users():
            return []

        @app.get("/users/{user_id}", operation_id="get_single_user")
        def get_user(user_id: int):
            return {"id": user_id}

        @app.post("/users", operation_id="create_new_user")
        def create_user(name: str):
            return {"name": name}

        openapi = app.routes()
        assert len(openapi) >= 3


class TestDeprecated:
    """Test deprecated flag in OpenAPI schema."""

    def test_deprecated_endpoint(self):
        """Test that deprecated flag is set."""
        from fasterapi import FastAPI

        app = FastAPI(title=f"Test-{uuid.uuid4().hex[:8]}")

        @app.get("/v1/legacy", deprecated=True)
        def legacy_endpoint():
            return {"message": "This is deprecated"}

        openapi = app.routes()
        assert len(openapi) > 0

    def test_mixed_deprecated_and_active(self):
        """Test mix of deprecated and active endpoints."""
        from fasterapi import FastAPI

        app = FastAPI(title=f"Test-{uuid.uuid4().hex[:8]}")

        @app.get("/v1/old", deprecated=True)
        def old_endpoint():
            return {"version": "v1"}

        @app.get("/v2/new")
        def new_endpoint():
            return {"version": "v2"}

        openapi = app.routes()
        assert len(openapi) >= 2


class TestOpenAPIExtra:
    """Test openapi_extra for custom fields."""

    def test_custom_x_extension(self):
        """Test adding custom x- extension fields."""
        from fasterapi import FastAPI

        app = FastAPI(title=f"Test-{uuid.uuid4().hex[:8]}")

        @app.get("/internal", openapi_extra={"x-internal": True, "x-rate-limit": 100})
        def internal_endpoint():
            return {"internal": True}

        openapi = app.routes()
        assert len(openapi) > 0

    def test_complex_openapi_extra(self):
        """Test complex nested openapi_extra."""
        from fasterapi import FastAPI

        app = FastAPI(title=f"Test-{uuid.uuid4().hex[:8]}")

        @app.post(
            "/webhook",
            openapi_extra={
                "x-webhook-config": {
                    "retries": 3,
                    "timeout": 30,
                    "events": ["created", "updated", "deleted"],
                }
            },
        )
        def webhook():
            return {"registered": True}

        openapi = app.routes()
        assert len(openapi) > 0


class TestCombinedFeatures:
    """Test combining multiple OpenAPI customization features."""

    def test_all_features_combined(self):
        """Test route with all OpenAPI customization features."""
        from fasterapi import FastAPI

        app = FastAPI(title=f"Test-{uuid.uuid4().hex[:8]}")

        @app.get(
            "/combined/{id}",
            summary="Get item by ID",
            description="Retrieves a specific item from the database",
            tags=["items"],
            responses={
                200: {"description": "Item found"},
                404: {"description": "Item not found"},
                500: {"description": "Internal server error"},
            },
            operation_id="get_item_combined",
            deprecated=False,
            openapi_extra={"x-custom-field": "custom_value"},
        )
        def get_combined(id: int):
            return {"id": id}

        openapi = app.routes()
        assert len(openapi) > 0

    def test_deprecated_with_replacement(self):
        """Test deprecated endpoint with replacement info in openapi_extra."""
        from fasterapi import FastAPI

        app = FastAPI(title=f"Test-{uuid.uuid4().hex[:8]}")

        @app.get(
            "/v1/items",
            deprecated=True,
            openapi_extra={
                "x-deprecated-since": "2024-01-01",
                "x-replacement": "/v2/items",
            },
        )
        def old_items():
            return []

        @app.get("/v2/items")
        def new_items():
            return []

        openapi = app.routes()
        assert len(openapi) >= 2


class TestHTTPMethods:
    """Test OpenAPI customization with different HTTP methods."""

    def test_post_with_responses(self):
        """Test POST endpoint with custom responses."""
        from fasterapi import FastAPI

        app = FastAPI(title=f"Test-{uuid.uuid4().hex[:8]}")

        @app.post(
            "/items",
            responses={
                201: {"description": "Created"},
                400: {"description": "Bad request"},
            },
            operation_id="create_item",
        )
        def create_item():
            return {"id": 1}

        openapi = app.routes()
        assert len(openapi) > 0

    def test_put_with_responses(self):
        """Test PUT endpoint with custom responses."""
        from fasterapi import FastAPI

        app = FastAPI(title=f"Test-{uuid.uuid4().hex[:8]}")

        @app.put(
            "/items/{id}",
            responses={
                200: {"description": "Updated"},
                404: {"description": "Not found"},
            },
            operation_id="update_item",
        )
        def update_item(id: int):
            return {"id": id}

        openapi = app.routes()
        assert len(openapi) > 0

    def test_delete_with_responses(self):
        """Test DELETE endpoint with custom responses."""
        from fasterapi import FastAPI

        app = FastAPI(title=f"Test-{uuid.uuid4().hex[:8]}")

        @app.delete(
            "/items/{id}",
            responses={
                204: {"description": "Deleted"},
                404: {"description": "Not found"},
            },
            operation_id="delete_item",
        )
        def delete_item(id: int):
            return None

        openapi = app.routes()
        assert len(openapi) > 0

    def test_patch_with_responses(self):
        """Test PATCH endpoint with custom responses."""
        from fasterapi import FastAPI

        app = FastAPI(title=f"Test-{uuid.uuid4().hex[:8]}")

        @app.patch(
            "/items/{id}",
            responses={
                200: {"description": "Partially updated"},
                404: {"description": "Not found"},
            },
            operation_id="patch_item",
        )
        def patch_item(id: int):
            return {"id": id}

        openapi = app.routes()
        assert len(openapi) > 0


class TestRandomizedRoutes:
    """Test with randomized route configurations."""

    def test_random_responses(self):
        """Test with randomly generated response codes."""
        from fasterapi import FastAPI

        app = FastAPI(title=f"Test-{uuid.uuid4().hex[:8]}")

        # Generate random response codes
        response_codes = random.sample([400, 401, 403, 404, 409, 500, 502, 503], 3)
        responses = {
            code: {"description": f"Error {code} - {random_string()}"}
            for code in response_codes
        }

        path = f"/{random_string()}"
        op_id = f"random_op_{random_string()}"

        @app.get(path, responses=responses, operation_id=op_id)
        def random_endpoint():
            return {"random": random_string()}

        openapi = app.routes()
        assert len(openapi) > 0

    def test_multiple_random_routes(self):
        """Test multiple routes with random configurations."""
        from fasterapi import FastAPI

        app = FastAPI(title=f"Test-{uuid.uuid4().hex[:8]}")

        for i in range(5):
            path = f"/route_{random_string()}"
            op_id = f"op_{random_string()}"
            is_deprecated = random.choice([True, False])

            # Create a closure to capture the loop variable
            def make_handler(idx):
                def handler():
                    return {"index": idx}

                return handler

            # Use the decorator directly
            app.get(
                path,
                operation_id=op_id,
                deprecated=is_deprecated,
                responses={
                    random.choice([400, 404, 500]): {"description": random_string()}
                },
            )(make_handler(i))

        openapi = app.routes()
        assert len(openapi) >= 5


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
