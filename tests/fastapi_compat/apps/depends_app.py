"""
Dependency injection application for FastAPI compatibility testing.

Tests:
- Simple dependencies
- Nested dependencies
- Cached dependencies (use_cache)
- Yield dependencies (cleanup)
- Async dependencies
- Class-based dependencies

Can be run with either FastAPI or FasterAPI by setting TEST_FRAMEWORK env var.
"""

import os
from contextlib import contextmanager
from typing import Dict, Generator, List, Optional
from uuid import uuid4

# Import framework based on environment
FRAMEWORK = os.environ.get("TEST_FRAMEWORK", "fasterapi")

if FRAMEWORK == "fastapi":
    from fastapi import Depends, FastAPI, HTTPException, Query
    from pydantic import BaseModel
else:
    from pydantic import BaseModel

    from fasterapi import Depends, FastAPI, HTTPException, Query


# Track dependency calls for testing
dependency_calls: List[str] = []
cleanup_calls: List[str] = []
cache_test_counter: Dict[str, int] = {"db_connection": 0, "settings": 0}


# Pydantic Models
class User(BaseModel):
    id: str
    username: str


class Settings(BaseModel):
    app_name: str = "Depends Test App"
    debug: bool = True
    version: str = "1.0.0"


# Simple dependencies
def get_query_param(q: Optional[str] = Query(None)):
    """Simple query parameter dependency."""
    dependency_calls.append("get_query_param")
    return q


def get_required_header(x_token: str = None):  # Would normally use Header()
    """Simple header dependency (simulated)."""
    dependency_calls.append("get_required_header")
    if not x_token:
        x_token = "default-token"
    return x_token


# Nested dependencies
def get_settings() -> Settings:
    """Get application settings."""
    dependency_calls.append("get_settings")
    cache_test_counter["settings"] += 1
    return Settings()


def get_db_connection(settings: Settings = Depends(get_settings)) -> str:
    """Get database connection (depends on settings)."""
    dependency_calls.append("get_db_connection")
    cache_test_counter["db_connection"] += 1
    return f"db://{settings.app_name}:{uuid4().hex[:8]}"


def get_current_user(
    db: str = Depends(get_db_connection),
    token: str = Depends(get_required_header),
) -> User:
    """Get current user (depends on db and token)."""
    dependency_calls.append("get_current_user")
    return User(id=str(uuid4()), username=f"user-{token[:8]}")


# Deeply nested
def get_request_id() -> str:
    """Generate request ID."""
    dependency_calls.append("get_request_id")
    return str(uuid4())


def get_logger(request_id: str = Depends(get_request_id)) -> dict:
    """Get logger with request ID."""
    dependency_calls.append("get_logger")
    return {"request_id": request_id, "name": "app_logger"}


def get_tracer(
    logger: dict = Depends(get_logger),
    settings: Settings = Depends(get_settings),
) -> dict:
    """Get tracer (depends on logger and settings)."""
    dependency_calls.append("get_tracer")
    return {
        "logger": logger,
        "settings": settings.model_dump(),
        "tracing_enabled": True,
    }


# Yield dependencies (cleanup)
def get_db_session() -> Generator[str, None, None]:
    """Database session with cleanup."""
    session_id = str(uuid4())
    dependency_calls.append(f"db_session_open:{session_id[:8]}")
    try:
        yield f"session:{session_id}"
    finally:
        cleanup_calls.append(f"db_session_close:{session_id[:8]}")


async def get_async_resource():
    """Async resource with cleanup."""
    resource_id = str(uuid4())
    dependency_calls.append(f"async_resource_open:{resource_id[:8]}")
    try:
        yield f"async:{resource_id}"
    finally:
        cleanup_calls.append(f"async_resource_close:{resource_id[:8]}")


def get_file_handle() -> Generator[str, None, None]:
    """File handle with cleanup."""
    file_id = str(uuid4())[:8]
    dependency_calls.append(f"file_open:{file_id}")
    try:
        yield f"file:{file_id}"
    finally:
        cleanup_calls.append(f"file_close:{file_id}")


# Class-based dependencies
class DBSession:
    """Class-based database session."""

    def __init__(self, connection_string: str = None):
        self.session_id = str(uuid4())
        self.connection_string = connection_string or "default-connection"
        dependency_calls.append(f"DBSession.__init__:{self.session_id[:8]}")

    def query(self, table: str) -> List[dict]:
        return [{"id": 1, "table": table}]


class UserRepository:
    """Repository that depends on DBSession."""

    def __init__(self, session: DBSession = Depends(DBSession)):
        self.session = session
        dependency_calls.append(f"UserRepository.__init__")

    def get_user(self, user_id: str) -> dict:
        return {"id": user_id, "from_repo": True}


class ServiceWithMultipleDeps:
    """Service with multiple dependencies."""

    def __init__(
        self,
        settings: Settings = Depends(get_settings),
        db: str = Depends(get_db_connection),
        logger: dict = Depends(get_logger),
    ):
        self.settings = settings
        self.db = db
        self.logger = logger
        dependency_calls.append("ServiceWithMultipleDeps.__init__")

    def process(self, data: str) -> dict:
        return {
            "processed": data,
            "app": self.settings.app_name,
            "request_id": self.logger["request_id"],
        }


def create_app() -> FastAPI:
    """Create and configure the FastAPI application."""

    app = FastAPI(
        title="Depends Test App",
        description="Dependency injection testing application",
        version="1.0.0",
    )

    # Simple dependency
    @app.get("/simple")
    async def simple_depends(q: str = Depends(get_query_param)):
        """Endpoint with simple dependency."""
        return {"query": q}

    # Nested dependencies
    @app.get("/nested")
    async def nested_depends(user: User = Depends(get_current_user)):
        """Endpoint with nested dependencies."""
        return {"user": user.model_dump()}

    # Deeply nested
    @app.get("/deep")
    async def deep_depends(tracer: dict = Depends(get_tracer)):
        """Endpoint with deeply nested dependencies."""
        return {"tracer": tracer}

    # Multiple dependencies
    @app.get("/multiple")
    async def multiple_depends(
        q: str = Depends(get_query_param),
        user: User = Depends(get_current_user),
        settings: Settings = Depends(get_settings),
    ):
        """Endpoint with multiple dependencies."""
        return {
            "query": q,
            "user": user.model_dump(),
            "settings": settings.model_dump(),
        }

    # Yield dependencies
    @app.get("/yield")
    async def yield_depends(session: str = Depends(get_db_session)):
        """Endpoint with yield dependency."""
        return {"session": session}

    @app.get("/yield-async")
    async def yield_async_depends(resource: str = Depends(get_async_resource)):
        """Endpoint with async yield dependency."""
        return {"resource": resource}

    @app.get("/yield-multiple")
    async def yield_multiple_depends(
        session: str = Depends(get_db_session),
        file: str = Depends(get_file_handle),
    ):
        """Endpoint with multiple yield dependencies."""
        return {"session": session, "file": file}

    # Class-based dependencies
    @app.get("/class-simple")
    async def class_simple_depends(session: DBSession = Depends(DBSession)):
        """Endpoint with class-based dependency."""
        return {"session_id": session.session_id[:8]}

    @app.get("/class-nested")
    async def class_nested_depends(repo: UserRepository = Depends(UserRepository)):
        """Endpoint with nested class dependencies."""
        user = repo.get_user("user-123")
        return {"user": user}

    @app.get("/class-multi")
    async def class_multi_depends(
        service: ServiceWithMultipleDeps = Depends(ServiceWithMultipleDeps),
    ):
        """Endpoint with service having multiple dependencies."""
        result = service.process("test-data")
        return {"result": result}

    # Dependency call tracking
    @app.get("/dependency-calls")
    async def get_dependency_calls():
        """Get dependency call log."""
        return {"calls": dependency_calls.copy()}

    @app.get("/cleanup-calls")
    async def get_cleanup_calls():
        """Get cleanup call log."""
        return {"calls": cleanup_calls.copy()}

    @app.get("/cache-counts")
    async def get_cache_counts():
        """Get cache test counters."""
        return cache_test_counter.copy()

    @app.post("/clear-logs")
    async def clear_logs():
        """Clear all logs."""
        dependency_calls.clear()
        cleanup_calls.clear()
        for key in cache_test_counter:
            cache_test_counter[key] = 0
        return {"message": "Logs cleared"}

    @app.get("/health")
    async def health():
        """Health check."""
        return {"status": "healthy", "framework": FRAMEWORK}

    return app


# Create app instance
app = create_app()


def clear_all_logs():
    """Clear all logs (for testing)."""
    dependency_calls.clear()
    cleanup_calls.clear()
    for key in cache_test_counter:
        cache_test_counter[key] = 0


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=8000)
