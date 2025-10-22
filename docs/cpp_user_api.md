## FasterAPI C++ User API Documentation

### Overview

The FasterAPI C++ User API provides a clean, modern, FastAPI-style interface for building high-performance HTTP applications directly in C++. It combines the expressiveness of Python's FastAPI with the raw performance of C++.

### Table of Contents

1. [Quick Start](#quick-start)
2. [Application Setup](#application-setup)
3. [Route Registration](#route-registration)
4. [Request & Response](#request--response)
5. [Path Parameters](#path-parameters)
6. [Query Parameters](#query-parameters)
7. [Middleware](#middleware)
8. [Route Builder Pattern](#route-builder-pattern)
9. [WebSocket](#websocket)
10. [Server-Sent Events (SSE)](#server-sent-events-sse)
11. [Static Files](#static-files)
12. [Error Handling](#error-handling)
13. [Complete Examples](#complete-examples)

---

## Quick Start

### Minimal Example

```cpp
#include "fasterapi/http/app.h"

using namespace fasterapi;

int main() {
    auto app = App();

    app.get("/", [](Request& req, Response& res) {
        res.json({{"message", "Hello, World!"}});
    });

    return app.run("0.0.0.0", 8000);
}
```

**Compile:**
```bash
g++ -std=c++20 -o myapp main.cpp -lfasterapi_http
./myapp
```

**Test:**
```bash
curl http://localhost:8000/
# {"message":"Hello, World!"}
```

---

## Application Setup

### Basic Configuration

```cpp
#include "fasterapi/http/app.h"

using namespace fasterapi;

int main() {
    // Default configuration
    auto app = App();

    // Custom configuration
    App::Config config;
    config.title = "My API";
    config.version = "1.0.0";
    config.description = "A high-performance API";
    config.enable_http2 = true;
    config.enable_compression = true;
    config.enable_cors = true;
    config.cors_origin = "*";
    config.enable_docs = true;  // Swagger UI at /docs

    auto app2 = App(config);

    // ... register routes ...

    return app.run("0.0.0.0", 8000);
}
```

### Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `title` | string | "FasterAPI Application" | Application title |
| `version` | string | "1.0.0" | API version |
| `description` | string | "" | API description |
| `enable_http2` | bool | false | Enable HTTP/2 support |
| `enable_http3` | bool | false | Enable HTTP/3 support |
| `enable_compression` | bool | true | Enable zstd compression |
| `enable_cors` | bool | false | Enable CORS middleware |
| `cors_origin` | string | "*" | CORS allowed origin |
| `cert_path` | string | "" | TLS certificate path |
| `key_path` | string | "" | TLS private key path |
| `max_connections` | uint32_t | 10000 | Maximum concurrent connections |
| `max_request_size` | uint32_t | 16MB | Maximum request body size |
| `enable_docs` | bool | true | Enable /docs and /openapi.json |
| `docs_url` | string | "/docs" | Documentation URL |
| `openapi_url` | string | "/openapi.json" | OpenAPI spec URL |

---

## Route Registration

### HTTP Methods

```cpp
app.get("/path", handler);      // GET request
app.post("/path", handler);     // POST request
app.put("/path", handler);      // PUT request
app.del("/path", handler);      // DELETE request
app.patch("/path", handler);    // PATCH request
app.head("/path", handler);     // HEAD request
app.options("/path", handler);  // OPTIONS request
```

### Handler Function

Handlers receive `Request` and `Response` by reference:

```cpp
app.get("/example", [](Request& req, Response& res) {
    // Access request
    auto method = req.method();
    auto path = req.path();

    // Send response
    res.json({{"status", "ok"}});
});
```

### Method Chaining

Return `App&` allows method chaining:

```cpp
app.get("/", handler1)
   .get("/health", handler2)
   .post("/data", handler3);
```

---

## Request & Response

### Request Object

```cpp
app.get("/info", [](Request& req, Response& res) {
    // HTTP method
    std::string method = req.method();  // "GET", "POST", etc.

    // Path and query
    std::string path = req.path();
    std::string query = req.query_string();

    // Headers
    std::string auth = req.header("Authorization");
    std::string ua = req.user_agent();
    auto all_headers = req.headers();  // map<string, string>

    // Body
    std::string body = req.body();
    std::string json = req.json_body();

    // Client info
    std::string ip = req.client_ip();

    res.json({{"method", method}, {"path", path}});
});
```

### Response Object

#### Status Codes

```cpp
res.status(200);          // Set custom status
res.ok();                 // 200 OK
res.created();            // 201 Created
res.no_content();         // 204 No Content
res.bad_request();        // 400 Bad Request
res.unauthorized();       // 401 Unauthorized
res.forbidden();          // 403 Forbidden
res.not_found();          // 404 Not Found
res.internal_error();     // 500 Internal Server Error
```

#### Headers

```cpp
res.header("X-Custom", "value")
   .header("Cache-Control", "no-cache")
   .content_type("application/json");
```

#### Response Types

```cpp
// JSON response
res.json({
    {"name", "John"},
    {"age", "30"}
});

// JSON from vector of pairs
res.json({
    {"key1", "value1"},
    {"key2", "value2"}
});

// HTML response
res.html("<h1>Hello</h1>");

// Plain text response
res.text("Hello World");

// File download
res.file("/path/to/file.pdf");

// Raw body
res.send("raw body content");

// Redirect
res.redirect("/new-path");
res.redirect("/other", 301);  // Permanent redirect
```

#### CORS

```cpp
res.cors();              // Default "*"
res.cors("https://example.com");
```

#### Cookies

```cpp
res.cookie("session_id", "abc123", 3600);  // Max age in seconds

res.cookie(
    "token",           // name
    "xyz789",          // value
    3600,              // max_age (seconds)
    "/",               // path
    true,              // http_only
    true,              // secure
    "Strict"           // same_site
);
```

---

## Path Parameters

Extract dynamic segments from the URL path.

### Syntax

Use `{param_name}` in the path:

```cpp
app.get("/users/{id}", [](Request& req, Response& res) {
    auto user_id = req.path_param("id");
    res.json({{"user_id", user_id}});
});
```

**Example:**
- Request: `GET /users/123`
- `user_id` = `"123"`

### Multiple Parameters

```cpp
app.get("/users/{user_id}/posts/{post_id}", [](Request& req, Response& res) {
    auto user_id = req.path_param("user_id");
    auto post_id = req.path_param("post_id");

    res.json({
        {"user", user_id},
        {"post", post_id}
    });
});
```

**Example:**
- Request: `GET /users/42/posts/789`
- `user_id` = `"42"`, `post_id` = `"789"`

### Wildcard Parameters

Use `*param` to capture the rest of the path:

```cpp
app.get("/files/*path", [](Request& req, Response& res) {
    auto file_path = req.path_param("path");
    res.json({{"file", file_path}});
});
```

**Example:**
- Request: `GET /files/docs/manual.pdf`
- `path` = `"docs/manual.pdf"`

### Optional Parameters

```cpp
auto id_opt = req.path_param_optional("id");
if (id_opt.has_value()) {
    std::string id = id_opt.value();
    // Use id
} else {
    // Parameter not found
}
```

---

## Query Parameters

Extract parameters from the query string.

### Basic Usage

```cpp
app.get("/search", [](Request& req, Response& res) {
    auto query = req.query_param("q");
    auto page = req.query_param("page");

    res.json({
        {"query", query},
        {"page", page.empty() ? "1" : page}
    });
});
```

**Example:**
- Request: `GET /search?q=fasterapi&page=2`
- `query` = `"fasterapi"`, `page` = `"2"`

### Type Conversion

```cpp
app.get("/items", [](Request& req, Response& res) {
    auto limit_str = req.query_param("limit");
    int limit = limit_str.empty() ? 10 : std::stoi(limit_str);

    auto offset_str = req.query_param("offset");
    int offset = offset_str.empty() ? 0 : std::stoi(offset_str);

    res.json({
        {"limit", std::to_string(limit)},
        {"offset", std::to_string(offset)}
    });
});
```

### Optional Query Parameters

```cpp
auto category = req.query_param_optional("category");
if (category.has_value()) {
    // Filter by category
} else {
    // No category filter
}
```

---

## Middleware

Middleware functions execute before route handlers.

### Global Middleware

Runs for all routes:

```cpp
app.use([](Request& req, Response& res, auto next) {
    std::cout << req.method() << " " << req.path() << std::endl;

    // Continue to next middleware or handler
    next();
});
```

### Path-Specific Middleware

Runs for routes matching a prefix:

```cpp
app.use("/api", [](Request& req, Response& res, auto next) {
    // Only runs for /api/* routes
    std::cout << "API request" << std::endl;
    next();
});
```

### Multiple Middleware

Middleware executes in order:

```cpp
// 1. Logging
app.use([](Request& req, Response& res, auto next) {
    std::cout << "Request started" << std::endl;
    next();
});

// 2. Authentication
app.use([](Request& req, Response& res, auto next) {
    auto auth = req.header("Authorization");
    if (auth.empty()) {
        res.unauthorized().json({{"error", "Auth required"}});
        return;  // Don't call next()
    }
    next();
});

// 3. CORS
app.use([](Request& req, Response& res, auto next) {
    res.cors();
    next();
});
```

### Timing Middleware

```cpp
app.use([](Request& req, Response& res, auto next) {
    auto start = std::chrono::steady_clock::now();

    next();

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::cout << "Request took " << duration.count() << "ms" << std::endl;
});
```

---

## Route Builder Pattern

For advanced route configuration:

```cpp
app.route("POST", "/api/users")
   .tag("Users")
   .summary("Create a user")
   .description("Creates a new user account")
   .require_auth()
   .rate_limit(100)  // 100 requests/min
   .handler([](Request& req, Response& res) {
       // Handler logic
       res.created().json({{"user_id", "123"}});
   });
```

### Builder Methods

| Method | Description |
|--------|-------------|
| `.tag(string)` | Add tag for OpenAPI grouping |
| `.summary(string)` | Short description |
| `.description(string)` | Long description |
| `.require_auth()` | Add authentication middleware |
| `.require_role(string)` | Add role-based authorization |
| `.rate_limit(int)` | Limit requests per minute |
| `.use(middleware)` | Add route-specific middleware |
| `.response_model(code, schema)` | Define response schema |
| `.handler(func)` | Set the route handler |

### Example

```cpp
app.route("GET", "/admin/stats")
   .tag("Admin")
   .summary("Get server statistics")
   .require_auth()
   .require_role("admin")
   .handler([](Request& req, Response& res) {
       auto stats = req.raw()->stats();  // Hypothetical
       res.json({{"requests", std::to_string(stats.total_requests)}});
   });
```

---

## WebSocket

Real-time bidirectional communication.

### Basic WebSocket

```cpp
app.websocket("/ws/echo", [](http::WebSocketConnection& ws) {
    std::cout << "WebSocket opened: " << ws.get_id() << std::endl;

    ws.on_text_message = [&ws](const std::string& message) {
        std::cout << "Received: " << message << std::endl;
        ws.send_text("Echo: " + message);
    };

    ws.on_close = [](uint16_t code, const char* reason) {
        std::cout << "WebSocket closed" << std::endl;
    };

    ws.on_error = [](const char* error) {
        std::cerr << "Error: " << error << std::endl;
    };
});
```

### WebSocket Chat Room

```cpp
std::vector<http::WebSocketConnection*> connections;
std::mutex connections_mutex;

app.websocket("/ws/chat", [&](http::WebSocketConnection& ws) {
    // Add connection
    {
        std::lock_guard<std::mutex> lock(connections_mutex);
        connections.push_back(&ws);
    }

    // Handle messages
    ws.on_text_message = [&](const std::string& message) {
        // Broadcast to all connections
        std::lock_guard<std::mutex> lock(connections_mutex);
        for (auto* conn : connections) {
            if (conn->is_open()) {
                conn->send_text(message);
            }
        }
    };

    // Remove on close
    ws.on_close = [&](uint16_t code, const char* reason) {
        std::lock_guard<std::mutex> lock(connections_mutex);
        auto it = std::find(connections.begin(), connections.end(), &ws);
        if (it != connections.end()) {
            connections.erase(it);
        }
    };
});
```

---

## Server-Sent Events (SSE)

One-way server-to-client push notifications.

### Basic SSE

```cpp
app.sse("/events/time", [](http::SSEConnection& sse) {
    for (int i = 0; i < 10; i++) {
        if (!sse.is_open()) break;

        auto now = std::time(nullptr);
        sse.send(
            std::to_string(now),  // data
            "time-update",         // event type
            std::to_string(i)      // event ID
        );

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    sse.close();
});
```

### SSE with JSON

```cpp
app.sse("/events/metrics", [](http::SSEConnection& sse) {
    int counter = 0;
    while (sse.is_open() && counter < 100) {
        std::string json_data = "{\"counter\":" + std::to_string(counter) +
                               ",\"timestamp\":" + std::to_string(std::time(nullptr)) + "}";

        sse.send(json_data, "metrics", std::to_string(counter));

        counter++;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
});
```

### Keep-Alive

```cpp
app.sse("/events/stream", [](http::SSEConnection& sse) {
    while (sse.is_open()) {
        // Send actual event
        sse.send("Event data", "my-event");

        // Keep-alive ping
        sse.ping();

        std::this_thread::sleep_for(std::chrono::seconds(30));
    }
});
```

---

## Static Files

Serve static files from a directory.

### Basic Static Files

```cpp
// Serve files from ./public at /static
app.static_files("/static", "./public");
```

**Example:**
- File: `./public/style.css`
- URL: `http://localhost:8000/static/style.css`

### Multiple Directories

```cpp
app.static_files("/css", "./assets/css");
app.static_files("/js", "./assets/js");
app.static_files("/images", "./assets/images");
```

---

## Error Handling

### Custom Error Responses

```cpp
app.get("/user/{id}", [](Request& req, Response& res) {
    auto id = req.path_param("id");

    if (id.empty()) {
        res.bad_request().json({
            {"error", "Missing user ID"},
            {"code", "MISSING_PARAM"}
        });
        return;
    }

    // Lookup user...
    bool user_exists = false; // Hypothetical

    if (!user_exists) {
        res.not_found().json({
            {"error", "User not found"},
            {"user_id", id}
        });
        return;
    }

    // Success
    res.json({{"user_id", id}, {"name", "John"}});
});
```

### Validation Middleware

```cpp
auto validate_json = [](Request& req, Response& res, auto next) {
    auto content_type = req.header("Content-Type");

    if (req.method() == "POST" || req.method() == "PUT") {
        if (content_type != "application/json") {
            res.bad_request().json({
                {"error", "Content-Type must be application/json"}
            });
            return;
        }
    }

    next();
};

app.use("/api", validate_json);
```

---

## Complete Examples

### REST API

```cpp
#include "fasterapi/http/app.h"
#include <map>
#include <mutex>

using namespace fasterapi;

// Simple in-memory user store
std::map<int, std::string> users;
std::mutex users_mutex;
int next_id = 1;

int main() {
    auto app = App();

    // Create user
    app.post("/users", [](Request& req, Response& res) {
        auto name = req.json_body();  // Simplified

        std::lock_guard<std::mutex> lock(users_mutex);
        int id = next_id++;
        users[id] = name;

        res.created().json({
            {"id", std::to_string(id)},
            {"name", name}
        });
    });

    // Get user
    app.get("/users/{id}", [](Request& req, Response& res) {
        auto id_str = req.path_param("id");
        int id = std::stoi(id_str);

        std::lock_guard<std::mutex> lock(users_mutex);
        auto it = users.find(id);

        if (it == users.end()) {
            res.not_found().json({{"error", "User not found"}});
        } else {
            res.json({
                {"id", std::to_string(id)},
                {"name", it->second}
            });
        }
    });

    // Update user
    app.put("/users/{id}", [](Request& req, Response& res) {
        auto id_str = req.path_param("id");
        int id = std::stoi(id_str);
        auto new_name = req.json_body();

        std::lock_guard<std::mutex> lock(users_mutex);
        if (users.find(id) == users.end()) {
            res.not_found().json({{"error", "User not found"}});
        } else {
            users[id] = new_name;
            res.json({{"id", std::to_string(id)}, {"name", new_name}});
        }
    });

    // Delete user
    app.del("/users/{id}", [](Request& req, Response& res) {
        auto id_str = req.path_param("id");
        int id = std::stoi(id_str);

        std::lock_guard<std::mutex> lock(users_mutex);
        users.erase(id);
        res.no_content();
    });

    return app.run("0.0.0.0", 8000);
}
```

### Authentication Example

```cpp
#include "fasterapi/http/app.h"
#include <set>

using namespace fasterapi;

std::set<std::string> valid_tokens = {"secret123", "token456"};

int main() {
    auto app = App();

    // Authentication middleware
    auto auth_middleware = [](Request& req, Response& res, auto next) {
        auto auth_header = req.header("Authorization");

        if (auth_header.empty()) {
            res.unauthorized().json({
                {"error", "Authorization header required"}
            });
            return;
        }

        // Extract token (Bearer <token>)
        auto token = auth_header.substr(7);  // Skip "Bearer "

        if (valid_tokens.find(token) == valid_tokens.end()) {
            res.unauthorized().json({
                {"error", "Invalid token"}
            });
            return;
        }

        next();
    };

    // Public route
    app.get("/public", [](Request& req, Response& res) {
        res.json({{"message", "Public data"}});
    });

    // Protected routes
    app.use("/api", auth_middleware);

    app.get("/api/protected", [](Request& req, Response& res) {
        res.json({{"message", "Protected data"}});
    });

    return app.run("0.0.0.0", 8000);
}
```

---

## API Reference Summary

### App Class

```cpp
App(const Config& config = Config{});
App& get(const string& path, Handler handler);
App& post(const string& path, Handler handler);
App& put(const string& path, Handler handler);
App& del(const string& path, Handler handler);
App& patch(const string& path, Handler handler);
App& head(const string& path, Handler handler);
App& options(const string& path, Handler handler);
RouteBuilder route(const string& method, const string& path);
App& websocket(const string& path, WSHandler handler);
App& sse(const string& path, SSEHandler handler);
App& use(MiddlewareFunc middleware);
App& use(const string& path, MiddlewareFunc middleware);
App& static_files(const string& url_path, const string& directory);
App& mount(const string& path, App& sub_app);
int run(const string& host = "0.0.0.0", uint16_t port = 8000);
int start(const string& host = "0.0.0.0", uint16_t port = 8000);
int stop();
bool is_running() const;
```

### Request Class

```cpp
string method() const;
string path() const;
string query_string() const;
string header(const string& name) const;
map<string, string> headers() const;
string path_param(const string& name) const;
string query_param(const string& name) const;
optional<string> path_param_optional(const string& name) const;
optional<string> query_param_optional(const string& name) const;
string body() const;
string json_body() const;
string client_ip() const;
string user_agent() const;
HttpRequest* raw() const;
```

### Response Class

```cpp
Response& status(int code);
Response& ok();
Response& created();
Response& no_content();
Response& bad_request();
Response& unauthorized();
Response& forbidden();
Response& not_found();
Response& internal_error();
Response& header(const string& name, const string& value);
Response& content_type(const string& type);
Response& send(const string& body);
Response& json(const string& json_str);
Response& json(const vector<pair<string, string>>& pairs);
Response& html(const string& html_str);
Response& text(const string& text_str);
Response& file(const string& path);
Response& cors(const string& origin = "*");
Response& cookie(...);
Response& redirect(const string& url, int code = 302);
HttpResponse* raw() const;
```

---

## Next Steps

- Explore the [complete examples](/examples/)
- Check out the [architecture documentation](/docs/cpp_api_architecture.md)
- View the [API reference](/docs/api_reference.md)
- See [performance benchmarks](/benchmarks/)

---

**FasterAPI C++ - High-performance web framework with FastAPI-style ergonomics**
