"""
Trusted Host middleware for FastAPI compatibility.

Validates that requests come from trusted hosts.
"""

from typing import Any, Optional, Sequence

from ..exceptions import HTTPException
from ..status import HTTP_400_BAD_REQUEST
from .base import BaseHTTPMiddleware, RequestResponseEndpoint


class TrustedHostMiddleware(BaseHTTPMiddleware):
    """
    Middleware that validates the Host header against a list of trusted hosts.

    This helps prevent Host header attacks.

    Usage:
        from fasterapi.middleware import TrustedHostMiddleware

        app.add_middleware(
            TrustedHostMiddleware,
            allowed_hosts=["example.com", "*.example.com"],
        )
    """

    def __init__(
        self,
        app: Any,
        allowed_hosts: Optional[Sequence[str]] = None,
        www_redirect: bool = True,
    ) -> None:
        """
        Initialize TrustedHost middleware.

        Args:
            app: The application
            allowed_hosts: List of allowed host patterns. Supports wildcards (*.example.com)
                          Use ["*"] to allow any host (not recommended for production)
            www_redirect: If True, redirect www.domain to domain
        """
        super().__init__(app)

        if allowed_hosts is None:
            allowed_hosts = ["*"]

        self.allowed_hosts = list(allowed_hosts)
        self.allow_any = "*" in allowed_hosts
        self.www_redirect = www_redirect

        # Separate exact matches from wildcard patterns
        self.exact_hosts: set[str] = set()
        self.wildcard_patterns: list[str] = []

        for host in allowed_hosts:
            if host == "*":
                continue
            elif host.startswith("*."):
                # Wildcard pattern
                self.wildcard_patterns.append(host[2:].lower())  # Remove "*."
            else:
                self.exact_hosts.add(host.lower())

    def is_valid_host(self, host: str) -> bool:
        """Check if the host is in the allowed list."""
        if self.allow_any:
            return True

        # Remove port if present
        if ":" in host:
            host = host.split(":")[0]

        host = host.lower()

        # Check exact match
        if host in self.exact_hosts:
            return True

        # Check wildcard patterns
        for pattern in self.wildcard_patterns:
            if host.endswith(f".{pattern}") or host == pattern:
                return True

        return False

    async def dispatch(
        self,
        request: Any,
        call_next: RequestResponseEndpoint,
    ) -> Any:
        """Validate the Host header."""
        # Get host from request
        host: str = ""

        if hasattr(request, "headers"):
            headers = request.headers
            if isinstance(headers, dict):
                host = headers.get("host", headers.get("Host", ""))
            elif hasattr(headers, "get"):
                host = headers.get("host") or headers.get("Host") or ""

        # Also check X-Forwarded-Host for proxied requests
        if hasattr(request, "headers"):
            headers = request.headers
            if isinstance(headers, dict):
                forwarded_host = headers.get("x-forwarded-host", "")
            elif hasattr(headers, "get"):
                forwarded_host = headers.get("x-forwarded-host") or ""
            else:
                forwarded_host = ""

            if forwarded_host:
                host = forwarded_host

        if not host:
            # No host header - reject
            raise HTTPException(
                status_code=HTTP_400_BAD_REQUEST,
                detail="Missing Host header",
            )

        # Validate host
        if not self.is_valid_host(host):
            raise HTTPException(
                status_code=HTTP_400_BAD_REQUEST,
                detail="Invalid host header",
            )

        # Handle www redirect
        if self.www_redirect:
            host_without_port = host.split(":")[0].lower()
            if host_without_port.startswith("www."):
                # Check if non-www version is allowed
                non_www = host_without_port[4:]
                if non_www in self.exact_hosts:
                    # Return redirect response
                    scheme = "https"
                    if hasattr(request, "url"):
                        url = request.url
                        if hasattr(url, "scheme"):
                            scheme = url.scheme

                    # Build redirect URL
                    path = "/"
                    if hasattr(request, "url"):
                        url = request.url
                        if hasattr(url, "path"):
                            path = url.path

                    redirect_url = f"{scheme}://{non_www}{path}"

                    return RedirectResponse(redirect_url, status_code=301)

        return await call_next(request)


class RedirectResponse:
    """Simple redirect response object."""

    def __init__(self, url: str, status_code: int = 307) -> None:
        self.status_code = status_code
        self.headers = {"Location": url}
        self.body = b""


__all__ = [
    "TrustedHostMiddleware",
]
