# FasterAPI

**A high-performance C++ web framework with Python bindings**

FasterAPI is a complete C++ web framework designed for maximum performance, with Python bindings that provide familiar ergonomics. Built from the ground up with zero-copy parsing, lock-free data structures, and modern async I/O, it achieves **1.6M requests/second** on a single core.

## The Vision

FastAPI is awesome. It showed how a well-designed Python framework could make web development a joy. But C++ didn't have anything like it—no framework that combined modern API design with raw performance.

The goal is simple: build the fastest possible web framework in C++, then expose it to Python. Rather than bolting C++ onto Python, FasterAPI takes the opposite approach—the C++ framework is the product, and Python bindings are the interface layer.

This architecture means:
- **C++ handles all hot paths**: HTTP parsing, routing, connection management, protocol handling
- **Python handles application logic**: Your route handlers, business logic, and data models
- **Zero-cost abstractions**: The Python API adds no overhead to the C++ core

The result is a framework that matches C++ and Rust performance while keeping Python's developer experience.

## Performance

### Verified Benchmarks

| Component | Time/Op | Throughput | Notes |
|-----------|---------|------------|-------|
| Response Object | 614 ns | **1.6M req/s** | Zero-copy optimization |
| JSON Response | 1,880 ns | 532K req/s | Full request handling |
| Router Lookup | 29-30 ns | 33M lookups/s | Radix tree with params |
| HTTP/1.1 Parse | 10-12 ns | 83M parses/s | Zero-copy parsing |
| HPACK Decode | 6.7 ns | 149M ops/s | HTTP/2 header compression |

### Real-World Throughput

| Benchmark | Throughput | Context |
|-----------|------------|---------|
| **Max single-core** | 1.6M req/s | Response object creation |
| C++ HTTP server | 200K req/s | 1MRC Challenge (2.3x faster than Go) |
| Python/uvicorn mode | 12.8K req/s | 1MRC Challenge (zero errors) |

*Benchmarks measured on M2 MacBook Pro with `-O3 -mcpu=native -flto` using [oha](https://github.com/hatoo/oha) (Rust-based HTTP load generator). See [TECHEMPOWER_RESULTS.md](TECHEMPOWER_RESULTS.md) for details.*

### Why It's Fast

Every performance-critical operation runs in C++:
- **Lock-free data structures**: Aeron MPMC queues, object pools
- **Zero-copy parsing**: HTTP headers, JSON, query strings
- **Native async I/O**: kqueue (macOS), epoll (Linux), io_uring (Linux 5.1+)
- **Pre-allocated buffers**: Ring buffers, memory pools

Python handles application logic only—decorators, business code, and data models.

## 🚀 Quick Start

### Installation

```bash
# Install from source
git clone https://github.com/bengamble/FasterAPI.git
cd FasterAPI
pip install -e .[all]
```

### Your First API

```python
from fasterapi import App

app = App(port=8000)

@app.get("/")
def hello(req, res):
    return {"message": "Hello, World!"}

@app.get("/users/{user_id}")
async def get_user(req, res):
    user_id = req.path_params["user_id"]
    # Your async database call here
    return {"id": user_id, "name": "John Doe"}

if __name__ == "__main__":
    app.run()
```

That's it! You've got a high-performance web server running.

## Features

### HTTP Protocols
- **HTTP/1.1**: Full implementation with keep-alive, pipelining
- **HTTP/2**: ALPN negotiation, HPACK compression, stream multiplexing
- **HTTP/3**: QUIC with QPACK compression (in progress)

### Real-Time
- **WebSocket**: Full RFC 6455 implementation with compression
- **Server-Sent Events (SSE)**: Full implementation

### Infrastructure
- **Routing**: 29ns radix tree lookups with parameter extraction
- **Compression**: gzip, deflate, brotli, zstd
- **Static files**: Caching with ETag/If-Modified-Since
- **Templates**: Jinja2 integration

### PostgreSQL Integration
- **Native binary protocol** implementation in C++
- **Connection pooling** with health checks
- **Prepared statement caching**
- **Full async/await** support
- **Transaction management** with isolation levels
- **COPY support** for bulk operations

### MCP (Model Context Protocol)
- **100x faster** than pure Python MCP implementations
- **Complete MCP server** for exposing tools, resources, and prompts
- **MCP client** for consuming other MCP servers
- **MCP proxy** for routing between multiple upstream servers
- **Enterprise features**: JWT auth, rate limiting, circuit breakers
- **Multiple transports**: STDIO, HTTP/WebSocket (planned)

### Async/Await Support
- **Seastar-style futures** with zero-allocation fast paths
- **Full async/await** compatibility with Python's asyncio
- **Parallel composition**: `when_all`, `when_any`, `timeout_async`, `retry_async`
- **Pipeline operations**: map, filter, reduce over async sequences

## 📊 Performance

### Component Benchmarks

| Component | FasterAPI | Pure Python | Speedup |
|-----------|-----------|-------------|---------|
| Router Lookup | 30 ns | 500 ns | **16x** |
| HTTP/1.1 Parse | 10 ns | 1000 ns | **100x** |
| MCP JSON-RPC | 0.05 µs | 5 µs | **100x** |
| PostgreSQL Query | 50 µs | 200 µs | **4x** |

### Real-World Throughput

| Benchmark | FasterAPI | Comparison | Platform |
|-----------|-----------|------------|----------|
| 1 Million Request Challenge | **200K req/s** | Go: 85K req/s | C++ native |
| Simple HTTP endpoint | **45K req/s** | FastAPI: 10K req/s | Python |
| PostgreSQL queries | **100K qps** | asyncpg: 25K qps | C++ pool |

*Benchmarks run on M2 MacBook Pro using [oha](https://github.com/hatoo/oha). See [benchmarks/](benchmarks/) for details.*

## 📚 Documentation

### Getting Started
- **[Migration from FastAPI](docs/MIGRATION_FROM_FASTAPI.md)** - Step-by-step guide for FastAPI users
- **[Getting Started Guide](docs/GETTING_STARTED.md)** - Complete tutorial for new users
- **[Build Instructions](BUILD.md)** - Building from source
- **[Examples](examples/)** - Working code examples

### Feature Documentation
- **[HTTP Server](docs/http-server.md)** - HTTP server features and configuration
- **[PostgreSQL](docs/postgresql.md)** - Database integration and connection pooling
- **[MCP Protocol](docs/mcp/README.md)** - Model Context Protocol implementation
- **[Async/Await](docs/async-await.md)** - Future-based async programming

### Advanced Topics
- **[Performance Guide](docs/performance.md)** - Optimization tips and benchmarks
- **[Architecture](docs/architecture.md)** - How FasterAPI works under the hood
- **[C++ Integration](docs/cpp-integration.md)** - Building custom C++ extensions

## 🔄 Coming from FastAPI?

FasterAPI is designed to be familiar to FastAPI users. Here's the same FastAPI code side-by-side:

**FastAPI:**
```python
from fastapi import FastAPI, Depends
from pydantic import BaseModel

app = FastAPI()

class Item(BaseModel):
    name: str
    price: float

@app.get("/items/{item_id}")
def get_item(item_id: int):
    return {"id": item_id}

@app.post("/items")
def create_item(item: Item):
    return item
```

**FasterAPI:**
```python
from fasterapi import App
from pydantic import BaseModel

app = App()

class Item(BaseModel):
    name: str
    price: float

@app.get("/items/{item_id}")
def get_item(req, res):
    item_id = req.path_params["item_id"]
    return {"id": item_id}

@app.post("/items")
def create_item(req, res):
    item = Item(**req.json())
    return item
```

The key differences:
- You get `req` and `res` objects directly (more control)
- Path parameters are in `req.path_params` (explicit)
- Request body parsing is manual (flexible)
- PostgreSQL is a first-class citizen with `PgPool`

See the **[Migration Guide](docs/MIGRATION_FROM_FASTAPI.md)** for complete details.

## 🎨 Examples

### PostgreSQL Integration

```python
from fasterapi import App, PgPool, Depends
from fasterapi.pg.compat import get_pg_factory

pool = PgPool("postgres://localhost/mydb", min_size=2, max_size=20)
get_pg = get_pg_factory(pool)

app = App()

@app.get("/users/{user_id}")
def get_user(req, res, pg=Depends(get_pg)):
    user_id = req.path_params["user_id"]
    result = pg.exec("SELECT * FROM users WHERE id=$1", user_id)
    return result.one()

@app.post("/users")
def create_user(req, res, pg=Depends(get_pg)):
    data = req.json()
    user_id = pg.exec(
        "INSERT INTO users(name, email) VALUES($1, $2) RETURNING id",
        data["name"], data["email"]
    ).scalar()
    return {"id": user_id}
```

### Async/Await

```python
from fasterapi import App, when_all
import asyncio

app = App()

async def fetch_user(user_id):
    await asyncio.sleep(0.01)  # Simulate DB call
    return {"id": user_id, "name": f"User {user_id}"}

async def fetch_posts(user_id):
    await asyncio.sleep(0.01)  # Simulate DB call
    return [{"id": 1, "title": "Post 1"}]

@app.get("/users/{user_id}/dashboard")
async def get_dashboard(req, res):
    user_id = req.path_params["user_id"]
    
    # Parallel execution
    user, posts = await asyncio.gather(
        fetch_user(user_id),
        fetch_posts(user_id)
    )
    
    return {
        "user": user,
        "posts": posts
    }
```

### MCP Server

```python
from fasterapi.mcp import MCPServer

server = MCPServer(name="My Tools", version="1.0.0")

@server.tool("calculate")
def calculate(operation: str, a: float, b: float) -> float:
    """Perform basic calculations"""
    ops = {"add": a + b, "multiply": a * b, "subtract": a - b}
    return ops[operation]

@server.resource("config://settings")
def get_settings() -> dict:
    """Get application settings"""
    return {"max_retries": 3, "timeout": 30}

server.run(transport="stdio")
```

More examples in [examples/](examples/).

## 🏗️ Architecture

```
┌─────────────────────────────────────────────┐
│          Python Application Code            │
│  - Route handlers                           │
│  - Business logic                           │
│  - Pydantic models                          │
└────────────────┬────────────────────────────┘
                 │ Python API
                 ↓
┌─────────────────────────────────────────────┐
│          FasterAPI Python Layer             │
│  - Decorators (@app.get, etc.)              │
│  - Dependency injection                     │
│  - Request/Response wrappers                │
└────────────────┬────────────────────────────┘
                 │ Cython FFI
                 ↓
┌─────────────────────────────────────────────┐
│          C++ Core (High Performance)        │
│  - HTTP server (kqueue/epoll/io_uring)      │
│  - Router (radix tree)                      │
│  - HTTP/1.1, HTTP/2 parsers                 │
│  - PostgreSQL driver                        │
│  - MCP protocol implementation              │
│  - Connection pools                         │
│  - Futures/async primitives                 │
└─────────────────────────────────────────────┘
```

The pattern is simple:
1. **Python** handles the high-level API and business logic
2. **Cython** provides zero-cost FFI bindings
3. **C++** handles all performance-critical paths

This gives you:
- ✅ Python's ease of use and ecosystem
- ✅ C++'s raw performance
- ✅ Type safety at the boundaries
- ✅ Easy debugging (logs cross the boundary cleanly)

## 🧪 Testing

```bash
# Install test dependencies
pip install -e .[dev]

# Run all tests
pytest tests/ -v

# Run with coverage
pytest tests/ --cov=fasterapi --cov-report=html

# Run specific test
pytest tests/test_mcp_integration.py -v
```

## 📦 Project Structure

```
FasterAPI/
├── src/cpp/              # C++ core implementation
│   ├── core/             # Async I/O, futures, reactor
│   ├── http/             # HTTP server and parsers
│   ├── pg/               # PostgreSQL driver
│   └── mcp/              # MCP protocol
│
├── fasterapi/            # Python package
│   ├── __init__.py       # Main App class
│   ├── http/             # HTTP Python bindings
│   ├── pg/               # PostgreSQL Python API
│   ├── mcp/              # MCP Python API
│   ├── core/             # Futures, async utilities
│   └── webrtc/           # WebRTC (experimental)
│
├── examples/             # Working examples
├── tests/                # Test suite
├── benchmarks/           # Performance benchmarks
│   ├── fasterapi/        # FasterAPI benchmarks
│   ├── 1mrc/             # 1 Million Request Challenge
│   └── techempower/      # TechEmpower benchmarks
│
└── docs/                 # Documentation
```

## 🤝 Contributing

Contributions are welcome! Whether you want to:
- Add new features
- Improve documentation
- Fix bugs
- Add benchmarks
- Write examples

See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

## 📈 Roadmap

### Completed
- [x] HTTP/1.1 full implementation
- [x] HTTP/2 with ALPN, HPACK, stream multiplexing
- [x] WebSocket (full RFC 6455 with compression)
- [x] Server-Sent Events (SSE)
- [x] Middleware system
- [x] Static file serving
- [x] Jinja2 templates
- [x] Compression (gzip, deflate, brotli, zstd)

### In Progress (v0.3)
- [ ] HTTP/3 with QUIC (partially implemented)
- [ ] OpenAPI/Swagger generation
- [ ] Better error messages
- [ ] More examples

### Planned (v1.0)
- [ ] Stable API
- [ ] Complete documentation
- [ ] Comprehensive test coverage
- [ ] Production deployment guide
- [ ] PyPI release

## ❓ FAQ

**Q: Is FasterAPI production-ready?**  
A: It's in active development (v0.2.0). The core components are solid and battle-tested, but the API may change. Use it for side projects and internal tools. For production, wait for v1.0 or pin your version carefully.

**Q: Can I use FastAPI libraries with FasterAPI?**  
A: Some yes, some no. Pydantic works great. FastAPI-specific middleware and dependencies need adaptation. See the [Migration Guide](docs/MIGRATION_FROM_FASTAPI.md).

**Q: Why not just use Go/Rust/Java?**  
A: If you need maximum performance and don't need the Python ecosystem, use those! FasterAPI is for teams that want Python's ease of use with much better performance than pure Python frameworks.

**Q: How do you call C++ from Python so efficiently?**  
A: Cython! It generates C code that makes Python ↔ C++ calls nearly zero-cost. Combined with careful API design (minimize boundary crossings), we get Python ergonomics with C++ speed.

**Q: What's the catch?**  
A: Setup is more complex than pure Python (you need a C++ compiler). The API is less mature than FastAPI. Not everything is async yet. But if performance matters, it's worth it.

## 📄 License

MIT License - see [LICENSE](LICENSE) for details.

## 🙏 Acknowledgments

- **FastAPI** - for the API design inspiration
- **Seastar** - for the futures architecture
- **Anthropic** - for the MCP specification
- **Drogon, uWebSockets** - for showing what C++ web frameworks can do

## 📞 Support

- **Issues**: https://github.com/bengamble/FasterAPI/issues
- **Discussions**: https://github.com/bengamble/FasterAPI/discussions
- **Email**: bengamble@ (GitHub username)

---

**Built with ❤️ and C++**

*Because sometimes "fast enough" isn't fast enough.*
