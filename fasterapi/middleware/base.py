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

    async def __call__(self, request: Any, call_next: RequestResponseEndpoint) -> Any:
        """Handle the request through the dispatch function."""
        return await self.dispatch_func(request, call_next)

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
