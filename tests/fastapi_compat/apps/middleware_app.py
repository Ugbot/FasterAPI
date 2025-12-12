"""
Middleware application for FastAPI compatibility testing.

Tests:
- CORS middleware
- Custom BaseHTTPMiddleware
- Request timing middleware
- Multiple middleware stack
- Middleware order

Can be run with either FastAPI or FasterAPI by setting TEST_FRAMEWORK env var.
"""

import os
import time
from typing import Callable, Dict, List

# Import framework based on environment
FRAMEWORK = os.environ.get("TEST_FRAMEWORK", "fasterapi")

if FRAMEWORK == "fastapi":
    from fastapi import FastAPI, Request, Response
    from fastapi.middleware.cors import CORSMiddleware
    from starlette.middleware.base import BaseHTTPMiddleware
else:
    from fasterapi import FastAPI, Request
    from fasterapi.middleware import BaseHTTPMiddleware, CORSMiddleware
    from fasterapi.responses import Response


# Track middleware execution for testing
middleware_calls: List[str] = []
timing_data: Dict[str, float] = {}


class TimingMiddleware(BaseHTTPMiddleware):
    """Middleware that tracks request timing."""

    async def dispatch(self, request, call_next):
        middleware_calls.append("timing_start")
        start_time = time.perf_counter()

        response = await call_next(request)

        process_time = time.perf_counter() - start_time
        timing_data["last_request_time"] = process_time

        # Add timing header
        if hasattr(response, "headers"):
            if isinstance(response.headers, dict):
                response.headers["X-Process-Time"] = str(process_time)
            elif hasattr(response.headers, "__setitem__"):
                response.headers["X-Process-Time"] = str(process_time)

        middleware_calls.append("timing_end")
        return response


class RequestIDMiddleware(BaseHTTPMiddleware):
    """Middleware that adds a request ID."""

    def __init__(self, app, header_name: str = "X-Request-ID"):
        super().__init__(app)
        self.header_name = header_name
        self.counter = 0

    async def dispatch(self, request, call_next):
        middleware_calls.append("request_id_start")
        self.counter += 1
        request_id = f"req-{self.counter:06d}"

        # Store request ID (would normally use request.state)
        timing_data["last_request_id"] = request_id

        response = await call_next(request)

        # Add request ID header
        if hasattr(response, "headers"):
            if isinstance(response.headers, dict):
                response.headers[self.header_name] = request_id
            elif hasattr(response.headers, "__setitem__"):
                response.headers[self.header_name] = request_id

        middleware_calls.append("request_id_end")
        return response


class LoggingMiddleware(BaseHTTPMiddleware):
    """Middleware that logs requests."""

    async def dispatch(self, request, call_next):
        middleware_calls.append("logging_start")

        # Log request info
        method = getattr(request, "method", "UNKNOWN")
        path = "/"
        if hasattr(request, "url"):
            url = request.url
            if hasattr(url, "path"):
                path = url.path

        timing_data["last_request_method"] = str(method)
        timing_data["last_request_path"] = str(path)

        response = await call_next(request)

        # Log response info
        status_code = getattr(response, "status_code", 200)
        timing_data["last_response_status"] = status_code

        middleware_calls.append("logging_end")
        return response


class ConditionalMiddleware(BaseHTTPMiddleware):
    """Middleware that only processes certain paths."""

    def __init__(self, app, protected_paths: List[str] = None):
        super().__init__(app)
        self.protected_paths = protected_paths or ["/protected"]

    async def dispatch(self, request, call_next):
        path = "/"
        if hasattr(request, "url"):
            url = request.url
            if hasattr(url, "path"):
                path = url.path

        is_protected = any(path.startswith(p) for p in self.protected_paths)

        if is_protected:
            middleware_calls.append("conditional_protected")
            # In real app, would check auth here
        else:
            middleware_calls.append("conditional_public")

        return await call_next(request)


def create_app() -> FastAPI:
    """Create and configure the FastAPI application."""

    app = FastAPI(
        title="Middleware Test App",
        description="Middleware testing application",
        version="1.0.0",
    )

    # Add CORS middleware
    app.add_middleware(
        CORSMiddleware,
        allow_origins=["http://localhost:3000", "https://example.com"],
        allow_credentials=True,
        allow_methods=["GET", "POST", "PUT", "DELETE", "OPTIONS"],
        allow_headers=["*"],
        expose_headers=["X-Process-Time", "X-Request-ID"],
    )

    # Add custom middleware (order matters - last added runs first)
    app.add_middleware(ConditionalMiddleware, protected_paths=["/protected", "/admin"])
    app.add_middleware(LoggingMiddleware)
    app.add_middleware(RequestIDMiddleware, header_name="X-Request-ID")
    app.add_middleware(TimingMiddleware)

    @app.get("/")
    async def root():
        """Root endpoint."""
        return {"message": "Middleware Test App", "framework": FRAMEWORK}

    @app.get("/health")
    async def health():
        """Health check endpoint."""
        return {"status": "healthy"}

    @app.get("/slow")
    async def slow_endpoint():
        """Slow endpoint for timing testing."""
        import asyncio

        await asyncio.sleep(0.1)
        return {"message": "This was slow"}

    @app.get("/protected/data")
    async def protected_data():
        """Protected endpoint."""
        return {"data": "secret", "protected": True}

    @app.get("/admin/stats")
    async def admin_stats():
        """Admin stats endpoint."""
        return {"stats": "admin only"}

    @app.get("/public/info")
    async def public_info():
        """Public endpoint."""
        return {"info": "public"}

    @app.get("/middleware-calls")
    async def get_middleware_calls():
        """Get middleware call log."""
        return {"calls": middleware_calls.copy()}

    @app.get("/timing-data")
    async def get_timing_data():
        """Get timing data."""
        return timing_data.copy()

    @app.post("/clear-logs")
    async def clear_logs():
        """Clear middleware logs."""
        middleware_calls.clear()
        timing_data.clear()
        return {"message": "Logs cleared"}

    @app.options("/cors-test")
    async def cors_preflight():
        """CORS preflight test."""
        return {}

    @app.get("/cors-test")
    async def cors_test():
        """CORS test endpoint."""
        return {"cors": "enabled"}

    @app.post("/cors-test")
    async def cors_post():
        """CORS POST test."""
        return {"method": "POST", "cors": "enabled"}

    return app


# Create app instance
app = create_app()


def clear_middleware_logs():
    """Clear middleware logs (for testing)."""
    middleware_calls.clear()
    timing_data.clear()


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=8000)
