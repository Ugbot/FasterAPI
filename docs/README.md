# FasterAPI Documentation

Complete documentation for FasterAPI - the high-performance Python web framework with a C++ core.

## 📚 Table of Contents

### Getting Started
- **[Installation & Quick Start](../README.md#quick-start)** - Get up and running in 5 minutes
- **[Getting Started Guide](GETTING_STARTED.md)** - Complete tutorial for beginners
- **[Migration from FastAPI](MIGRATION_FROM_FASTAPI.md)** - For FastAPI users

### Core Features
- **[HTTP Server](http-server.md)** - HTTP/1.1 server, routing, and requests
- **[PostgreSQL Integration](postgresql.md)** - Database connections, pooling, and queries
- **[Async/Await](async-await.md)** - Futures and async programming
- **[MCP Protocol](mcp/README.md)** - Model Context Protocol implementation

### Advanced Topics
- **[Performance Guide](performance.md)** - Optimization tips and benchmarks
- **[Architecture](architecture.md)** - How FasterAPI works under the hood
- **[Building from Source](../BUILD.md)** - Compilation and development setup
- **[C++ Extensions](cpp-integration.md)** - Creating custom C++ components

### Reference
- **[API Reference](api-reference.md)** - Complete API documentation
- **[Configuration](configuration.md)** - Application configuration options
- **[Deployment](deployment.md)** - Production deployment guide

## 🚀 Quick Links

### Most Common Tasks

**Creating a basic API:**
```python
from fasterapi import App

app = App()

@app.get("/")
def home(req, res):
    return {"message": "Hello, World!"}

if __name__ == "__main__":
    app.run()
```

**Adding PostgreSQL:**
```python
from fasterapi import App, Depends
from fasterapi.pg import PgPool
from fasterapi.pg.compat import get_pg_factory

pool = PgPool("postgres://localhost/mydb", min_size=2, max_size=20)
get_pg = get_pg_factory(pool)

@app.get("/users")
def list_users(req, res, pg=Depends(get_pg)):
    return pg.exec("SELECT * FROM users").all()
```

**Using async/await:**
```python
import asyncio

@app.get("/data")
async def get_data(req, res):
    result1, result2 = await asyncio.gather(
        fetch_from_api1(),
        fetch_from_api2()
    )
    return {"api1": result1, "api2": result2}
```

See the [Getting Started Guide](GETTING_STARTED.md) for complete examples.

## 📖 Core Concepts

### Request/Response Model

Unlike many frameworks, FasterAPI gives you explicit access to request and response objects:

```python
@app.get("/example")
def handler(req, res):
    # req = Request object
    path = req.path
    method = req.method
    headers = req.headers
    query = req.query_params
    body = req.json()
    
    # res = Response object
    res.status(200)
    res.set_header("X-Custom", "value")
    
    return {"data": "..."}
```

This gives you more control and makes the data flow explicit.

### C++ Performance Paths

FasterAPI moves hot paths to C++ for maximum performance:

| Component | Language | Performance |
|-----------|----------|-------------|
| Router | C++ | 30ns per lookup |
| HTTP Parser | C++ | 10ns per request |
| PostgreSQL Driver | C++ | 4x faster than asyncpg |
| MCP Protocol | C++ | 100x faster than Python |
| Business Logic | Python | Your code |

You write Python. FasterAPI runs the slow parts in C++.

### Dependency Injection

Share resources across routes efficiently:

```python
from fasterapi import Depends

def get_db():
    # Called once per request
    return pool.get()

@app.get("/users")
def list_users(req, res, db=Depends(get_db)):
    return db.exec("SELECT * FROM users").all()
```

Dependencies can be:
- Database connections
- Authentication/authorization
- Configuration
- Caching layers
- Any shared resource

## 🎯 Use Cases

### FasterAPI is Great For:

✅ **High-throughput APIs**
- Need to handle 10K+ requests/sec per core
- Low latency requirements (<10ms)
- CPU-bound processing

✅ **Database-heavy Applications**
- Complex queries
- High query volume
- Need connection pooling
- Require transactions

✅ **Real-time Systems**
- WebSocket servers
- Server-Sent Events (SSE)
- Streaming data

✅ **AI/LLM Integration**
- MCP protocol servers
- Tool execution
- High-performance inference pipelines

### When to Use Something Else:

❌ **Quick Prototypes**
→ Use FastAPI (faster to write, good enough performance)

❌ **Maximum Framework Maturity**
→ Use Django or Flask (more mature ecosystems)

❌ **Need OpenAPI/Swagger Out of the Box**
→ Use FastAPI (built-in docs generation)

❌ **Absolute Maximum Performance**
→ Use pure C++ (Go, Rust, Java) if you don't need Python

## 📊 Performance Expectations

### Component Performance

Measured on M2 MacBook Pro:

| Operation | Time | Throughput |
|-----------|------|------------|
| Route lookup | 30 ns | 33M ops/s |
| HTTP/1.1 parse | 10 ns | 100M ops/s |
| PostgreSQL query (simple) | 50 µs | 20K qps/core |
| PostgreSQL query (complex) | 200 µs | 5K qps/core |
| MCP tool call | 1 µs | 1M ops/s |

### Request Throughput

| Scenario | Requests/sec | Notes |
|----------|--------------|-------|
| Hello World | 45K | Single core, no I/O |
| Database CRUD | 10-20K | With connection pool |
| Complex queries | 5-10K | Multiple joins |
| C++ libuv server | 200K | Pure C++ implementation |

See [Performance Guide](performance.md) for optimization tips.

## 🔧 Development Workflow

### Local Development

```bash
# 1. Clone and install
git clone https://github.com/bengamble/FasterAPI.git
cd FasterAPI
pip install -e .[dev]

# 2. Make changes to Python code
# No rebuild needed - editable install!

# 3. Make changes to C++ code
cmake --build build

# 4. Run tests
pytest tests/ -v

# 5. Run your app
python main.py
```

### Testing

```bash
# All tests
pytest tests/ -v

# Specific test file
pytest tests/test_http.py -v

# With coverage
pytest tests/ --cov=fasterapi --cov-report=html

# C++ tests
cd build && ctest -V
```

### Debugging

**Python code:** Use standard Python debuggers (pdb, ipdb, VS Code debugger)

**C++ code:** 
```bash
# Build with debug symbols
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Run under gdb/lldb
lldb -- python main.py
```

## 📦 Project Examples

### Small API (Single File)
```python
# api.py
from fasterapi import App

app = App()

@app.get("/")
def home(req, res):
    return {"status": "ok"}

if __name__ == "__main__":
    app.run()
```

### Medium Application (Structured)
```
my_api/
├── main.py          # Entry point
├── models.py        # Pydantic models
├── database.py      # DB setup
├── routers/
│   ├── users.py
│   └── posts.py
└── dependencies.py
```

### Large Application (Modular)
```
my_api/
├── api/
│   ├── __init__.py
│   ├── main.py
│   ├── config.py
│   ├── dependencies.py
│   ├── models/
│   │   ├── users.py
│   │   └── posts.py
│   ├── routers/
│   │   ├── users.py
│   │   └── posts.py
│   ├── services/
│   │   ├── auth.py
│   │   └── email.py
│   └── database/
│       ├── connection.py
│       └── migrations/
├── tests/
├── requirements.txt
└── README.md
```

See [examples/](../examples/) for working code.

## 🤝 Contributing

Contributions welcome! See [CONTRIBUTING.md](../CONTRIBUTING.md) for:
- Code style guide
- Testing requirements
- Pull request process
- Development setup

## 📝 Writing Documentation

Found a gap in the docs? Please contribute!

1. Fork the repository
2. Add/update documentation in `docs/`
3. Submit a pull request

We especially need:
- More examples
- Common use case guides
- Error troubleshooting
- Performance tuning tips

## 🆘 Getting Help

### Documentation
- Start with [Getting Started Guide](GETTING_STARTED.md)
- Check [examples/](../examples/)
- Read relevant feature docs

### Community
- **GitHub Issues**: Bug reports and feature requests
- **GitHub Discussions**: Questions and community help
- **Source Code**: Read the code when docs are unclear

### Common Issues

**Build failures:**
- Check [BUILD.md](../BUILD.md) for prerequisites
- Make sure CMake and C++ compiler are installed
- Try `pip install -e .[all] --force-reinstall`

**Import errors:**
- Verify installation: `python -c "from fasterapi import App"`
- Check C++ libraries exist: `ls fasterapi/_native/`
- Reinstall if needed

**PostgreSQL connection errors:**
- Check PostgreSQL is running
- Verify connection string
- Test with psql first

**Performance not as expected:**
- Profile your code
- Check query plans
- Review [Performance Guide](performance.md)
- Make sure you're using Release build

## 📚 Further Reading

### Official Documentation
- [MCP Specification](https://spec.modelcontextprotocol.io/) - Model Context Protocol
- [PostgreSQL Documentation](https://www.postgresql.org/docs/) - PostgreSQL reference
- [Seastar](http://seastar.io/) - Inspiration for futures design

### Related Projects
- [FastAPI](https://fastapi.tiangolo.com/) - Python web framework
- [Drogon](https://github.com/drogonframework/drogon) - C++ web framework
- [uWebSockets](https://github.com/uNetworking/uWebSockets) - WebSocket library

### Benchmarks
- [TechEmpower](https://www.techempower.com/benchmarks/) - Framework benchmarks
- [1MRC Challenge](https://github.com/Kavishankarks/1mrc) - 1 Million Request Challenge

## 📄 License

FasterAPI is MIT licensed. See [LICENSE](../LICENSE).

---

**Ready to get started?** → [Getting Started Guide](GETTING_STARTED.md)

**Coming from FastAPI?** → [Migration Guide](MIGRATION_FROM_FASTAPI.md)

**Need help?** → [GitHub Discussions](https://github.com/bengamble/FasterAPI/discussions)

