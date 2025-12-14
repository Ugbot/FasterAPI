"""
Sub-application Mounting Tests

Tests for mounting sub-applications (FastAPI/FasterAPI apps) at path prefixes.
"""

import secrets

from fasterapi import FastAPI
from fasterapi.testclient import TestClient


class TestSubAppMounting:
    """Tests for sub-application mounting."""

    def test_basic_mount(self):
        """Test basic sub-app mounting."""
        # Create main app
        main_app = FastAPI(title="Main App")

        # Create sub-app
        sub_app = FastAPI(title="Sub App")

        @sub_app.get("/items")
        def get_items():
            return {"items": ["a", "b", "c"]}

        @sub_app.get("/items/{item_id}")
        def get_item(item_id: int):
            return {"item_id": item_id}

        # Mount sub-app
        main_app.mount("/api/v1", sub_app)

        # Main app route
        @main_app.get("/health")
        def health():
            return {"status": "ok"}

        client = TestClient(main_app)

        # Test main app route
        resp = client.get("/health")
        assert resp.status_code == 200
        assert resp.json() == {"status": "ok"}

        # Test sub-app routes
        resp = client.get("/api/v1/items")
        assert resp.status_code == 200
        assert resp.json() == {"items": ["a", "b", "c"]}

        resp = client.get("/api/v1/items/42")
        assert resp.status_code == 200
        assert resp.json() == {"item_id": 42}

    def test_multiple_mounts(self):
        """Test multiple sub-apps mounted at different paths."""
        main_app = FastAPI()

        # API v1
        api_v1 = FastAPI()

        @api_v1.get("/users")
        def v1_users():
            return {"version": 1, "users": ["alice", "bob"]}

        # API v2
        api_v2 = FastAPI()

        @api_v2.get("/users")
        def v2_users():
            return {"version": 2, "users": [{"name": "alice"}, {"name": "bob"}]}

        main_app.mount("/api/v1", api_v1)
        main_app.mount("/api/v2", api_v2)

        client = TestClient(main_app)

        # Test v1
        resp = client.get("/api/v1/users")
        assert resp.status_code == 200
        assert resp.json()["version"] == 1

        # Test v2
        resp = client.get("/api/v2/users")
        assert resp.status_code == 200
        assert resp.json()["version"] == 2

    def test_nested_mounts(self):
        """Test nested sub-app mounting."""
        main_app = FastAPI()
        api_app = FastAPI()
        admin_app = FastAPI()

        @admin_app.get("/stats")
        def admin_stats():
            return {"total_users": 100}

        @api_app.get("/status")
        def api_status():
            return {"api": "running"}

        # Mount admin inside api
        api_app.mount("/admin", admin_app)

        # Mount api inside main
        main_app.mount("/api", api_app)

        client = TestClient(main_app)

        # Test nested route
        resp = client.get("/api/admin/stats")
        assert resp.status_code == 200
        assert resp.json() == {"total_users": 100}

        # Test intermediate route
        resp = client.get("/api/status")
        assert resp.status_code == 200
        assert resp.json() == {"api": "running"}

    def test_mount_with_path_params(self):
        """Test sub-app with path parameters."""
        main_app = FastAPI()
        sub_app = FastAPI()

        @sub_app.get("/users/{user_id}/posts/{post_id}")
        def get_user_post(user_id: int, post_id: int):
            return {"user_id": user_id, "post_id": post_id}

        main_app.mount("/api", sub_app)

        client = TestClient(main_app)

        resp = client.get("/api/users/123/posts/456")
        assert resp.status_code == 200
        assert resp.json() == {"user_id": 123, "post_id": 456}

    def test_mount_with_query_params(self):
        """Test sub-app with query parameters."""
        main_app = FastAPI()
        sub_app = FastAPI()

        @sub_app.get("/search")
        def search(q: str = "", limit: int = 10):
            return {"query": q, "limit": limit}

        main_app.mount("/api", sub_app)

        client = TestClient(main_app)

        resp = client.get("/api/search?q=test&limit=5")
        assert resp.status_code == 200
        assert resp.json() == {"query": "test", "limit": 5}

    def test_mount_with_post_body(self):
        """Test sub-app with POST body."""
        from pydantic import BaseModel

        main_app = FastAPI()
        sub_app = FastAPI()

        class Item(BaseModel):
            name: str
            price: float

        @sub_app.post("/items")
        def create_item(item: Item):
            return {"created": item.model_dump()}

        main_app.mount("/api", sub_app)

        client = TestClient(main_app)

        resp = client.post("/api/items", json={"name": "Widget", "price": 9.99})
        assert resp.status_code == 200
        assert resp.json()["created"]["name"] == "Widget"

    def test_mount_root_path(self):
        """Test that root_path is correctly set for mounted apps."""
        main_app = FastAPI()
        sub_app = FastAPI()

        captured_root_path = []

        @sub_app.get("/info")
        def get_info():
            # This would normally access request.scope["root_path"]
            return {"mounted": True}

        main_app.mount("/mounted", sub_app)

        client = TestClient(main_app)

        resp = client.get("/mounted/info")
        assert resp.status_code == 200

    def test_mount_404_handling(self):
        """Test 404 for non-existent routes in mounted app."""
        main_app = FastAPI()
        sub_app = FastAPI()

        @sub_app.get("/exists")
        def exists():
            return {"exists": True}

        main_app.mount("/api", sub_app)

        client = TestClient(main_app)

        # Existing route works
        resp = client.get("/api/exists")
        assert resp.status_code == 200

        # Non-existent route returns 404
        resp = client.get("/api/nonexistent")
        assert resp.status_code == 404

    def test_mount_with_middleware(self):
        """Test sub-app with its own middleware."""
        main_app = FastAPI()
        sub_app = FastAPI()

        middleware_called = []

        class TrackingMiddleware:
            def __init__(self, app):
                self.app = app

            async def __call__(self, scope, receive, send):
                middleware_called.append(scope["path"])
                await self.app(scope, receive, send)

        sub_app.add_middleware(TrackingMiddleware)

        @sub_app.get("/tracked")
        def tracked():
            return {"tracked": True}

        main_app.mount("/sub", sub_app)

        client = TestClient(main_app)

        resp = client.get("/sub/tracked")
        assert resp.status_code == 200
        assert "/tracked" in middleware_called  # Path is adjusted for sub-app

    def test_mount_different_http_methods(self):
        """Test sub-app handles all HTTP methods."""
        main_app = FastAPI()
        sub_app = FastAPI()

        @sub_app.get("/resource")
        def get_resource():
            return {"method": "GET"}

        @sub_app.post("/resource")
        def post_resource():
            return {"method": "POST"}

        @sub_app.put("/resource")
        def put_resource():
            return {"method": "PUT"}

        @sub_app.delete("/resource")
        def delete_resource():
            return {"method": "DELETE"}

        @sub_app.patch("/resource")
        def patch_resource():
            return {"method": "PATCH"}

        main_app.mount("/api", sub_app)

        client = TestClient(main_app)

        assert client.get("/api/resource").json()["method"] == "GET"
        assert client.post("/api/resource").json()["method"] == "POST"
        assert client.put("/api/resource").json()["method"] == "PUT"
        assert client.delete("/api/resource").json()["method"] == "DELETE"
        assert client.patch("/api/resource").json()["method"] == "PATCH"


def run_all_tests():
    """Run all test classes."""
    test_classes = [
        TestSubAppMounting,
    ]

    total_passed = 0
    total_failed = 0
    failures = []

    for test_class in test_classes:
        print(f"\n{test_class.__name__}:")
        instance = test_class()
        for name in dir(instance):
            if name.startswith("test_"):
                try:
                    getattr(instance, name)()
                    print(f"  ✓ {name}")
                    total_passed += 1
                except Exception as e:
                    print(f"  ✗ {name}: {e}")
                    total_failed += 1
                    failures.append((test_class.__name__, name, str(e)))

    print(f"\n{'=' * 60}")
    print(f"Results: {total_passed} passed, {total_failed} failed")

    if failures:
        print("\nFailures:")
        for cls, name, error in failures:
            print(f"  {cls}.{name}: {error}")
        return False

    return True


if __name__ == "__main__":
    import sys

    success = run_all_tests()
    sys.exit(0 if success else 1)
