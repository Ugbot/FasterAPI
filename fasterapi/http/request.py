"""
HTTP Request Python Wrapper

Zero-copy access to HTTP request data.
FastAPI-compatible async methods for body access.
"""

import asyncio
import json as json_module
from enum import Enum
from typing import Any, AsyncIterator, Awaitable, Callable, Dict, Optional, Union

# Import data structures
from fasterapi.datastructures import URL, Address, FormData, State, UploadFile


class Method(Enum):
    """HTTP methods."""

    GET = "GET"
    POST = "POST"
    PUT = "PUT"
    DELETE = "DELETE"
    PATCH = "PATCH"
    HEAD = "HEAD"
    OPTIONS = "OPTIONS"
    CONNECT = "CONNECT"
    TRACE = "TRACE"


class Request:
    """
    HTTP request object with zero-copy access to headers and body.

    FastAPI-compatible with async methods:
    - await request.json()
    - await request.form()
    - await request.body()
    - async for chunk in request.stream()

    Features:
    - Zero-copy header access
    - Streaming body support
    - Path parameter extraction
    - Query parameter parsing
    - Multipart form data
    - JSON body parsing
    - Request.state for middleware data
    """

    def __init__(self, method: str = "GET", path: str = "/", **kwargs):
        """
        Create a new HTTP request.

        Args:
            method: HTTP method
            path: Request path
            **kwargs: Additional request data
        """
        self.method = Method(method.upper())
        self.path = path
        self.query = kwargs.get("query", "")
        self.version = kwargs.get("version", "HTTP/1.1")
        self.protocol = kwargs.get("protocol", "HTTP/1.1")
        self._headers = kwargs.get("headers", {})
        self.query_params = kwargs.get("query_params", {})
        self.path_params = kwargs.get("path_params", {})
        self._body = kwargs.get("body", "")
        self._body_bytes = kwargs.get("body_bytes", b"")
        self._client_ip = kwargs.get("client_ip", "127.0.0.1")
        self._client_port = kwargs.get("client_port", 0)
        self.request_id = kwargs.get("request_id", 0)
        self.timestamp = kwargs.get("timestamp", 0)
        self.secure = kwargs.get("secure", False)

        # ASGI scope for compatibility
        self.scope = kwargs.get("scope", {})

        # Receive callable for streaming (ASGI)
        self._receive: Optional[Callable[[], Awaitable[dict]]] = kwargs.get("receive")

        # Cached parsed data
        self._json: Any = None
        self._form: Optional[FormData] = None
        self._body_consumed = False

        # Request state for middleware
        self._state = State()

    def get_method(self) -> Method:
        """Get HTTP method."""
        return self.method

    def get_path(self) -> str:
        """Get request path."""
        return self.path

    def get_query(self) -> str:
        """Get query string."""
        return self.query

    def get_version(self) -> str:
        """Get HTTP version."""
        return self.version

    # =========================================================================
    # FastAPI-compatible properties
    # =========================================================================

    @property
    def headers(self) -> Dict[str, str]:
        """Get all headers (FastAPI-compatible property)."""
        return self._headers

    @property
    def url(self) -> URL:
        """Get request URL (FastAPI-compatible property)."""
        scheme = "https" if self.secure else "http"
        host = self.get_header("host") or "localhost"
        query_str = f"?{self.query}" if self.query else ""
        return URL(f"{scheme}://{host}{self.path}{query_str}")

    @property
    def base_url(self) -> URL:
        """Get base URL without path."""
        scheme = "https" if self.secure else "http"
        host = self.get_header("host") or "localhost"
        return URL(f"{scheme}://{host}")

    @property
    def client(self) -> Optional[Address]:
        """Get client address (FastAPI-compatible property)."""
        if self._client_ip:
            return Address(self._client_ip, self._client_port or None)
        return None

    @property
    def state(self) -> State:
        """Get request state for storing middleware data."""
        return self._state

    @property
    def cookies(self) -> Dict[str, str]:
        """Parse and return cookies from Cookie header."""
        cookie_header = self.get_header("cookie")
        if not cookie_header:
            return {}
        result = {}
        for item in cookie_header.split(";"):
            item = item.strip()
            if "=" in item:
                key, value = item.split("=", 1)
                result[key.strip()] = value.strip()
        return result

    # =========================================================================
    # Header access methods
    # =========================================================================

    def get_header(self, name: str) -> str:
        """
        Get header value.

        Args:
            name: Header name (case-insensitive)

        Returns:
            Header value, or empty string if not found
        """
        return self._headers.get(name.lower(), "")

    def get_headers(self) -> Dict[str, str]:
        """Get all headers."""
        return self._headers.copy()

    def get_query_param(self, name: str) -> str:
        """
        Get query parameter.

        Args:
            name: Parameter name

        Returns:
            Parameter value, or empty string if not found
        """
        return self.query_params.get(name, "")

    def get_path_param(self, name: str) -> str:
        """
        Get path parameter (from route pattern).

        Args:
            name: Parameter name

        Returns:
            Parameter value, or empty string if not found
        """
        return self.path_params.get(name, "")

    def get_body(self) -> str:
        """Get request body."""
        return self._body

    def get_body_bytes(self) -> bytes:
        """Get request body as bytes."""
        return self._body_bytes

    def get_content_type(self) -> str:
        """Get content type."""
        return self.get_header("content-type")

    def get_content_length(self) -> int:
        """Get content length."""
        try:
            return int(self.get_header("content-length") or "0")
        except ValueError:
            return 0

    def is_json(self) -> bool:
        """Check if request has JSON body."""
        content_type = self.get_content_type().lower()
        return "application/json" in content_type

    def is_multipart(self) -> bool:
        """Check if request has multipart body."""
        content_type = self.get_content_type().lower()
        return "multipart/form-data" in content_type

    def get_client_ip(self) -> str:
        """Get client IP address."""
        return self._client_ip

    def get_user_agent(self) -> str:
        """Get user agent."""
        return self.get_header("user-agent")

    def get_request_id(self) -> int:
        """Get request ID."""
        return self.request_id

    def get_timestamp(self) -> int:
        """Get request timestamp."""
        return self.timestamp

    def is_secure(self) -> bool:
        """Check if request is over HTTPS."""
        return self.secure

    def get_protocol(self) -> str:
        """Get protocol (HTTP/1.1, HTTP/2, HTTP/3)."""
        return self.protocol

    # =========================================================================
    # FastAPI-compatible async methods
    # =========================================================================

    async def body(self) -> bytes:
        """
        Get request body as bytes (async, FastAPI-compatible).

        If using ASGI receive, streams the body. Otherwise returns cached body.

        Returns:
            Request body as bytes
        """
        # If we have cached body bytes, return them
        if self._body_bytes:
            return self._body_bytes

        # If we have a string body, encode it
        if self._body:
            self._body_bytes = self._body.encode("utf-8")
            return self._body_bytes

        # If we have ASGI receive, stream the body
        if self._receive and not self._body_consumed:
            chunks = []
            while True:
                message = await self._receive()
                body_chunk = message.get("body", b"")
                if body_chunk:
                    chunks.append(body_chunk)
                if not message.get("more_body", False):
                    break
            self._body_bytes = b"".join(chunks)
            self._body_consumed = True
            return self._body_bytes

        return b""

    async def json(self) -> Any:
        """
        Parse JSON body (async, FastAPI-compatible).

        Returns:
            Parsed JSON data

        Raises:
            ValueError: If body is not valid JSON
        """
        # Return cached JSON if available
        if self._json is not None:
            return self._json

        body_bytes = await self.body()

        if not body_bytes:
            raise ValueError("Request body is empty")

        try:
            self._json = json_module.loads(body_bytes.decode("utf-8"))
            return self._json
        except json_module.JSONDecodeError as e:
            raise ValueError(f"Invalid JSON: {e}")

    async def form(self) -> FormData:
        """
        Parse form data (async, FastAPI-compatible).

        Handles both multipart/form-data and application/x-www-form-urlencoded.

        Returns:
            FormData dictionary with fields and files
        """
        # Return cached form if available
        if self._form is not None:
            return self._form

        # Keep original content_type for boundary parsing (case-sensitive)
        content_type = self.get_content_type()
        content_type_lower = content_type.lower()
        body_bytes = await self.body()

        self._form = FormData()

        if "multipart/form-data" in content_type_lower:
            # Parse multipart form data (use original content_type for boundary)
            self._form = await self._parse_multipart(body_bytes, content_type)
        elif "application/x-www-form-urlencoded" in content_type_lower:
            # Parse URL-encoded form data
            import urllib.parse

            body_str = body_bytes.decode("utf-8")
            for key, value in urllib.parse.parse_qsl(body_str):
                self._form[key] = value

        return self._form

    async def _parse_multipart(self, body: bytes, content_type: str) -> FormData:
        """
        Parse multipart/form-data body.

        Args:
            body: Raw body bytes
            content_type: Content-Type header value

        Returns:
            FormData with parsed fields and files
        """
        form_data = FormData()

        # Extract boundary from content-type
        boundary = None
        for part in content_type.split(";"):
            part = part.strip()
            if part.startswith("boundary="):
                boundary = part[9:].strip("\"'")
                break

        if not boundary:
            return form_data

        # Parse multipart body using CRLF-based boundary markers
        boundary_bytes = b"\r\n--" + boundary.encode("utf-8")
        end_boundary = b"--" + boundary.encode("utf-8") + b"--"

        # Remove the initial boundary (doesn't have leading CRLF)
        initial_boundary = b"--" + boundary.encode("utf-8")
        if body.startswith(initial_boundary):
            body = body[len(initial_boundary) :]

        # Split by boundary
        parts = body.split(boundary_bytes)

        for part in parts:
            # Skip empty parts and end boundary marker
            if not part or part.strip() in (b"", b"--"):
                continue

            # Check if this part is or contains the end boundary
            if part.startswith(b"--"):
                continue

            # Remove leading CRLF
            if part.startswith(b"\r\n"):
                part = part[2:]

            # Split headers from content
            if b"\r\n\r\n" in part:
                headers_section, content = part.split(b"\r\n\r\n", 1)
            elif b"\n\n" in part:
                headers_section, content = part.split(b"\n\n", 1)
            else:
                continue

            # Parse part headers
            part_headers = {}
            for line in headers_section.decode("utf-8", errors="replace").split("\n"):
                line = line.strip("\r\n ")
                if ":" in line:
                    key, value = line.split(":", 1)
                    part_headers[key.strip().lower()] = value.strip()

            # Get content-disposition
            disposition = part_headers.get("content-disposition", "")

            # Parse name and filename from content-disposition
            name = None
            filename = None
            for item in disposition.split(";"):
                item = item.strip()
                if item.startswith("name="):
                    name = item[5:].strip("\"'")
                elif item.startswith("filename="):
                    filename = item[9:].strip("\"'")

            if not name:
                continue

            if filename:
                # This is a file upload
                content_type_part = part_headers.get(
                    "content-type", "application/octet-stream"
                )
                upload_file = UploadFile.from_bytes(
                    content, filename=filename, content_type=content_type_part
                )
                form_data[name] = upload_file
            else:
                # This is a regular form field
                form_data[name] = content.decode("utf-8", errors="replace")

        return form_data

    async def stream(self) -> AsyncIterator[bytes]:
        """
        Stream request body in chunks (async, FastAPI-compatible).

        Yields:
            Body chunks as bytes
        """
        if self._receive and not self._body_consumed:
            while True:
                message = await self._receive()
                body_chunk = message.get("body", b"")
                if body_chunk:
                    yield body_chunk
                if not message.get("more_body", False):
                    break
            self._body_consumed = True
        elif self._body_bytes:
            yield self._body_bytes
        elif self._body:
            yield self._body.encode("utf-8")

    # =========================================================================
    # Sync convenience methods (for non-async contexts)
    # =========================================================================

    def json_sync(self) -> Any:
        """
        Parse JSON body synchronously.

        For use in non-async contexts. Prefer async json() method.
        """
        if self._json is not None:
            return self._json

        body = self._body_bytes or self._body.encode("utf-8") if self._body else b""

        if not body:
            raise ValueError("Request body is empty")

        try:
            self._json = json_module.loads(body.decode("utf-8"))
            return self._json
        except json_module.JSONDecodeError as e:
            raise ValueError(f"Invalid JSON: {e}")

    def form_sync(self) -> Dict[str, str]:
        """
        Parse URL-encoded form data synchronously.

        For use in non-async contexts. Does NOT handle multipart.
        """
        import urllib.parse

        body = self._body or (
            self._body_bytes.decode("utf-8") if self._body_bytes else ""
        )
        return dict(urllib.parse.parse_qsl(body))

    def __repr__(self) -> str:
        """String representation of request."""
        return f"Request(method={self.method.value}, path='{self.path}', protocol='{self.protocol}')"
