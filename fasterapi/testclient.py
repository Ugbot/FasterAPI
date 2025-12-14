"""
FastAPI-compatible TestClient for FasterAPI.

Provides a synchronous test client for testing FasterAPI applications
without starting a real server.

Usage:
    from fasterapi import FastAPI
    from fasterapi.testclient import TestClient

    app = FastAPI()

    @app.get("/")
    def read_root():
        return {"Hello": "World"}

    client = TestClient(app)
    response = client.get("/")
    assert response.status_code == 200
    assert response.json() == {"Hello": "World"}
"""

import asyncio
import io
import json as json_module
from typing import Any, Dict, List, Optional, Union
from urllib.parse import parse_qs, urlencode, urlparse

from fasterapi.datastructures import FormData, UploadFile
from fasterapi.http.request import Request


class TestResponse:
    """
    Response object returned by TestClient.

    Mimics httpx.Response / requests.Response API.
    """

    def __init__(
        self,
        status_code: int = 200,
        headers: Optional[Dict[str, str]] = None,
        content: bytes = b"",
        request: Optional[Any] = None,
    ):
        self.status_code = status_code
        self.headers = headers or {}
        self.content = content
        self.request = request
        self._json: Any = None

    @property
    def text(self) -> str:
        """Get response body as text."""
        return self.content.decode("utf-8")

    def json(self) -> Any:
        """Parse response body as JSON."""
        if self._json is None:
            self._json = json_module.loads(self.content)
        return self._json

    @property
    def ok(self) -> bool:
        """Check if response was successful (2xx)."""
        return 200 <= self.status_code < 300

    @property
    def is_success(self) -> bool:
        """Alias for ok."""
        return self.ok

    @property
    def is_redirect(self) -> bool:
        """Check if response is a redirect (3xx)."""
        return 300 <= self.status_code < 400

    @property
    def is_client_error(self) -> bool:
        """Check if response is client error (4xx)."""
        return 400 <= self.status_code < 500

    @property
    def is_server_error(self) -> bool:
        """Check if response is server error (5xx)."""
        return 500 <= self.status_code < 600

    def raise_for_status(self) -> None:
        """Raise HTTPError if response was not successful."""
        if not self.ok:
            raise HTTPError(f"HTTP {self.status_code}", response=self)

    def __repr__(self) -> str:
        return f"<TestResponse [{self.status_code}]>"


class HTTPError(Exception):
    """HTTP error exception."""

    def __init__(self, message: str, response: Optional[TestResponse] = None):
        super().__init__(message)
        self.response = response


class TestClient:
    """
    Synchronous test client for FasterAPI applications.

    Allows testing FastAPI/FasterAPI apps without starting a server.
    Uses the app's ASGI interface directly.

    Usage:
        app = FastAPI()

        @app.get("/items/{item_id}")
        def read_item(item_id: int):
            return {"item_id": item_id}

        client = TestClient(app)
        response = client.get("/items/42")
        assert response.json() == {"item_id": 42}
    """

    def __init__(
        self,
        app: Any,
        base_url: str = "http://testserver",
        raise_server_exceptions: bool = True,
        cookies: Optional[Dict[str, str]] = None,
    ):
        """
        Initialize TestClient.

        Args:
            app: FasterAPI/FastAPI application instance
            base_url: Base URL for requests
            raise_server_exceptions: If True, re-raise exceptions from handlers
            cookies: Default cookies for all requests
        """
        self.app = app
        self.base_url = base_url.rstrip("/")
        self.raise_server_exceptions = raise_server_exceptions
        self.cookies: Dict[str, str] = cookies or {}

    def _build_scope(
        self,
        method: str,
        path: str,
        headers: Dict[str, str],
        query_string: str = "",
    ) -> dict:
        """Build ASGI scope dictionary."""
        parsed = urlparse(path)
        actual_path = parsed.path or "/"
        actual_query = query_string or parsed.query

        # Build header list
        header_list = []
        for key, value in headers.items():
            header_list.append((key.lower().encode(), value.encode()))

        return {
            "type": "http",
            "asgi": {"version": "3.0"},
            "http_version": "1.1",
            "method": method.upper(),
            "path": actual_path,
            "raw_path": actual_path.encode(),
            "query_string": actual_query.encode() if actual_query else b"",
            "root_path": "",
            "headers": header_list,
            "server": ("testserver", 80),
            "client": ("testclient", 50000),
            "scheme": "http",
        }

    def _make_request(
        self,
        method: str,
        url: str,
        content: Optional[bytes] = None,
        data: Optional[Dict[str, Any]] = None,
        json: Optional[Any] = None,
        files: Optional[Dict[str, Any]] = None,
        params: Optional[Dict[str, Any]] = None,
        headers: Optional[Dict[str, str]] = None,
        cookies: Optional[Dict[str, str]] = None,
    ) -> TestResponse:
        """Make a request to the app."""

        # Build headers
        request_headers: Dict[str, str] = {
            "host": "testserver",
            "user-agent": "testclient",
        }

        # Add cookies
        all_cookies = {**self.cookies, **(cookies or {})}
        if all_cookies:
            cookie_str = "; ".join(f"{k}={v}" for k, v in all_cookies.items())
            request_headers["cookie"] = cookie_str

        if headers:
            request_headers.update({k.lower(): v for k, v in headers.items()})

        # Build query string
        query_string = ""
        if params:
            query_string = urlencode(params)

        # Build body
        body = b""
        if content is not None:
            body = content
        elif json is not None:
            body = json_module.dumps(json).encode("utf-8")
            request_headers.setdefault("content-type", "application/json")
        elif data is not None and files is None:
            body = urlencode(data).encode("utf-8")
            request_headers.setdefault(
                "content-type", "application/x-www-form-urlencoded"
            )
        elif files is not None:
            body, content_type = self._encode_multipart(data or {}, files)
            request_headers["content-type"] = content_type

        if body:
            request_headers["content-length"] = str(len(body))

        # Build ASGI scope
        scope = self._build_scope(method, url, request_headers, query_string)

        # Create receive/send callables
        body_sent = False
        response_started = False
        response_body = []
        response_status = 200
        response_headers: Dict[str, str] = {}

        async def receive():
            nonlocal body_sent
            if not body_sent:
                body_sent = True
                return {"type": "http.request", "body": body, "more_body": False}
            return {"type": "http.disconnect"}

        async def send(message):
            nonlocal response_started, response_status, response_headers
            if message["type"] == "http.response.start":
                response_started = True
                response_status = message["status"]
                for key, value in message.get("headers", []):
                    response_headers[key.decode().lower()] = value.decode()
            elif message["type"] == "http.response.body":
                response_body.append(message.get("body", b""))

        # Call the ASGI app
        try:
            asyncio.run(self.app(scope, receive, send))
        except Exception as e:
            if self.raise_server_exceptions:
                raise
            return TestResponse(
                status_code=500,
                content=str(e).encode("utf-8"),
                headers={"content-type": "text/plain"},
            )

        # Build response
        full_body = b"".join(response_body)

        return TestResponse(
            status_code=response_status,
            headers=response_headers,
            content=full_body,
        )

    def _encode_multipart(
        self,
        data: Dict[str, Any],
        files: Dict[str, Any],
    ) -> tuple:
        """
        Encode multipart form data.

        Returns:
            Tuple of (body_bytes, content_type)
        """
        import uuid

        boundary = f"----WebKitFormBoundary{uuid.uuid4().hex[:16]}"

        parts = []

        # Add regular form fields
        for key, value in data.items():
            parts.append(f"--{boundary}\r\n".encode())
            parts.append(
                f'Content-Disposition: form-data; name="{key}"\r\n\r\n'.encode()
            )
            parts.append(str(value).encode("utf-8"))
            parts.append(b"\r\n")

        # Add files
        for key, file_info in files.items():
            if isinstance(file_info, tuple):
                if len(file_info) == 2:
                    filename, file_content = file_info
                    content_type = "application/octet-stream"
                else:
                    filename, file_content, content_type = file_info
            elif isinstance(file_info, bytes):
                filename = key
                file_content = file_info
                content_type = "application/octet-stream"
            else:
                # Assume file-like object
                filename = getattr(file_info, "name", key)
                file_content = (
                    file_info.read() if hasattr(file_info, "read") else bytes(file_info)
                )
                content_type = "application/octet-stream"

            # Handle file-like objects in tuples
            if hasattr(file_content, "read"):
                file_content = file_content.read()

            if isinstance(file_content, str):
                file_content = file_content.encode("utf-8")

            parts.append(f"--{boundary}\r\n".encode())
            parts.append(
                f'Content-Disposition: form-data; name="{key}"; filename="{filename}"\r\n'.encode()
            )
            parts.append(f"Content-Type: {content_type}\r\n\r\n".encode())
            parts.append(file_content)
            parts.append(b"\r\n")

        parts.append(f"--{boundary}--\r\n".encode())

        body = b"".join(parts)
        content_type = f"multipart/form-data; boundary={boundary}"

        return body, content_type

    def get(
        self,
        url: str,
        params: Optional[Dict[str, Any]] = None,
        headers: Optional[Dict[str, str]] = None,
        cookies: Optional[Dict[str, str]] = None,
        **kwargs,
    ) -> TestResponse:
        """Make a GET request."""
        return self._make_request(
            "GET", url, params=params, headers=headers, cookies=cookies, **kwargs
        )

    def post(
        self,
        url: str,
        content: Optional[bytes] = None,
        data: Optional[Dict[str, Any]] = None,
        json: Optional[Any] = None,
        files: Optional[Dict[str, Any]] = None,
        params: Optional[Dict[str, Any]] = None,
        headers: Optional[Dict[str, str]] = None,
        cookies: Optional[Dict[str, str]] = None,
        **kwargs,
    ) -> TestResponse:
        """Make a POST request."""
        return self._make_request(
            "POST",
            url,
            content=content,
            data=data,
            json=json,
            files=files,
            params=params,
            headers=headers,
            cookies=cookies,
            **kwargs,
        )

    def put(
        self,
        url: str,
        content: Optional[bytes] = None,
        data: Optional[Dict[str, Any]] = None,
        json: Optional[Any] = None,
        files: Optional[Dict[str, Any]] = None,
        params: Optional[Dict[str, Any]] = None,
        headers: Optional[Dict[str, str]] = None,
        cookies: Optional[Dict[str, str]] = None,
        **kwargs,
    ) -> TestResponse:
        """Make a PUT request."""
        return self._make_request(
            "PUT",
            url,
            content=content,
            data=data,
            json=json,
            files=files,
            params=params,
            headers=headers,
            cookies=cookies,
            **kwargs,
        )

    def patch(
        self,
        url: str,
        content: Optional[bytes] = None,
        data: Optional[Dict[str, Any]] = None,
        json: Optional[Any] = None,
        files: Optional[Dict[str, Any]] = None,
        params: Optional[Dict[str, Any]] = None,
        headers: Optional[Dict[str, str]] = None,
        cookies: Optional[Dict[str, str]] = None,
        **kwargs,
    ) -> TestResponse:
        """Make a PATCH request."""
        return self._make_request(
            "PATCH",
            url,
            content=content,
            data=data,
            json=json,
            files=files,
            params=params,
            headers=headers,
            cookies=cookies,
            **kwargs,
        )

    def delete(
        self,
        url: str,
        params: Optional[Dict[str, Any]] = None,
        headers: Optional[Dict[str, str]] = None,
        cookies: Optional[Dict[str, str]] = None,
        **kwargs,
    ) -> TestResponse:
        """Make a DELETE request."""
        return self._make_request(
            "DELETE", url, params=params, headers=headers, cookies=cookies, **kwargs
        )

    def options(
        self,
        url: str,
        params: Optional[Dict[str, Any]] = None,
        headers: Optional[Dict[str, str]] = None,
        cookies: Optional[Dict[str, str]] = None,
        **kwargs,
    ) -> TestResponse:
        """Make an OPTIONS request."""
        return self._make_request(
            "OPTIONS", url, params=params, headers=headers, cookies=cookies, **kwargs
        )

    def head(
        self,
        url: str,
        params: Optional[Dict[str, Any]] = None,
        headers: Optional[Dict[str, str]] = None,
        cookies: Optional[Dict[str, str]] = None,
        **kwargs,
    ) -> TestResponse:
        """Make a HEAD request."""
        return self._make_request(
            "HEAD", url, params=params, headers=headers, cookies=cookies, **kwargs
        )

    def request(
        self,
        method: str,
        url: str,
        **kwargs,
    ) -> TestResponse:
        """Make a request with any HTTP method."""
        return self._make_request(method, url, **kwargs)

    def __enter__(self) -> "TestClient":
        """Context manager entry."""
        return self

    def __exit__(self, *args) -> None:
        """Context manager exit."""
        pass
