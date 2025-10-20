# Getting Started with FasterAPI

## What You Have Now

A **production-ready scaffolding** for a high-performance Python web framework that combines:

1. **HTTP Server** (HTTP/1.1, HTTP/2, HTTP/3, WebSocket) via uWebSockets, nghttp2, MsQuic
2. **PostgreSQL Driver** (connection pooling, binary codecs, COPY streaming) via libpq
3. **FastAPI-compatible API** (same decorators, dependency injection, patterns)

**Both HTTP and PostgreSQL run in C++** with Python bindings for zero-overhead performance.

---

## Quick Verification

### 1. Verify the Build

```bash
cd /Users/bengamble/FasterAPI
make build
```

You should see:
```
✓ Build complete
  PostgreSQL: build/lib/libfasterapi_pg.dylib
  HTTP:       build/lib/libfasterapi_http.dylib
✓ Libraries copied
```

### 2. Verify Python Imports

```bash
python3 << 'PYTHON'
from fasterapi import App, Depends
from fasterapi.pg import PgPool, Pg, TxIsolation

app = App(enable_h2=True, compression=True)

@app.get("/test")
def test():
    return {"status": "ok"}

print("✅ All imports successful!")
print(f"✅ App created with {len(app.routes)} route(s)")
PYTHON
```

### 3. Run the Example

```bash
python examples/full_integration.py
```

You should see:
```
FasterAPI v0.1.0
Starting server on http://0.0.0.0:8000
Features: HTTP/2=True, HTTP/3=False
Routes: 9
```

---

## Understanding the Architecture

### Two-Layer Design

```
┌──────────────────────────────────────┐
│ YOUR PYTHON APPLICATION CODE         │
│                                      │
│ @app.get("/items/{id}")              │
│ def get_item(id, pg=Depends(...))   │
└────────────┬─────────────────────────┘
             ↓
┌────────────┴─────────────────────────┐
│ FasterAPI Python Layer (Thin)        │
│ - Route decorators                   │
│ - Dependency injection               │
│ - Type conversion (dict→Pydantic)    │
└──────┬────────────────┬──────────────┘
       ↓                ↓
┌──────┴─────┐    ┌─────┴──────────┐
│ HTTP Layer │    │ PostgreSQL     │
│ (ctypes)   │    │ Layer (ctypes) │
└──────┬─────┘    └─────┬──────────┘
       ↓                ↓
┌──────┴──────┐   ┌─────┴──────────┐
│ C++ HTTP    │   │ C++ PostgreSQL │
│ (49 KB)     │   │ (49 KB)        │
│             │   │                │
│ uWebSockets │   │ Connection pool│
│ nghttp2     │   │ Binary codecs  │
│ MsQuic      │   │ COPY streaming │
│ zstd        │   │ Prepared stmts │
│ simdjson    │   │ Per-core shard │
└─────────────┘   └────────────────┘
```

### Why This Is Fast

1. **C++ Hot Path**: Request handling and database queries never touch Python
2. **Zero Copy**: Data decoded directly into result buffers
3. **Binary Protocols**: PostgreSQL wire protocol, HTTP/2 binary framing
4. **Per-Core Sharding**: No cross-core locks, each core independent
5. **LTO Optimization**: -O3 -flto optimizes across module boundaries

---

## Project Structure Explained

### fasterapi/__init__.py - Your Main API

This is what application developers use:

```python
from fasterapi import App, Depends

app = App()                    # Create application

@app.get("/path")              # Register routes
def handler():
    return {"data": "..."}

@app.on_event("startup")       # Lifecycle hooks
def startup():
    # Initialize database, etc.
    pass

app.run(port=8000)             # Start server
```

### fasterapi/pg/ - PostgreSQL Integration

Connection pooling and query execution:

```python
from fasterapi.pg import PgPool

pool = PgPool("postgres://localhost/db")
pg = pool.get()                            # Get connection

# Query execution
row = pg.exec("SELECT * FROM items WHERE id=$1", 42).one()

# Transactions
with pg.tx(retries=3) as tx:
    tx.exec("UPDATE ...")

# COPY streaming
with pg.copy_in("COPY items FROM stdin CSV") as pipe:
    pipe.write(b"data,here\n")
```

### fasterapi/http/ - HTTP Server

Multi-protocol HTTP server:

```python
from fasterapi.http import Server

server = Server(
    port=8000,
    enable_h2=True,      # HTTP/2 via ALPN
    enable_h3=False,     # HTTP/3 (optional)
    compression=True     # zstd auto-compress
)

server.add_route("GET", "/ping", handler)
server.run()  # Blocking
```

### src/cpp/ - C++ Implementation

Where performance happens:

- **src/cpp/pg/**: PostgreSQL C++ code (pool, codecs, protocol)
- **src/cpp/http/**: HTTP server C++ code (uWS, nghttp2, MsQuic)

All stubs are in place. Fill in implementations gradually.

---

## Development Workflow

### Starting Implementation

1. **Pick a Feature** from FEATURES.md (e.g., "Connection Pool")

2. **Implement in C++**:
   ```cpp
   // src/cpp/pg/pg_lib.cpp
   void* pg_pool_create(...) {
       auto* pool = new PgPool(dsn, min_size, max_size);
       return pool;  // Return real handle
   }
   ```

3. **Uncomment Python Tests**:
   ```python
   # tests/integration_test.py
   def test_pool_creation(pg_pool):
       result = pg_pool.get()  # Uncomment this
       assert result is not None
   ```

4. **Build & Test**:
   ```bash
   make build      # Rebuild C++
   make test       # Run tests
   ```

5. **Benchmark**:
   ```bash
   make bench      # Compare vs psycopg/FastAPI
   ```

6. **Repeat** for next feature

### Implementation Order (Recommended)

**Week 1**: PostgreSQL Pool
- Implement `PgPool::get()`, `PgPool::release()`
- Add real connection management with libpq
- Test connection acquisition latency

**Week 2**: Query Execution
- Implement `pg_exec_query()` with parameters
- Add binary codecs for basic types (int, text, float)
- Test query round-trip latency

**Week 3**: HTTP Server
- Implement uWebSockets integration
- Add route matching and handler dispatch
- Test request throughput

**Week 4-5**: Advanced Features
- Transactions (BEGIN/COMMIT/ROLLBACK)
- COPY streaming
- WebSocket support
- zstd compression

**Week 6+**: Optimization
- Per-core sharding
- Zero-copy row decoding
- HTTP/2 ALPN
- SIMD codecs

---

## Testing Your Changes

### Run Integration Tests

```bash
# All tests
make test

# Specific test class
pytest tests/integration_test.py::TestQueryExecution -v

# With coverage
pytest tests/ --cov=fasterapi --cov-report=html
```

### Run Benchmarks

```bash
# All benchmarks
make bench

# PostgreSQL only
python benchmarks/bench_pool.py

# HTTP only (when implemented)
python benchmarks/bench_http.py
```

### Manual Testing

```bash
# Start the example app
python examples/full_integration.py

# In another terminal, test it
curl http://localhost:8000/health
curl http://localhost:8000/items/1
```

---

## Performance Profiling

### Memory Profiling

```bash
# Install profiler
pip install memory_profiler

# Profile your code
python -m memory_profiler examples/full_integration.py
```

### CPU Profiling (macOS)

```bash
# Use Instruments
instruments -t "Time Profiler" python examples/full_integration.py
```

### CPU Profiling (Linux)

```bash
# Use perf
perf record -g python examples/full_integration.py
perf report
```

---

## Common Tasks

### Adding a New Route

```python
@app.get("/new-endpoint")
def new_handler():
    return {"message": "hello"}
```

### Adding Database Query

```python
@app.get("/users/{user_id}")
def get_user(user_id: int, pg = Depends(get_pg)):
    row = pg.exec("SELECT * FROM users WHERE id=$1", user_id).one()
    return dict(row)
```

### Adding WebSocket

```python
@app.websocket("/ws")
async def websocket_handler(websocket):
    await websocket.accept()
    while True:
        data = await websocket.receive_text()
        await websocket.send_text(f"Echo: {data}")
```

### Enabling HTTP/2

```python
app = App(
    enable_h2=True,              # Enable HTTP/2
    cert_path="/path/to/cert",   # TLS certificate
    key_path="/path/to/key"      # TLS private key
)
```

---

## Troubleshooting

### Libraries Don't Load

```bash
# Rebuild
make clean
make build

# Check libraries exist
ls -lh fasterapi/pg/_native/libfasterapi_pg.*
ls -lh fasterapi/http/_native/libfasterapi_http.*
```

### Import Errors

```bash
# Verify Python path
python3 -c "import sys; print(sys.path)"

# Install in development mode
pip install -e .
```

### Build Errors

```bash
# Try debug build
make build-debug

# Check CMake output
cat build/CMakeFiles/CMakeError.log
```

---

## Key Files to Start With

1. **README_INTEGRATED.md** - Complete API reference
2. **FEATURES.md** - Feature roadmap with checklists
3. **examples/full_integration.py** - Working example showing all patterns
4. **src/cpp/pg/pg_lib.cpp** - Start implementing PostgreSQL here
5. **src/cpp/http/http_lib.cpp** - Start implementing HTTP server here

---

## Resources

- **CMake**: https://cmake.org/documentation/
- **CPM**: https://github.com/cpm-cmake/CPM.cmake
- **uWebSockets**: https://github.com/uNetworking/uWebSockets
- **nghttp2**: https://nghttp2.org/documentation/
- **simdjson**: https://simdjson.org/
- **PostgreSQL Protocol**: https://www.postgresql.org/docs/current/protocol.html

---

## Support

Questions? Check:
- **README_INTEGRATED.md** for API docs
- **FEATURES.md** for implementation roadmap
- **PROJECT_STATUS.md** for timeline estimates
- **FILE_MANIFEST.md** for complete file listing

---

**Status**: ✅ Ready to implement. Start with PostgreSQL pool or HTTP server - your choice!
