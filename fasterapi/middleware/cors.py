"""
CORS (Cross-Origin Resource Sharing) middleware for FastAPI compatibility.

This wraps/leverages the C++ CORS handling capabilities.
"""

import re
from typing import Any, Callable, Optional, Sequence

from .base import BaseHTTPMiddleware, RequestResponseEndpoint


class CORSMiddleware(BaseHTTPMiddleware):
    """
    CORS middleware that handles Cross-Origin Resource Sharing headers.

    Usage:
        from fasterapi.middleware import CORSMiddleware

        app.add_middleware(
            CORSMiddleware,
            allow_origins=["http://localhost:3000", "https://example.com"],
            allow_credentials=True,
            allow_methods=["*"],
            allow_headers=["*"],
        )

    For development with any origin:
        app.add_middleware(
            CORSMiddleware,
            allow_origins=["*"],
            allow_methods=["*"],
            allow_headers=["*"],
        )
    """

    def __init__(
        self,
        app: Any,
        allow_origins: Sequence[str] = (),
        allow_methods: Sequence[str] = ("GET",),
        allow_headers: Sequence[str] = (),
        allow_credentials: bool = False,
        allow_origin_regex: Optional[str] = None,
        expose_headers: Sequence[str] = (),
        max_age: int = 600,
    ) -> None:
        super().__init__(app)

        # Process allow_origins
        if "*" in allow_origins:
            self.allow_all_origins = True
            self.allow_origins: set[str] = set()
        else:
            self.allow_all_origins = False
            self.allow_origins = set(allow_origins)

        # Process allow_methods
        if "*" in allow_methods:
            self.allow_all_methods = True
            self.allow_methods: set[str] = set()
        else:
            self.allow_all_methods = False
            self.allow_methods = {method.upper() for method in allow_methods}

        # Process allow_headers
        if "*" in allow_headers:
            self.allow_all_headers = True
            self.allow_headers: set[str] = set()
        else:
            self.allow_all_headers = False
            self.allow_headers = {header.lower() for header in allow_headers}

        self.allow_credentials = allow_credentials
        self.allow_origin_regex = (
            re.compile(allow_origin_regex) if allow_origin_regex else None
        )
        self.expose_headers = set(expose_headers)
        self.max_age = max_age

        # Simple headers that don't require preflight
        self.simple_headers: dict[str, str] = {}
        if self.allow_all_origins:
            self.simple_headers["Access-Control-Allow-Origin"] = "*"
        if self.allow_credentials:
            self.simple_headers["Access-Control-Allow-Credentials"] = "true"
        if self.expose_headers:
            self.simple_headers["Access-Control-Expose-Headers"] = ", ".join(
                self.expose_headers
            )

    def is_allowed_origin(self, origin: str) -> bool:
        """Check if the origin is allowed."""
        if self.allow_all_origins:
            return True
        if origin in self.allow_origins:
            return True
        if self.allow_origin_regex and self.allow_origin_regex.fullmatch(origin):
            return True
        return False

    def preflight_response(
        self,
        request_headers: dict[str, str],
    ) -> tuple[int, dict[str, str], str]:
        """Generate preflight response for OPTIONS request."""
        origin = request_headers.get("origin", "")

        headers: dict[str, str] = {}

        # Check origin
        if not self.is_allowed_origin(origin):
            # Return 400 for disallowed origins
            return (400, {}, "Disallowed CORS origin")

        # Set origin header
        if self.allow_all_origins and not self.allow_credentials:
            headers["Access-Control-Allow-Origin"] = "*"
        else:
            headers["Access-Control-Allow-Origin"] = origin
            headers["Vary"] = "Origin"

        # Check method
        requested_method = request_headers.get("access-control-request-method", "")
        if requested_method:
            if self.allow_all_methods:
                headers["Access-Control-Allow-Methods"] = requested_method
            elif requested_method.upper() in self.allow_methods:
                headers["Access-Control-Allow-Methods"] = ", ".join(
                    sorted(self.allow_methods)
                )
            else:
                return (400, {}, "Disallowed CORS method")

        # Check headers
        requested_headers = request_headers.get("access-control-request-headers", "")
        if requested_headers:
            if self.allow_all_headers:
                headers["Access-Control-Allow-Headers"] = requested_headers
            else:
                requested_set = {
                    h.strip().lower() for h in requested_headers.split(",")
                }
                if requested_set.issubset(self.allow_headers):
                    headers["Access-Control-Allow-Headers"] = ", ".join(
                        sorted(self.allow_headers)
                    )
                else:
                    return (400, {}, "Disallowed CORS headers")

        # Credentials
        if self.allow_credentials:
            headers["Access-Control-Allow-Credentials"] = "true"

        # Max age
        headers["Access-Control-Max-Age"] = str(self.max_age)

        return (200, headers, "")

    def add_cors_headers(
        self,
        response_headers: dict[str, str],
        request_headers: dict[str, str],
    ) -> dict[str, str]:
        """Add CORS headers to response."""
        origin = request_headers.get("origin", "")

        if not origin or not self.is_allowed_origin(origin):
            return response_headers

        # Copy existing headers
        headers = dict(response_headers)

        # Set origin
        if self.allow_all_origins and not self.allow_credentials:
            headers["Access-Control-Allow-Origin"] = "*"
        else:
            headers["Access-Control-Allow-Origin"] = origin
            # Add Vary header
            existing_vary = headers.get("Vary", "")
            if existing_vary:
                if "origin" not in existing_vary.lower():
                    headers["Vary"] = f"{existing_vary}, Origin"
            else:
                headers["Vary"] = "Origin"

        # Credentials
        if self.allow_credentials:
            headers["Access-Control-Allow-Credentials"] = "true"

        # Expose headers
        if self.expose_headers:
            headers["Access-Control-Expose-Headers"] = ", ".join(self.expose_headers)

        return headers

    async def dispatch(
        self,
        request: Any,
        call_next: RequestResponseEndpoint,
    ) -> Any:
        """Handle CORS for the request."""
        # Get request headers
        if hasattr(request, "headers"):
            if isinstance(request.headers, dict):
                request_headers = {k.lower(): v for k, v in request.headers.items()}
            else:
                request_headers = {
                    k.lower(): v for k, v in dict(request.headers).items()
                }
        else:
            request_headers = {}

        origin = request_headers.get("origin")

        # If no origin header, this isn't a CORS request
        if not origin:
            return await call_next(request)

        # Get request method
        method = getattr(request, "method", "GET")
        if hasattr(method, "upper"):
            method = method.upper()

        # Handle preflight OPTIONS request
        if method == "OPTIONS":
            status, headers, body = self.preflight_response(request_headers)
            # Return preflight response
            # This creates a minimal response object
            return PreflightResponse(status, headers, body)

        # Handle actual request
        response = await call_next(request)

        # Add CORS headers to response
        if hasattr(response, "headers"):
            if isinstance(response.headers, dict):
                response.headers = self.add_cors_headers(
                    response.headers, request_headers
                )
            elif hasattr(response.headers, "update"):
                cors_headers = self.add_cors_headers(
                    dict(response.headers), request_headers
                )
                response.headers.update(cors_headers)

        return response


class PreflightResponse:
    """Simple response object for CORS preflight."""

    def __init__(
        self,
        status_code: int,
        headers: dict[str, str],
        body: str,
    ) -> None:
        self.status_code = status_code
        self.headers = headers
        self.body = body.encode() if isinstance(body, str) else body


__all__ = [
    "CORSMiddleware",
]
