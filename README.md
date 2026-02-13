# FasterAPI

**High-Performance C++ Web Framework with Python Bindings**

FasterAPI is a C++ web framework designed for raw performance, with Python bindings via Cython. The C++ core handles all hot paths—HTTP parsing, routing, connection management—while Python handles application logic.

## Performance

| Component | Time | Throughput |
|-----------|------|------------|
| Response Object | 614 ns | 1.6M req/s |
| JSON Response | 1,880 ns | 532K req/s |
| Router Lookup | 29 ns | 33M lookups/s |
| HTTP/1.1 Parse | 10 ns | 83M parses/s |
| HPACK Decode | 6.7 ns | 149M ops/s |

*Benchmarks on M2 MacBook Pro with `-O3 -mcpu=native -flto`.*

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                 Python Application Code                      │
│            Route handlers • Business logic                   │
└─────────────────────────────────────────────────────────────┘
                              ↓ Cython FFI
┌─────────────────────────────────────────────────────────────┐
│                      C++ Core                                │
│  HTTP Server • Router • Parsers • PostgreSQL • Connection   │
│  Pool • Async I/O (kqueue/epoll/io_uring) • MCP Protocol    │
└─────────────────────────────────────────────────────────────┘
```

**Why it's fast:**
- Lock-free data structures (Aeron MPMC queues, object pools)
- Zero-copy HTTP parsing
- Pre-allocated buffers and ring buffers
- Native async I/O (kqueue, epoll, io_uring)
- No allocations in hot paths

## Features

### HTTP
- HTTP/1.1 with keep-alive and pipelining
- HTTP/2 with ALPN, HPACK, stream multiplexing
- HTTP/3 with QUIC (in progress)

### WebSocket & SSE
- Full RFC 6455 WebSocket with compression
- Server-Sent Events

### PostgreSQL
- Native binary protocol in C++
- Connection pooling with health checks
- Prepared statement caching
- Full async/await support

### MCP (Model Context Protocol)
- C++ MCP server implementation
- 100x faster than pure Python MCP
- Tools, resources, prompts support

### Infrastructure
- Radix tree router (29ns lookups)
- Compression (gzip, deflate, brotli, zstd)
- Static file serving with caching
- ZeroMQ IPC for multi-process parallelism

## Quick Start

```python
from fasterapi import App

app = App(port=8000)

@app.get("/")
def hello(req, res):
    return {"message": "Hello, World!"}

@app.get("/users/{user_id}")
async def get_user(req, res):
    user_id = req.path_params["user_id"]
    return {"id": user_id}

if __name__ == "__main__":
    app.run()
```

### PostgreSQL

```python
from fasterapi import App, PgPool, Depends
from fasterapi.pg.compat import get_pg_factory

pool = PgPool("postgres://localhost/mydb", min_size=2, max_size=20)
get_pg = get_pg_factory(pool)

app = App()

@app.get("/users/{user_id}")
def get_user(req, res, pg=Depends(get_pg)):
    user_id = req.path_params["user_id"]
    return pg.exec("SELECT * FROM users WHERE id=$1", user_id).one()
```

### MCP Server

```python
from fasterapi.mcp import MCPServer

server = MCPServer(name="My Tools", version="1.0.0")

@server.tool("calculate")
def calculate(operation: str, a: float, b: float) -> float:
    ops = {"add": a + b, "multiply": a * b}
    return ops[operation]

server.run(transport="stdio")
```

## Build

```bash
git clone https://github.com/bengamble/FasterAPI.git
cd FasterAPI
pip install -e .[all]
```

### From Source

```bash
mkdir build && cd build
cmake ..
ninja
```

## Project Structure

```
FasterAPI/
├── src/cpp/              # C++ core
│   ├── core/             # Async I/O, futures, reactor
│   ├── http/             # HTTP server and parsers
│   ├── pg/               # PostgreSQL driver
│   └── mcp/              # MCP protocol
├── fasterapi/            # Python package
│   ├── http/             # HTTP bindings
│   ├── pg/               # PostgreSQL API
│   ├── mcp/              # MCP API
│   └── core/             # Async utilities
├── tests/                # Test suite
└── benchmarks/           # Performance tests
```

## Design Principles

- **C++ handles hot paths**: HTTP parsing, routing, connections—all in C++
- **Python for logic**: Application code stays in Python
- **No allocations in hot paths**: Object pools, ring buffers, pre-allocated buffers
- **Lock-free where possible**: Aeron-style queues, CAS operations
- **No shortcuts**: Real implementations, no mocks outside tests
- **Cython over pybind**: Lower overhead FFI

## Multi-Process Architecture

FasterAPI uses ProcessPoolExecutor + ZeroMQ IPC for parallelism:

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│  Worker 0   │     │  Worker 1   │     │  Worker N   │
│  (Process)  │     │  (Process)  │     │  (Process)  │
└──────┬──────┘     └──────┬──────┘     └──────┬──────┘
       │                   │                   │
       └───────────────────┼───────────────────┘
                           │ ZeroMQ IPC
                           ↓
                    ┌──────────────┐
                    │   C++ Core   │
                    └──────────────┘
```

## Status

**v0.2.0**—core components solid, API may change.

| Component | Status |
|-----------|--------|
| HTTP/1.1 | Working |
| HTTP/2 | Working |
| HTTP/3 | In progress |
| WebSocket | Working |
| SSE | Working |
| PostgreSQL | Working |
| MCP | Working |
| Router | Working |

## Comparison

| Framework | Throughput | Language |
|-----------|------------|----------|
| FasterAPI | 200K req/s | C++ + Python |
| FastAPI | 10K req/s | Python |
| Go stdlib | 85K req/s | Go |

*1 Million Request Challenge benchmark.*

## License

MIT License
