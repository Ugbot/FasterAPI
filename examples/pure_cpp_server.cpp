/**
 * Pure C++ FasterAPI Server Example
 *
 * This example demonstrates how to build a complete HTTP server using only C++,
 * without any Python/ZMQ bridges. Perfect for maximum performance applications.
 *
 * Features demonstrated:
 * - HTTP routes (GET, POST, PUT, DELETE)
 * - Path parameters
 * - Query parameters
 * - Request body parsing
 * - WebSocket endpoints
 * - Health checks
 *
 * Build:
 *   cmake --build build --target pure_cpp_server
 *
 * Run:
 *   DYLD_LIBRARY_PATH=build/lib ./build/examples/pure_cpp_server
 *
 * Test:
 *   curl http://127.0.0.1:8080/health
 *   curl http://127.0.0.1:8080/api/users
 *   curl -X POST http://127.0.0.1:8080/api/users -d '{"name":"Alice"}'
 *   wscat -c ws://127.0.0.1:8080/ws/echo
 */

#include "../src/cpp/http/app.h"
#include "../src/cpp/core/logger.h"

#include <iostream>
#include <csignal>
#include <atomic>

static std::atomic<bool> g_running{true};

void signal_handler(int) {
    std::cout << "\nShutting down..." << std::endl;
    g_running = false;
}

int main() {
    // Set up signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Configure logging (optional)
    fasterapi::core::Logger::instance().set_level(fasterapi::core::LogLevel::INFO);

    std::cout << "=== Pure C++ FasterAPI Server ===" << std::endl;
    std::cout << "No Python, no ZMQ, just pure C++ performance." << std::endl;
    std::cout << std::endl;

    // Create App with pure C++ mode enabled
    fasterapi::App::Config config;
    config.pure_cpp_mode = true;  // Disables all Python/ZMQ bridges

    fasterapi::App app(config);

    // ========================================
    // HTTP Routes
    // ========================================

    // Health check endpoint
    app.get("/health", [](fasterapi::Request& req, fasterapi::Response& res) {
        res.json(R"({"status":"ok","mode":"pure_cpp"})");
    });

    // Simple GET endpoint - home page
    app.get("/", [](fasterapi::Request& req, fasterapi::Response& res) {
        res.html(R"(
<!DOCTYPE html>
<html>
<head><title>FasterAPI Pure C++</title></head>
<body>
    <h1>Welcome to FasterAPI Pure C++ Server</h1>
    <p>This server runs entirely in C++ with no Python overhead.</p>
    <h2>Endpoints:</h2>
    <ul>
        <li>GET /health - Health check</li>
        <li>GET /api/users - List users</li>
        <li>GET /api/users/:id - Get user by ID</li>
        <li>POST /api/users - Create user</li>
        <li>PUT /api/users/:id - Update user</li>
        <li>DELETE /api/users/:id - Delete user</li>
        <li>WebSocket /ws/echo - Echo WebSocket</li>
    </ul>
</body>
</html>
)");
    });

    // GET with path parameter
    app.get("/api/users/{id}", [](fasterapi::Request& req, fasterapi::Response& res) {
        std::string user_id = req.path_param("id");
        if (user_id.empty()) {
            user_id = "unknown";
        }
        res.json("{\"id\":\"" + user_id + "\",\"name\":\"User " + user_id + "\"}");
    });

    // GET list with query params
    app.get("/api/users", [](fasterapi::Request& req, fasterapi::Response& res) {
        // Check for query params (limit, offset)
        auto limit_opt = req.query_param_optional("limit");
        auto offset_opt = req.query_param_optional("offset");

        std::string limit = limit_opt.value_or("10");
        std::string offset = offset_opt.value_or("0");

        res.json("{\"users\":[{\"id\":1,\"name\":\"Alice\"},{\"id\":2,\"name\":\"Bob\"}],\"limit\":"
                 + limit + ",\"offset\":" + offset + "}");
    });

    // POST with body
    app.post("/api/users", [](fasterapi::Request& req, fasterapi::Response& res) {
        std::string body = req.body();
        if (body.empty()) {
            body = "{}";
        }

        // Return created response
        res.status(201).json("{\"id\":123,\"created\":true,\"data\":" + body + "}");
    });

    // PUT with path param and body
    app.put("/api/users/{id}", [](fasterapi::Request& req, fasterapi::Response& res) {
        std::string user_id = req.path_param("id");
        if (user_id.empty()) {
            user_id = "unknown";
        }
        std::string body = req.body();
        if (body.empty()) {
            body = "{}";
        }

        res.json("{\"id\":\"" + user_id + "\",\"updated\":true,\"data\":" + body + "}");
    });

    // DELETE
    app.del("/api/users/{id}", [](fasterapi::Request& req, fasterapi::Response& res) {
        std::string user_id = req.path_param("id");
        if (user_id.empty()) {
            user_id = "unknown";
        }

        res.json("{\"id\":\"" + user_id + "\",\"deleted\":true}");
    });

    // ========================================
    // WebSocket Endpoints
    // ========================================

    // Echo WebSocket - all messages are echoed back
    app.websocket("/ws/echo", [](fasterapi::http::WebSocketConnection& ws) {
        std::cout << "[WS] New connection on /ws/echo (id=" << ws.get_id() << ")" << std::endl;

        // Set up message handlers - these run entirely in C++
        ws.on_text_message = [&ws](const std::string& msg) {
            std::cout << "[WS] Received: " << msg << std::endl;
            std::string response = "Echo: " + msg;
            ws.send_text(response);
        };

        ws.on_binary_message = [&ws](const uint8_t* data, size_t len) {
            std::cout << "[WS] Received " << len << " binary bytes" << std::endl;
            ws.send_binary(data, len);
        };

        ws.on_close = [](uint16_t code, const char* reason) {
            std::cout << "[WS] Connection closed: code=" << code
                     << " reason=" << (reason ? reason : "none") << std::endl;
        };

        ws.on_error = [](const char* error) {
            std::cerr << "[WS] Error: " << (error ? error : "unknown") << std::endl;
        };
    });

    // Chat-style WebSocket (broadcasts to all connections would need connection tracking)
    app.websocket("/ws/chat", [](fasterapi::http::WebSocketConnection& ws) {
        std::cout << "[Chat] New connection (id=" << ws.get_id() << ")" << std::endl;

        ws.on_text_message = [&ws](const std::string& msg) {
            // Prefix with connection ID for demo
            std::string response = "[User " + std::to_string(ws.get_id()) + "]: " + msg;
            ws.send_text(response);
        };

        ws.on_close = [&ws](uint16_t code, const char* reason) {
            std::cout << "[Chat] User " << ws.get_id() << " disconnected" << std::endl;
        };
    });

    // ========================================
    // Start Server
    // ========================================

    std::cout << "Starting server on http://127.0.0.1:8080" << std::endl;
    std::cout << "WebSocket endpoints:" << std::endl;
    std::cout << "  - ws://127.0.0.1:8080/ws/echo" << std::endl;
    std::cout << "  - ws://127.0.0.1:8080/ws/chat" << std::endl;
    std::cout << std::endl;
    std::cout << "Press Ctrl+C to stop..." << std::endl;
    std::cout << std::endl;

    // Run the server using UnifiedServer (multi-protocol support)
    int result = app.run_unified("127.0.0.1", 8080);

    if (result != 0) {
        std::cerr << "Server failed to start with error code: " << result << std::endl;
        return 1;
    }

    return 0;
}
