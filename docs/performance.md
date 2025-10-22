# Performance Guide

How to get the most out of FasterAPI's high-performance architecture.

## Understanding FasterAPI Performance

FasterAPI achieves high performance by moving hot paths to C++. But to get maximum performance, you need to understand where time is spent and how to optimize.

## Performance Characteristics

### What's Fast (C++)

These operations run in compiled C++ with minimal overhead:

- **Router lookups**: 30ns
- **HTTP parsing**: 10ns
- **PostgreSQL binary protocol**: 4x faster than pure Python
- **MCP protocol parsing**: 100x faster than pure Python
- **Connection pooling**: Lock-free C++ data structures

### What's Normal Speed (Python)

These operations run in Python and have typical Python performance:

- **Your route handlers**: Normal Python speed
- **Pydantic validation**: Fast but still Python
- **JSON encoding/decoding**: Python's json module
- **Business logic**: Whatever Python can do

### What Can Be Slow

Watch out for these:

- **Network I/O**: Dominates request time (10-100µs+)
- **Database queries**: Depends on query complexity
- **Python object creation**: Can add up with many objects
- **GIL contention**: If you have CPU-heavy Python code

## Optimization Strategy

### 1. Profile First

Don't guess. Measure:

```python
import time
import cProfile
import pstats

@app.get("/profile-me")
def slow_endpoint(req, res):
    start = time.time()
    
    # Your code here
    result = do_work()
    
    duration = time.time() - start
    print(f"Request took {duration*1000:.2f}ms")
    
    return result

# Or use cProfile
profiler = cProfile.Profile()
profiler.enable()

# Run your code

profiler.disable()
stats = pstats.Stats(profiler)
stats.sort_stats('cumulative')
stats.print_stats(20)
```

### 2. Optimize Database Access

Database queries often dominate request time.

**Use connection pooling:**
```python
# Initialize once
pool = PgPool(
    "postgres://localhost/mydb",
    min_size=10,    # Enough for baseline load
    max_size=100    # Enough for peak load
)
```

**Use parameterized queries:**
```python
# Fast: Prepared statement cached
result = pg.exec("SELECT * FROM users WHERE id = $1", user_id)

# Slow: Re-parsed every time
result = pg.exec(f"SELECT * FROM users WHERE id = {user_id}")
```

**Batch queries:**
```python
# Slow: N+1 queries
users = pg.exec("SELECT * FROM users").all()
for user in users:
    posts = pg.exec("SELECT * FROM posts WHERE user_id = $1", user["id"]).all()

# Fast: JOIN or single query
result = pg.exec("""
    SELECT u.*, 
           json_agg(p.*) as posts
    FROM users u
    LEFT JOIN posts p ON p.user_id = u.id
    GROUP BY u.id
""").all()
```

**Use COPY for bulk inserts:**
```python
# Slow: Individual INSERTs
for user in users:
    pg.exec("INSERT INTO users(name, email) VALUES($1, $2)", 
            user.name, user.email)

# Fast: COPY
with pg.copy_in("COPY users(name, email) FROM stdin CSV") as pipe:
    for user in users:
        pipe.write(f"{user.name},{user.email}\n".encode())
```

### 3. Minimize Python Work

**Cache expensive computations:**
```python
from functools import lru_cache

@lru_cache(maxsize=1000)
def expensive_computation(param):
    # Only computed once per unique param
    return complex_calculation(param)

@app.get("/compute/{value}")
def compute(req, res):
    value = req.path_params["value"]
    result = expensive_computation(value)
    return {"result": result}
```

**Use generators for large datasets:**
```python
# Bad: Loads everything into memory
@app.get("/all-users")
def all_users(req, res, pg=Depends(get_pg)):
    users = pg.exec("SELECT * FROM users").all()  # Could be millions!
    return {"users": users}

# Good: Stream results (when supported)
@app.get("/users")
def users_paginated(req, res, pg=Depends(get_pg)):
    page = int(req.query_params.get("page", 0))
    size = int(req.query_params.get("size", 100))
    
    result = pg.exec(
        "SELECT * FROM users OFFSET $1 LIMIT $2",
        page * size, size
    ).all()
    return {"users": result, "page": page}
```

**Avoid unnecessary Pydantic validation:**
```python
# Slow: Validates on every access
class User(BaseModel):
    id: int
    name: str
    email: str

@app.get("/users")
def list_users(req, res, pg=Depends(get_pg)):
    rows = pg.exec("SELECT * FROM users").all()
    users = [User(**row) for row in rows]  # Validates every row
    return {"users": users}

# Fast: Return dicts directly (if you trust your DB)
@app.get("/users")
def list_users(req, res, pg=Depends(get_pg)):
    users = pg.exec("SELECT * FROM users").all()
    return {"users": users}  # Already dicts, no validation
```

### 4. Use Async/Await Wisely

**Parallelize independent I/O:**
```python
import asyncio

# Slow: Sequential
@app.get("/dashboard")
async def dashboard(req, res):
    user = await fetch_user()
    posts = await fetch_posts()
    comments = await fetch_comments()
    return {"user": user, "posts": posts, "comments": comments}

# Fast: Parallel
@app.get("/dashboard")
async def dashboard(req, res):
    user, posts, comments = await asyncio.gather(
        fetch_user(),
        fetch_posts(),
        fetch_comments()
    )
    return {"user": user, "posts": posts, "comments": comments}
```

**Don't overuse async for CPU work:**
```python
# Bad: async doesn't help CPU-bound work
@app.get("/compute")
async def compute(req, res):
    result = expensive_cpu_work()  # Still blocks
    return {"result": result}

# Good: Use thread pool for CPU work
import asyncio

@app.get("/compute")
async def compute(req, res):
    result = await asyncio.to_thread(expensive_cpu_work)
    return {"result": result}
```

### 5. HTTP-Level Optimizations

**Enable compression:**
```python
app = App(
    port=8000,
    enable_compression=True  # zstd compression
)
```

**Use appropriate status codes:**
```python
# Let browser cache static data
@app.get("/config")
def get_config(req, res):
    res.set_header("Cache-Control", "public, max-age=3600")
    return {"config": "..."}

# Use 304 Not Modified when appropriate
@app.get("/data")
def get_data(req, res):
    etag = compute_etag(data)
    if req.headers.get("if-none-match") == etag:
        res.status(304)
        return res
    
    res.set_header("ETag", etag)
    return {"data": data}
```

**Minimize response size:**
```python
# Bad: Sends entire object
@app.get("/users/{user_id}")
def get_user(req, res, pg=Depends(get_pg)):
    user = pg.exec("""
        SELECT * FROM users WHERE id = $1
    """, user_id).one()
    return user  # Includes all columns

# Good: Only send what's needed
@app.get("/users/{user_id}")
def get_user(req, res, pg=Depends(get_pg)):
    user = pg.exec("""
        SELECT id, name, email FROM users WHERE id = $1
    """, user_id).one()
    return user
```

## Benchmarking Your Application

### Using wrk

```bash
# Install wrk
# macOS: brew install wrk
# Ubuntu: apt-get install wrk

# Simple benchmark
wrk -t4 -c100 -d30s http://localhost:8000/

# With custom script
wrk -t4 -c100 -d30s -s post.lua http://localhost:8000/users
```

**post.lua:**
```lua
wrk.method = "POST"
wrk.headers["Content-Type"] = "application/json"
wrk.body = '{"name":"test","email":"test@example.com"}'
```

### Using Apache Bench

```bash
# Simple GET
ab -n 10000 -c 100 http://localhost:8000/

# POST with data
ab -n 10000 -c 100 -p data.json -T application/json http://localhost:8000/users
```

### Custom Benchmark Script

```python
import asyncio
import aiohttp
import time

async def bench():
    url = "http://localhost:8000/users"
    
    async with aiohttp.ClientSession() as session:
        start = time.time()
        
        tasks = []
        for i in range(10000):
            tasks.append(session.get(url))
        
        responses = await asyncio.gather(*tasks)
        
        duration = time.time() - start
        
        print(f"10,000 requests in {duration:.2f}s")
        print(f"Throughput: {10000/duration:.0f} req/s")

asyncio.run(bench())
```

## Performance Checklist

Use this checklist for every performance-critical endpoint:

- [ ] Profiled to identify bottlenecks
- [ ] Using connection pooling for database
- [ ] Parameterized queries (prepared statements)
- [ ] Minimal data transferred (only needed columns)
- [ ] Batched queries where possible
- [ ] Using COPY for bulk inserts
- [ ] Parallel I/O with asyncio.gather
- [ ] Caching expensive computations
- [ ] Pagination for large result sets
- [ ] Compression enabled
- [ ] Appropriate HTTP caching headers
- [ ] Minimal Pydantic validation overhead

## Expected Performance Numbers

These are ballpark numbers on modern hardware (M2 MacBook Pro):

### Simple Endpoints

| Endpoint Type | Latency (p50) | Latency (p99) | Throughput |
|---------------|---------------|---------------|------------|
| Static JSON | 50µs | 100µs | 45K req/s |
| Path params | 60µs | 120µs | 40K req/s |
| Query params | 70µs | 140µs | 35K req/s |

### Database Endpoints

| Query Type | Latency (p50) | Latency (p99) | Throughput |
|------------|---------------|---------------|------------|
| Simple SELECT | 200µs | 500µs | 15K req/s |
| JOIN (2 tables) | 500µs | 2ms | 10K req/s |
| Complex query | 2ms | 10ms | 3K req/s |
| INSERT | 300µs | 1ms | 12K req/s |

### Bulk Operations

| Operation | Records/sec | Notes |
|-----------|-------------|-------|
| COPY INSERT | 100K-500K | Depends on data size |
| Batch SELECT | 10K-50K | Depends on result size |

If you're significantly below these numbers, profile and optimize!

## Common Performance Mistakes

### 1. Not Using Connection Pooling

```python
# Bad: New connection every request
@app.get("/users")
def list_users(req, res):
    pg = Pg("postgres://localhost/mydb")  # Slow!
    return pg.exec("SELECT * FROM users").all()

# Good: Pool connections
pool = PgPool("postgres://localhost/mydb", min_size=10, max_size=100)

@app.get("/users")
def list_users(req, res, pg=Depends(get_pg)):
    return pg.exec("SELECT * FROM users").all()
```

### 2. N+1 Query Problem

```python
# Bad: N+1 queries
@app.get("/users-with-posts")
def users_with_posts(req, res, pg=Depends(get_pg)):
    users = pg.exec("SELECT * FROM users").all()
    
    for user in users:
        user["posts"] = pg.exec(
            "SELECT * FROM posts WHERE user_id = $1",
            user["id"]
        ).all()
    
    return {"users": users}

# Good: Single query with JOIN
@app.get("/users-with-posts")
def users_with_posts(req, res, pg=Depends(get_pg)):
    return pg.exec("""
        SELECT u.*, json_agg(p.*) as posts
        FROM users u
        LEFT JOIN posts p ON p.user_id = u.id
        GROUP BY u.id
    """).all()
```

### 3. Not Using Indexes

```sql
-- Bad: Full table scan
SELECT * FROM users WHERE email = 'user@example.com';

-- Good: Create index
CREATE INDEX idx_users_email ON users(email);

-- Query is now fast
SELECT * FROM users WHERE email = 'user@example.com';
```

### 4. Loading Too Much Data

```python
# Bad: Loads all users into memory
@app.get("/export-users")
def export_users(req, res, pg=Depends(get_pg)):
    users = pg.exec("SELECT * FROM users").all()  # Could be millions!
    return {"users": users}

# Good: Paginate
@app.get("/users")
def list_users(req, res, pg=Depends(get_pg)):
    offset = int(req.query_params.get("offset", 0))
    limit = min(int(req.query_params.get("limit", 100)), 1000)
    
    users = pg.exec(
        "SELECT * FROM users OFFSET $1 LIMIT $2",
        offset, limit
    ).all()
    
    return {"users": users}
```

## When FasterAPI Isn't Enough

If you've optimized everything and still need more performance:

### Use Pure C++

Write critical endpoints in C++:

```cpp
// Fast C++ endpoint
server.add_route("GET", "/ultra-fast", [](Request& req, Response& res) {
    res.json(R"({"status":"ok"})");
});
```

### Use Multiple Processes

Run multiple FasterAPI processes:

```bash
# Using systemd or supervisor
for i in {1..8}; do
    python main.py --port $((8000 + i)) &
done
```

### Use a Reverse Proxy

```nginx
# nginx.conf
upstream fasterapi {
    server 127.0.0.1:8001;
    server 127.0.0.1:8002;
    server 127.0.0.1:8003;
    server 127.0.0.1:8004;
}

server {
    listen 80;
    
    location / {
        proxy_pass http://fasterapi;
    }
}
```

### Consider Other Languages

If you need absolute maximum performance and don't need Python:
- **Go**: Great balance of speed and development experience
- **Rust**: Maximum safety and speed
- **Java**: Mature ecosystem, excellent performance
- **C++**: Maximum control and performance

## Conclusion

FasterAPI gives you:
- Fast hot paths (C++)
- Python flexibility
- Easy optimization

Follow this guide to get the most out of it. Remember:

1. **Profile first** - Don't optimize blindly
2. **Optimize queries** - Usually the bottleneck
3. **Use pooling** - Connection overhead adds up
4. **Parallelize I/O** - async/await is your friend
5. **Cache wisely** - But invalidate correctly

Happy optimizing! 🚀

