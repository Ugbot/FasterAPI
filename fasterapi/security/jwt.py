"""
JWT Authentication for FasterAPI

JSON Web Token (JWT) encoding, decoding, and validation.

Features:
- HS256/HS384/HS512 HMAC algorithms
- Standard claim validation (exp, nbf, iss, aud)
- Clock skew tolerance
- Required claims enforcement
- FastAPI-compatible JWTBearer dependency
- JWTAuthMiddleware for app-wide protection

Usage:
    from fasterapi.security import JWTBearer, JWTConfig

    config = JWTConfig(secret_key="your-secret-key")
    jwt_bearer = JWTBearer(config)

    @app.get("/protected")
    async def protected_route(token_data: dict = Depends(jwt_bearer)):
        return {"user": token_data["sub"]}
"""

import base64
import hashlib
import hmac
import json
import time
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable, Dict, List, Optional


class JWTAlgorithm(Enum):
    """Supported JWT algorithms."""

    HS256 = "HS256"
    HS384 = "HS384"
    HS512 = "HS512"


class JWTError(Exception):
    """JWT validation error."""

    def __init__(self, message: str, error_type: str = "invalid_token"):
        super().__init__(message)
        self.message = message
        self.error_type = error_type


@dataclass
class JWTConfig:
    """JWT configuration."""

    # Secret key for HMAC algorithms
    secret_key: str = ""

    # Allowed algorithms
    algorithms: List[JWTAlgorithm] = field(default_factory=lambda: [JWTAlgorithm.HS256])

    # Token extraction
    auto_error: bool = True  # Raise exception on auth failure
    scheme_name: str = "Bearer"

    # Claim validation
    verify_exp: bool = True
    verify_nbf: bool = True
    clock_skew: int = 60  # Seconds of clock skew allowed

    # Expected values
    issuer: Optional[str] = None
    audience: Optional[List[str]] = None

    # Required claims
    required_claims: List[str] = field(default_factory=list)


def base64url_encode(data: bytes) -> str:
    """Encode bytes to base64url string (no padding)."""
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def base64url_decode(data: str) -> bytes:
    """Decode base64url string to bytes."""
    # Add padding if needed
    padding = 4 - (len(data) % 4)
    if padding != 4:
        data += "=" * padding
    return base64.urlsafe_b64decode(data)


class JWT:
    """
    JSON Web Token encoder/decoder.

    Usage:
        jwt = JWT(config)

        # Create token
        token = jwt.encode({"sub": "user123", "exp": time.time() + 3600})

        # Decode and verify token
        claims = jwt.decode(token)
    """

    ALGORITHM_MAP = {
        JWTAlgorithm.HS256: hashlib.sha256,
        JWTAlgorithm.HS384: hashlib.sha384,
        JWTAlgorithm.HS512: hashlib.sha512,
    }

    def __init__(self, config: JWTConfig):
        """
        Initialize JWT handler.

        Args:
            config: JWT configuration
        """
        self.config = config

    def encode(
        self,
        payload: Dict[str, Any],
        algorithm: JWTAlgorithm = JWTAlgorithm.HS256,
    ) -> str:
        """
        Encode a payload to a JWT token.

        Args:
            payload: Token payload (claims)
            algorithm: Signing algorithm

        Returns:
            JWT token string
        """
        # Build header
        header = {"alg": algorithm.value, "typ": "JWT"}
        header_b64 = base64url_encode(
            json.dumps(header, separators=(",", ":")).encode()
        )

        # Build payload
        payload_b64 = base64url_encode(
            json.dumps(payload, separators=(",", ":")).encode()
        )

        # Sign
        signing_input = f"{header_b64}.{payload_b64}"
        signature = self._sign(signing_input, algorithm)
        signature_b64 = base64url_encode(signature)

        return f"{signing_input}.{signature_b64}"

    def decode(
        self,
        token: str,
        verify: bool = True,
    ) -> Dict[str, Any]:
        """
        Decode and verify a JWT token.

        Args:
            token: JWT token string
            verify: Whether to verify signature and claims

        Returns:
            Token claims

        Raises:
            JWTError: If token is invalid
        """
        # Split token
        parts = token.split(".")
        if len(parts) != 3:
            raise JWTError("Invalid token format")

        header_b64, payload_b64, signature_b64 = parts

        # Decode header
        try:
            header = json.loads(base64url_decode(header_b64))
        except Exception:
            raise JWTError("Invalid token header")

        # Check algorithm
        alg_str = header.get("alg")
        if not alg_str:
            raise JWTError("Missing algorithm in header")

        try:
            algorithm = JWTAlgorithm(alg_str)
        except ValueError:
            raise JWTError(f"Unsupported algorithm: {alg_str}")

        if algorithm not in self.config.algorithms:
            raise JWTError(f"Algorithm not allowed: {alg_str}")

        # Decode payload
        try:
            payload = json.loads(base64url_decode(payload_b64))
        except Exception:
            raise JWTError("Invalid token payload")

        if verify:
            # Verify signature
            signing_input = f"{header_b64}.{payload_b64}"
            expected_signature = self._sign(signing_input, algorithm)

            try:
                actual_signature = base64url_decode(signature_b64)
            except Exception:
                raise JWTError("Invalid signature encoding")

            if not hmac.compare_digest(expected_signature, actual_signature):
                raise JWTError("Invalid signature")

            # Validate claims
            self._validate_claims(payload)

        return payload

    def _sign(self, data: str, algorithm: JWTAlgorithm) -> bytes:
        """Sign data with HMAC."""
        hash_func = self.ALGORITHM_MAP[algorithm]
        return hmac.new(
            self.config.secret_key.encode(),
            data.encode(),
            hash_func,
        ).digest()

    def _validate_claims(self, claims: Dict[str, Any]) -> None:
        """Validate token claims."""
        now = int(time.time())

        # Expiration
        if self.config.verify_exp and "exp" in claims:
            exp = claims["exp"]
            if not isinstance(exp, (int, float)):
                raise JWTError("Invalid exp claim")
            if exp + self.config.clock_skew < now:
                raise JWTError("Token has expired", "token_expired")

        # Not before
        if self.config.verify_nbf and "nbf" in claims:
            nbf = claims["nbf"]
            if not isinstance(nbf, (int, float)):
                raise JWTError("Invalid nbf claim")
            if nbf - self.config.clock_skew > now:
                raise JWTError("Token not yet valid", "token_not_yet_valid")

        # Issuer
        if self.config.issuer:
            if claims.get("iss") != self.config.issuer:
                raise JWTError("Invalid issuer", "invalid_issuer")

        # Audience
        if self.config.audience:
            aud = claims.get("aud")
            if isinstance(aud, str):
                aud = [aud]
            if not aud or not any(a in self.config.audience for a in aud):
                raise JWTError("Invalid audience", "invalid_audience")

        # Required claims
        for claim in self.config.required_claims:
            if claim not in claims:
                raise JWTError(f"Missing required claim: {claim}", "missing_claim")


class JWTBearer:
    """
    FastAPI-compatible JWT Bearer authentication.

    Usage as dependency:
        config = JWTConfig(secret_key="your-secret")
        jwt_bearer = JWTBearer(config)

        @app.get("/protected")
        async def protected(claims: dict = Depends(jwt_bearer)):
            return {"user": claims["sub"]}
    """

    def __init__(
        self,
        config: JWTConfig,
        auto_error: bool = True,
    ):
        """
        Initialize JWT Bearer auth.

        Args:
            config: JWT configuration
            auto_error: Raise HTTPException on failure
        """
        self.config = config
        self.auto_error = auto_error
        self.jwt = JWT(config)

    async def __call__(
        self,
        request: Any = None,
        authorization: str = None,
    ) -> Optional[Dict[str, Any]]:
        """
        Extract and verify JWT from request.

        Can be used as a FastAPI dependency.

        Args:
            request: HTTP request (for extracting header)
            authorization: Authorization header value

        Returns:
            Token claims if valid, None if auto_error=False and invalid

        Raises:
            HTTPException: If auto_error=True and token is invalid
        """
        # Get authorization header
        if authorization is None and request is not None:
            # Try to get from request headers
            if hasattr(request, "headers"):
                authorization = request.headers.get("authorization", "")
            elif hasattr(request, "get_header"):
                authorization = request.get_header("authorization")

        if not authorization:
            if self.auto_error:
                from fasterapi.exceptions import HTTPException

                raise HTTPException(
                    status_code=401,
                    detail="Not authenticated",
                    headers={"WWW-Authenticate": "Bearer"},
                )
            return None

        # Extract token
        scheme, _, token = authorization.partition(" ")
        if scheme.lower() != "bearer":
            if self.auto_error:
                from fasterapi.exceptions import HTTPException

                raise HTTPException(
                    status_code=401,
                    detail="Invalid authentication scheme",
                    headers={"WWW-Authenticate": "Bearer"},
                )
            return None

        if not token:
            if self.auto_error:
                from fasterapi.exceptions import HTTPException

                raise HTTPException(
                    status_code=401,
                    detail="Token not provided",
                    headers={"WWW-Authenticate": "Bearer"},
                )
            return None

        # Verify token
        try:
            claims = self.jwt.decode(token)
            return claims
        except JWTError as e:
            if self.auto_error:
                from fasterapi.exceptions import HTTPException

                raise HTTPException(
                    status_code=401,
                    detail=e.message,
                    headers={"WWW-Authenticate": f'Bearer error="{e.error_type}"'},
                )
            return None

    def create_token(
        self,
        subject: str,
        expires_in: int = 3600,
        claims: Optional[Dict[str, Any]] = None,
    ) -> str:
        """
        Create a new JWT token.

        Args:
            subject: Token subject (usually user ID)
            expires_in: Expiration time in seconds
            claims: Additional claims to include

        Returns:
            JWT token string
        """
        now = int(time.time())
        payload = {
            "sub": subject,
            "iat": now,
            "exp": now + expires_in,
        }

        if self.config.issuer:
            payload["iss"] = self.config.issuer

        if self.config.audience:
            payload["aud"] = (
                self.config.audience[0]
                if len(self.config.audience) == 1
                else self.config.audience
            )

        if claims:
            payload.update(claims)

        return self.jwt.encode(payload)

    def decode_token(self, token: str) -> Dict[str, Any]:
        """
        Decode and verify a token.

        Args:
            token: JWT token string

        Returns:
            Token claims

        Raises:
            JWTError: If token is invalid
        """
        return self.jwt.decode(token)


class JWTAuthMiddleware:
    """
    JWT Authentication Middleware.

    Validates JWT tokens on all requests and adds claims to request state.

    Usage:
        app.add_middleware(
            JWTAuthMiddleware,
            jwt_bearer=jwt_bearer,
            exclude_paths=["/login", "/docs"],
        )
    """

    def __init__(
        self,
        app: Any,
        jwt_bearer: JWTBearer,
        exclude_paths: Optional[List[str]] = None,
        exclude_methods: Optional[List[str]] = None,
    ):
        """
        Initialize JWT auth middleware.

        Args:
            app: ASGI application
            jwt_bearer: JWTBearer instance
            exclude_paths: Paths to exclude from auth
            exclude_methods: HTTP methods to exclude (e.g., ["OPTIONS"])
        """
        self.app = app
        self.jwt_bearer = jwt_bearer
        self.exclude_paths = set(exclude_paths or [])
        self.exclude_methods = set(m.upper() for m in (exclude_methods or []))

    async def __call__(self, scope: Dict, receive: Callable, send: Callable) -> None:
        """ASGI interface."""
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return

        path = scope.get("path", "/")
        method = scope.get("method", "GET").upper()

        # Check exclusions
        if path in self.exclude_paths or method in self.exclude_methods:
            await self.app(scope, receive, send)
            return

        # Extract authorization header
        headers = dict(scope.get("headers", []))
        auth_header = headers.get(b"authorization", b"").decode()

        # Verify token
        try:
            claims = await self.jwt_bearer(authorization=auth_header)
            # Store claims in scope for later access
            scope["jwt_claims"] = claims
        except Exception:
            # Send 401 response
            await self._send_unauthorized(send)
            return

        await self.app(scope, receive, send)

    async def _send_unauthorized(self, send: Callable) -> None:
        """Send 401 Unauthorized response."""
        body = b'{"detail": "Not authenticated"}'

        await send(
            {
                "type": "http.response.start",
                "status": 401,
                "headers": [
                    (b"content-type", b"application/json"),
                    (b"content-length", str(len(body)).encode()),
                    (b"www-authenticate", b"Bearer"),
                ],
            }
        )
        await send(
            {
                "type": "http.response.body",
                "body": body,
            }
        )


def create_jwt_config(
    secret_key: str,
    algorithm: str = "HS256",
    issuer: Optional[str] = None,
    audience: Optional[List[str]] = None,
) -> JWTConfig:
    """
    Create a JWT configuration.

    Args:
        secret_key: Secret key for signing
        algorithm: Algorithm (HS256, HS384, HS512)
        issuer: Token issuer
        audience: Valid audiences

    Returns:
        JWTConfig instance
    """
    return JWTConfig(
        secret_key=secret_key,
        algorithms=[JWTAlgorithm(algorithm)],
        issuer=issuer,
        audience=audience,
    )
