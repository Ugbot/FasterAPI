"""
JWT Authentication Tests

Comprehensive tests for FasterAPI JWT security module.
Tests cover:
- Token encoding/decoding
- Signature verification
- Claim validation (exp, nbf, iss, aud)
- JWTBearer dependency
- JWTAuthMiddleware
- OAuth2PasswordBearer
"""

import asyncio
import secrets
import time
from typing import Any, Callable, Dict, List

from fasterapi import Depends, FastAPI
from fasterapi.security import (
    JWT,
    JWTAlgorithm,
    JWTAuthMiddleware,
    JWTBearer,
    JWTConfig,
    JWTError,
    OAuth2PasswordBearer,
    base64url_decode,
    base64url_encode,
    create_jwt_config,
)


def generate_secret() -> str:
    """Generate a random secret key."""
    return secrets.token_urlsafe(32)


class TestBase64URLEncoding:
    """Tests for base64url encoding/decoding."""

    def test_encode_decode_roundtrip(self):
        """Test that encoding and decoding are inverses."""
        for _ in range(10):
            original = secrets.token_bytes(secrets.randbelow(100) + 1)
            encoded = base64url_encode(original)
            decoded = base64url_decode(encoded)
            assert decoded == original, f"Roundtrip failed for {len(original)} bytes"

    def test_no_padding(self):
        """Test that encoded strings have no padding."""
        for length in [1, 2, 3, 4, 5, 10, 15, 20]:
            data = secrets.token_bytes(length)
            encoded = base64url_encode(data)
            assert "=" not in encoded, (
                f"Padding found in encoded string of length {length}"
            )

    def test_url_safe_characters(self):
        """Test that encoded strings use URL-safe characters."""
        for _ in range(10):
            data = secrets.token_bytes(50)
            encoded = base64url_encode(data)
            assert "+" not in encoded, "Non-URL-safe '+' found"
            assert "/" not in encoded, "Non-URL-safe '/' found"


class TestJWTEncodeDecode:
    """Tests for JWT token encoding and decoding."""

    def test_encode_decode_hs256(self):
        """Test HS256 token encoding and decoding."""
        secret = generate_secret()
        config = JWTConfig(secret_key=secret)
        jwt = JWT(config)

        payload = {"sub": f"user_{secrets.token_hex(8)}", "role": "admin"}
        token = jwt.encode(payload, JWTAlgorithm.HS256)

        decoded = jwt.decode(token, verify=False)
        assert decoded["sub"] == payload["sub"]
        assert decoded["role"] == payload["role"]

    def test_encode_decode_hs384(self):
        """Test HS384 token encoding and decoding."""
        secret = generate_secret()
        config = JWTConfig(secret_key=secret, algorithms=[JWTAlgorithm.HS384])
        jwt = JWT(config)

        payload = {"sub": secrets.token_hex(16), "data": list(range(10))}
        token = jwt.encode(payload, JWTAlgorithm.HS384)

        decoded = jwt.decode(token)
        assert decoded["sub"] == payload["sub"]
        assert decoded["data"] == payload["data"]

    def test_encode_decode_hs512(self):
        """Test HS512 token encoding and decoding."""
        secret = generate_secret()
        config = JWTConfig(secret_key=secret, algorithms=[JWTAlgorithm.HS512])
        jwt = JWT(config)

        payload = {
            "user_id": secrets.randbelow(1000000),
            "permissions": ["read", "write"],
        }
        token = jwt.encode(payload, JWTAlgorithm.HS512)

        decoded = jwt.decode(token)
        assert decoded["user_id"] == payload["user_id"]
        assert decoded["permissions"] == payload["permissions"]

    def test_signature_verification(self):
        """Test that signature verification works."""
        secret1 = generate_secret()
        secret2 = generate_secret()

        config1 = JWTConfig(secret_key=secret1)
        config2 = JWTConfig(secret_key=secret2)

        jwt1 = JWT(config1)
        jwt2 = JWT(config2)

        token = jwt1.encode({"sub": "test"})

        # Same secret should work
        decoded = jwt1.decode(token)
        assert decoded["sub"] == "test"

        # Different secret should fail
        try:
            jwt2.decode(token)
            assert False, "Should have raised JWTError"
        except JWTError as e:
            assert "signature" in e.message.lower()

    def test_invalid_token_format(self):
        """Test that invalid token formats are rejected."""
        config = JWTConfig(secret_key=generate_secret())
        jwt = JWT(config)

        invalid_tokens = [
            "",
            "single",
            "two.parts",
            "four.parts.here.present",
            "not.valid.base64!!!",
        ]

        for token in invalid_tokens:
            try:
                jwt.decode(token)
                assert False, f"Should have rejected: {token}"
            except JWTError:
                pass  # Expected

    def test_algorithm_not_allowed(self):
        """Test that disallowed algorithms are rejected."""
        secret = generate_secret()
        config_hs256 = JWTConfig(secret_key=secret, algorithms=[JWTAlgorithm.HS256])
        config_hs512 = JWTConfig(secret_key=secret, algorithms=[JWTAlgorithm.HS512])

        jwt_hs256 = JWT(config_hs256)
        jwt_hs512 = JWT(config_hs512)

        # Create token with HS512
        token = jwt_hs512.encode({"sub": "test"}, JWTAlgorithm.HS512)

        # Try to decode with HS256-only config
        try:
            jwt_hs256.decode(token)
            assert False, "Should have rejected algorithm"
        except JWTError as e:
            assert "algorithm" in e.message.lower()


class TestClaimValidation:
    """Tests for JWT claim validation."""

    def test_expired_token(self):
        """Test that expired tokens are rejected."""
        config = JWTConfig(secret_key=generate_secret(), verify_exp=True)
        jwt = JWT(config)

        # Token expired 10 minutes ago
        payload = {"sub": "test", "exp": int(time.time()) - 600}
        token = jwt.encode(payload)

        try:
            jwt.decode(token)
            assert False, "Should have rejected expired token"
        except JWTError as e:
            assert e.error_type == "token_expired"

    def test_not_yet_valid_token(self):
        """Test that tokens not yet valid are rejected."""
        config = JWTConfig(secret_key=generate_secret(), verify_nbf=True)
        jwt = JWT(config)

        # Token valid 10 minutes from now
        payload = {"sub": "test", "nbf": int(time.time()) + 600}
        token = jwt.encode(payload)

        try:
            jwt.decode(token)
            assert False, "Should have rejected not-yet-valid token"
        except JWTError as e:
            assert e.error_type == "token_not_yet_valid"

    def test_clock_skew_tolerance(self):
        """Test that clock skew is tolerated."""
        config = JWTConfig(
            secret_key=generate_secret(),
            verify_exp=True,
            clock_skew=120,  # 2 minutes tolerance
        )
        jwt = JWT(config)

        # Token expired 1 minute ago (within skew tolerance)
        payload = {"sub": "test", "exp": int(time.time()) - 60}
        token = jwt.encode(payload)

        decoded = jwt.decode(token)
        assert decoded["sub"] == "test"

    def test_issuer_validation(self):
        """Test issuer claim validation."""
        config = JWTConfig(
            secret_key=generate_secret(),
            issuer="https://auth.example.com",
        )
        jwt = JWT(config)

        # Valid issuer
        valid_token = jwt.encode({"sub": "test", "iss": "https://auth.example.com"})
        decoded = jwt.decode(valid_token)
        assert decoded["sub"] == "test"

        # Invalid issuer
        invalid_token = jwt.encode({"sub": "test", "iss": "https://evil.com"})
        try:
            jwt.decode(invalid_token)
            assert False, "Should have rejected invalid issuer"
        except JWTError as e:
            assert e.error_type == "invalid_issuer"

    def test_audience_validation(self):
        """Test audience claim validation."""
        config = JWTConfig(
            secret_key=generate_secret(),
            audience=["api.example.com", "web.example.com"],
        )
        jwt = JWT(config)

        # Valid audience (string)
        valid_token1 = jwt.encode({"sub": "test", "aud": "api.example.com"})
        decoded = jwt.decode(valid_token1)
        assert decoded["sub"] == "test"

        # Valid audience (list)
        valid_token2 = jwt.encode({"sub": "test", "aud": ["web.example.com", "other"]})
        decoded = jwt.decode(valid_token2)
        assert decoded["sub"] == "test"

        # Invalid audience
        invalid_token = jwt.encode({"sub": "test", "aud": "evil.com"})
        try:
            jwt.decode(invalid_token)
            assert False, "Should have rejected invalid audience"
        except JWTError as e:
            assert e.error_type == "invalid_audience"

    def test_required_claims(self):
        """Test that required claims are enforced."""
        config = JWTConfig(
            secret_key=generate_secret(),
            required_claims=["sub", "role"],
        )
        jwt = JWT(config)

        # Missing required claim
        incomplete_token = jwt.encode({"sub": "test"})  # Missing 'role'
        try:
            jwt.decode(incomplete_token)
            assert False, "Should have rejected missing claim"
        except JWTError as e:
            assert e.error_type == "missing_claim"
            assert "role" in e.message

        # All required claims present
        complete_token = jwt.encode({"sub": "test", "role": "admin"})
        decoded = jwt.decode(complete_token)
        assert decoded["role"] == "admin"


class TestJWTBearer:
    """Tests for JWTBearer dependency."""

    def test_create_and_verify_token(self):
        """Test token creation and verification."""
        config = JWTConfig(secret_key=generate_secret())
        jwt_bearer = JWTBearer(config)

        user_id = f"user_{secrets.token_hex(8)}"
        token = jwt_bearer.create_token(user_id, expires_in=3600)

        # Verify token
        claims = jwt_bearer.decode_token(token)
        assert claims["sub"] == user_id
        assert "exp" in claims
        assert "iat" in claims

    def test_create_token_with_claims(self):
        """Test token creation with additional claims."""
        config = JWTConfig(
            secret_key=generate_secret(),
            issuer="test-issuer",
            audience=["test-api"],
        )
        jwt_bearer = JWTBearer(config)

        token = jwt_bearer.create_token(
            "user123",
            claims={"role": "admin", "permissions": ["read", "write", "delete"]},
        )

        claims = jwt_bearer.decode_token(token)
        assert claims["sub"] == "user123"
        assert claims["role"] == "admin"
        assert claims["permissions"] == ["read", "write", "delete"]
        assert claims["iss"] == "test-issuer"
        assert claims["aud"] == "test-api"

    def test_call_with_valid_authorization(self):
        """Test __call__ with valid authorization header."""
        config = JWTConfig(secret_key=generate_secret())
        jwt_bearer = JWTBearer(config)

        token = jwt_bearer.create_token("user456")
        auth_header = f"Bearer {token}"

        async def test():
            claims = await jwt_bearer(authorization=auth_header)
            assert claims["sub"] == "user456"

        asyncio.run(test())

    def test_call_with_missing_authorization(self):
        """Test __call__ with missing authorization."""
        config = JWTConfig(secret_key=generate_secret())
        jwt_bearer = JWTBearer(config, auto_error=True)

        async def test():
            try:
                await jwt_bearer(authorization=None)
                assert False, "Should have raised HTTPException"
            except Exception as e:
                assert "401" in str(type(e).__name__) or "Not authenticated" in str(e)

        asyncio.run(test())

    def test_call_with_auto_error_false(self):
        """Test __call__ with auto_error=False returns None."""
        config = JWTConfig(secret_key=generate_secret())
        jwt_bearer = JWTBearer(config, auto_error=False)

        async def test():
            result = await jwt_bearer(authorization=None)
            assert result is None

            result = await jwt_bearer(authorization="Invalid header")
            assert result is None

        asyncio.run(test())

    def test_call_with_invalid_scheme(self):
        """Test __call__ with non-Bearer scheme."""
        config = JWTConfig(secret_key=generate_secret())
        jwt_bearer = JWTBearer(config, auto_error=False)

        async def test():
            result = await jwt_bearer(authorization="Basic dXNlcjpwYXNz")
            assert result is None

        asyncio.run(test())


class TestJWTAuthMiddleware:
    """Tests for JWTAuthMiddleware."""

    def test_middleware_allows_excluded_paths(self):
        """Test that excluded paths bypass auth."""
        config = JWTConfig(secret_key=generate_secret())
        jwt_bearer = JWTBearer(config)

        calls = []

        async def app(scope, receive, send):
            calls.append(scope["path"])
            await send({"type": "http.response.start", "status": 200, "headers": []})
            await send({"type": "http.response.body", "body": b"OK"})

        middleware = JWTAuthMiddleware(
            app, jwt_bearer, exclude_paths=["/login", "/health"]
        )

        async def mock_receive():
            return {"type": "http.request", "body": b""}

        async def mock_send(message):
            pass

        async def test():
            # Create mock scope without auth
            scope = {
                "type": "http",
                "path": "/login",
                "method": "POST",
                "headers": [],
            }
            await middleware(scope, mock_receive, mock_send)
            assert "/login" in calls

        asyncio.run(test())

    def test_middleware_allows_excluded_methods(self):
        """Test that excluded methods bypass auth."""
        config = JWTConfig(secret_key=generate_secret())
        jwt_bearer = JWTBearer(config)

        calls = []

        async def app(scope, receive, send):
            calls.append((scope["path"], scope["method"]))

        middleware = JWTAuthMiddleware(app, jwt_bearer, exclude_methods=["OPTIONS"])

        async def test():
            scope = {
                "type": "http",
                "path": "/api/resource",
                "method": "OPTIONS",
                "headers": [],
            }
            await middleware(scope, lambda: None, lambda x: None)
            assert ("/api/resource", "OPTIONS") in calls

        asyncio.run(test())

    def test_middleware_rejects_unauthenticated(self):
        """Test that middleware rejects requests without valid token."""
        config = JWTConfig(secret_key=generate_secret())
        jwt_bearer = JWTBearer(config)

        responses = []

        async def mock_send(message):
            responses.append(message)

        async def app(scope, receive, send):
            await send({"type": "http.response.start", "status": 200, "headers": []})
            await send({"type": "http.response.body", "body": b"OK"})

        middleware = JWTAuthMiddleware(app, jwt_bearer)

        async def test():
            scope = {
                "type": "http",
                "path": "/protected",
                "method": "GET",
                "headers": [],
            }
            await middleware(scope, lambda: None, mock_send)
            # Should get 401 response
            assert any(r.get("status") == 401 for r in responses)

        asyncio.run(test())

    def test_middleware_stores_claims_in_scope(self):
        """Test that valid claims are stored in scope."""
        config = JWTConfig(secret_key=generate_secret())
        jwt_bearer = JWTBearer(config)
        token = jwt_bearer.create_token("test_user")

        captured_scope = {}

        async def app(scope, receive, send):
            captured_scope.update(scope)

        middleware = JWTAuthMiddleware(app, jwt_bearer)

        async def test():
            scope = {
                "type": "http",
                "path": "/protected",
                "method": "GET",
                "headers": [(b"authorization", f"Bearer {token}".encode())],
            }
            await middleware(scope, lambda: None, lambda x: None)
            assert "jwt_claims" in captured_scope
            assert captured_scope["jwt_claims"]["sub"] == "test_user"

        asyncio.run(test())

    def test_middleware_passes_non_http(self):
        """Test that non-HTTP requests pass through."""
        config = JWTConfig(secret_key=generate_secret())
        jwt_bearer = JWTBearer(config)

        calls = []

        async def app(scope, receive, send):
            calls.append(scope["type"])

        middleware = JWTAuthMiddleware(app, jwt_bearer)

        async def test():
            scope = {"type": "websocket", "path": "/ws"}
            await middleware(scope, lambda: None, lambda x: None)
            assert "websocket" in calls

        asyncio.run(test())


class TestOAuth2PasswordBearer:
    """Tests for OAuth2PasswordBearer with mock request."""

    def test_extracts_bearer_token(self):
        """Test that bearer token is extracted correctly."""
        oauth2 = OAuth2PasswordBearer(tokenUrl="/token")
        token = secrets.token_urlsafe(32)

        class MockRequest:
            def __init__(self, auth_header):
                self.headers = {"authorization": auth_header} if auth_header else {}

        async def test():
            request = MockRequest(f"Bearer {token}")
            result = await oauth2(request)
            assert result == token

        asyncio.run(test())

    def test_rejects_missing_auth(self):
        """Test that missing auth raises exception."""
        oauth2 = OAuth2PasswordBearer(tokenUrl="/token", auto_error=True)

        class MockRequest:
            headers = {}

        async def test():
            try:
                await oauth2(MockRequest())
                assert False, "Should have raised"
            except Exception as e:
                assert "authenticated" in str(e).lower() or "401" in str(
                    type(e).__name__
                )

        asyncio.run(test())

    def test_auto_error_false_returns_none(self):
        """Test that auto_error=False returns None on failure."""
        oauth2 = OAuth2PasswordBearer(tokenUrl="/token", auto_error=False)

        class MockRequest:
            def __init__(self, auth_header=None):
                self.headers = {"authorization": auth_header} if auth_header else {}

        async def test():
            result = await oauth2(MockRequest(None))
            assert result is None

            result = await oauth2(MockRequest("NotBearer token"))
            assert result is None

        asyncio.run(test())


class TestCreateJWTConfig:
    """Tests for create_jwt_config helper."""

    def test_creates_config_with_defaults(self):
        """Test basic config creation."""
        secret = generate_secret()
        config = create_jwt_config(secret)

        assert config.secret_key == secret
        assert JWTAlgorithm.HS256 in config.algorithms

    def test_creates_config_with_options(self):
        """Test config creation with all options."""
        config = create_jwt_config(
            secret_key="my-secret",
            algorithm="HS512",
            issuer="my-app",
            audience=["api", "web"],
        )

        assert config.secret_key == "my-secret"
        assert JWTAlgorithm.HS512 in config.algorithms
        assert config.issuer == "my-app"
        assert config.audience == ["api", "web"]


class TestIntegrationWithFasterAPI:
    """Integration tests with FasterAPI app."""

    def test_protected_route(self):
        """Test protected route with JWT dependency."""
        from fasterapi.testclient import TestClient

        app = FastAPI()
        config = JWTConfig(secret_key=generate_secret())
        jwt_bearer = JWTBearer(config)

        @app.post("/login")
        def login():
            token = jwt_bearer.create_token("user123", claims={"role": "admin"})
            return {"access_token": token, "token_type": "bearer"}

        @app.get("/protected")
        async def protected(claims: dict = Depends(jwt_bearer)):
            return {"user": claims["sub"], "role": claims.get("role")}

        client = TestClient(app)

        # Login to get token
        login_response = client.post("/login")
        assert login_response.status_code == 200
        token = login_response.json()["access_token"]

        # Access protected route with token
        protected_response = client.get(
            "/protected", headers={"Authorization": f"Bearer {token}"}
        )
        assert protected_response.status_code == 200
        data = protected_response.json()
        assert data["user"] == "user123"
        assert data["role"] == "admin"

        # Access protected route without token
        no_auth_response = client.get("/protected")
        assert no_auth_response.status_code == 401

    def test_multiple_protected_routes(self):
        """Test multiple protected routes with different access levels."""
        from fasterapi.testclient import TestClient

        app = FastAPI()
        config = JWTConfig(secret_key=generate_secret())
        jwt_bearer = JWTBearer(config)

        @app.get("/users/me")
        async def get_me(claims: dict = Depends(jwt_bearer)):
            return {"user_id": claims["sub"]}

        @app.get("/admin/stats")
        async def admin_stats(claims: dict = Depends(jwt_bearer)):
            if claims.get("role") != "admin":
                from fasterapi.exceptions import HTTPException

                raise HTTPException(status_code=403, detail="Admin only")
            return {"total_users": 100}

        client = TestClient(app)

        # Regular user token
        user_token = jwt_bearer.create_token("regular_user", claims={"role": "user"})

        # Admin token
        admin_token = jwt_bearer.create_token("admin_user", claims={"role": "admin"})

        # Regular user can access /users/me
        response = client.get(
            "/users/me", headers={"Authorization": f"Bearer {user_token}"}
        )
        assert response.status_code == 200

        # Regular user cannot access /admin/stats
        response = client.get(
            "/admin/stats", headers={"Authorization": f"Bearer {user_token}"}
        )
        assert response.status_code == 403

        # Admin can access both
        response = client.get(
            "/users/me", headers={"Authorization": f"Bearer {admin_token}"}
        )
        assert response.status_code == 200

        response = client.get(
            "/admin/stats", headers={"Authorization": f"Bearer {admin_token}"}
        )
        assert response.status_code == 200


def run_all_tests():
    """Run all test classes."""
    test_classes = [
        TestBase64URLEncoding,
        TestJWTEncodeDecode,
        TestClaimValidation,
        TestJWTBearer,
        TestJWTAuthMiddleware,
        TestOAuth2PasswordBearer,
        TestCreateJWTConfig,
        TestIntegrationWithFasterAPI,
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
