"""Minimal FastAPI app showing FasterAPI PostgreSQL integration.

Demonstrates:
- Pool initialization in startup event
- Dependency injection with Depends(get_pg)
- Simple queries with exec()
- COPY IN for bulk insert
"""

from fastapi import FastAPI, Depends
from pydantic import BaseModel

from fasterapi.pg import PgPool, Pg, TxIsolation
from fasterapi.pg.compat import get_pg_factory

# Global pool (initialized on startup)
pool: PgPool | None = None


def startup_pool() -> None:
    """Initialize connection pool on app startup."""
    global pool
    dsn = "postgres://postgres:postgres@localhost:5432/mydb"
    pool = PgPool(dsn, min_size=2, max_size=20)
    print("PostgreSQL pool initialized")


def shutdown_pool() -> None:
    """Close pool on app shutdown."""
    global pool
    if pool:
        pool.close()
        print("PostgreSQL pool closed")


# Dependency injection factory
def get_pg_dependency() -> Pg:
    """FastAPI dependency that returns a Pg handle."""
    if not pool:
        raise RuntimeError("Pool not initialized")
    return pool.get()


# Models
class Item(BaseModel):
    id: int
    name: str
    price: float


class CreateItemRequest(BaseModel):
    name: str
    price: float


# App setup
app = FastAPI()
app.add_event_handler("startup", startup_pool)
app.add_event_handler("shutdown", shutdown_pool)


# Routes
@app.get("/health")
def health_check():
    """Health check endpoint."""
    return {"status": "ok"}


@app.get("/items/{item_id}")
def get_item(item_id: int, pg: Pg = Depends(get_pg_dependency)) -> Item:
    """Get item by ID.
    
    Demonstrates:
    - Simple query execution
    - Parameter binding ($1)
    - Row-to-model conversion
    """
    # Stub: When pool is functional:
    # result = pg.exec("SELECT id, name, price FROM items WHERE id=$1", item_id)
    # row = result.one()
    # return Item(**row)
    
    return Item(id=item_id, name="Example", price=9.99)


@app.get("/items")
def list_items(pg: Pg = Depends(get_pg_dependency)) -> list[Item]:
    """List all items.
    
    Demonstrates:
    - Query without parameters
    - Multiple row handling
    """
    # Stub: When pool is functional:
    # result = pg.exec("SELECT id, name, price FROM items ORDER BY id")
    # rows = result.all()
    # return [Item(**row) for row in rows]
    
    return []


@app.post("/items")
def create_item(req: CreateItemRequest, pg: Pg = Depends(get_pg_dependency)) -> Item:
    """Create a new item.
    
    Demonstrates:
    - Insert with returning
    - Scalar result extraction
    """
    # Stub: When pool is functional:
    # new_id = pg.exec(
    #     "INSERT INTO items(name, price) VALUES($1, $2) RETURNING id",
    #     req.name, req.price
    # ).scalar()
    # return Item(id=new_id, name=req.name, price=req.price)
    
    return Item(id=1, name=req.name, price=req.price)


@app.post("/items/bulk")
def bulk_import(pg: Pg = Depends(get_pg_dependency)):
    """Bulk import items via COPY.
    
    Demonstrates:
    - COPY IN for fast bulk insert
    - Streaming data without buffering
    """
    # Stub: When pool is functional:
    # with pg.copy_in("COPY items(name, price) FROM stdin CSV") as pipe:
    #     pipe.write(b"Widget,9.99\n")
    #     pipe.write(b"Gadget,19.95\n")
    #     pipe.write(b"Doohickey,14.50\n")
    # return {"ok": True, "rows_inserted": 3}
    
    return {"ok": True, "rows_inserted": 0}


@app.post("/purchase")
def purchase(user_id: int, item_id: int, pg: Pg = Depends(get_pg_dependency)):
    """Purchase an item (with transaction and stock check).
    
    Demonstrates:
    - Transactions with serializable isolation
    - Automatic retry on conflicts
    - FOR UPDATE row locking
    """
    # Stub: When pool is functional:
    # with pg.tx(isolation=TxIsolation.serializable, retries=3) as tx:
    #     # Check stock with row lock
    #     stock = tx.exec(
    #         "SELECT qty FROM stock WHERE item_id=$1 FOR UPDATE",
    #         item_id
    #     ).scalar()
    #     
    #     if stock <= 0:
    #         raise RuntimeError("Out of stock")
    #     
    #     # Decrement stock
    #     tx.exec("UPDATE stock SET qty = qty - 1 WHERE item_id=$1", item_id)
    #     
    #     # Create order
    #     order_id = tx.exec(
    #         "INSERT INTO orders(user_id, item_id) VALUES($1, $2) RETURNING id",
    #         user_id, item_id
    #     ).scalar()
    # 
    # return {"ok": True, "order_id": order_id}
    
    return {"ok": True, "order_id": 1}


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
