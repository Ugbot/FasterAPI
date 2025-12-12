"""
API Key authentication schemes for FastAPI compatibility.

Provides APIKeyHeader, APIKeyQuery, and APIKeyCookie for extracting
API keys from different request locations.
"""

from typing import Any, Optional

from ..exceptions import HTTPException
from ..status import HTTP_403_FORBIDDEN


class APIKeyBase:
    """Base class for API key authentication."""

    model: Any = None  # For OpenAPI schema generation
    scheme_name: Optional[str] = None

    def __init__(
        self,
        *,
        name: str,
        scheme_name: Optional[str] = None,
        description: Optional[str] = None,
        auto_error: bool = True,
    ) -> None:
        self.name = name
        self.scheme_name = scheme_name or self.__class__.__name__
        self.description = description
        self.auto_error = auto_error

    async def __call__(self, request: Any) -> Optional[str]:
        raise NotImplementedError


class APIKeyHeader(APIKeyBase):
    """
    API Key authentication via HTTP header.

    Usage:
        api_key_header = APIKeyHeader(name="X-API-Key")

        @app.get("/protected")
        async def protected(api_key: str = Depends(api_key_header)):
            return {"api_key": api_key}
    """

    def __init__(
        self,
        *,
        name: str,
        scheme_name: Optional[str] = None,
        description: Optional[str] = None,
        auto_error: bool = True,
    ) -> None:
        super().__init__(
            name=name,
            scheme_name=scheme_name,
            description=description,
            auto_error=auto_error,
        )
        # OpenAPI security scheme
        self.model = {
            "type": "apiKey",
            "in": "header",
            "name": name,
            "description": description,
        }

    async def __call__(self, request: Any) -> Optional[str]:
        """Extract API key from request header."""
        api_key: Optional[str] = None

        # Try different ways to access headers
        if hasattr(request, "headers"):
            headers = request.headers
            if isinstance(headers, dict):
                # Case-insensitive header lookup
                header_lower = self.name.lower()
                for key, value in headers.items():
                    if key.lower() == header_lower:
                        api_key = value
                        break
            elif hasattr(headers, "get"):
                api_key = headers.get(self.name)

        if not api_key:
            if self.auto_error:
                raise HTTPException(
                    status_code=HTTP_403_FORBIDDEN,
                    detail="Not authenticated",
                )
            return None

        return api_key


class APIKeyQuery(APIKeyBase):
    """
    API Key authentication via query parameter.

    Usage:
        api_key_query = APIKeyQuery(name="api_key")

        @app.get("/protected")
        async def protected(api_key: str = Depends(api_key_query)):
            return {"api_key": api_key}
    """

    def __init__(
        self,
        *,
        name: str,
        scheme_name: Optional[str] = None,
        description: Optional[str] = None,
        auto_error: bool = True,
    ) -> None:
        super().__init__(
            name=name,
            scheme_name=scheme_name,
            description=description,
            auto_error=auto_error,
        )
        # OpenAPI security scheme
        self.model = {
            "type": "apiKey",
            "in": "query",
            "name": name,
            "description": description,
        }

    async def __call__(self, request: Any) -> Optional[str]:
        """Extract API key from query parameter."""
        api_key: Optional[str] = None

        # Try different ways to access query params
        if hasattr(request, "query_params"):
            query_params = request.query_params
            if isinstance(query_params, dict):
                api_key = query_params.get(self.name)
            elif hasattr(query_params, "get"):
                api_key = query_params.get(self.name)
        elif hasattr(request, "query"):
            # Alternative attribute name
            query = request.query
            if isinstance(query, dict):
                api_key = query.get(self.name)
            elif hasattr(query, "get"):
                api_key = query.get(self.name)

        if not api_key:
            if self.auto_error:
                raise HTTPException(
                    status_code=HTTP_403_FORBIDDEN,
                    detail="Not authenticated",
                )
            return None

        return api_key


class APIKeyCookie(APIKeyBase):
    """
    API Key authentication via cookie.

    Usage:
        api_key_cookie = APIKeyCookie(name="session")

        @app.get("/protected")
        async def protected(api_key: str = Depends(api_key_cookie)):
            return {"api_key": api_key}
    """

    def __init__(
        self,
        *,
        name: str,
        scheme_name: Optional[str] = None,
        description: Optional[str] = None,
        auto_error: bool = True,
    ) -> None:
        super().__init__(
            name=name,
            scheme_name=scheme_name,
            description=description,
            auto_error=auto_error,
        )
        # OpenAPI security scheme
        self.model = {
            "type": "apiKey",
            "in": "cookie",
            "name": name,
            "description": description,
        }

    async def __call__(self, request: Any) -> Optional[str]:
        """Extract API key from cookie."""
        api_key: Optional[str] = None

        # Try different ways to access cookies
        if hasattr(request, "cookies"):
            cookies = request.cookies
            if isinstance(cookies, dict):
                api_key = cookies.get(self.name)
            elif hasattr(cookies, "get"):
                api_key = cookies.get(self.name)

        if not api_key:
            if self.auto_error:
                raise HTTPException(
                    status_code=HTTP_403_FORBIDDEN,
                    detail="Not authenticated",
                )
            return None

        return api_key


__all__ = [
    "APIKeyBase",
    "APIKeyHeader",
    "APIKeyQuery",
    "APIKeyCookie",
]
