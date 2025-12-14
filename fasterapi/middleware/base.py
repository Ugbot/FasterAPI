"""
Base middleware classes for FastAPI compatibility.

Provides BaseHTTPMiddleware and the dispatch pattern for custom middleware.
"""

import asyncio
from typing import Any, Awaitable, Callable, Optional

# Type aliases
RequestResponseEndpoint = Callable[[Any], Awaitable[Any]]
DispatchFunction = Callable[[Any, RequestResponseEndpoint], Awaitable[Any]]


class BaseHTTPMiddleware:
    """
    Base class for HTTP middleware using the dispatch pattern.

    This is ASGI-compatible and works with add_middleware().

    Usage:
        class CustomMiddleware(BaseHTTPMiddleware):
            async def dispatch(self, request, call_next):
                # Pre-processing
                response = await call_next(request)
                # Post-processing
                return response

        app.add_middleware(CustomMiddleware)

    Or with initialization parameters:
        class TimingMiddleware(BaseHTTPMiddleware):
            def __init__(self, app, header_name: str = "X-Process-Time"):
                super().__init__(app)
                self.header_name = header_name

            async def dispatch(self, request, call_next):
                import time
                start = time.perf_counter()
                response = await call_next(request)
                process_time = time.perf_counter() - start
                response.headers[self.header_name] = str(process_time)
                return response
    """

    def __init__(
        self,
        app: Any,
        dispatch: Optional[DispatchFunction] = None,
    ) -> None:
        self.app = app
        self.dispatch_func = dispatch or self.dispatch

    async def __call__(self, scope: dict, receive: Callable, send: Callable) -> Any:
        """ASGI interface - handles HTTP requests through dispatch pattern."""
        if scope["type"] != "http":
            # Pass through non-HTTP requests
            await self.app(scope, receive, send)
            return

        # Build request object from scope
        from fasterapi.http.request import Request

        # Read body
        body = b""
        while True:
            message = await receive()
            body += message.get("body", b"")
            if not message.get("more_body", False):
                break

        # Parse headers
        headers = {
            k.decode() if isinstance(k, bytes) else k: v.decode()
            if isinstance(v, bytes)
            else v
            for k, v in scope.get("headers", [])
        }

        # Parse query params
        from urllib.parse import parse_qs

        query_string = scope.get("query_string", b"").decode()
        query_params = {}
        if query_string:
            for key, values in parse_qs(query_string).items():
                query_params[key] = values[0] if len(values) == 1 else values

        request = Request(
            method=scope.get("method", "GET"),
            path=scope.get("path", "/"),
            headers=headers,
            query_params=query_params,
            body_bytes=body,
            scope=scope,
        )

        # Response capture
        response_started = False
        response_status = 200
        response_headers = {}
        response_body = b""

        async def call_next(req):
            nonlocal response_started, response_status, response_headers, response_body

            # Create a new receive that returns empty body (already consumed)
            async def empty_receive():
                return {"type": "http.request", "body": b"", "more_body": False}

            # Capture response
            async def capture_send(message):
                nonlocal \
                    response_started, \
                    response_status, \
                    response_headers, \
                    response_body
                if message["type"] == "http.response.start":
                    response_started = True
                    response_status = message.get("status", 200)
                    response_headers = {
                        k.decode() if isinstance(k, bytes) else k: v.decode()
                        if isinstance(v, bytes)
                        else v
                        for k, v in message.get("headers", [])
                    }
                elif message["type"] == "http.response.body":
                    response_body += message.get("body", b"")

            await self.app(scope, empty_receive, capture_send)

            # Return a response-like object
            return _CapturedResponse(response_status, response_headers, response_body)

        # Call dispatch
        response = await self.dispatch_func(request, call_next)

        # Send the response
        if hasattr(response, "status_code"):
            status = response.status_code
        else:
            status = 200

        if hasattr(response, "headers"):
            resp_headers = response.headers
        else:
            resp_headers = {}

        if hasattr(response, "body"):
            resp_body = response.body
        else:
            resp_body = b""

        # Convert headers to ASGI format
        header_list = [
            (
                k.encode() if isinstance(k, str) else k,
                v.encode() if isinstance(v, str) else v,
            )
            for k, v in resp_headers.items()
        ]

        await send(
            {
                "type": "http.response.start",
                "status": status,
                "headers": header_list,
            }
        )
        await send(
            {
                "type": "http.response.body",
                "body": resp_body
                if isinstance(resp_body, bytes)
                else resp_body.encode(),
            }
        )

    async def dispatch(
        self,
        request: Any,
        call_next: RequestResponseEndpoint,
    ) -> Any:
        """
        Override this method to implement custom middleware logic.

        Args:
            request: The incoming request object
            call_next: Coroutine to call the next middleware or route handler

        Returns:
            Response object
        """
        return await call_next(request)


class _CapturedResponse:
    """Simple response object for middleware."""

    def __init__(self, status_code: int, headers: dict, body: bytes):
        self.status_code = status_code
        self.headers = headers
        self.body = body


class MiddlewareStack:
    """
    Manages a stack of middleware for sequential execution.

    Middleware is executed in LIFO order (last added, first executed).
    """

    def __init__(self) -> None:
        self._middleware: list[tuple[type, dict]] = []

    def add(
        self,
        middleware_class: type,
        **options: Any,
    ) -> None:
        """Add middleware to the stack."""
        self._middleware.append((middleware_class, options))

    def build_dispatch_chain(
        self,
        app: Any,
        final_handler: RequestResponseEndpoint,
    ) -> RequestResponseEndpoint:
        """
        Build the middleware dispatch chain.

        Returns a callable that processes requests through all middleware.
        """
        handler = final_handler

        # Build chain in reverse order so first-added middleware runs first
        for middleware_class, options in reversed(self._middleware):
            middleware_instance = middleware_class(app, **options)

            # Capture the current handler in closure
            current_handler = handler

            async def make_call_next(
                request: Any,
                mw: Any = middleware_instance,
                next_handler: RequestResponseEndpoint = current_handler,
            ) -> Any:
                return await mw(request, next_handler)

            handler = make_call_next

        return handler

    def __iter__(self):
        return iter(self._middleware)

    def __len__(self) -> int:
        return len(self._middleware)


__all__ = [
    "BaseHTTPMiddleware",
    "MiddlewareStack",
    "RequestResponseEndpoint",
    "DispatchFunction",
]
