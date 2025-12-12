"""
GZip compression middleware for FastAPI compatibility.

This can work alongside the C++ compression layer or independently.
"""

import gzip
import io
from typing import Any, Set

from .base import BaseHTTPMiddleware, RequestResponseEndpoint


class GZipMiddleware(BaseHTTPMiddleware):
    """
    Middleware for gzip-compressing responses.

    Note: The C++ server may already handle compression. This middleware
    provides a Python-level fallback and additional configuration options.

    Usage:
        from fasterapi.middleware import GZipMiddleware

        app.add_middleware(GZipMiddleware, minimum_size=1000)
    """

    # Content types that should be compressed
    COMPRESSIBLE_TYPES: Set[str] = {
        "text/html",
        "text/css",
        "text/plain",
        "text/javascript",
        "text/xml",
        "application/json",
        "application/javascript",
        "application/xml",
        "application/xhtml+xml",
        "application/rss+xml",
        "application/atom+xml",
        "image/svg+xml",
    }

    def __init__(
        self,
        app: Any,
        minimum_size: int = 500,
        compresslevel: int = 9,
    ) -> None:
        """
        Initialize GZip middleware.

        Args:
            app: The application
            minimum_size: Minimum response body size to compress (bytes)
            compresslevel: Compression level 1-9 (9 = max compression)
        """
        super().__init__(app)
        self.minimum_size = minimum_size
        self.compresslevel = compresslevel

    def should_compress(
        self,
        request_headers: dict[str, str],
        response_headers: dict[str, str],
        body: bytes,
    ) -> bool:
        """Determine if the response should be compressed."""
        # Check if client accepts gzip
        accept_encoding = request_headers.get("accept-encoding", "")
        if "gzip" not in accept_encoding.lower():
            return False

        # Check if response is already compressed
        content_encoding = response_headers.get("content-encoding", "")
        if content_encoding:
            return False

        # Check body size
        if len(body) < self.minimum_size:
            return False

        # Check content type
        content_type = response_headers.get("content-type", "")
        base_type = content_type.split(";")[0].strip().lower()
        if base_type not in self.COMPRESSIBLE_TYPES:
            # Also compress if it looks like text
            if not base_type.startswith("text/"):
                return False

        return True

    def compress(self, body: bytes) -> bytes:
        """Compress the body using gzip."""
        buf = io.BytesIO()
        with gzip.GzipFile(
            mode="wb",
            fileobj=buf,
            compresslevel=self.compresslevel,
        ) as f:
            f.write(body)
        return buf.getvalue()

    async def dispatch(
        self,
        request: Any,
        call_next: RequestResponseEndpoint,
    ) -> Any:
        """Handle the request and optionally compress the response."""
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

        # Call the next handler
        response = await call_next(request)

        # Get response body
        body: bytes = b""
        if hasattr(response, "body"):
            body = (
                response.body
                if isinstance(response.body, bytes)
                else response.body.encode()
            )

        # Get response headers
        if hasattr(response, "headers"):
            if isinstance(response.headers, dict):
                response_headers = {k.lower(): v for k, v in response.headers.items()}
            else:
                response_headers = {
                    k.lower(): v for k, v in dict(response.headers).items()
                }
        else:
            response_headers = {}

        # Check if we should compress
        if body and self.should_compress(request_headers, response_headers, body):
            compressed_body = self.compress(body)

            # Only use compressed if it's actually smaller
            if len(compressed_body) < len(body):
                response.body = compressed_body

                # Update headers
                if hasattr(response, "headers"):
                    if isinstance(response.headers, dict):
                        response.headers["Content-Encoding"] = "gzip"
                        response.headers["Content-Length"] = str(len(compressed_body))
                        # Add Vary header
                        existing_vary = response.headers.get("Vary", "")
                        if existing_vary:
                            if "accept-encoding" not in existing_vary.lower():
                                response.headers["Vary"] = (
                                    f"{existing_vary}, Accept-Encoding"
                                )
                        else:
                            response.headers["Vary"] = "Accept-Encoding"

        return response


__all__ = [
    "GZipMiddleware",
]
