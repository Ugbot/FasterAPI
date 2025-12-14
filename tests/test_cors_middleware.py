"""
CORS Middleware Tests

Tests for Cross-Origin Resource Sharing middleware.
"""

from fasterapi import FastAPI
from fasterapi.middleware import CORSMiddleware
from fasterapi.testclient import TestClient


class TestCORSMiddleware:
    """Tests for CORS middleware."""

    def test_cors_with_allowed_origin(self):
        """Test CORS headers are added for allowed origins."""
        app = FastAPI()
        app.add_middleware(
            CORSMiddleware,
            allow_origins=["http://localhost:3000"],
            allow_methods=["GET", "POST"],
            allow_headers=["Content-Type"],
        )

        @app.get("/api/data")
        def get_data():
            return {"data": "test"}

        client = TestClient(app)
        resp = client.get("/api/data", headers={"Origin": "http://localhost:3000"})

        assert resp.status_code == 200
        assert (
            resp.headers.get("access-control-allow-origin") == "http://localhost:3000"
        )

    def test_cors_with_credentials(self):
        """Test CORS with credentials enabled."""
        app = FastAPI()
        app.add_middleware(
            CORSMiddleware,
            allow_origins=["http://localhost:3000"],
            allow_credentials=True,
        )

        @app.get("/api/data")
        def get_data():
            return {"data": "test"}

        client = TestClient(app)
        resp = client.get("/api/data", headers={"Origin": "http://localhost:3000"})

        assert resp.headers.get("access-control-allow-credentials") == "true"

    def test_cors_preflight_request(self):
        """Test CORS preflight OPTIONS request."""
        app = FastAPI()
        app.add_middleware(
            CORSMiddleware,
            allow_origins=["http://localhost:3000"],
            allow_methods=["GET", "POST", "PUT"],
            allow_headers=["content-type", "authorization"],
            max_age=600,
        )

        @app.post("/api/data")
        def post_data():
            return {"created": True}

        client = TestClient(app)
        resp = client.options(
            "/api/data",
            headers={
                "Origin": "http://localhost:3000",
                "Access-Control-Request-Method": "POST",
                "Access-Control-Request-Headers": "Content-Type",
            },
        )

        assert resp.status_code == 200
        assert "access-control-allow-methods" in resp.headers
        assert "access-control-allow-headers" in resp.headers
        assert resp.headers.get("access-control-max-age") == "600"

    def test_cors_disallowed_origin(self):
        """Test that disallowed origins don't get CORS headers."""
        app = FastAPI()
        app.add_middleware(
            CORSMiddleware,
            allow_origins=["http://localhost:3000"],
        )

        @app.get("/api/data")
        def get_data():
            return {"data": "test"}

        client = TestClient(app)
        resp = client.get("/api/data", headers={"Origin": "http://evil.com"})

        assert resp.status_code == 200
        assert "access-control-allow-origin" not in resp.headers

    def test_cors_no_origin_header(self):
        """Test that requests without Origin don't get CORS headers."""
        app = FastAPI()
        app.add_middleware(
            CORSMiddleware,
            allow_origins=["http://localhost:3000"],
        )

        @app.get("/api/data")
        def get_data():
            return {"data": "test"}

        client = TestClient(app)
        resp = client.get("/api/data")

        assert resp.status_code == 200
        assert "access-control-allow-origin" not in resp.headers

    def test_cors_wildcard_origin(self):
        """Test CORS with wildcard origin."""
        app = FastAPI()
        app.add_middleware(
            CORSMiddleware,
            allow_origins=["*"],
        )

        @app.get("/api/data")
        def get_data():
            return {"data": "test"}

        client = TestClient(app)
        resp = client.get("/api/data", headers={"Origin": "http://any-origin.com"})

        assert resp.status_code == 200
        assert resp.headers.get("access-control-allow-origin") == "*"

    def test_cors_wildcard_methods(self):
        """Test CORS with wildcard methods."""
        app = FastAPI()
        app.add_middleware(
            CORSMiddleware,
            allow_origins=["http://localhost:3000"],
            allow_methods=["*"],
        )

        @app.get("/api/data")
        def get_data():
            return {"data": "test"}

        client = TestClient(app)
        resp = client.options(
            "/api/data",
            headers={
                "Origin": "http://localhost:3000",
                "Access-Control-Request-Method": "PATCH",
            },
        )

        assert resp.status_code == 200
        assert "PATCH" in resp.headers.get("access-control-allow-methods", "")

    def test_cors_expose_headers(self):
        """Test CORS expose headers."""
        app = FastAPI()
        app.add_middleware(
            CORSMiddleware,
            allow_origins=["http://localhost:3000"],
            expose_headers=["X-Custom-Header", "X-Another-Header"],
        )

        @app.get("/api/data")
        def get_data():
            return {"data": "test"}

        client = TestClient(app)
        resp = client.get("/api/data", headers={"Origin": "http://localhost:3000"})

        exposed = resp.headers.get("access-control-expose-headers", "")
        assert "X-Custom-Header" in exposed
        assert "X-Another-Header" in exposed

    def test_cors_multiple_origins(self):
        """Test CORS with multiple allowed origins."""
        app = FastAPI()
        app.add_middleware(
            CORSMiddleware,
            allow_origins=["http://localhost:3000", "https://example.com"],
        )

        @app.get("/api/data")
        def get_data():
            return {"data": "test"}

        client = TestClient(app)

        # Test first origin
        resp = client.get("/api/data", headers={"Origin": "http://localhost:3000"})
        assert (
            resp.headers.get("access-control-allow-origin") == "http://localhost:3000"
        )

        # Test second origin
        resp = client.get("/api/data", headers={"Origin": "https://example.com"})
        assert resp.headers.get("access-control-allow-origin") == "https://example.com"

    def test_cors_vary_header(self):
        """Test that Vary header is set correctly."""
        app = FastAPI()
        app.add_middleware(
            CORSMiddleware,
            allow_origins=["http://localhost:3000"],
        )

        @app.get("/api/data")
        def get_data():
            return {"data": "test"}

        client = TestClient(app)
        resp = client.get("/api/data", headers={"Origin": "http://localhost:3000"})

        # When not using wildcard, Vary should include Origin
        assert "Origin" in resp.headers.get("vary", "")


def run_all_tests():
    """Run all test classes."""
    test_classes = [
        TestCORSMiddleware,
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
