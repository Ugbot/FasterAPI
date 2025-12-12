"""
FastAPI-compatible middleware for FasterAPI.

Provides BaseHTTPMiddleware, CORSMiddleware, GZipMiddleware, and TrustedHostMiddleware.

Usage:
    from fasterapi.middleware import CORSMiddleware

    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_methods=["*"],
        allow_headers=["*"],
    )

Custom middleware:
    from fasterapi.middleware import BaseHTTPMiddleware

    class TimingMiddleware(BaseHTTPMiddleware):
        async def dispatch(self, request, call_next):
            import time
            start = time.perf_counter()
            response = await call_next(request)
            process_time = time.perf_counter() - start
            response.headers["X-Process-Time"] = str(process_time)
            return response

    app.add_middleware(TimingMiddleware)
"""

from fasterapi.middleware.base import (
    BaseHTTPMiddleware,
    DispatchFunction,
    MiddlewareStack,
    RequestResponseEndpoint,
)
from fasterapi.middleware.cors import CORSMiddleware
from fasterapi.middleware.gzip import GZipMiddleware
from fasterapi.middleware.trustedhost import TrustedHostMiddleware

__all__ = [
    # Base
    "BaseHTTPMiddleware",
    "MiddlewareStack",
    "RequestResponseEndpoint",
    "DispatchFunction",
    # Middleware implementations
    "CORSMiddleware",
    "GZipMiddleware",
    "TrustedHostMiddleware",
]
