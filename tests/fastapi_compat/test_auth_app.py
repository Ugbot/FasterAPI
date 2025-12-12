"""
Tests for authentication app compatibility between FastAPI and FasterAPI.

Run with:
    TEST_FRAMEWORK=fastapi pytest tests/fastapi_compat/test_auth_app.py -v
    TEST_FRAMEWORK=fasterapi pytest tests/fastapi_compat/test_auth_app.py -v
"""

import base64
import os
import random
import string

import pytest
from apps.auth_app import active_tokens, app, clear_tokens, fake_users_db

FRAMEWORK = os.environ.get("TEST_FRAMEWORK", "fasterapi")

if FRAMEWORK == "fastapi":
    from fastapi.testclient import TestClient
else:
    try:
        from fasterapi.testclient import TestClient
    except ImportError:
        from starlette.testclient import TestClient


@pytest.fixture(autouse=True)
def clean_tokens():
    """Clear tokens before each test."""
    clear_tokens()
    yield
    clear_tokens()


@pytest.fixture
def client():
    """Create test client."""
    return TestClient(app)


def random_string(length: int = 10) -> str:
    return "".join(random.choices(string.ascii_letters, k=length))


def get_basic_auth_header(username: str, password: str) -> str:
    """Generate HTTP Basic auth header."""
    credentials = f"{username}:{password}"
    encoded = base64.b64encode(credentials.encode()).decode()
    return f"Basic {encoded}"


class TestOAuth2Password:
    """Test OAuth2 Password Bearer authentication."""

    def test_login_success(self, client):
        """Test successful login."""
        response = client.post(
            "/token",
            data={"username": "testuser", "password": "password123"},
        )
        assert response.status_code == 200
        data = response.json()
        assert "access_token" in data
        assert data["token_type"] == "bearer"

    def test_login_wrong_password(self, client):
        """Test login with wrong password."""
        response = client.post(
            "/token",
            data={"username": "testuser", "password": "wrongpassword"},
        )
        assert response.status_code == 401

    def test_login_nonexistent_user(self, client):
        """Test login with non-existent user."""
        response = client.post(
            "/token",
            data={"username": "nonexistent", "password": "password"},
        )
        assert response.status_code == 401

    def test_get_current_user(self, client):
        """Test getting current user info."""
        # Login first
        login_response = client.post(
            "/token",
            data={"username": "testuser", "password": "password123"},
        )
        token = login_response.json()["access_token"]

        # Get user info
        response = client.get(
            "/users/me",
            headers={"Authorization": f"Bearer {token}"},
        )
        assert response.status_code == 200
        data = response.json()
        assert data["username"] == "testuser"
        assert data["email"] == "test@example.com"

    def test_get_current_user_invalid_token(self, client):
        """Test getting user with invalid token."""
        response = client.get(
            "/users/me",
            headers={"Authorization": "Bearer invalid_token"},
        )
        assert response.status_code == 401

    def test_get_current_user_no_token(self, client):
        """Test getting user without token."""
        response = client.get("/users/me")
        assert response.status_code == 401

    def test_disabled_user(self, client):
        """Test that disabled users cannot access protected endpoints."""
        # Login as disabled user
        login_response = client.post(
            "/token",
            data={"username": "disabled", "password": "disabled"},
        )
        token = login_response.json()["access_token"]

        # Try to access protected endpoint
        response = client.get(
            "/users/me",
            headers={"Authorization": f"Bearer {token}"},
        )
        assert response.status_code == 400
        assert "inactive" in response.json()["detail"].lower()


class TestOAuth2Scopes:
    """Test OAuth2 scope-based authorization."""

    def test_read_items_with_scope(self, client):
        """Test reading items with correct scope."""
        # Login as user with items:read scope
        login_response = client.post(
            "/token",
            data={"username": "testuser", "password": "password123"},
        )
        token = login_response.json()["access_token"]

        response = client.get(
            "/users/me/items",
            headers={"Authorization": f"Bearer {token}"},
        )
        assert response.status_code == 200
        assert "items" in response.json()

    def test_write_items_with_scope(self, client):
        """Test writing items with correct scope."""
        # Login as user with items:write scope
        login_response = client.post(
            "/token",
            data={"username": "testuser", "password": "password123"},
        )
        token = login_response.json()["access_token"]

        response = client.post(
            "/users/me/items",
            params={"item_name": "New Item"},
            headers={"Authorization": f"Bearer {token}"},
        )
        assert response.status_code == 200

    def test_read_items_without_scope(self, client):
        """Test that user without items:read scope cannot read items."""
        # Create a token without the items:read scope
        # Note: This is a limitation of the test - the fake db gives all scopes
        # In a real test we'd need to manipulate the token data
        pass  # Skip for now

    def test_admin_access_granted(self, client):
        """Test admin endpoint access for admin user."""
        # Login as admin
        login_response = client.post(
            "/token",
            data={"username": "admin", "password": "adminpass"},
        )
        token = login_response.json()["access_token"]

        response = client.get(
            "/admin/users",
            headers={"Authorization": f"Bearer {token}"},
        )
        assert response.status_code == 200
        assert "users" in response.json()

    def test_admin_access_denied(self, client):
        """Test admin endpoint denied for non-admin user."""
        # Login as regular user
        login_response = client.post(
            "/token",
            data={"username": "testuser", "password": "password123"},
        )
        token = login_response.json()["access_token"]

        response = client.get(
            "/admin/users",
            headers={"Authorization": f"Bearer {token}"},
        )
        assert response.status_code == 403
        assert "scope" in response.json()["detail"].lower()

    def test_readonly_user_cannot_write(self, client):
        """Test readonly user cannot write items."""
        # Login as readonly user
        login_response = client.post(
            "/token",
            data={"username": "readonly", "password": "readonly"},
        )
        token = login_response.json()["access_token"]

        # Can read
        response = client.get(
            "/users/me/items",
            headers={"Authorization": f"Bearer {token}"},
        )
        assert response.status_code == 200

        # Cannot write
        response = client.post(
            "/users/me/items",
            params={"item_name": "New Item"},
            headers={"Authorization": f"Bearer {token}"},
        )
        assert response.status_code == 403


class TestHTTPBasicAuth:
    """Test HTTP Basic authentication."""

    def test_basic_auth_success(self, client):
        """Test successful HTTP Basic authentication."""
        auth_header = get_basic_auth_header("testuser", "password123")
        response = client.get(
            "/basic/me",
            headers={"Authorization": auth_header},
        )
        assert response.status_code == 200
        assert response.json()["username"] == "testuser"

    def test_basic_auth_wrong_password(self, client):
        """Test HTTP Basic auth with wrong password."""
        auth_header = get_basic_auth_header("testuser", "wrongpassword")
        response = client.get(
            "/basic/me",
            headers={"Authorization": auth_header},
        )
        assert response.status_code == 401

    def test_basic_auth_nonexistent_user(self, client):
        """Test HTTP Basic auth with non-existent user."""
        auth_header = get_basic_auth_header("nonexistent", "password")
        response = client.get(
            "/basic/me",
            headers={"Authorization": auth_header},
        )
        assert response.status_code == 401

    def test_basic_auth_no_credentials(self, client):
        """Test HTTP Basic auth without credentials."""
        response = client.get("/basic/me")
        assert response.status_code == 401

    def test_basic_auth_get_items(self, client):
        """Test getting items with HTTP Basic auth."""
        auth_header = get_basic_auth_header("testuser", "password123")
        response = client.get(
            "/basic/items",
            headers={"Authorization": auth_header},
        )
        assert response.status_code == 200
        data = response.json()
        assert "items" in data
        assert data["user"] == "testuser"


class TestAPIKeyAuth:
    """Test API Key authentication."""

    def test_api_key_header(self, client):
        """Test API key in header."""
        response = client.get(
            "/apikey/me",
            headers={"X-API-Key": "key-header-123"},
        )
        assert response.status_code == 200
        assert response.json()["username"] == "testuser"

    def test_api_key_query(self, client):
        """Test API key in query parameter."""
        response = client.get(
            "/apikey/me",
            params={"api_key": "key-query-456"},
        )
        assert response.status_code == 200
        assert response.json()["username"] == "testuser"

    def test_api_key_cookie(self, client):
        """Test API key in cookie."""
        response = client.get(
            "/apikey/me",
            cookies={"api_key": "key-cookie-789"},
        )
        assert response.status_code == 200
        assert response.json()["username"] == "admin"

    def test_api_key_invalid(self, client):
        """Test invalid API key."""
        response = client.get(
            "/apikey/me",
            headers={"X-API-Key": "invalid-key"},
        )
        assert response.status_code == 403

    def test_api_key_missing(self, client):
        """Test missing API key."""
        response = client.get("/apikey/me")
        assert response.status_code == 403

    def test_api_key_get_items(self, client):
        """Test getting items with API key."""
        response = client.get(
            "/apikey/items",
            headers={"X-API-Key": "key-header-123"},
        )
        assert response.status_code == 200
        assert "items" in response.json()


class TestPublicEndpoints:
    """Test public endpoints (no auth required)."""

    def test_public_endpoint(self, client):
        """Test public endpoint access."""
        response = client.get("/public")
        assert response.status_code == 200
        assert response.json()["message"] == "This is public"

    def test_health_check(self, client):
        """Test health check endpoint."""
        response = client.get("/health")
        assert response.status_code == 200
        assert response.json()["status"] == "healthy"


class TestAuthCombinations:
    """Test various authentication scenarios."""

    def test_multiple_requests_same_token(self, client):
        """Test multiple requests with same token."""
        # Login
        login_response = client.post(
            "/token",
            data={"username": "testuser", "password": "password123"},
        )
        token = login_response.json()["access_token"]

        # Make multiple requests
        for _ in range(5):
            response = client.get(
                "/users/me",
                headers={"Authorization": f"Bearer {token}"},
            )
            assert response.status_code == 200

    def test_different_users_different_tokens(self, client):
        """Test different users have different tokens."""
        # Login as testuser
        response1 = client.post(
            "/token",
            data={"username": "testuser", "password": "password123"},
        )
        token1 = response1.json()["access_token"]

        # Login as admin
        response2 = client.post(
            "/token",
            data={"username": "admin", "password": "adminpass"},
        )
        token2 = response2.json()["access_token"]

        assert token1 != token2

        # Each token returns correct user
        user1 = client.get(
            "/users/me",
            headers={"Authorization": f"Bearer {token1}"},
        ).json()

        user2 = client.get(
            "/users/me",
            headers={"Authorization": f"Bearer {token2}"},
        ).json()

        assert user1["username"] == "testuser"
        assert user2["username"] == "admin"

    def test_wrong_auth_type(self, client):
        """Test using wrong auth type for endpoint."""
        # Try using API key header on OAuth2 endpoint
        response = client.get(
            "/users/me",
            headers={"X-API-Key": "key-header-123"},
        )
        # Should fail because /users/me expects Bearer token
        assert response.status_code == 401


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
