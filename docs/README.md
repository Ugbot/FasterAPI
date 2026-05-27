# FasterAPI Documentation

> **Note — FasterAPI is an experimental testbed, not a framework.**
> It is a collection of fast C++ web pieces (and Python shims over them)
> built to explore how far AI-assisted systems design can be pushed.
> Maturity varies by component. If you want the actual web framework
> being built on top of this toolkit, open an issue and ping
> [@ugbot](https://github.com/ugbot).

Notes and exploration writeups for the pieces in this repo. This is a
parts bin — most docs here describe a single component in isolation
rather than "the framework", because there isn't one stable framework.

## What's documented

### Orientation
- [Getting Started](GETTING_STARTED.md) — using the pieces directly.
  Not a framework tutorial.
- [Migration from FastAPI](MIGRATION_FROM_FASTAPI.md) — the API
  *resembles* FastAPI in places. Read the banner before you migrate
  anything real.

### Components
- [HTTP / PostgreSQL piece](postgresql.md) — native binary-protocol PG
  driver with a connection pool.
- [MCP server piece](mcp/README.md) — Model Context Protocol in C++.
- [HTTP/3 + QUIC algorithms](HTTP3_ALGORITHMS.md) — partial
  implementation notes.

### Internals
- [Architecture](architecture.md) — how the pieces are wired when used
  together.
- [C++ API architecture](cpp_api_architecture.md) and
  [C++ user API](cpp_user_api.md) — using the C++ core directly.
- [C++ server optimisations](CPP_SERVER_OPTIMIZATIONS.md)
- [Python ↔ C++ optimisation notes](python_cpp_optimization.md)
- [Performance notes](performance.md) — component microbenchmarks. Not
  end-to-end framework throughput.

### Build
- [Top-level README](../README.md) — install + build.
- [MCP build notes](mcp/build.md)

### History
- `archive/` — earlier exploration writeups, kept for context. Each
  carries a "research note" header — claims reflect what was being
  tried at the time, not the current state.
- `reports/` — investigation reports from individual experiments.

> Some docs the old index used to link to (`http-server.md`,
> `async-await.md`, `cpp-integration.md`, `api-reference.md`,
> `configuration.md`, `deployment.md`) were never written. For those
> pieces, the code under `src/cpp/` and `fasterapi/` is the spec.

## Component microbenchmarks

Isolated-piece numbers. **Do not quote these as end-to-end framework
throughput** — there is no single "framework" wiring to measure end to
end.

| Operation        | Time   | Throughput     |
|------------------|--------|----------------|
| Route lookup     | 30 ns  | 33M ops/s      |
| HTTP/1.1 parse   | 10 ns  | 100M ops/s     |
| PG query (simple)| 50 µs  | ~20K qps/core  |
| MCP tool call    | 1 µs   | 1M ops/s       |

See [Performance notes](performance.md) for context on each number.

## Using the pieces

Treat snippets in these docs as examples of a single piece working in
isolation. The API is not stable; if you need stability, vendor the
piece you care about.

```python
from fasterapi import App

app = App()

@app.get("/")
def home(req, res):
    return {"status": "ok"}
```

## Development

```bash
git clone https://github.com/ugbot/FasterAPI.git
cd FasterAPI
pip install -e .[dev]
pytest tests/ -v
```

C++ rebuild:

```bash
cmake --build build
```

## Help and contact

- **GitHub Issues** — bug reports, questions, or pinging
  [@ugbot](https://github.com/ugbot) about the real framework
  built on top of this toolkit.
- **Source** — when a doc is wrong or missing, the code is the spec.

## License

MIT. See [LICENSE](../LICENSE).
