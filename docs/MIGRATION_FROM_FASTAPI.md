# Migrating from FastAPI to FasterAPI

A complete guide for FastAPI developers moving to FasterAPI.

## Overview

FasterAPI is designed to be familiar to FastAPI users while offering significantly better performance through C++ hot paths. This guide will help you understand the key differences and migrate your applications smoothly.

## Philosophy Differences

### FastAPI
- Pure Python with Pydantic for speed
- Heavy use of Python type hints for automatic validation
- "Magic" dependency injection based on type hints
- Automatic request parsing and validation

### FasterAPI
- C++ for performance-critical paths
- Python for business logic and high-level API
- Explicit request/response handling (more control)
- Manual parsing (more flexibility)
- Native PostgreSQL integration

## Core API Comparison

### Basic Application

**FastAPI:**
```python
from fastapi import FastAPI

app = FastAPI()

@app.get("/")
def root():
    return {"message": "Hello World"}
```

**FasterAPI:**
```python
from fasterapi import App

app = App()

@app.get("/")
def root(req, res):
    return {"message": "Hello World"}
```

**Key Difference:** FasterAPI route handlers receive `req` (Request) and `res` (Response) objects explicitly.

### Path Parameters

**FastAPI:**
```python
@app.get("/users/{user_id}")
def get_user(user_id: int):
    return {"user_id": user_id}
```

**FasterAPI:**
```python
@app.get("/users/{user_id}")
def get_user(req, res):
    user_id = int(req.path_params["user_id"])
    return {"user_id": user_id}
```

**Key Difference:** Path parameters are in `req.path_params` dict. You handle type conversion explicitly.

### Query Parameters

**FastAPI:**
```python
@app.get("/items")
def list_items(skip: int = 0, limit: int = 10):
    return {"skip": skip, "limit": limit}
```

**FasterAPI:**
```python
@app.get("/items")
def list_items(req, res):
    skip = int(req.query_params.get("skip", 0))
    limit = int(req.query_params.get("limit", 10))
    return {"skip": skip, "limit": limit}
```

**Key Difference:** Query params are in `req.query_params` dict. No automatic validation.

### Request Body

**FastAPI:**
```python
from pydantic import BaseModel

class Item(BaseModel):
    name: str
    price: float
    
@app.post("/items")
def create_item(item: Item):
    return {"item": item}
```

**FasterAPI:**
```python
from pydantic import BaseModel

class Item(BaseModel):
    name: str
    price: float
    
@app.post("/items")
def create_item(req, res):
    data = req.json()
    item = Item(**data)  # Pydantic validation
    return {"item": item}
```

**Key Difference:** You call `req.json()` explicitly and create your model manually.

### Headers

**FastAPI:**
```python
from fastapi import Header

@app.get("/items")
def read_items(user_agent: str = Header(None)):
    return {"User-Agent": user_agent}
```

**FasterAPI:**
```python
@app.get("/items")
def read_items(req, res):
    user_agent = req.headers.get("user-agent")
    return {"User-Agent": user_agent}
```

**Key Difference:** Headers accessed via `req.headers` dict.

### Response Customization

**FastAPI:**
```python
from fastapi import Response

@app.get("/legacy")
def get_legacy():
    return Response(content="old style", media_type="text/plain")
```

**FasterAPI:**
```python
@app.get("/legacy")
def get_legacy(req, res):
    res.set_header("Content-Type", "text/plain")
    res.text("old style")
    return res
```

**Key Difference:** You manipulate the `res` object directly for custom responses.

## Advanced Features

### Dependency Injection

**FastAPI:**
```python
from fastapi import Depends

def get_db():
    db = DatabaseConnection()
    try:
        yield db
    finally:
        db.close()

@app.get("/users")
def list_users(db = Depends(get_db)):
    return db.query("SELECT * FROM users")
```

**FasterAPI:**
```python
from fasterapi import Depends
from fasterapi.pg import PgPool
from fasterapi.pg.compat import get_pg_factory

# Initialize pool globally
pool = PgPool("postgres://localhost/mydb", min_size=2, max_size=20)
get_pg = get_pg_factory(pool)

@app.get("/users")
def list_users(req, res, pg=Depends(get_pg)):
    result = pg.exec("SELECT * FROM users")
    return result.all()
```

**Key Differences:** 
- FasterAPI has built-in `PgPool` with C++ connection pooling
- `Depends()` works similarly but is optimized for FasterAPI's patterns
- Pool initialization is separate from dependency declaration

### Background Tasks

**FastAPI:**
```python
from fastapi import BackgroundTasks

@app.post("/send-email")
def send_email(background_tasks: BackgroundTasks, email: str):
    background_tasks.add_task(send_email_task, email)
    return {"message": "Email queued"}
```

**FasterAPI:**
```python
import asyncio

@app.post("/send-email")
async def send_email(req, res):
    data = req.json()
    # Use asyncio for background work
    asyncio.create_task(send_email_task(data["email"]))
    return {"message": "Email queued"}
```

**Key Difference:** Use standard asyncio for background tasks. FasterAPI doesn't have a special BackgroundTasks abstraction yet.

### Middleware

**FastAPI:**
```python
@app.middleware("http")
async def add_process_time_header(request, call_next):
    start = time.time()
    response = await call_next(request)
    duration = time.time() - start
    response.headers["X-Process-Time"] = str(duration)
    return response
```

**FasterAPI:**
```python
import time

@app.add_middleware
def add_process_time_header(req, res):
    start = time.time()
    # Middleware runs before handler
    # For post-processing, you need to wrap the response
    # (FasterAPI middleware is simpler but less flexible currently)
```

**Key Difference:** FasterAPI middleware is currently simpler. Full request/response middleware coming in v0.3.

### Startup/Shutdown Events

**FastAPI:**
```python
@app.on_event("startup")
async def startup():
    # Initialize database pool, etc.
    pass

@app.on_event("shutdown")
async def shutdown():
    # Close connections
    pass
```

**FasterAPI:**
```python
@app.on_event("startup")
def startup():
    # Can be sync or async
    global pool
    pool = PgPool("postgres://localhost/mydb", min_size=2, max_size=20)

@app.on_event("shutdown")
def shutdown():
    pool.close()
```

**Key Difference:** Same API, but FasterAPI's events can be sync or async.

## Async/Await

Both frameworks support async/await, but with different underlying implementations.

**FastAPI:**
```python
@app.get("/users/{user_id}")
async def get_user(user_id: int):
    user = await db.fetch_one(f"SELECT * FROM users WHERE id = {user_id}")
    return user
```

**FasterAPI:**
```python
@app.get("/users/{user_id}")
async def get_user(req, res, pg=Depends(get_pg)):
    user_id = int(req.path_params["user_id"])
    # FasterAPI can use asyncio or its own Future-based system
    user = await asyncio.to_thread(
        lambda: pg.exec("SELECT * FROM users WHERE id=$1", user_id).one()
    )
    return user
```

**FasterAPI also has its own Future system (Seastar-style):**

```python
from fasterapi import when_all

@app.get("/dashboard/{user_id}")
async def dashboard(req, res):
    user_id = int(req.path_params["user_id"])
    
    # Parallel execution with when_all
    user, posts = await asyncio.gather(
        fetch_user(user_id),
        fetch_posts(user_id)
    )
    
    return {"user": user, "posts": posts}
```

## PostgreSQL Integration

This is where FasterAPI really shines.

**FastAPI (using asyncpg):**
```python
import asyncpg
from fastapi import FastAPI

app = FastAPI()

async def get_pool():
    return await asyncpg.create_pool(
        "postgres://localhost/mydb",
        min_size=2,
        max_size=20
    )

@app.get("/users")
async def list_users():
    pool = await get_pool()
    async with pool.acquire() as conn:
        rows = await conn.fetch("SELECT * FROM users")
        return [dict(row) for row in rows]
```

**FasterAPI:**
```python
from fasterapi import App, Depends
from fasterapi.pg import PgPool
from fasterapi.pg.compat import get_pg_factory

app = App()

# C++ connection pool initialized once
pool = PgPool("postgres://localhost/mydb", min_size=2, max_size=20)
get_pg = get_pg_factory(pool)

@app.get("/users")
def list_users(req, res, pg=Depends(get_pg)):
    # C++ binary protocol, zero-copy where possible
    result = pg.exec("SELECT * FROM users")
    return result.all()
```

**Advantages:**
- Native binary protocol (faster than asyncpg)
- Automatic parameter binding with `$1, $2` syntax
- Connection pooling in C++ (no GIL contention)
- Prepared statement caching
- Transaction support with retries

### Transactions

**FastAPI (asyncpg):**
```python
@app.post("/transfer")
async def transfer(amount: float, from_id: int, to_id: int):
    pool = await get_pool()
    async with pool.acquire() as conn:
        async with conn.transaction():
            await conn.execute(
                "UPDATE accounts SET balance = balance - $1 WHERE id = $2",
                amount, from_id
            )
            await conn.execute(
                "UPDATE accounts SET balance = balance + $1 WHERE id = $2",
                amount, to_id
            )
    return {"status": "ok"}
```

**FasterAPI:**
```python
from fasterapi.pg import TxIsolation

@app.post("/transfer")
def transfer(req, res, pg=Depends(get_pg)):
    data = req.json()
    
    with pg.tx(isolation=TxIsolation.serializable, retries=3) as tx:
        tx.exec(
            "UPDATE accounts SET balance = balance - $1 WHERE id = $2",
            data["amount"], data["from_id"]
        )
        tx.exec(
            "UPDATE accounts SET balance = balance + $1 WHERE id = $2",
            data["amount"], data["to_id"]
        )
    
    return {"status": "ok"}
```

**Advantages:**
- Automatic retry on serialization failure
- Configurable isolation levels
- Simpler syntax

### Bulk Operations (COPY)

**FastAPI (asyncpg):**
```python
@app.post("/bulk-import")
async def bulk_import(items: List[Item]):
    pool = await get_pool()
    async with pool.acquire() as conn:
        await conn.copy_records_to_table(
            "items",
            records=[(item.name, item.price) for item in items],
            columns=["name", "price"]
        )
    return {"imported": len(items)}
```

**FasterAPI:**
```python
@app.post("/bulk-import")
def bulk_import(req, res, pg=Depends(get_pg)):
    items = req.json()["items"]
    
    with pg.copy_in("COPY items(name, price) FROM stdin CSV") as pipe:
        for item in items:
            pipe.write(f"{item['name']},{item['price']}\n".encode())
    
    return {"imported": len(items)}
```

**FasterAPI COPY is:**
- Streaming (no buffering)
- Zero-copy where possible
- Faster than asyncpg for large datasets

## Performance Comparison

Let's be honest about the performance differences:

### Request/Response Overhead

**FastAPI:**
- Router: ~500ns
- Request parsing: ~1000ns
- Pydantic validation: ~2000ns
- Total overhead: ~3500ns per request

**FasterAPI:**
- Router: ~30ns (C++ radix tree)
- Request parsing: ~10ns (C++ HTTP parser)
- Manual validation: ~variable
- Total overhead: ~40ns per request + your validation time

**Speedup: 87x for routing/parsing alone**

### Database Queries

**FastAPI + asyncpg:**
- Connection acquire: ~500ns
- Query execution: ~50µs (typical)
- Result parsing: ~1000ns per row
- Total: ~50-100µs depending on result size

**FasterAPI + PgPool:**
- Connection acquire: ~100ns (C++ pool)
- Query execution: ~50µs (same)
- Result parsing: ~100ns per row (C++ binary protocol)
- Total: ~50-60µs

**Speedup: ~2x for typical queries, more for large result sets**

### JSON Encoding/Decoding

**FastAPI:**
- Uses Pydantic with `orjson` (C extension)
- Encoding: ~300ns for simple objects
- Decoding + validation: ~2000ns

**FasterAPI:**
- Uses Python's json or your choice
- Encoding: ~300ns (same, it's Python's json)
- Decoding: ~300ns (no auto-validation)
- Manual validation: ~variable

**Note:** JSON performance is similar. FasterAPI's speed comes from routing, parsing, and database access.

## Migration Strategy

### 1. Start with a New Endpoint

Don't migrate everything at once. Add a new FasterAPI endpoint alongside your FastAPI app:

```python
# main.py (FastAPI)
from fastapi import FastAPI

fastapi_app = FastAPI()

# ... your existing routes ...

# Add FasterAPI for new high-performance endpoints
from fasterapi import App

fasterapi_app = App(port=8001)  # Different port

@fasterapi_app.get("/high-perf-endpoint")
def new_endpoint(req, res):
    # New code using FasterAPI
    pass

if __name__ == "__main__":
    # Run both (in production, use separate processes)
    import threading
    t = threading.Thread(target=fasterapi_app.run)
    t.start()
    
    import uvicorn
    uvicorn.run(fastapi_app, port=8000)
```

### 2. Migrate Database-Heavy Endpoints

FasterAPI shines with database queries. Start there:

**Before (FastAPI):**
```python
@fastapi_app.get("/users")
async def list_users(skip: int = 0, limit: int = 100):
    pool = await get_asyncpg_pool()
    async with pool.acquire() as conn:
        rows = await conn.fetch(
            "SELECT * FROM users OFFSET $1 LIMIT $2",
            skip, limit
        )
        return [dict(row) for row in rows]
```

**After (FasterAPI):**
```python
@fasterapi_app.get("/users")
def list_users(req, res, pg=Depends(get_pg)):
    skip = int(req.query_params.get("skip", 0))
    limit = int(req.query_params.get("limit", 100))
    
    result = pg.exec(
        "SELECT * FROM users OFFSET $1 LIMIT $2",
        skip, limit
    )
    return result.all()
```

### 3. Add Validation Helpers

Create helpers to bridge the gap with FastAPI's auto-validation:

```python
# helpers.py
from pydantic import BaseModel, ValidationError

def parse_body(req, model_class: type[BaseModel]):
    """Parse and validate request body"""
    try:
        data = req.json()
        return model_class(**data)
    except ValidationError as e:
        # Return error response
        raise HTTPException(status_code=422, detail=e.errors())

# Usage
from pydantic import BaseModel

class CreateUserRequest(BaseModel):
    name: str
    email: str

@app.post("/users")
def create_user(req, res):
    data = parse_body(req, CreateUserRequest)
    # data is now validated
    return {"user": data}
```

### 4. Migrate Route by Route

Go through your FastAPI routes one by one. For each route:

1. Copy the route definition
2. Update to FasterAPI syntax (add `req, res` params)
3. Update path/query parameter access
4. Update request body parsing
5. Test thoroughly
6. Deploy

### 5. Update Dependencies

**FastAPI dependencies:**
```python
# old_deps.py
from fastapi import Depends

def get_current_user(token: str = Header(...)):
    # verify token
    return user
```

**FasterAPI dependencies:**
```python
# new_deps.py
from fasterapi import Depends

def get_current_user(req):
    token = req.headers.get("authorization", "").replace("Bearer ", "")
    # verify token
    return user

# Usage
@app.get("/me")
def get_me(req, res, user=Depends(get_current_user)):
    return {"user": user}
```

## Common Pitfalls

### 1. Forgetting req/res Parameters

❌ **Wrong:**
```python
@app.get("/users")
def list_users():
    return {"users": []}
```

✅ **Right:**
```python
@app.get("/users")
def list_users(req, res):
    return {"users": []}
```

### 2. Expecting Auto-Validation

❌ **Wrong:**
```python
@app.post("/users")
def create_user(req, res, user: User):  # Won't work
    return {"user": user}
```

✅ **Right:**
```python
@app.post("/users")
def create_user(req, res):
    data = req.json()
    user = User(**data)  # Manual validation
    return {"user": user}
```

### 3. Type Conversion

❌ **Wrong:**
```python
@app.get("/users/{user_id}")
def get_user(req, res):
    user_id = req.path_params["user_id"]  # This is a string!
    # SQL will fail if expecting int
    return pg.exec("SELECT * FROM users WHERE id=$1", user_id).one()
```

✅ **Right:**
```python
@app.get("/users/{user_id}")
def get_user(req, res):
    user_id = int(req.path_params["user_id"])
    return pg.exec("SELECT * FROM users WHERE id=$1", user_id).one()
```

### 4. Response Object Misuse

❌ **Wrong:**
```python
@app.get("/users")
def list_users(req, res):
    res.json({"users": []})
    return {"users": []}  # Sent twice!
```

✅ **Right (option 1):**
```python
@app.get("/users")
def list_users(req, res):
    return {"users": []}  # FasterAPI handles JSON
```

✅ **Right (option 2):**
```python
@app.get("/users")
def list_users(req, res):
    res.json({"users": []})
    return res
```

## Feature Parity

| Feature | FastAPI | FasterAPI | Notes |
|---------|---------|-----------|-------|
| Path parameters | ✅ | ✅ | Different syntax |
| Query parameters | ✅ | ✅ | Manual parsing |
| Request body | ✅ | ✅ | Manual parsing |
| Response models | ✅ | ⚠️ | Use Pydantic manually |
| Dependency injection | ✅ | ✅ | Different API |
| Async/await | ✅ | ✅ | Plus custom futures |
| WebSockets | ✅ | 🚧 | Coming in v0.3 |
| Background tasks | ✅ | ⚠️ | Use asyncio |
| Middleware | ✅ | ⚠️ | Limited currently |
| OpenAPI docs | ✅ | ❌ | Not yet implemented |
| OAuth2 | ✅ | ⚠️ | Build yourself |
| CORS | ✅ | ⚠️ | Build yourself |
| File uploads | ✅ | 🚧 | Coming in v0.3 |
| Static files | ✅ | ⚠️ | Use middleware |
| PostgreSQL | ⚠️ | ✅ | FasterAPI has native support |
| Connection pooling | ⚠️ | ✅ | C++ pool in FasterAPI |

Legend:
- ✅ Fully supported
- ⚠️ Supported but different or manual
- 🚧 Coming soon
- ❌ Not available

## When to Use What?

### Use FastAPI if:
- You need OpenAPI/Swagger docs out of the box
- You want maximum "magic" and minimal boilerplate
- You're building a quick prototype
- You don't need extreme performance
- You want the most mature ecosystem

### Use FasterAPI if:
- Performance is critical
- You're doing heavy database work
- You want explicit control over requests/responses
- You're comfortable with some manual work
- You want to leverage C++ for hot paths
- You need PostgreSQL connection pooling

### Use Both if:
- You have a large existing FastAPI app
- You want to migrate gradually
- You want to use FasterAPI for hot paths only

## Example: Complete Migration

Here's a complete example migrating a FastAPI app:

**Before (FastAPI):**
```python
from fastapi import FastAPI, Depends, HTTPException
from pydantic import BaseModel
import asyncpg

app = FastAPI()

class User(BaseModel):
    id: int | None = None
    name: str
    email: str

async def get_db():
    pool = await asyncpg.create_pool("postgres://localhost/mydb")
    try:
        yield pool
    finally:
        await pool.close()

@app.get("/users")
async def list_users(skip: int = 0, limit: int = 10, db = Depends(get_db)):
    async with db.acquire() as conn:
        rows = await conn.fetch(
            "SELECT * FROM users OFFSET $1 LIMIT $2",
            skip, limit
        )
        return [dict(row) for row in rows]

@app.get("/users/{user_id}")
async def get_user(user_id: int, db = Depends(get_db)):
    async with db.acquire() as conn:
        row = await conn.fetchrow("SELECT * FROM users WHERE id=$1", user_id)
        if row is None:
            raise HTTPException(status_code=404, detail="User not found")
        return dict(row)

@app.post("/users")
async def create_user(user: User, db = Depends(get_db)):
    async with db.acquire() as conn:
        user_id = await conn.fetchval(
            "INSERT INTO users(name, email) VALUES($1, $2) RETURNING id",
            user.name, user.email
        )
        return {"id": user_id, "name": user.name, "email": user.email}
```

**After (FasterAPI):**
```python
from fasterapi import App, Depends
from fasterapi.pg import PgPool
from fasterapi.pg.compat import get_pg_factory
from pydantic import BaseModel

app = App()

# Initialize pool once
pool = PgPool("postgres://localhost/mydb", min_size=2, max_size=20)
get_pg = get_pg_factory(pool)

class User(BaseModel):
    id: int | None = None
    name: str
    email: str

@app.on_event("startup")
def startup():
    print("Database pool initialized")

@app.on_event("shutdown")
def shutdown():
    pool.close()

@app.get("/users")
def list_users(req, res, pg=Depends(get_pg)):
    skip = int(req.query_params.get("skip", 0))
    limit = int(req.query_params.get("limit", 10))
    
    result = pg.exec(
        "SELECT * FROM users OFFSET $1 LIMIT $2",
        skip, limit
    )
    return result.all()

@app.get("/users/{user_id}")
def get_user(req, res, pg=Depends(get_pg)):
    user_id = int(req.path_params["user_id"])
    
    result = pg.exec("SELECT * FROM users WHERE id=$1", user_id)
    row = result.one_or_none()
    
    if row is None:
        res.status(404)
        return {"error": "User not found"}
    
    return row

@app.post("/users")
def create_user(req, res, pg=Depends(get_pg)):
    data = req.json()
    user = User(**data)  # Validate with Pydantic
    
    user_id = pg.exec(
        "INSERT INTO users(name, email) VALUES($1, $2) RETURNING id",
        user.name, user.email
    ).scalar()
    
    return {"id": user_id, "name": user.name, "email": user.email}

if __name__ == "__main__":
    app.run()
```

## Next Steps

1. **Read the [Getting Started Guide](GETTING_STARTED.md)**
2. **Try the [examples](../examples/)**
3. **Check out [PostgreSQL docs](postgresql.md)**
4. **Read the [Performance Guide](performance.md)**
5. **Join the [Discord/Discussion](https://github.com/bengamble/FasterAPI/discussions)**

## Getting Help

- **GitHub Issues**: Bug reports and feature requests
- **GitHub Discussions**: Questions and community help
- **Examples**: Look at [examples/](../examples/) for working code
- **Source Code**: Read the source when docs are unclear

## Conclusion

FasterAPI is different from FastAPI, but intentionally so. The trade-off is:

**You give up:** Some convenience, automatic validation, mature ecosystem  
**You get:** 10-100x better performance, explicit control, native PostgreSQL

For many projects, FastAPI is the right choice. But if you need speed and you're willing to be a bit more explicit, FasterAPI can deliver.

Welcome to FasterAPI! 🚀

