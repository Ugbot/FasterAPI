"""
HTTP authentication utilities for FasterAPI.

Provides HTTP Basic, HTTP Bearer, and HTTP Digest authentication.
"""

import base64
from typing import Any, NamedTuple, Optional

from fasterapi.exceptions import HTTPException


class HTTPBasicCredentials(NamedTuple):
    """
    HTTP Basic authentication credentials.

    Attributes:
        username: The username
        password: The password
    """

    username: str
    password: str


class HTTPAuthorizationCredentials(NamedTuple):
    """
    HTTP authorization credentials.

    Attributes:
        scheme: Authorization scheme (e.g., "Bearer", "Basic")
        credentials: The credentials string
    """

    scheme: str
    credentials: str


class HTTPBase:
    """
    Base class for HTTP authentication schemes.
    """

    def __init__(
        self,
        *,
        scheme: str,
        scheme_name: Optional[str] = None,
        description: Optional[str] = None,
        auto_error: bool = True,
    ):
        """
        Initialize HTTP authentication scheme.

        Args:
            scheme: HTTP authentication scheme name
            scheme_name: Name for OpenAPI schema
            description: Description for OpenAPI
            auto_error: If True, raise HTTPException on auth failure
        """
        self.scheme = scheme
        self.scheme_name = scheme_name or self.__class__.__name__
        self.description = description
        self.auto_error = auto_error


class HTTPBasic(HTTPBase):
    """
    HTTP Basic authentication.

    Extracts and decodes Basic authentication credentials.

    Usage:
        security = HTTPBasic()

        @app.get("/protected")
        def protected(credentials: HTTPBasicCredentials = Depends(security)):
            if not verify(credentials.username, credentials.password):
                raise HTTPException(status_code=401)
            return {"user": credentials.username}
    """

    def __init__(
        self,
        *,
        scheme_name: Optional[str] = None,
        realm: Optional[str] = None,
        description: Optional[str] = None,
        auto_error: bool = True,
    ):
        """
        Initialize HTTP Basic authentication.

        Args:
            scheme_name: Name for OpenAPI schema
            realm: Authentication realm
            description: Description for OpenAPI
            auto_error: If True, raise HTTPException on auth failure
        """
        super().__init__(
            scheme="basic",
            scheme_name=scheme_name,
            description=description,
            auto_error=auto_error,
        )
        self.realm = realm

    async def __call__(self, request: Any) -> Optional[HTTPBasicCredentials]:
        """
        Extract Basic auth credentials from request.

        Args:
            request: HTTP request object

        Returns:
            HTTPBasicCredentials, or None if auto_error is False

        Raises:
            HTTPException: If authentication fails and auto_error is True
        """
        # Get Authorization header
        authorization = None
        if hasattr(request, "get_header"):
            authorization = request.get_header("authorization")
        elif hasattr(request, "headers"):
            authorization = request.headers.get("authorization")

        www_authenticate = f'Basic realm="{self.realm}"' if self.realm else "Basic"

        if not authorization:
            if self.auto_error:
                raise HTTPException(
                    status_code=401,
                    detail="Not authenticated",
                    headers={"WWW-Authenticate": www_authenticate},
                )
            return None

        # Parse Basic credentials
        scheme, _, credentials = authorization.partition(" ")
        if scheme.lower() != "basic":
            if self.auto_error:
                raise HTTPException(
                    status_code=401,
                    detail="Invalid authentication scheme",
                    headers={"WWW-Authenticate": www_authenticate},
                )
            return None

        # Decode credentials
        try:
            decoded = base64.b64decode(credentials).decode("utf-8")
            username, _, password = decoded.partition(":")
            return HTTPBasicCredentials(username=username, password=password)
        except Exception:
            if self.auto_error:
                raise HTTPException(
                    status_code=401,
                    detail="Invalid credentials format",
                    headers={"WWW-Authenticate": www_authenticate},
                )
            return None


class HTTPBearer(HTTPBase):
    """
    HTTP Bearer token authentication.

    Extracts Bearer tokens from the Authorization header.

    Usage:
        security = HTTPBearer()

        @app.get("/protected")
        def protected(
            credentials: HTTPAuthorizationCredentials = Depends(security)
        ):
            token = credentials.credentials
            return verify_token(token)
    """

    def __init__(
        self,
        *,
        bearerFormat: Optional[str] = None,
        scheme_name: Optional[str] = None,
        description: Optional[str] = None,
        auto_error: bool = True,
    ):
        """
        Initialize HTTP Bearer authentication.

        Args:
            bearerFormat: Bearer token format (e.g., "JWT")
            scheme_name: Name for OpenAPI schema
            description: Description for OpenAPI
            auto_error: If True, raise HTTPException on auth failure
        """
        super().__init__(
            scheme="bearer",
            scheme_name=scheme_name,
            description=description,
            auto_error=auto_error,
        )
        self.bearerFormat = bearerFormat

    async def __call__(self, request: Any) -> Optional[HTTPAuthorizationCredentials]:
        """
        Extract Bearer token from request.

        Args:
            request: HTTP request object

        Returns:
            HTTPAuthorizationCredentials, or None if auto_error is False

        Raises:
            HTTPException: If authentication fails and auto_error is True
        """
        # Get Authorization header
        authorization = None
        if hasattr(request, "get_header"):
            authorization = request.get_header("authorization")
        elif hasattr(request, "headers"):
            authorization = request.headers.get("authorization")

        if not authorization:
            if self.auto_error:
                raise HTTPException(
                    status_code=401,
                    detail="Not authenticated",
                    headers={"WWW-Authenticate": "Bearer"},
                )
            return None

        # Parse Bearer token
        scheme, _, credentials = authorization.partition(" ")
        if scheme.lower() != "bearer":
            if self.auto_error:
                raise HTTPException(
                    status_code=401,
                    detail="Invalid authentication scheme",
                    headers={"WWW-Authenticate": "Bearer"},
                )
            return None

        if not credentials:
            if self.auto_error:
                raise HTTPException(
                    status_code=401,
                    detail="Invalid credentials",
                    headers={"WWW-Authenticate": "Bearer"},
                )
            return None

        return HTTPAuthorizationCredentials(scheme=scheme, credentials=credentials)


class HTTPDigest(HTTPBase):
    """
    HTTP Digest authentication.

    Note: This is a placeholder. Digest authentication is rarely used
    in modern APIs. Consider using OAuth2 or API keys instead.
    """

    def __init__(
        self,
        *,
        scheme_name: Optional[str] = None,
        description: Optional[str] = None,
        auto_error: bool = True,
    ):
        """
        Initialize HTTP Digest authentication.

        Args:
            scheme_name: Name for OpenAPI schema
            description: Description for OpenAPI
            auto_error: If True, raise HTTPException on auth failure
        """
        super().__init__(
            scheme="digest",
            scheme_name=scheme_name,
            description=description,
            auto_error=auto_error,
        )

    async def __call__(self, request: Any) -> Optional[HTTPAuthorizationCredentials]:
        """Extract Digest credentials from request."""
        authorization = None
        if hasattr(request, "get_header"):
            authorization = request.get_header("authorization")
        elif hasattr(request, "headers"):
            authorization = request.headers.get("authorization")

        if not authorization:
            if self.auto_error:
                raise HTTPException(
                    status_code=401,
                    detail="Not authenticated",
                    headers={"WWW-Authenticate": "Digest"},
                )
            return None

        scheme, _, credentials = authorization.partition(" ")
        if scheme.lower() != "digest":
            if self.auto_error:
                raise HTTPException(
                    status_code=401,
                    detail="Invalid authentication scheme",
                    headers={"WWW-Authenticate": "Digest"},
                )
            return None

        return HTTPAuthorizationCredentials(scheme=scheme, credentials=credentials)
