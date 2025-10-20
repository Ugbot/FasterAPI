"""FastAPI compatibility layer for PostgreSQL integration.

Provides Depends() wrappers and request-scoped connection management.
"""

from typing import Optional, Callable, Any
from contextlib import contextmanager
import contextvars

from .pool import PgPool, Pg


class Depends:
    """FastAPI-style dependency injection."""
    
    def __init__(self, dependency: Callable):
        self.dependency = dependency
    
    def __call__(self):
        return self.dependency()

# Context var for request-scoped connection
_request_pg: contextvars.ContextVar[Optional[Pg]] = contextvars.ContextVar(
    "request_pg", default=None
)


def get_pg_factory(pool: PgPool) -> Callable[[], Pg]:
    """Create a dependency factory for FastAPI Depends.
    
    Usage:
        pool = PgPool("postgres://...")
        get_pg = get_pg_factory(pool)
        
        @app.get("/items/{id}")
        def get_item(item_id: int, pg = Depends(get_pg)):
            return pg.exec("SELECT * FROM items WHERE id=$1", item_id).one()
    
    Args:
        pool: PostgreSQL connection pool.
        
    Returns:
        Callable that returns Pg handle (suitable for FastAPI Depends).
    """
    def _get_pg() -> Pg:
        # Check if we're in request context (FastAPI decorator)
        # Return bound Pg handle from pool
        pg = pool.get()
        _request_pg.set(pg)
        return pg
    
    return _get_pg


@contextmanager
def request_scope(pool: PgPool):
    """Context manager for request-scoped Pg handle (non-FastAPI use).
    
    Usage:
        pool = PgPool("postgres://...")
        with request_scope(pool) as pg:
            result = pg.exec("SELECT 1").scalar()
    
    Args:
        pool: PostgreSQL connection pool.
        
    Yields:
        Pg handle bound to this request scope.
    """
    pg = pool.get()
    _request_pg.set(pg)
    try:
        yield pg
    finally:
        pool.release(pg._handle)
        _request_pg.set(None)


def get_current_pg() -> Optional[Pg]:
    """Get current request-scoped Pg handle.
    
    Returns:
        Pg handle if in request context, None otherwise.
    """
    return _request_pg.get()


class PgMiddleware:
    """ASGI middleware for automatic request-scoped Pg management (optional).
    
    Usage:
        app = FastAPI()
        pool = PgPool("postgres://...")
        app.add_middleware(PgMiddleware, pool=pool)
    """
    
    def __init__(self, app: Any, pool: PgPool):
        self.app = app
        self.pool = pool
    
    async def __call__(self, scope: dict, receive: Any, send: Any) -> None:
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return
        
        pg = self.pool.get()
        token = _request_pg.set(pg)
        try:
            await self.app(scope, receive, send)
        finally:
            self.pool.release(pg._handle)
            _request_pg.reset(token)
