# FasterAPI C++ Examples

This directory contains comprehensive examples demonstrating the FasterAPI C++ User API.

## Building the Examples

```bash
cd /path/to/FasterAPI
mkdir build && cd build
cmake ..
make

# Examples will be built to build/examples/
```

## Available Examples

### 1. Hello World (`hello_world.cpp`)

The simplest possible FasterAPI application - perfect for getting started.

**Run:**
```bash
./build/examples/hello_world
```

**Test:**
```bash
curl http://localhost:8000/
# {"message":"Hello, World!","from":"FasterAPI C++"}
```

**Code:**
```cpp
auto app = App();

app.get("/", [](Request& req, Response& res) {
    res.json({
        {"message", "Hello, World!"},
        {"from", "FasterAPI C++"}
    });
});

return app.run("0.0.0.0", 8000);
```

---

### 2. Comprehensive Demo (`cpp_app_demo.cpp`)

A complete demonstration showcasing all major features of the FasterAPI C++ API.

**Run:**
```bash
./build/examples/cpp_app_demo
```

**Features Demonstrated:**

#### Basic Routes
- `GET /` - Hello World with timestamp
- `POST /echo` - Echo request body
- `GET /status` - Simple status check

#### Path Parameters
- `GET /users/{id}` - Single path parameter
- `GET /users/{user_id}/posts/{post_id}` - Multiple parameters
- `GET /files/*path` - Wildcard parameter

#### Query Parameters
- `GET /search?q=...&page=...&limit=...` - Search with pagination
- `GET /filter?category=...&min_price=...` - Optional filters

#### Middleware
- Global logging middleware
- Path-specific API middleware
- Custom headers middleware

#### Route Builder Pattern
- `POST /api/users` - With authentication & rate limiting
- `GET /api/profile` - Protected route
- `DELETE /api/users/{id}` - Admin-only route

#### Error Handling
- `GET /not-found` - 404 responses
- `GET /bad-request` - 400 responses
- `GET /error` - 500 responses
- `POST /validate` - Custom validation

#### Response Types
- `GET /api/data.json` - JSON response
- `GET /page` - HTML response
- `GET /robots.txt` - Plain text
- `GET /redirect` - Redirect
- `GET /custom-headers` - Custom headers
- `GET /set-cookie` - Cookie handling

#### WebSocket
- `/ws/echo` - Echo server with event handlers

#### Server-Sent Events (SSE)
- `/events/time` - Time updates every second
- `/events/counter` - Counter from 1-100

#### Static Files
- `/static/*` - Serve files from ./public directory

**Test Examples:**

```bash
# Basic routes
curl http://localhost:8000/
curl -X POST http://localhost:8000/echo -d "test data"

# Path parameters
curl http://localhost:8000/users/123
curl http://localhost:8000/users/42/posts/789
curl http://localhost:8000/files/docs/manual.pdf

# Query parameters
curl "http://localhost:8000/search?q=fasterapi&page=2&limit=20"
curl "http://localhost:8000/filter?category=books&min_price=10"

# Protected routes (requires auth header)
curl -H "Authorization: Bearer token123" \
     -X POST http://localhost:8000/api/users \
     -H "Content-Type: application/json" \
     -d '{"name":"John"}'

# Error handling
curl http://localhost:8000/not-found
curl "http://localhost:8000/validate?email=invalid"
curl "http://localhost:8000/validate?email=valid@example.com"

# Different response types
curl http://localhost:8000/api/data.json
curl http://localhost:8000/page
curl http://localhost:8000/robots.txt
curl -I http://localhost:8000/redirect

# SSE (streams events)
curl http://localhost:8000/events/time
curl http://localhost:8000/events/counter

# WebSocket (use wscat or similar)
wscat -c ws://localhost:8000/ws/echo
```

---

## Creating Your Own Application

### Step 1: Create a new C++ file

```cpp
// my_app.cpp
#include "src/cpp/http/app.h"
#include <iostream>

using namespace fasterapi;

int main() {
    // Configure application
    App::Config config;
    config.title = "My Application";
    config.version = "1.0.0";
    config.enable_compression = true;
    config.enable_cors = true;

    auto app = App(config);

    // Add routes
    app.get("/", [](Request& req, Response& res) {
        res.json({{"status", "running"}});
    });

    app.get("/users/{id}", [](Request& req, Response& res) {
        auto id = req.path_param("id");
        res.json({{"user_id", id}});
    });

    // Add middleware
    app.use([](Request& req, Response& res, auto next) {
        std::cout << req.method() << " " << req.path() << std::endl;
        next();
    });

    // Run server
    return app.run("0.0.0.0", 8000);
}
```

### Step 2: Add to CMakeLists.txt

Add to the examples section:

```cmake
add_executable(my_app examples/my_app.cpp)
target_include_directories(my_app PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(my_app PRIVATE fasterapi_http)
set_target_properties(my_app PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
)
```

### Step 3: Build and run

```bash
cd build
cmake ..
make my_app
./examples/my_app
```

---

## Common Patterns

### RESTful CRUD API

```cpp
// In-memory data store
std::map<int, User> users;
std::mutex users_mutex;
int next_id = 1;

// CREATE
app.post("/users", [](Request& req, Response& res) {
    // Parse JSON body, create user, return 201
});

// READ (list)
app.get("/users", [](Request& req, Response& res) {
    // Return all users
});

// READ (single)
app.get("/users/{id}", [](Request& req, Response& res) {
    // Return specific user or 404
});

// UPDATE
app.put("/users/{id}", [](Request& req, Response& res) {
    // Update user or return 404
});

// DELETE
app.del("/users/{id}", [](Request& req, Response& res) {
    // Delete user, return 204
});
```

### Authentication

```cpp
// Auth middleware
auto auth = [](Request& req, Response& res, auto next) {
    auto token = req.header("Authorization");
    if (!validate_token(token)) {
        res.unauthorized().json({{"error", "Invalid token"}});
        return;
    }
    next();
};

// Apply to specific routes
app.use("/api", auth);

// Or to individual routes
app.route("GET", "/protected")
   .require_auth()
   .handler([](Request& req, Response& res) {
       // Protected logic
   });
```

### Rate Limiting

```cpp
std::map<std::string, int> request_counts;
std::mutex counts_mutex;

auto rate_limiter = [](Request& req, Response& res, auto next) {
    auto ip = req.client_ip();

    std::lock_guard<std::mutex> lock(counts_mutex);
    if (++request_counts[ip] > 100) {
        res.status(429).json({{"error", "Too many requests"}});
        return;
    }

    next();
};

app.use(rate_limiter);
```

---

## Documentation

- **User API Guide**: `/docs/cpp_user_api.md` - Complete API documentation
- **Architecture**: `/docs/cpp_api_architecture.md` - How the layers work together
- **OpenAPI**: `http://localhost:8000/docs` - Interactive API documentation (when server is running)

---

## Performance Tips

1. **Use references**: Pass Request and Response by reference, not by value
2. **Avoid copies**: Use move semantics for large objects
3. **Enable compression**: Set `config.enable_compression = true`
4. **Thread safety**: Use mutexes for shared state
5. **Connection pooling**: Reuse database connections
6. **Static files**: Serve from memory or use sendfile()

---

## Troubleshooting

### Compilation Errors

Make sure you have:
- C++20 compatible compiler (GCC 10+, Clang 11+)
- CMake 3.20+
- All dependencies installed

### Runtime Errors

**Port already in use:**
```bash
# Change port or kill existing process
sudo lsof -i :8000
./my_app  # Will show error if port is busy
```

**Segmentation fault:**
- Check that you're not accessing null pointers
- Ensure Request/Response objects are valid
- Use address sanitizer: `cmake -DCMAKE_CXX_FLAGS="-fsanitize=address"`

---

## Next Steps

1. Read the [full API documentation](/docs/cpp_user_api.md)
2. Explore the [architecture guide](/docs/cpp_api_architecture.md)
3. Build your first app using `hello_world.cpp` as a template
4. Add more advanced features from `cpp_app_demo.cpp`

**Happy coding with FasterAPI C++!** 🚀
