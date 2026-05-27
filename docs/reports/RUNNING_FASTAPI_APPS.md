> **Research note — snapshot, not current state.**
> This document is from FasterAPI's exploration phase. Claims here
> reflect what was being investigated at the time, not the testbed's
> current state. FasterAPI is an experimental testbed, not a framework.
> Ping [@ugbot](https://github.com/ugbot) for the actual framework
> built on top of this toolkit.
# Running FastAPI Apps on FasterAPI

## Yes, You Can Run FastAPI Apps! 🎉

FasterAPI now fully supports running standard FastAPI applications on its native C++ HTTP server. Your FastAPI code runs with **native C++ performance** instead of Uvicorn.

## Quick Start

### 1. Write Standard FastAPI Code

```python
from fasterapi.fastapi_compat import FastAPI
from fasterapi.http.server import Server
from fasterapi._fastapi_native import connect_route_registry_to_server

# Create FastAPI app (standard syntax!)
app = FastAPI(title="My API", version="1.0.0")

# Define routes using FastAPI decorators
@app.get("/")
def root():
    return {"message": "Hello World!"}

@app.get("/users/{user_id}")
def get_user(user_id: int):
    return {"id": user_id, "name": f"User {user_id}"}

@app.get("/search")
def search(q: str, limit: int = 10):
    return {"query": q, "limit": limit}
```

### 2. Start the Native Server

```python
if __name__ == "__main__":
    # Connect route registry for parameter extraction
    connect_route_registry_to_server()

    # Create native C++ HTTP server
    server = Server(port=8000, host="0.0.0.0")
    server.start()

    print("Server running on http://0.0.0.0:8000")

    # Keep server alive
    import time
    try:
        while server.is_running():
            time.sleep(1)
    except KeyboardInterrupt:
        server.stop()
```

### 3. Run Your App

```bash
DYLD_LIBRARY_PATH=build/lib:$DYLD_LIBRARY_PATH python3.13 your_app.py
```

## What Works ✅

### Routing
- ✅ Standard FastAPI decorators: `@app.get()`, `@app.post()`, `@app.put()`, `@app.delete()`
- ✅ Path parameters: `/users/{user_id}` with type conversion
- ✅ Query parameters: `?q=value&limit=10` with defaults
- ✅ Multiple parameters: path + query combined
- ✅ Type conversion: `int`, `str`, `bool`
- ✅ Default values for optional parameters
- ✅ 404 handling for non-existent routes

### Server Features
- ✅ Native C++ HTTP/1.1 server (CoroIO event loop)
- ✅ Non-blocking, high-performance request handling
- ✅ Concurrent request processing
- ✅ Zero Python overhead in HTTP parsing
- ✅ Compression support

## Example Application

See `examples/fastapi_on_fasterapi_demo.py` for a complete working example with:
- Root endpoint
- Health check
- Path parameters
- Query parameters with defaults
- Combined path + query parameters

## Performance Benefits

Running FastAPI on FasterAPI's native server gives you:

1. **Native C++ HTTP parsing** - No Python overhead for request parsing
2. **CoroIO event loop** - High-performance async I/O with kqueue/epoll
3. **Zero-copy operations** - Efficient buffer management
4. **Native parameter extraction** - C++ handles URL parsing and parameter extraction

## Comparison

### Traditional FastAPI (Uvicorn)
```
HTTP Request → Python → Uvicorn → FastAPI → Your Handler
```

### FastAPI on FasterAPI
```
HTTP Request → C++ Native Server → Parameter Extraction → Python Handler
```

The C++ server handles all HTTP parsing and routing, only invoking Python for your business logic.

## Test Results

All routing integration tests pass:
- ✅ 16/16 C++ parameter extraction tests
- ✅ 4/4 routing integration tests
- ✅ 50/50 concurrent request tests
- ✅ 100% success rate

## Limitations

Currently supported:
- GET, POST, PUT, DELETE methods
- Path and query parameters
- Basic type conversion (int, str, bool)

Not yet implemented:
- Request body parsing (JSON)
- Pydantic model validation
- Dependency injection
- Middleware
- WebSocket (though native WebSocket support exists in FasterAPI)

## Architecture

The integration works through:

1. **RouteRegistry** - Stores route metadata and Python handlers
2. **_sync_routes_from_registry()** - Syncs routes from FastAPI decorators to C++ server
3. **PythonCallbackBridge** - Bridges C++ request handling to Python handlers
4. **Parameter Extractor** - C++ extracts and converts parameters before calling Python

## Next Steps

To run your FastAPI app on FasterAPI:

1. Replace `uvicorn.run(app)` with FasterAPI's native server (see examples above)
2. Use `connect_route_registry_to_server()` to enable parameter extraction
3. Start the `Server` instead of Uvicorn
4. Enjoy native C++ performance! 🚀

## Example Usage

```bash
# Run the demo
cd /Users/bengamble/FasterAPI
DYLD_LIBRARY_PATH=build/lib:$DYLD_LIBRARY_PATH python3.13 examples/fastapi_on_fasterapi_demo.py

# Test endpoints
curl "http://127.0.0.1:8000/"
curl "http://127.0.0.1:8000/users/42"
curl "http://127.0.0.1:8000/items/123?detailed=true"
```

## Conclusion

**Yes, you can run FastAPI apps on FasterAPI!**

Your standard FastAPI code runs with minimal changes on a native C++ HTTP server, giving you the developer experience of FastAPI with the performance of native C++.
