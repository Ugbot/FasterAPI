# FasterAPI

*An experimental testbed for fast C++ web pieces and AI-assisted systems
design. Started as a joke on FastAPI's name. Grew into a parts bin.*

> **Note — FasterAPI is an experimental testbed, not a framework.**
> It is a collection of fast C++ web pieces (and Python shims over them)
> built to explore how far AI-assisted systems design can be pushed.
> Maturity varies by component. If you want the actual web framework
> being built on top of this toolkit, open an issue and ping
> [@ugbot](https://github.com/ugbot).

## What this actually is

`FasterAPI` started as a joke about being "faster than FastAPI". It
then became a sandbox for two separate things:

- **How far can AI-assisted C++ design be pushed?** No off-the-shelf web
  libraries. Real implementations of HTTP/1.1, HTTP/2 (HPACK + stream
  multiplexing), parts of HTTP/3, a binary-protocol PostgreSQL driver,
  an MCP server, etc. The point was to see what survives when you
  refuse shortcuts.
- **What does it actually take to put a C++ hot path under Python?**
  Cython FFI, object pools, ring buffers, ProcessPoolExecutor, ZeroMQ
  IPC, native event loops on each OS. A lot of the value of this repo
  is the answer to that question, not the framework-shaped output.

This is **not a web framework.** It is a pile of parts you could build
one from. Some pieces are solid and fast. Some are half-finished. Some
don't work. That's the point of a testbed — push ideas to the limit,
keep what survives, document the rest as exploration.

## What's in the box

Roughly in order of "this piece is genuinely useful" to "this piece is
an experiment":

- **HTTP/1.1 parser** — zero-copy, ~10 ns per parse.
- **Radix tree router** — ~29 ns lookups.
- **HTTP/2** — HPACK, stream multiplexing, ALPN.
- **HTTP/3 / QUIC** — partial; see `docs/HTTP3_ALGORITHMS.md`.
- **WebSocket** — RFC 6455 + compression.
- **Server-Sent Events.**
- **PostgreSQL driver** — native binary wire protocol, connection
  pool, prepared-statement cache, async.
- **MCP server** — Model Context Protocol in C++.
- **Lock-free primitives** — Aeron-style MPMC queues, object pools.
- **Native async I/O** — kqueue / epoll / io_uring / IOCP.
- **ZeroMQ IPC** — what we use to fan a single C++ accept loop out to
  Python worker processes.

Maturity varies by component. Read the code before trusting any one
piece.

## Numbers

Component-level microbenchmarks, on isolated pieces. **Not end-to-end
framework throughput**, don't quote these as "FasterAPI does X req/s":

| Component        | Time    | Throughput      |
|------------------|---------|-----------------|
| Response object  | 614 ns  | 1.6M ops/s      |
| JSON response    | 1,880 ns| 532K ops/s      |
| Router lookup    | 29 ns   | 33M lookups/s   |
| HTTP/1.1 parse   | 10 ns   | 83M parses/s    |
| HPACK decode     | 6.7 ns  | 149M ops/s      |

*Measured on an M2 MacBook Pro, `-O3 -mcpu=native -flto`.*

## Architecture (when wired together)

```
┌─────────────────────────────────────────────────────────────┐
│                 Python Application Code                      │
│            Route handlers • Business logic                   │
└─────────────────────────────────────────────────────────────┘
                              ↓ Cython FFI
┌─────────────────────────────────────────────────────────────┐
│                      C++ Core                                │
│  HTTP Server • Router • Parsers • PostgreSQL • Connection   │
│  Pool • Async I/O (kqueue/epoll/io_uring/IOCP) • MCP        │
└─────────────────────────────────────────────────────────────┘
```

For multi-core, the same C++ core fans out to Python worker processes
over ZeroMQ IPC:

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

## Example — using the HTTP piece

These snippets show the HTTP piece in isolation. The API is not stable;
treat them as examples, not a tutorial.

```python
from fasterapi import App

app = App(port=8000)

@app.get("/")
def hello(req, res):
    return {"message": "Hello, World!"}

@app.get("/users/{user_id}")
async def get_user(req, res):
    return {"id": req.path_params["user_id"]}

if __name__ == "__main__":
    app.run()
```

The PG driver and the MCP server are usable on their own too — see
`docs/postgresql.md` and `docs/mcp/`.

## Build

```bash
git clone https://github.com/ugbot/FasterAPI.git
cd FasterAPI
pip install -e .[all]
```

Or directly:

```bash
mkdir build && cd build
cmake ..
ninja
```

## Project structure

```
FasterAPI/
├── src/cpp/              # C++ core
│   ├── core/             # Async I/O, futures, reactor
│   ├── http/             # HTTP server and parsers
│   ├── pg/               # PostgreSQL driver
│   └── mcp/              # MCP protocol
├── fasterapi/            # Python package + Cython shims
├── tests/                # Test suite
├── benchmarks/           # Component microbenchmarks
└── docs/                 # Notes and exploration writeups
```

## Design discipline

The pieces that do work were built to a deliberate discipline:

- C++ on the hot paths; Python only for application logic.
- No allocations on hot paths — object pools, ring buffers,
  pre-allocated buffers.
- Lock-free where possible — Aeron-style queues, CAS.
- No mocks outside of tests. Real implementations.
- Cython over pybind for lower-overhead FFI.
- Bounded loops, asserts on invariants, explicit error paths.

## License

MIT.

## Want the actual web framework?

There is a real web framework being built *on top of* this toolkit.
This repo is the parts bin it draws from, not the framework itself. If
you're interested in that rather than the experiments, open an issue
and ping [@ugbot](https://github.com/ugbot).
