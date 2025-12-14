"""
FasterAPI - High-Performance Python Web Framework

A drop-in replacement for FastAPI with a high-performance C++ backend.
Change `from fastapi import ...` to `from fasterapi import ...` with minimal code changes.

Features:
- HTTP/1.1, HTTP/2, HTTP/3 support
- WebSocket support
- zstd compression
- PostgreSQL connection pooling
- FastAPI-compatible API
- Zero-copy operations
- Per-core sharding

Usage:
    from fasterapi import FastAPI, HTTPException, Depends, Query
    from fasterapi.security import OAuth2PasswordBearer
    from fasterapi.middleware import CORSMiddleware

    app = FastAPI()

    @app.get("/")
    async def root():
        return {"message": "Hello World"}
"""

from typing import Any, Callable, Dict, List, Optional, Union

# Status codes
from fasterapi import status

# Background tasks
from fasterapi.background import BackgroundTask, BackgroundTasks
from fasterapi.core import Future, Reactor, when_all, when_any
from fasterapi.core.combinators import (
    Pipeline,
    filter_async,
    map_async,
    reduce_async,
    retry_async,
    timeout_async,
)

# Data structures
from fasterapi.datastructures import (
    URL,
    Address,
    FormData,
    State,
    UploadFile,
)

# Exceptions
from fasterapi.exceptions import (
    HTTPException,
    RequestValidationError,
    WebSocketException,
)

# FastAPI compatibility imports
from fasterapi.fastapi_compat import FastAPI
from fasterapi.http import Request, Response

# Core imports
from fasterapi.http import Server as HttpServer
from fasterapi.http import WebSocket as NativeWebSocket
from fasterapi.http.sse import SSE, SSEStream

# Content Negotiation
from fasterapi.negotiation import (
    ContentNegotiator,
    MediaType,
    MediaTypes,
    accepts,
    get_accept_quality,
    parse_accept_header,
    select_media_type,
)

# Parameter descriptors
from fasterapi.params import (
    Body,
    Cookie,
    Depends,
    File,
    Form,
    Header,
    Path,
    Query,
    Security,
)
from fasterapi.pg import Pg, PgPool, TxIsolation

# Responses
from fasterapi.responses import (
    FileResponse,
    HTMLResponse,
    JSONResponse,
    ORJSONResponse,
    PlainTextResponse,
    RedirectResponse,
    StreamingResponse,
    UJSONResponse,
)
from fasterapi.responses import (
    Response as StarletteResponse,
)

# Routing
from fasterapi.routing import APIRouter

# WebSocket
from fasterapi.websockets import WebSocket, WebSocketDisconnect, WebSocketState


class App:
    """
    FasterAPI application with unified HTTP and PostgreSQL support.

    Features:
    - Multi-protocol HTTP server (H1/H2/H3)
    - WebSocket support
    - zstd compression
    - PostgreSQL integration
    - FastAPI-compatible decorators
    - Dependency injection
    - Middleware support
    """

    def __init__(
        self,
        port: int = 8000,
        host: str = "0.0.0.0",
        enable_h2: bool = False,
        enable_h3: bool = False,
        enable_compression: bool = True,
        **kwargs,
    ):
        """
        Create a new FasterAPI application.

        Args:
            port: Server port
            host: Server host
            enable_h2: Enable HTTP/2 support
            enable_h3: Enable HTTP/3 support
            enable_compression: Enable zstd compression
            **kwargs: Additional configuration options
        """
        self.port = port
        self.host = host
        self.enable_h2 = enable_h2
        self.enable_h3 = enable_h3
        self.enable_compression = enable_compression

        # Create HTTP server
        self.server = HttpServer(
            port=port,
            host=host,
            enable_h2=enable_h2,
            enable_h3=enable_h3,
            enable_compression=enable_compression,
            **kwargs,
        )

        # Route registry
        self.routes: Dict[str, List[Dict[str, Any]]] = {}

        # Middleware stack
        self.middleware: List[Callable] = []

        # Lifecycle hooks
        self.startup_hooks: List[Callable] = []
        self.shutdown_hooks: List[Callable] = []

        # Global state
        self.state: Dict[str, Any] = {}

    def get(self, path: str, **kwargs) -> Callable:
        """Add a GET route."""

        def decorator(func: Callable) -> Callable:
            self._add_route("GET", path, func, **kwargs)
            return func

        return decorator

    def post(self, path: str, **kwargs) -> Callable:
        """Add a POST route."""

        def decorator(func: Callable) -> Callable:
            self._add_route("POST", path, func, **kwargs)
            return func

        return decorator

    def put(self, path: str, **kwargs) -> Callable:
        """Add a PUT route."""

        def decorator(func: Callable) -> Callable:
            self._add_route("PUT", path, func, **kwargs)
            return func

        return decorator

    def delete(self, path: str, **kwargs) -> Callable:
        """Add a DELETE route."""

        def decorator(func: Callable) -> Callable:
            self._add_route("DELETE", path, func, **kwargs)
            return func

        return decorator

    def patch(self, path: str, **kwargs) -> Callable:
        """Add a PATCH route."""

        def decorator(func: Callable) -> Callable:
            self._add_route("PATCH", path, func, **kwargs)
            return func

        return decorator

    def websocket(self, path: str, **kwargs) -> Callable:
        """Add a WebSocket endpoint."""

        def decorator(func: Callable) -> Callable:
            self._add_websocket(path, func, **kwargs)
            return func

        return decorator

    def sse(self, path: str, **kwargs) -> Callable:
        """Add a Server-Sent Events (SSE) endpoint."""

        def decorator(func: Callable) -> Callable:
            self._add_sse(path, func, **kwargs)
            return func

        return decorator

    def _add_route(self, method: str, path: str, handler: Callable, **kwargs) -> None:
        """Add a route to the server."""
        if method not in self.routes:
            self.routes[method] = []

        self.routes[method].append(
            {"path": path, "handler": handler, "options": kwargs}
        )

        def route_wrapper(req: Request, res: Response) -> None:
            try:
                for middleware in self.middleware:
                    middleware(req, res)

                result = handler(req, res)

                if isinstance(result, dict):
                    res.json(result).send()
                elif isinstance(result, str):
                    res.text(result).send()
                elif isinstance(result, Response):
                    result.send()
                else:
                    res.json({"result": str(result)}).send()

            except Exception as e:
                res.status(500).json({"error": str(e)}).send()

        self.server.add_route(method, path, route_wrapper)

    def _add_websocket(self, path: str, handler: Callable, **kwargs) -> None:
        """Add a WebSocket endpoint to the server."""

        def websocket_wrapper(ws: NativeWebSocket) -> None:
            try:
                handler(ws)
            except Exception as e:
                print(f"WebSocket error: {e}")
                ws.close(1011, "Internal error")

        self.server.add_websocket(path, websocket_wrapper)

    def _add_sse(self, path: str, handler: Callable, **kwargs) -> None:
        """Add an SSE endpoint to the server."""

        def sse_wrapper(req: Request, res: Response) -> None:
            try:
                res.set_header("Content-Type", "text/event-stream")
                res.set_header("Cache-Control", "no-cache")
                res.set_header("Connection", "keep-alive")
                res.set_header("X-Accel-Buffering", "no")

                sse = SSE(native_handle=None)
                handler(sse)

            except Exception as e:
                print(f"SSE error: {e}")
                res.status(500).json({"error": str(e)}).send()

        self._add_route("GET", path, sse_wrapper, **kwargs)

    def add_middleware(self, func: Callable) -> Callable:
        """Add middleware to the application."""
        self.middleware.append(func)
        return func

    def on_event(self, event: str) -> Callable:
        """Add lifecycle event handler."""

        def decorator(func: Callable) -> Callable:
            if event == "startup":
                self.startup_hooks.append(func)
            elif event == "shutdown":
                self.shutdown_hooks.append(func)
            return func

        return decorator

    def run(self, **kwargs) -> None:
        """Run the application."""
        for hook in self.startup_hooks:
            try:
                hook()
            except Exception as e:
                print(f"Startup hook error: {e}")

        self.server.start()

        try:
            import time

            while self.server.is_running():
                time.sleep(0.1)
        except KeyboardInterrupt:
            print("\nShutting down...")
        finally:
            for hook in self.shutdown_hooks:
                try:
                    hook()
                except Exception as e:
                    print(f"Shutdown hook error: {e}")

            self.server.stop()

    def get_stats(self) -> Dict[str, Any]:
        """Get application statistics."""
        return {
            "server": self.server.get_stats(),
            "routes": {method: len(routes) for method, routes in self.routes.items()},
            "middleware_count": len(self.middleware),
            "startup_hooks": len(self.startup_hooks),
            "shutdown_hooks": len(self.shutdown_hooks),
        }


# Export all classes and functions for FastAPI compatibility
__all__ = [
    # FastAPI-compatible main class
    "FastAPI",
    # Legacy App class
    "App",
    # Exceptions
    "HTTPException",
    "RequestValidationError",
    "WebSocketException",
    # Status module
    "status",
    # Parameter descriptors
    "Query",
    "Path",
    "Header",
    "Cookie",
    "Body",
    "Form",
    "File",
    "Depends",
    "Security",
    # Data structures
    "UploadFile",
    "FormData",
    "URL",
    "Address",
    "State",
    # Responses
    "Response",
    "JSONResponse",
    "HTMLResponse",
    "PlainTextResponse",
    "RedirectResponse",
    "StreamingResponse",
    "FileResponse",
    "UJSONResponse",
    "ORJSONResponse",
    # Background tasks
    "BackgroundTask",
    "BackgroundTasks",
    # Routing
    "APIRouter",
    # WebSocket
    "WebSocket",
    "WebSocketDisconnect",
    "WebSocketState",
    # Request/Response (native)
    "Request",
    # PostgreSQL
    "PgPool",
    "Pg",
    "TxIsolation",
    # SSE
    "SSE",
    "SSEStream",
    # Async utilities
    "Future",
    "when_all",
    "when_any",
    "Reactor",
    "map_async",
    "filter_async",
    "reduce_async",
    "retry_async",
    "timeout_async",
    "Pipeline",
    # Content Negotiation
    "ContentNegotiator",
    "MediaType",
    "MediaTypes",
    "accepts",
    "get_accept_quality",
    "parse_accept_header",
    "select_media_type",
]
