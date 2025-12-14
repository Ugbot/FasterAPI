"""
FastAPI-compatible security utilities for FasterAPI.

Provides OAuth2, HTTP Basic, HTTP Bearer, API key, and JWT authentication schemes.

Usage:
    from fasterapi.security import OAuth2PasswordBearer, HTTPBasic, APIKeyHeader

    oauth2_scheme = OAuth2PasswordBearer(tokenUrl="token")

    @app.get("/users/me")
    def get_current_user(token: str = Depends(oauth2_scheme)):
        return decode_token(token)

JWT Usage:
    from fasterapi.security import JWTBearer, JWTConfig

    config = JWTConfig(secret_key="your-secret-key")
    jwt_bearer = JWTBearer(config)

    @app.get("/protected")
    async def protected(claims: dict = Depends(jwt_bearer)):
        return {"user": claims["sub"]}
"""

from fasterapi.security.api_key import (
    APIKeyCookie,
    APIKeyHeader,
    APIKeyQuery,
)
from fasterapi.security.http import (
    HTTPAuthorizationCredentials,
    HTTPBasic,
    HTTPBasicCredentials,
    HTTPBearer,
    HTTPDigest,
)
from fasterapi.security.jwt import (
    JWT,
    JWTAlgorithm,
    JWTAuthMiddleware,
    JWTBearer,
    JWTConfig,
    JWTError,
    base64url_decode,
    base64url_encode,
    create_jwt_config,
)
from fasterapi.security.oauth2 import (
    OAuth2,
    OAuth2AuthorizationCodeBearer,
    OAuth2PasswordBearer,
    OAuth2PasswordRequestForm,
    OAuth2PasswordRequestFormStrict,
    SecurityScopes,
)

__all__ = [
    # JWT
    "JWT",
    "JWTAlgorithm",
    "JWTAuthMiddleware",
    "JWTBearer",
    "JWTConfig",
    "JWTError",
    "base64url_decode",
    "base64url_encode",
    "create_jwt_config",
    # OAuth2
    "OAuth2",
    "OAuth2AuthorizationCodeBearer",
    "OAuth2PasswordBearer",
    "OAuth2PasswordRequestForm",
    "OAuth2PasswordRequestFormStrict",
    "SecurityScopes",
    # HTTP
    "HTTPAuthorizationCredentials",
    "HTTPBasic",
    "HTTPBasicCredentials",
    "HTTPBearer",
    "HTTPDigest",
    # API Key
    "APIKeyCookie",
    "APIKeyHeader",
    "APIKeyQuery",
]
