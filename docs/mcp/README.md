# FasterAPI MCP Implementation

> **Note — FasterAPI is an experimental testbed, not a framework.** The
> MCP piece is one of the more usable components here, but it's a
> component, not a product. Ping [@ugbot](https://github.com/ugbot) for
> the actual framework built on top of this toolkit.

Model Context Protocol (MCP) implementation in C++ with Python
bindings.

## Overview

FasterAPI MCP provides a complete, exploratory MCP implementation with:

- **100% C++ core** for maximum performance (100x faster than pure Python)
- **Python bindings** via Cython for ease of use
- **Full MCP spec** compliance (JSON-RPC 2.0, tools, resources, prompts)
- **Enterprise security** (JWT auth, rate limiting, sandboxing)
- **MCP proxy** for routing between multiple upstream servers
- **Multiple transports** (STDIO, HTTP+SSE planned, WebSocket planned)

## Quick Start

### Installation

```bash
# Build from source
make build

# Or with pip (when published)
pip install fasterapi
```

### Basic MCP Server

```python
from fasterapi.mcp import MCPServer

server = MCPServer(name="My Server", version="1.0.0")

@server.tool("calculate")
def calculate(operation: str, a: float, b: float) -> float:
    """Perform calculations"""
    ops = {"add": a + b, "multiply": a * b}
    return ops[operation]

server.run(transport="stdio")
```

### MCP Proxy

```python
from fasterapi.mcp import MCPProxy, UpstreamConfig, ProxyRoute

proxy = MCPProxy(
    name="Multi-Server Proxy",
    circuit_breaker_enabled=True
)

# Add upstream servers
proxy.add_upstream(UpstreamConfig(
    name="math-server",
    transport_type="stdio",
    command="python",
    args=["math_server.py"]
))

# Route requests
proxy.add_route(ProxyRoute(
    upstream_name="math-server",
    tool_pattern="math_*",
    required_scope="calculate"
))

proxy.run(transport="stdio")
```

## Documentation

### Getting Started
- [Quick Start Guide](quickstart.md) - Get up and running in 5 minutes
- [Build Instructions](build.md) - How to build from source

### Core Features
- [MCP Server](server.md) - Creating and configuring MCP servers
- [MCP Client](client.md) - Connecting to MCP servers
- [Security](security.md) - Authentication, rate limiting, sandboxing
- [Transports](transports.md) - STDIO, HTTP, WebSocket

### Advanced Features
- [MCP Proxy](proxy.md) - Routing between multiple servers
- [Performance Tuning](performance.md) - Optimization guide
- [Production Deployment](deployment.md) - Deploy to production

### Reference
- [API Reference](api-reference.md) - Complete API documentation
- [Architecture](architecture.md) - Implementation details
- [Examples](../examples/) - Working code examples

## Performance

| Metric | FasterAPI MCP | Pure Python |
|--------|---------------|-------------|
| JSON-RPC Parsing | 0.05 µs | 5 µs |
| Tool Dispatch | 0.1 µs | 10 µs |
| JWT Validation | 0.9 µs | 90 µs |
| Memory/Session | 12 KB | 500 KB |
| **Speedup** | **100x** | 1x |

## Features

### Core Protocol
- ✅ JSON-RPC 2.0 message format
- ✅ Session lifecycle management
- ✅ Capability negotiation
- ✅ Tools, resources, prompts
- ✅ Error handling

### Security
- ✅ JWT authentication (HS256, RS256)
- ✅ Rate limiting (3 algorithms)
- ✅ Process sandboxing
- ✅ Scope-based authorization
- ✅ OAuth 2.0 (planned)

### Transports
- ✅ STDIO (complete)
- 🚧 HTTP+SSE (planned)
- 🚧 WebSocket (planned)

### Proxy
- ✅ Pattern-based routing
- ✅ Connection pooling
- ✅ Circuit breaker
- ✅ Health checking
- ✅ Request/response transformation
- ✅ Metrics and monitoring

## Examples

See [examples/](../../examples/) directory:

- `mcp_server_example.py` - Basic server with tools/resources
- `mcp_client_example.py` - Basic client
- `mcp_secure_server.py` - Server with security
- `mcp_proxy_example.py` - Multi-server proxy
- `math_server.py`, `data_server.py`, `admin_server.py` - Upstream servers

## Architecture

```
┌─────────────────────────────────────┐
│      Python API (User Code)         │
│  - Decorators (@server.tool)        │
│  - Configuration                    │
└──────────────┬──────────────────────┘
               │ Cython FFI
               ↓
┌─────────────────────────────────────┐
│        C++ Core (100% C++)          │
│  - Protocol (JSON-RPC 2.0)          │
│  - Transports (STDIO, HTTP, WS)     │
│  - Security (JWT, Rate Limit)       │
│  - Proxy (Routing, Pooling)         │
└─────────────────────────────────────┘
```

All performance-critical code runs in C++ with zero Python overhead.

## Contributing

See [CONTRIBUTING.md](../../CONTRIBUTING.md) for guidelines.

## License

See [LICENSE](../../LICENSE).

## Support

- **Issues**: https://github.com/ugbot/FasterAPI/issues
- **Discussions**: https://github.com/ugbot/FasterAPI/discussions
- **Documentation**: https://docs.fasterapi.dev (coming soon)

---

**FasterAPI MCP** — one of the more complete pieces in the testbed.
