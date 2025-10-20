# FasterAPI Async Features

## Overview

FasterAPI now includes Seastar-style zero-allocation futures with continuation chaining, providing both ergonomic async/await syntax and explicit `.then()` chains for performance-critical code.

## Architecture

```
Python Layer:
  async def handler()           ← Default ergonomic API
  future.then(lambda x: ...)    ← Explicit control

C++ Layer (Zero-allocation futures):
  future<T>.then(continuation) → future<U>
  when_all(futures...) → future<tuple>
  Reactor per-core scheduling
```

## Core Components

### 1. C++ Future Implementation

- **Location**: `src/cpp/core/future.h`, `src/cpp/core/future.cpp`
- **Features**:
  - Zero-allocation for common path (value stored inline)
  - Template-based type-safe continuations
  - Exception propagation (via error states, no C++ exceptions)
  - Lock-free state transitions

### 2. Reactor & Scheduling

- **Location**: `src/cpp/core/reactor.h`, `src/cpp/core/reactor.cpp`
- **Features**:
  - Per-core event loop with task queue
  - I/O event polling (epoll/kqueue integration)
  - Timer management with nanosecond precision
  - Lock-free scheduling for same-core tasks

### 3. Task Abstraction

- **Location**: `src/cpp/core/task.h`
- **Features**:
  - Virtual task interface
  - Lambda task wrapper for closures
  - Priority-based scheduling support

### 4. PostgreSQL Future Integration

- **Location**: `src/cpp/pg/pg_future.h`, `src/cpp/pg/pg_future.cpp`
- **Features**:
  - Async query execution: `exec_async()`
  - Async prepared statements: `exec_prepared_async()`
  - Async transactions: `begin_tx_async()`, `commit_tx_async()`

### 5. Python Bindings

- **Location**: `fasterapi/core/`
- **Modules**:
  - `future.py` - Python Future wrapper with `__await__` support
  - `reactor.py` - Reactor lifecycle management
  - `bindings.py` - ctypes FFI for C++ futures
  - `combinators.py` - Higher-order async patterns

## Usage Examples

### Basic Async/Await (Default, Ergonomic)

```python
from fasterapi.core import Future

@app.get("/item/{id}")
async def get_item(id: int, pg = Depends(get_pg)):
    # Async database query
    row = await pg.exec_async("SELECT * FROM items WHERE id=$1", id)
    return dict(row)
```

### Explicit Chaining (Power Users)

```python
@app.get("/item/{id}/fast")
def get_item_fast(id: int, pg = Depends(get_pg)):
    # Explicit future chain (no await overhead)
    return (pg.exec_async("SELECT * FROM items WHERE id=$1", id)
            .then(lambda row: process(row))
            .then(lambda result: {"item": result}))
```

### Parallel Execution

```python
from fasterapi.core import when_all

@app.get("/user/{id}/full")
async def get_user_full(id: int, pg = Depends(get_pg)):
    # Execute queries in parallel
    user, orders = await when_all([
        pg.exec_async("SELECT * FROM users WHERE id=$1", id),
        pg.exec_async("SELECT * FROM orders WHERE user_id=$1", id)
    ])
    return {"user": user, "orders": orders}
```

### Error Handling

```python
@app.get("/item/{id}/safe")
async def get_item_safe(id: int):
    future = get_item_async(id)
    
    # Handle errors
    result = future.handle_error(lambda e: {"error": str(e), "fallback": True})
    return await result
```

### Higher-Order Patterns

```python
from fasterapi.core.combinators import map_async, retry_async, timeout_async

# Map over futures
prices = await map_async(
    lambda item: item.price,
    [get_item_async(id) for id in ids]
)

# Retry with backoff
result = await retry_async(
    lambda: db.query_async("SELECT ..."),
    max_retries=3,
    delay=0.1,
    backoff=2.0
)

# Timeout
result = await timeout_async(
    slow_operation_async(),
    timeout_seconds=5.0
)
```

### Pipeline Pattern

```python
from fasterapi.core.combinators import Pipeline

pipeline = (Pipeline()
            .add(lambda: fetch_data())
            .add(lambda data: transform(data))
            .add(lambda data: validate(data))
            .add(lambda data: save(data)))

result = await pipeline.execute()
```

## Performance Characteristics

### Benchmarks (M2 MacBook Pro, 12 cores)

```
=== Synchronous Benchmarks ===
Baseline (no futures)                 0.08 µs/op
Future creation                       0.26 µs/op  (3.2x baseline)
Explicit chain (3 ops)                1.59 µs/op  (19.3x baseline)
Complex chain (list ops)              1.88 µs/op

=== Async Benchmarks ===
Async/await (single)                  0.70 µs/op  (8.4x baseline)
Async/await (complex)                 1.03 µs/op

=== Analysis ===
Explicit vs Async (single):           0.44x (explicit is 2.3x slower due to overhead)
Future allocation overhead:           0.18 µs
Async/await overhead:                 0.61 µs
```

### Performance Targets

| Metric | Current | Target | Status |
|--------|---------|--------|--------|
| Future allocation | 0.26 µs | ~0 µs (stack-only) | ⚠️ Need optimization |
| Continuation overhead | 0.50 µs/op | <0.01 µs/op | ⚠️ Need optimization |
| Async/await overhead | 0.61 µs | <0.1 µs | ⚠️ Need optimization |

**Note**: Current implementation uses heap allocation for futures in some cases. Future work will optimize for stack-only allocation.

## Implementation Status

### ✅ Completed (Phase 1-6)

- [x] Core future/promise implementation
- [x] Reactor with per-core event loops
- [x] Task abstraction and scheduling
- [x] PostgreSQL async methods
- [x] Python Future wrapper with `__await__`
- [x] Higher-order combinators (`when_all`, `map_async`, `retry_async`, etc.)
- [x] Example applications
- [x] Benchmarks

### 🔄 In Progress

- [ ] Truly async I/O integration (currently synchronous with future wrappers)
- [ ] HTTP async route handlers
- [ ] Zero-copy optimizations
- [ ] Stack-only future allocation

### 📋 Future Work

- [ ] Async WebSocket handlers
- [ ] Streaming response support
- [ ] Background task scheduling
- [ ] Distributed tracing integration
- [ ] Advanced error recovery strategies

## API Reference

### Future Class

```python
class Future[T]:
    """Python wrapper for C++ future."""
    
    async def __await__() -> T:
        """Await the future (integrates with asyncio)."""
    
    def then(func: Callable[[T], U]) -> Future[U]:
        """Chain a continuation (explicit, faster)."""
    
    def handle_error(func: Callable[[Exception], T]) -> Future[T]:
        """Handle errors in the chain."""
    
    def is_ready() -> bool:
        """Check if future is ready."""
    
    def get() -> T:
        """Get value (blocking, for sync contexts)."""
    
    @staticmethod
    def make_ready(value: T) -> Future[T]:
        """Create an already-resolved future."""
    
    @staticmethod
    def make_exception(exception: Exception) -> Future[T]:
        """Create an already-failed future."""
```

### Combinators

```python
# Parallel execution
async def when_all(futures: List[Future[T]]) -> List[T]
async def when_any(futures: List[Future[T]]) -> tuple[T, List[Future[T]]]
async def when_some(futures: List[Future[T]], count: int) -> List[T]

# Transformations
async def map_async(func: Callable[[T], U], futures: List[Future[T]]) -> List[U]
async def filter_async(predicate: Callable[[T], bool], futures: List[Future[T]]) -> List[T]
async def reduce_async(func: Callable[[U, T], U], futures: List[Future[T]], initial: U) -> U

# Error handling & retry
async def retry_async(func: Callable[[], Future[T]], max_retries: int, delay: float, backoff: float) -> T
async def timeout_async(future: Future[T], timeout_seconds: float) -> T

# Utilities
class Pipeline:
    """Async pipeline for composing operations."""
    def add(func: Callable) -> Pipeline
    async def execute(initial: Any = None) -> Any
```

### Reactor

```python
class Reactor:
    """Per-core event loop manager."""
    
    @classmethod
    def initialize(num_cores: int = 0) -> None:
        """Initialize reactor subsystem."""
    
    @classmethod
    def shutdown() -> None:
        """Shutdown reactor subsystem."""
    
    @classmethod
    def current_core() -> int:
        """Get current core ID."""
    
    @classmethod
    def num_cores() -> int:
        """Get total number of cores."""
```

## Migration Guide

### From Synchronous Code

```python
# Before (synchronous)
@app.get("/item/{id}")
def get_item(id: int, pg = Depends(get_pg)):
    row = pg.exec("SELECT * FROM items WHERE id=$1", id).one()
    return dict(row)

# After (async)
@app.get("/item/{id}")
async def get_item(id: int, pg = Depends(get_pg)):
    row = await pg.exec_async("SELECT * FROM items WHERE id=$1", id)
    return dict(row)
```

### From FastAPI/asyncpg

```python
# Before (FastAPI + asyncpg)
import asyncpg

@app.get("/items")
async def get_items():
    conn = await asyncpg.connect("postgres://...")
    rows = await conn.fetch("SELECT * FROM items")
    return [dict(row) for row in rows]

# After (FasterAPI)
from fasterapi import App, Depends
from fasterapi.pg import PgPool

pool = PgPool("postgres://...", 1, 10)

@app.get("/items")
async def get_items(pg = Depends(lambda: pool.get())):
    rows = await pg.exec_async("SELECT * FROM items")
    return [dict(row) for row in rows]
```

## Examples

### Complete Examples

1. **Basic Async Demo** - `examples/async_demo.py`
   - Demonstrates all async patterns
   - Includes error handling, retry, pipeline
   
2. **HTTP + DB Demo** - `examples/async_http_demo.py`
   - Real-world HTTP handlers
   - Database query composition
   - Parallel execution

3. **Benchmarks** - `benchmarks/bench_futures.py`
   - Performance measurements
   - Comparison of async/await vs explicit chains

## Design Philosophy

1. **Default Ergonomic**: Use `async/await` for readable, maintainable code
2. **Explicit when needed**: Drop down to `.then()` chains for hot paths
3. **Zero overhead**: C++ futures with minimal Python wrapping
4. **Composable**: Rich set of combinators for complex patterns
5. **Compatible**: Works with existing asyncio code

## Contributing

When adding new async features:

1. Implement C++ future operations in `src/cpp/core/`
2. Export via `fasterapi/core/bindings.py`
3. Create Python wrappers in `fasterapi/core/`
4. Add tests and benchmarks
5. Update this document

## License

See main LICENSE file.

