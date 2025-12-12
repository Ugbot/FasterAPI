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
#include "../src/cpp/http/http1_connection.h"
#include "../src/cpp/core/logger.h"

#include <iostream>
#include <csignal>
#include <atomic>
#include <random>
#include <array>
#include <charconv>

static std::atomic<bool> g_running{true};

// TechEmpower World table simulation (10000 rows)
// In production, this would be PostgreSQL
static std::array<int, 10001> g_world_table;  // index 1-10000

// Thread-local random number generator for high performance
thread_local std::mt19937 t_rng{std::random_device{}()};
thread_local std::uniform_int_distribution<int> t_id_dist{1, 10000};
thread_local std::uniform_int_distribution<int> t_rand_dist{1, 10000};

// Fast integer to string conversion
inline void append_int(std::string& out, int val) {
    char buf[16];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), val);
    out.append(buf, ptr - buf);
}

// Parse queries parameter (clamp to 1-500)
inline int parse_queries(std::string_view param) {
    if (param.empty()) return 1;
    int val = 0;
    auto [ptr, ec] = std::from_chars(param.data(), param.data() + param.size(), val);
    if (ec != std::errc()) return 1;
    if (val < 1) return 1;
    if (val > 500) return 500;
    return val;
}

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

    // Initialize simulated World table (TechEmpower benchmark)
    std::mt19937 init_rng{42};  // Deterministic seed for reproducibility
    std::uniform_int_distribution<int> init_dist{1, 10000};
    for (int i = 1; i <= 10000; i++) {
        g_world_table[i] = init_dist(init_rng);
    }

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

    // TechEmpower Benchmark Endpoints
    // JSON serialization test - must return {"message":"Hello, World!"}
    app.get("/json", [](fasterapi::Request& req, fasterapi::Response& res) {
        res.header("Content-Type", "application/json");
        res.send(R"({"message":"Hello, World!"})");
    });

    // Plaintext test - must return "Hello, World!"
    app.get("/plaintext", [](fasterapi::Request& req, fasterapi::Response& res) {
        res.header("Content-Type", "text/plain");
        res.send("Hello, World!");
    });

    // TechEmpower DB test - single database query
    // Returns: {"id":N,"randomNumber":M}
    app.get("/db", [](fasterapi::Request& req, fasterapi::Response& res) {
        int id = t_id_dist(t_rng);
        int randomNumber = g_world_table[id];
        
        std::string json;
        json.reserve(48);
        json = R"({"id":)";
        append_int(json, id);
        json += R"(,"randomNumber":)";
        append_int(json, randomNumber);
        json += "}";
        
        res.header("Content-Type", "application/json");
        res.send(json);
    });

    // TechEmpower Queries test - multiple database queries
    // Returns: [{"id":N,"randomNumber":M}, ...]
    app.get("/queries", [](fasterapi::Request& req, fasterapi::Response& res) {
        auto queries_param = req.query_param_optional("queries");
        int queries = parse_queries(queries_param.value_or("1"));
        
        std::string json;
        json.reserve(32 * queries + 2);
        json = "[";
        
        for (int i = 0; i < queries; i++) {
            if (i > 0) json += ",";
            int id = t_id_dist(t_rng);
            int randomNumber = g_world_table[id];
            
            json += R"({"id":)";
            append_int(json, id);
            json += R"(,"randomNumber":)";
            append_int(json, randomNumber);
            json += "}";
        }
        json += "]";
        
        res.header("Content-Type", "application/json");
        res.send(json);
    });

    // TechEmpower Updates test - multiple database updates
    // Returns: [{"id":N,"randomNumber":M}, ...] where M is the new value
    app.get("/updates", [](fasterapi::Request& req, fasterapi::Response& res) {
        auto queries_param = req.query_param_optional("queries");
        int queries = parse_queries(queries_param.value_or("1"));
        
        std::string json;
        json.reserve(32 * queries + 2);
        json = "[";
        
        for (int i = 0; i < queries; i++) {
            if (i > 0) json += ",";
            int id = t_id_dist(t_rng);
            int newRandomNumber = t_rand_dist(t_rng);
            
            // Simulate update
            g_world_table[id] = newRandomNumber;
            
            json += R"({"id":)";
            append_int(json, id);
            json += R"(,"randomNumber":)";
            append_int(json, newRandomNumber);
            json += "}";
        }
        json += "]";
        
        res.header("Content-Type", "application/json");
        res.send(json);
    });

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
    // Ultra-Fast Callback for Maximum Performance
    // ========================================
    
    // This callback bypasses all routing and writes directly to a pre-allocated buffer.
    // Use for TechEmpower-style benchmarks where every nanosecond counts.
    app.set_ultra_fast_callback([](const fasterapi::http::Http1RequestView& req, 
                                    fasterapi::http::FastResponseWriter& writer) -> size_t {
        // Check path - only handle /plaintext and /json
        if (req.path == "/plaintext") {
            static constexpr char body[] = "Hello, World!";
            static constexpr size_t body_len = sizeof(body) - 1;
            
            writer.write_status_200();
            writer.write_content_type_text();
            writer.write_connection_keepalive();
            writer.write_content_length(body_len);
            writer.write_headers_end();
            writer.write(body, body_len);
            return writer.size;
        }
        else if (req.path == "/json") {
            static constexpr char body[] = R"({"message":"Hello, World!"})";
            static constexpr size_t body_len = sizeof(body) - 1;
            
            writer.write_status_200();
            writer.write_content_type_json();
            writer.write_connection_keepalive();
            writer.write_content_length(body_len);
            writer.write_headers_end();
            writer.write(body, body_len);
            return writer.size;
        }
        
        // Not handled by ultra-fast path - return 0 to fall through to normal routing
        return 0;
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
