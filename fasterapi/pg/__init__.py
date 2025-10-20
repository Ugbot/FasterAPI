"""FasterAPI PostgreSQL integration.

High-performance PostgreSQL driver with native C++ pooling, binary codecs, and
FastAPI-compatible DI. Zero Python on hot path: protocol, pooling, I/O, and
row decoding done in C++.

Example:
    from fasterapi import FastAPI, Depends
    from fasterapi.pg import PgPool, TxIsolation
    
    app = FastAPI()
    pool: PgPool | None = None
    
    @app.on_event("startup")
    def startup():
        global pool
        pool = PgPool("postgres://user:pass@localhost/db")
    
    @app.get("/items/{item_id}")
    def get_item(item_id: int, pg = Depends(lambda: pool.get())):
        row = pg.exec("SELECT id, name FROM items WHERE id=$1", item_id).one()
        return {"id": row["id"], "name": row["name"]}
"""

from .types import (
    TxIsolation,
    Row,
    QueryResult,
    PreparedQuery,
)
from .exceptions import (
    PgError,
    PgConnectionError,
    PgTimeout,
    PgCanceled,
    PgIntegrityError,
    PgDataError,
)
from .pool import PgPool, Pg, prepare

__all__ = [
    "PgPool",
    "Pg",
    "TxIsolation",
    "Row",
    "QueryResult",
    "PreparedQuery",
    "prepare",
    "PgError",
    "PgConnectionError",
    "PgTimeout",
    "PgCanceled",
    "PgIntegrityError",
    "PgDataError",
]

__version__ = "0.1.0"
