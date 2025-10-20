# FasterAPI

High-performance Python web framework with Seastar-style futures, native C++ pooling, binary codecs, and zero-Python hot paths. Drop-in DX matching FastAPI's style.

## 🚀 New: Async Future Chaining

FasterAPI now includes **Seastar-style zero-allocation futures** with continuation chaining! See [ASYNC_FEATURES.md](ASYNC_FEATURES.md) for complete documentation.

### Quick Example

```python
from fasterapi import App, Future, when_all

app = App()

# Async/await (ergonomic)
@app.get("/user/{id}")
async def get_user(id: int, pg = Depends(get_pg)):
    user = await pg.exec_async("SELECT * FROM users WHERE id=$1", id)
    return dict(user)

# Explicit chaining (performance)
@app.get("/fast")
def fast_endpoint():
    return (fetch_data()
            .then(process)
            .then(respond))

# Parallel execution
@app.get("/summary")
async def summary(pg = Depends(get_pg)):
    user, orders = await when_all([
        pg.exec_async("SELECT * FROM users WHERE id=1"),
        pg.exec_async("SELECT * FROM orders WHERE user_id=1")
    ])
    return {"user": user, "orders": orders}
```

## PostgreSQL Integration

High-performance PostgreSQL driver with native C++ pooling, binary codecs, and zero-Python hot paths.

## Design Philosophy

- **Zero Python on hot path**: All I/O, pooling, protocol parsing, and row decoding done in C++ (-O3, LTO)
- **Per-core sharding**: Avoid cross-core locks; each core has its own connection pool and prepared statement cache
- **Binary protocol**: Network byte order, minimal serialization overhead
- **Zero-copy rows**: Decode directly into result buffers; defer materialization
- **FastAPI DX**: `Depends(get_pg)`, familiar query API, familiar errors

## Architecture

```
┌─────────────────────────────────────────────────┐
│ FastAPI Application (Python)                    │
│ - Route handlers with Depends(get_pg)           │
│ - pg.exec(...) returns QueryResult[Row]         │
│ - pg.tx() context manager for transactions      │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│ fasterapi/pg/ (Python Layer)                    │
│ - PgPool: Async pool with per-core sharding     │
│ - Pg: Connection handle, query builder          │
│ - Row: Dict-like row with attr access           │
│ - QueryResult: .all(), .one(), .scalar(), etc   │
│ - Exceptions: PgError, PgTimeout, etc           │
└────────────────────┬────────────────────────────┘
                     │
        ┌────────────▼────────────┐
        │ ctypes FFI (bindings.py)│
        └────────────┬────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│ libfasterapi_pg.so/dylib (C++ Layer)            │
│ - PgPool: Per-core sharded pool                 │
│ - PgConnection: Non-blocking libpq (phase 1)   │
│ - PgCodec: Binary type codecs                   │
│ - PgProtocol: Message framing, encoding         │
│ - C-exported functions for ctypes               │
└────────────────────┬────────────────────────────┘
                     │
        ┌────────────▼────────────┐
        │ libpq (phase 1)         │
        │ Native protocol (phase2)│
        └────────────┬────────────┘
                     │
        ┌────────────▼────────────┐
        │ PostgreSQL Server       │
        └─────────────────────────┘
```

## Quick Start

### Installation

```bash
# Clone and enter workspace
git clone <repo>
cd FasterAPI

# Install dependencies
pip install -e ".[dev]"

# Build C++ library
make build
```

### Basic Usage

```python
from fastapi import FastAPI, Depends
from pydantic import BaseModel
from fasterapi.pg import PgPool

app = FastAPI()
pool = None

@app.on_event("startup")
def startup():
    global pool
    pool = PgPool("postgres://user:pass@localhost/mydb")

def get_pg():
    return pool.get()

class Item(BaseModel):
    id: int
    name: str
    price: float

@app.get("/items/{item_id}")
def get_item(item_id: int, pg = Depends(get_pg)) -> Item:
    row = pg.exec("SELECT id, name, price FROM items WHERE id=$1", item_id).one()
    return Item(**row)

@app.post("/items/bulk")
def bulk_import(pg = Depends(get_pg)):
    with pg.copy_in("COPY items(name, price) FROM stdin CSV") as pipe:
        pipe.write(b"Widget,9.99\n")
        pipe.write(b"Gadget,19.95\n")
    return {"ok": True}

@app.post("/purchase")
def purchase(user_id: int, item_id: int, pg = Depends(get_pg)):
    with pg.tx(retries=3) as tx:
        stock = tx.exec("SELECT qty FROM stock WHERE item_id=$1 FOR UPDATE", item_id).scalar()
        if stock <= 0:
            raise RuntimeError("out_of_stock")
        tx.exec("UPDATE stock SET qty=qty-1 WHERE item_id=$1", item_id)
        tx.exec("INSERT INTO orders(user_id,item_id) VALUES($1,$2)", user_id, item_id)
    return {"ok": True}
```

## Project Structure

```
FasterAPI/
├── README.md                    # This file
├── FEATURES.md                  # Feature roadmap with performance targets
├── planning.md                  # Design doc from inception
├── LICENSE                      # MIT
├── CMakeLists.txt              # C++ build configuration
├── Makefile                     # Convenience targets (build, test, bench, clean)
├── setup.py                     # Python packaging
│
├── fasterapi/
│   ├── pg/
│   │   ├── __init__.py         # Public API exports
│   │   ├── types.py            # TxIsolation, Row, QueryResult, PreparedQuery
│   │   ├── exceptions.py       # PgError hierarchy
│   │   ├── pool.py             # PgPool, Pg (connection handle)
│   │   ├── bindings.py         # ctypes FFI to C++ library
│   │   ├── compat.py           # FastAPI Depends support
│   │   └── _native/            # Compiled library (.so/.dylib)
│
├── src/cpp/pg/
│   ├── pg_lib.cpp              # C-exported FFI entry points
│   ├── pg_pool.h               # Per-core pool interface
│   ├── pg_connection.h         # Single connection (non-blocking libpq)
│   ├── pg_protocol.h           # Binary protocol, parameter encoding
│   └── pg_codec.h              # Type codecs (int, float, text, etc)
│
├── tests/
│   ├── conftest.py             # pytest fixtures (postgres container, schema)
│   └── integration_test.py      # Test suite (all features stubbed)
│
├── benchmarks/
│   ├── runner.py               # Orchestrate all benchmarks
│   ├── bench_pool.py           # Connection pool & query latency
│   └── bench_codecs.py         # Type codec performance
│
└── examples/
    └── basic_app.py            # Minimal FastAPI demo
```

## Development Workflow

### Phase 1: Stubs → Working MVP

1. **All stubs are already in place** (this repo). Python types, C++ interfaces, test suite.
2. **Fill in implementations incrementally**:
   - Start with PgPool (C++ + Python)
   - Add query execution (exec, exec_prepared)
   - Add transactions
   - Add COPY, error handling, observability
3. **Tests drive development**: Enable test assertions as features are implemented.
4. **Benchmarks track progress**: Compare against psycopg/asyncpg at each milestone.

### Building

```bash
make build              # Release (optimized) build
make build-debug        # Debug build with symbols
make clean              # Remove build artifacts
```

### Testing

```bash
make test               # Run integration tests (requires PostgreSQL)
pytest tests/ -v       # Run specific test file
```

### Benchmarking

```bash
make bench              # Run all benchmarks
python benchmarks/bench_pool.py      # Run specific benchmark
```

### Linting

```bash
make lint               # Python type checking
python -m mypy fasterapi/pg --ignore-missing-imports
```

## Performance Targets

### Phase 1 (Current - Stubs)

| Metric | Target | vs psycopg |
|--------|--------|-----------|
| Connection acquisition | < 100µs | 10x faster |
| Query latency (simple) | < 500µs | 10x faster |
| Query latency (prepared) | < 200µs | 20x faster |
| COPY throughput | > 1 GB/sec | 100x faster |

### Phase 2 & 3

See FEATURES.md for progressive performance targets.

## API Reference

### Connection Pool

```python
pool = PgPool(
    dsn="postgres://user:pass@localhost/db",
    min_size=2,                         # Connections per core
    max_size=20,
    idle_timeout_secs=600,
    health_check_interval_secs=30,
)

# Get connection
pg = pool.get(core_id=None, deadline_ms=5000)

# Pool stats
stats = pool.stats()  # -> {"in_use": 1, "idle": 2, ...}

# Shutdown
pool.close()
```

### Query Execution

```python
# Simple query
result = pg.exec("SELECT 1")
value = result.scalar()              # First column of first row

# Query with parameters
row = pg.exec("SELECT * FROM items WHERE id=$1", item_id).one()

# Multiple results
rows = pg.exec("SELECT * FROM items").all()

# Streaming (no buffering)
for row in pg.exec("SELECT * FROM huge_table").stream(chunk_size=1000):
    process(row)

# Convert to model
from pydantic import BaseModel
class Item(BaseModel):
    id: int
    name: str

items = pg.exec("SELECT id, name FROM items").model(Item)

# Prepared statements (fastest)
from fasterapi.pg import prepare
Q = prepare("SELECT * FROM items WHERE id=$1")
item = pg.run(Q, item_id)
```

### Transactions

```python
# Simple transaction
with pg.tx() as tx:
    tx.exec("INSERT INTO items(name) VALUES($1)", "Widget")

# With isolation level
with pg.tx(isolation=TxIsolation.serializable) as tx:
    # Transaction body

# With retries on serialization failures
with pg.tx(isolation=TxIsolation.serializable, retries=3) as tx:
    # Auto-retry up to 3 times
    pass
```

### COPY Operations

```python
# Bulk insert (fast)
with pg.copy_in("COPY items(name, price) FROM stdin CSV") as pipe:
    pipe.write(b"Widget,9.99\n")
    pipe.write(b"Gadget,19.95\n")

# Bulk export to HTTP response
response = pg.copy_out_response(
    "COPY items TO stdout CSV HEADER",
    filename="items.csv"
)
```

### Dependency Injection

```python
from fastapi import Depends
from fasterapi.pg.compat import get_pg_factory

pool = PgPool(...)
get_pg = get_pg_factory(pool)

@app.get("/items/{id}")
def handler(item_id: int, pg = Depends(get_pg)):
    # pg is request-scoped, automatically released
    return pg.exec("SELECT * FROM items WHERE id=$1", item_id).one()
```

## Error Handling

```python
from fasterapi.pg import PgError, PgConnectionError, PgTimeout, PgCanceled

try:
    pg.exec("SELECT ...", deadline_ms=1000)
except PgTimeout:
    # Query exceeded deadline
    pass
except PgConnectionError:
    # Pool exhausted or connection lost
    pass
except PgCanceled:
    # Query was canceled
    pass
except PgError as e:
    # Base exception: all PG errors
    print(e.code, e.detail)
```

## Contributing

See FEATURES.md for the implementation roadmap. Each feature is well-defined with tests and performance targets.

1. Pick a feature
2. Implement stubs in C++
3. Enable integration tests
4. Run benchmarks
5. Optimize if needed
6. Create PR

## Performance Profiling

```bash
# Generate flamegraph
python -m perf bench benchmarks/bench_pool.py --profile

# Run under perf
perf record -g python benchmarks/bench_pool.py
perf report

# Memory profiling
python -m memory_profiler benchmarks/bench_pool.py
```

## License

MIT

## References

- **Design Doc**: `planning.md`
- **Feature Roadmap**: `FEATURES.md`
- **Example App**: `examples/basic_app.py`
- **PostgreSQL Protocol**: https://www.postgresql.org/docs/current/protocol.html
- **libpq**: https://www.postgresql.org/docs/current/libpq.html
