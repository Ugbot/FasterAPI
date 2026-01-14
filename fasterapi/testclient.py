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
        self._lifespan_started = False

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
        """Context manager entry - triggers lifespan startup."""
        self._run_lifespan("startup")
        self._lifespan_started = True
        return self

    def __exit__(self, *args) -> None:
        """Context manager exit - triggers lifespan shutdown."""
        if self._lifespan_started:
            self._run_lifespan("shutdown")
            self._lifespan_started = False

    def _run_lifespan(self, phase: str) -> None:
        """
        Run ASGI lifespan startup or shutdown.

        Args:
            phase: Either "startup" or "shutdown"
        """
        # Check if app has lifespan handling capability
        if not (hasattr(self.app, '_lifespan') or hasattr(self.app, '_on_startup')):
            return

        # For startup, check if there are handlers
        if phase == "startup":
            has_lifespan = getattr(self.app, '_lifespan', None) is not None
            has_handlers = bool(getattr(self.app, '_on_startup', []))
            if not has_lifespan and not has_handlers:
                return
        else:
            # For shutdown, only run if startup was done
            has_lifespan_context = getattr(self.app, '_lifespan_context', None) is not None
            has_handlers = bool(getattr(self.app, '_on_shutdown', []))
            if not has_lifespan_context and not has_handlers:
                return

        # Build lifespan scope
        scope = {
            "type": "lifespan",
            "asgi": {"version": "3.0"},
        }

        # Track state
        phase_complete = False
        phase_failed = False
        failure_message = ""
        messages_sent = 0

        async def receive():
            nonlocal messages_sent
            messages_sent += 1
            if messages_sent == 1:
                return {"type": f"lifespan.{phase}"}
            # After startup/shutdown, app will wait for next message
            # We raise an exception to break out of the while loop
            raise asyncio.CancelledError("Lifespan phase complete")

        async def send(message):
            nonlocal phase_complete, phase_failed, failure_message
            if message["type"] == f"lifespan.{phase}.complete":
                phase_complete = True
            elif message["type"] == f"lifespan.{phase}.failed":
                phase_failed = True
                failure_message = message.get("message", f"{phase.title()} failed")

        try:
            asyncio.run(self.app(scope, receive, send))
        except asyncio.CancelledError:
            # Expected - we cancel after phase completes
            pass
        except Exception as e:
            if not phase_complete and phase == "startup":
                raise RuntimeError(f"Lifespan {phase} failed: {e}") from e

        if phase_failed and phase == "startup":
            raise RuntimeError(f"Lifespan startup failed: {failure_message}")

    def websocket_connect(
        self,
        url: str,
        subprotocols: Optional[List[str]] = None,
        headers: Optional[Dict[str, str]] = None,
    ) -> "WebSocketTestSession":
        """
        Connect to a WebSocket endpoint for testing.

        Usage:
            with client.websocket_connect("/ws/chat") as ws:
                ws.send_text("Hello")
                response = ws.receive_text()
                assert response == "Echo: Hello"

        Args:
            url: WebSocket URL path (e.g., "/ws/echo")
            subprotocols: List of WebSocket subprotocols
            headers: Additional headers to send

        Returns:
            WebSocketTestSession context manager
        """
        return WebSocketTestSession(
            self.app,
            url,
            subprotocols=subprotocols,
            headers=headers,
            base_url=self.base_url,
        )


class WebSocketTestSession:
    """
    WebSocket test session for simulating WebSocket connections.

    Provides send/receive methods that work with ASGI WebSocket protocol.
    Used as a context manager.
    """

    def __init__(
        self,
        app: Any,
        url: str,
        subprotocols: Optional[List[str]] = None,
        headers: Optional[Dict[str, str]] = None,
        base_url: str = "http://testserver",
    ):
        self.app = app
        self.url = url
        self.subprotocols = subprotocols or []
        self.headers = headers or {}
        self.base_url = base_url

        # Message queues
        self._receive_queue: List[Dict[str, Any]] = []
        self._send_queue: List[Dict[str, Any]] = []
        self._closed = False
        self._accepted = False
        self._close_code: Optional[int] = None
        self._close_reason: Optional[str] = None
        self._task: Optional[asyncio.Task] = None
        self._loop: Optional[asyncio.AbstractEventLoop] = None

    def _build_scope(self) -> dict:
        """Build ASGI WebSocket scope."""
        from urllib.parse import urlparse, parse_qs

        parsed = urlparse(self.url)
        path = parsed.path or "/"
        query_string = parsed.query or ""

        # Build headers
        header_list = [
            (b"host", b"testserver"),
            (b"connection", b"upgrade"),
            (b"upgrade", b"websocket"),
            (b"sec-websocket-version", b"13"),
            (b"sec-websocket-key", b"testkey123456789012345678"),
        ]

        if self.subprotocols:
            header_list.append(
                (b"sec-websocket-protocol", ", ".join(self.subprotocols).encode())
            )

        for key, value in self.headers.items():
            header_list.append((key.lower().encode(), value.encode()))

        return {
            "type": "websocket",
            "asgi": {"version": "3.0"},
            "http_version": "1.1",
            "scheme": "ws",
            "path": path,
            "raw_path": path.encode(),
            "query_string": query_string.encode(),
            "root_path": "",
            "headers": header_list,
            "server": ("testserver", 80),
            "client": ("testclient", 50000),
            "subprotocols": self.subprotocols,
        }

    async def _run_app(self):
        """Run the ASGI app with WebSocket scope."""
        scope = self._build_scope()

        # Send initial connect message
        self._receive_queue.append({"type": "websocket.connect"})

        async def receive():
            """ASGI receive callable."""
            while True:
                if self._receive_queue:
                    return self._receive_queue.pop(0)
                # Wait for messages
                await asyncio.sleep(0.001)

        async def send(message):
            """ASGI send callable."""
            self._send_queue.append(message)

            if message["type"] == "websocket.accept":
                self._accepted = True
            elif message["type"] == "websocket.close":
                self._closed = True
                self._close_code = message.get("code", 1000)
                self._close_reason = message.get("reason", "")

        try:
            await self.app(scope, receive, send)
        except Exception as e:
            # App raised an exception - close the connection
            self._closed = True
            self._close_code = 1011
            self._close_reason = str(e)

    def __enter__(self) -> "WebSocketTestSession":
        """Start the WebSocket connection."""
        # Create new event loop for this session
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)

        # Start the app task
        self._task = self._loop.create_task(self._run_app())

        # Wait for accept or close
        start_time = asyncio.get_event_loop().time()
        while not self._accepted and not self._closed:
            self._loop.run_until_complete(asyncio.sleep(0.001))
            if asyncio.get_event_loop().time() - start_time > 5.0:
                raise TimeoutError("WebSocket connection timed out")
            # Process any pending messages
            self._loop.run_until_complete(asyncio.sleep(0))

        if self._closed and not self._accepted:
            raise RuntimeError(
                f"WebSocket connection rejected with code {self._close_code}"
            )

        return self

    def __exit__(self, *args) -> None:
        """Close the WebSocket connection."""
        if not self._closed:
            self.close()

        # Cancel the task
        if self._task and not self._task.done():
            self._task.cancel()
            try:
                self._loop.run_until_complete(self._task)
            except asyncio.CancelledError:
                pass

        # Close the loop
        if self._loop:
            self._loop.close()
            self._loop = None

    def send(self, message: Dict[str, Any]) -> None:
        """Send a raw ASGI WebSocket message."""
        if self._closed:
            raise RuntimeError("WebSocket is closed")
        self._receive_queue.append(message)
        # Process the message
        self._loop.run_until_complete(asyncio.sleep(0.01))

    def send_text(self, data: str) -> None:
        """Send a text message."""
        self.send({"type": "websocket.receive", "text": data})

    def send_bytes(self, data: bytes) -> None:
        """Send a binary message."""
        self.send({"type": "websocket.receive", "bytes": data})

    def send_json(self, data: Any) -> None:
        """Send a JSON message."""
        self.send_text(json_module.dumps(data))

    def receive(self, timeout: float = 5.0) -> Dict[str, Any]:
        """Receive a raw ASGI WebSocket message."""
        start_time = asyncio.get_event_loop().time()

        while True:
            # Check for messages (skip accept messages)
            for msg in self._send_queue:
                if msg["type"] in ("websocket.send", "websocket.close"):
                    self._send_queue.remove(msg)
                    if msg["type"] == "websocket.close":
                        self._closed = True
                        self._close_code = msg.get("code", 1000)
                    return msg

            if self._closed:
                return {"type": "websocket.close", "code": self._close_code}

            # Wait for messages
            self._loop.run_until_complete(asyncio.sleep(0.01))

            if asyncio.get_event_loop().time() - start_time > timeout:
                raise TimeoutError("Timed out waiting for WebSocket message")

    def receive_text(self, timeout: float = 5.0) -> str:
        """Receive a text message."""
        msg = self.receive(timeout)
        if msg["type"] == "websocket.close":
            raise RuntimeError(f"WebSocket closed with code {msg.get('code', 1000)}")
        if "text" not in msg:
            raise RuntimeError(f"Expected text message, got: {msg}")
        return msg["text"]

    def receive_bytes(self, timeout: float = 5.0) -> bytes:
        """Receive a binary message."""
        msg = self.receive(timeout)
        if msg["type"] == "websocket.close":
            raise RuntimeError(f"WebSocket closed with code {msg.get('code', 1000)}")
        if "bytes" not in msg:
            raise RuntimeError(f"Expected bytes message, got: {msg}")
        return msg["bytes"]

    def receive_json(self, timeout: float = 5.0) -> Any:
        """Receive a JSON message."""
        text = self.receive_text(timeout)
        return json_module.loads(text)

    def close(self, code: int = 1000, reason: str = "") -> None:
        """Close the WebSocket connection."""
        if not self._closed:
            self._receive_queue.append({
                "type": "websocket.disconnect",
                "code": code,
            })
            self._closed = True
            self._close_code = code
            # Give the app time to process
            self._loop.run_until_complete(asyncio.sleep(0.01))
