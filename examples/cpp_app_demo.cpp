/**
 * @file cpp_app_demo.cpp
 * @brief Comprehensive demonstration of FasterAPI C++ User API
 *
 * This example showcases all the major features of the high-level C++ API:
 * - Route registration with different HTTP methods
 * - Path parameters and query parameters
 * - Middleware (global and route-specific)
 * - JSON responses
 * - Error handling
 * - Static file serving
 * - WebSocket support
 * - Server-Sent Events
 * - OpenAPI documentation
 * - Route builder pattern
 *
 * Compile and run:
 * @code
 * mkdir build && cd build
 * cmake ..
 * make cpp_app_demo
 * ./cpp_app_demo
 * @endcode
 *
 * Then visit:
 * - http://localhost:8000/          - Hello World
 * - http://localhost:8000/docs      - Interactive API documentation
 * - http://localhost:8000/users/123 - Path parameter example
 * - http://localhost:8000/health    - Health check
 */

#include "../src/cpp/http/app.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <random>

using namespace fasterapi;

// =============================================================================
// Example 1: Basic Routes
// =============================================================================

void setup_basic_routes(App& app) {
    // Simple GET route
    app.get("/", [](Request& req, Response& res) {
        res.json({
            {"message", "Hello from FasterAPI C++!"},
            {"version", "1.0.0"},
            {"timestamp", std::to_string(std::time(nullptr))}
        });
    });

    // POST route with body parsing
    app.post("/echo", [](Request& req, Response& res) {
        auto body = req.body();
        res.json({
            {"echoed", body},
            {"length", std::to_string(body.length())}
        });
    });

    // Multiple methods for same path
    app.get("/status", [](Request& req, Response& res) {
        res.json({{"status", "GET OK"}});
    });

    app.post("/status", [](Request& req, Response& res) {
        res.created().json({{"status", "POST OK"}});
    });
}

// =============================================================================
// Example 2: Path Parameters
// =============================================================================

void setup_path_parameters(App& app) {
    // Single path parameter
    app.get("/users/{id}", [](Request& req, Response& res) {
        auto user_id = req.path_param("id");

        res.json({
            {"user_id", user_id},
            {"name", "User " + user_id},
            {"email", "user" + user_id + "@example.com"}
        });
    });

    // Multiple path parameters
    app.get("/users/{user_id}/posts/{post_id}", [](Request& req, Response& res) {
        auto user_id = req.path_param("user_id");
        auto post_id = req.path_param("post_id");

        res.json({
            {"user_id", user_id},
            {"post_id", post_id},
            {"title", "Post " + post_id + " by User " + user_id}
        });
    });

    // Wildcard parameter
    app.get("/files/*path", [](Request& req, Response& res) {
        auto file_path = req.path_param("path");

        res.json({
            {"requested_file", file_path},
            {"exists", "false"},  // Placeholder
            {"size", "0"}
        });
    });
}

// =============================================================================
// Example 3: Query Parameters
// =============================================================================

void setup_query_parameters(App& app) {
    // Query parameters with defaults
    app.get("/search", [](Request& req, Response& res) {
        auto query = req.query_param("q");
        auto page_str = req.query_param("page");
        auto limit_str = req.query_param("limit");

        int page = page_str.empty() ? 1 : std::stoi(page_str);
        int limit = limit_str.empty() ? 10 : std::stoi(limit_str);

        res.json({
            {"query", query},
            {"page", std::to_string(page)},
            {"limit", std::to_string(limit)},
            {"results", "[]"}  // Placeholder
        });
    });

    // Optional query parameters
    app.get("/filter", [](Request& req, Response& res) {
        auto category = req.query_param_optional("category");
        auto min_price = req.query_param_optional("min_price");
        auto max_price = req.query_param_optional("max_price");

        std::vector<std::pair<std::string, std::string>> response;
        response.push_back({"filters_applied", "true"});

        if (category.has_value()) {
            response.push_back({"category", category.value()});
        }

        if (min_price.has_value()) {
            response.push_back({"min_price", min_price.value()});
        }

        if (max_price.has_value()) {
            response.push_back({"max_price", max_price.value()});
        }

        res.json(response);
    });
}

// =============================================================================
// Example 4: Middleware
// =============================================================================

void setup_middleware(App& app) {
    // Global middleware - logs all requests
    app.use([](Request& req, Response& res, auto next) {
        auto start = std::chrono::steady_clock::now();

        std::cout << "[" << req.method() << "] " << req.path() << std::endl;

        // Call next middleware or handler
        next();

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "  -> Completed in " << duration.count() << "ms" << std::endl;
    });

    // Path-specific middleware - only for /api/* routes
    app.use("/api", [](Request& req, Response& res, auto next) {
        std::cout << "  -> API middleware executed" << std::endl;
        next();
    });

    // Add CORS headers middleware
    app.use([](Request& req, Response& res, auto next) {
        res.header("X-Powered-By", "FasterAPI/1.0");
        next();
    });
}

// =============================================================================
// Example 5: Route Builder Pattern
// =============================================================================

void setup_route_builder(App& app) {
    // Advanced route with metadata and middleware
    app.route("POST", "/api/users")
        .tag("Users")
        .summary("Create a new user")
        .description("Creates a new user account with the provided information")
        .require_auth()
        .rate_limit(100)  // 100 requests per minute
        .handler([](Request& req, Response& res) {
            auto body = req.json_body();

            // Simulate user creation
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(1000, 9999);
            int user_id = dis(gen);

            res.created().json({
                {"id", std::to_string(user_id)},
                {"status", "created"},
                {"message", "User created successfully"}
            });
        });

    // Protected route requiring authentication
    app.route("GET", "/api/profile")
        .tag("Users")
        .summary("Get user profile")
        .require_auth()
        .handler([](Request& req, Response& res) {
            res.json({
                {"user_id", "123"},
                {"username", "john_doe"},
                {"email", "john@example.com"},
                {"created_at", "2024-01-01T00:00:00Z"}
            });
        });

    // Admin-only route
    app.route("DELETE", "/api/users/{id}")
        .tag("Admin")
        .summary("Delete a user")
        .require_auth()
        .require_role("admin")
        .handler([](Request& req, Response& res) {
            auto user_id = req.path_param("id");

            res.json({
                {"deleted_user_id", user_id},
                {"status", "deleted"}
            });
        });
}

// =============================================================================
// Example 6: Error Handling
// =============================================================================

void setup_error_handling(App& app) {
    // 404 Not Found
    app.get("/not-found", [](Request& req, Response& res) {
        res.not_found().json({
            {"error", "Resource not found"},
            {"code", "NOT_FOUND"}
        });
    });

    // 400 Bad Request
    app.get("/bad-request", [](Request& req, Response& res) {
        res.bad_request().json({
            {"error", "Invalid request parameters"},
            {"code", "BAD_REQUEST"}
        });
    });

    // 500 Internal Server Error
    app.get("/error", [](Request& req, Response& res) {
        res.internal_error().json({
            {"error", "Internal server error"},
            {"code", "INTERNAL_ERROR"}
        });
    });

    // Custom validation example
    app.post("/validate", [](Request& req, Response& res) {
        auto email = req.query_param("email");

        if (email.empty()) {
            res.bad_request().json({
                {"error", "Email is required"},
                {"field", "email"}
            });
            return;
        }

        if (email.find('@') == std::string::npos) {
            res.bad_request().json({
                {"error", "Invalid email format"},
                {"field", "email"}
            });
            return;
        }

        res.json({{"message", "Email is valid"}});
    });
}

// =============================================================================
// Example 7: Different Response Types
// =============================================================================

void setup_response_types(App& app) {
    // JSON response
    app.get("/api/data.json", [](Request& req, Response& res) {
        res.json({
            {"type", "json"},
            {"data", "[1,2,3,4,5]"}
        });
    });

    // HTML response
    app.get("/page", [](Request& req, Response& res) {
        res.html(R"(
<!DOCTYPE html>
<html>
<head><title>FasterAPI C++</title></head>
<body>
    <h1>Hello from FasterAPI!</h1>
    <p>This is an HTML response.</p>
</body>
</html>
        )");
    });

    // Plain text response
    app.get("/robots.txt", [](Request& req, Response& res) {
        res.text("User-agent: *\nDisallow: /admin/\n");
    });

    // Redirect
    app.get("/redirect", [](Request& req, Response& res) {
        res.redirect("/");
    });

    // Custom headers
    app.get("/custom-headers", [](Request& req, Response& res) {
        res.header("X-Custom-Header", "CustomValue")
           .header("X-Request-ID", "12345")
           .json({{"message", "Check the headers!"}});
    });

    // Cookies
    app.get("/set-cookie", [](Request& req, Response& res) {
        res.cookie("session_id", "abc123", 3600, "/", true, false, "Strict")
           .json({{"message", "Cookie set"}});
    });
}

// =============================================================================
// Example 8: WebSocket (Placeholder)
// =============================================================================

void setup_websocket(App& app) {
    app.websocket("/ws/echo", [](http::WebSocketConnection& ws) {
        std::cout << "WebSocket connection established: " << ws.get_id() << std::endl;

        // Set up event handlers
        ws.on_text_message = [&ws](const std::string& message) {
            std::cout << "Received: " << message << std::endl;
            ws.send_text("Echo: " + message);
        };

        ws.on_close = [](uint16_t code, const char* reason) {
            std::cout << "WebSocket closed: " << code << " - " << reason << std::endl;
        };

        ws.on_error = [](const char* error) {
            std::cerr << "WebSocket error: " << error << std::endl;
        };
    });
}

// =============================================================================
// Example 9: Server-Sent Events
// =============================================================================

void setup_sse(App& app) {
    app.sse("/events/time", [](http::SSEConnection& sse) {
        std::cout << "SSE connection established: " << sse.get_id() << std::endl;

        // Send time updates every second for 10 seconds
        for (int i = 0; i < 10; i++) {
            if (!sse.is_open()) break;

            auto now = std::time(nullptr);
            sse.send(
                std::to_string(now),  // data
                "time-update",         // event type
                std::to_string(i),     // event ID
                -1                     // retry (default)
            );

            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        sse.close();
    });

    app.sse("/events/counter", [](http::SSEConnection& sse) {
        for (int i = 1; i <= 100; i++) {
            if (!sse.is_open()) break;

            sse.send(
                "{\"count\":" + std::to_string(i) + "}",
                "counter",
                std::to_string(i)
            );

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

// =============================================================================
// Example 10: Static Files
// =============================================================================

void setup_static_files(App& app) {
    // Serve static files from ./public directory
    app.static_files("/static", "./public");

    // Alternative: serve from absolute path
    // app.static_files("/assets", "/var/www/assets");
}

// =============================================================================
// Main Application
// =============================================================================

int main() {
    std::cout << "=================================================\n";
    std::cout << "  FasterAPI C++ - High-Level API Demo\n";
    std::cout << "=================================================\n\n";

    // Create application with configuration
    App::Config config;
    config.title = "FasterAPI C++ Demo";
    config.version = "1.0.0";
    config.description = "Comprehensive demonstration of FasterAPI C++ API";
    config.enable_http2 = false;
    config.enable_http3 = false;
    config.enable_compression = true;
    config.enable_cors = true;
    config.cors_origin = "*";
    config.enable_docs = true;

    auto app = App(config);

    // Setup all examples
    std::cout << "Setting up routes...\n";

    setup_basic_routes(app);
    setup_path_parameters(app);
    setup_query_parameters(app);
    setup_middleware(app);
    setup_route_builder(app);
    setup_error_handling(app);
    setup_response_types(app);
    setup_websocket(app);
    setup_sse(app);
    setup_static_files(app);

    // Print registered routes
    std::cout << "\nRegistered routes:\n";
    for (const auto& [method, path] : app.routes()) {
        std::cout << "  " << method << " " << path << "\n";
    }

    std::cout << "\n=================================================\n";
    std::cout << "Starting server...\n";
    std::cout << "=================================================\n\n";

    // Run the server (blocks until stopped)
    int result = app.run("0.0.0.0", 8000);

    if (result != 0) {
        std::cerr << "Failed to start server: " << result << std::endl;
        return 1;
    }

    return 0;
}
