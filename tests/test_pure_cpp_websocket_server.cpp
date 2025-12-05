/**
 * Pure C++ WebSocket Test Server
 *
 * This server tests WebSocket functionality in pure C++ mode (no ZMQ/Python).
 * Used by test_pure_cpp_websocket.py for E2E verification.
 *
 * Features tested:
 * - WebSocket handshake
 * - Text message echo
 * - Binary message echo
 * - Multiple endpoints
 * - HTTP endpoints alongside WebSocket
 *
 * Build: cmake --build build --target test_pure_cpp_websocket_server
 * Run:   DYLD_LIBRARY_PATH=build/lib ./build/tests/test_pure_cpp_websocket_server
 */

#include "../src/cpp/http/app.h"
#include "../src/cpp/core/logger.h"

#include <iostream>
#include <csignal>
#include <atomic>
#include <cstring>
#include <thread>
#include <chrono>

static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_message_count{0};
static std::atomic<uint64_t> g_connection_count{0};

void signal_handler(int) {
    std::cout << "\nShutting down..." << std::endl;
    g_running = false;
}

int main(int argc, char** argv) {
    // Parse port from command line (default: 8700)
    int port = 8700;
    if (argc > 1) {
        port = std::atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            std::cerr << "Invalid port: " << argv[1] << std::endl;
            return 1;
        }
    }

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Configure logging
    fasterapi::core::Logger::instance().set_level(fasterapi::core::LogLevel::INFO);

    std::cout << "=== Pure C++ WebSocket Test Server ===" << std::endl;
    std::cout << "Port: " << port << std::endl;

    // Create App in pure C++ mode
    fasterapi::App::Config config;
    config.pure_cpp_mode = true;

    fasterapi::App app(config);

    // ========================================
    // HTTP Endpoints (for health checks)
    // ========================================

    app.get("/test-health", [](fasterapi::Request& req, fasterapi::Response& res) {
        res.json(R"({"status":"ok","mode":"pure_cpp_websocket_test"})");
    });

    app.get("/stats", [](fasterapi::Request& req, fasterapi::Response& res) {
        std::string json = "{\"connections\":" + std::to_string(g_connection_count.load()) +
                          ",\"messages\":" + std::to_string(g_message_count.load()) + "}";
        res.json(json);
    });

    // ========================================
    // WebSocket Endpoints
    // ========================================

    // Echo endpoint - echoes text and binary messages
    app.websocket("/ws/echo", [](fasterapi::http::WebSocketConnection& ws) {
        uint64_t conn_id = ws.get_id();
        g_connection_count++;
        std::cout << "[Echo] Connection " << conn_id << " opened" << std::endl;

        ws.on_text_message = [&ws, conn_id](const std::string& msg) {
            g_message_count++;
            std::cout << "[Echo] Conn " << conn_id << " recv: \"" << msg << "\"" << std::endl;

            // Echo back with prefix
            std::string response = "Echo: " + msg;
            ws.send_text(response);
            std::cout << "[Echo] Conn " << conn_id << " sent: \"" << response << "\"" << std::endl;
        };

        ws.on_binary_message = [&ws, conn_id](const uint8_t* data, size_t len) {
            g_message_count++;
            std::cout << "[Echo] Conn " << conn_id << " recv binary: " << len << " bytes" << std::endl;

            // Echo back binary data
            ws.send_binary(data, len);
            std::cout << "[Echo] Conn " << conn_id << " sent binary: " << len << " bytes" << std::endl;
        };

        ws.on_close = [conn_id](uint16_t code, const char* reason) {
            g_connection_count--;
            std::cout << "[Echo] Connection " << conn_id << " closed: code=" << code
                     << " reason=" << (reason ? reason : "none") << std::endl;
        };

        ws.on_error = [conn_id](const char* error) {
            std::cerr << "[Echo] Connection " << conn_id << " error: "
                     << (error ? error : "unknown") << std::endl;
        };
    });

    // Uppercase endpoint - converts text to uppercase
    app.websocket("/ws/uppercase", [](fasterapi::http::WebSocketConnection& ws) {
        uint64_t conn_id = ws.get_id();
        g_connection_count++;
        std::cout << "[Upper] Connection " << conn_id << " opened" << std::endl;

        ws.on_text_message = [&ws, conn_id](const std::string& msg) {
            g_message_count++;

            // Convert to uppercase
            std::string upper = msg;
            for (char& c : upper) {
                c = std::toupper(static_cast<unsigned char>(c));
            }

            ws.send_text(upper);
            std::cout << "[Upper] Conn " << conn_id << ": \"" << msg << "\" -> \"" << upper << "\"" << std::endl;
        };

        ws.on_close = [conn_id](uint16_t code, const char* reason) {
            g_connection_count--;
            std::cout << "[Upper] Connection " << conn_id << " closed" << std::endl;
        };
    });

    // Reverse endpoint - reverses the message
    app.websocket("/ws/reverse", [](fasterapi::http::WebSocketConnection& ws) {
        uint64_t conn_id = ws.get_id();
        g_connection_count++;

        ws.on_text_message = [&ws](const std::string& msg) {
            g_message_count++;

            // Reverse the string
            std::string reversed(msg.rbegin(), msg.rend());
            ws.send_text(reversed);
        };

        ws.on_close = [conn_id](uint16_t code, const char* reason) {
            g_connection_count--;
        };
    });

    // JSON endpoint - wraps message in JSON
    app.websocket("/ws/json", [](fasterapi::http::WebSocketConnection& ws) {
        uint64_t conn_id = ws.get_id();
        g_connection_count++;

        ws.on_text_message = [&ws, conn_id](const std::string& msg) {
            g_message_count++;

            // Wrap in JSON (simple escaping)
            std::string escaped;
            for (char c : msg) {
                if (c == '"') escaped += "\\\"";
                else if (c == '\\') escaped += "\\\\";
                else if (c == '\n') escaped += "\\n";
                else escaped += c;
            }

            std::string json = "{\"id\":" + std::to_string(conn_id) +
                              ",\"message\":\"" + escaped + "\"}";
            ws.send_text(json);
        };

        ws.on_close = [](uint16_t code, const char* reason) {
            g_connection_count--;
        };
    });

    // ========================================
    // Start Server
    // ========================================

    std::cout << "Starting server on http://127.0.0.1:" << port << std::endl;
    std::cout << "WebSocket endpoints:" << std::endl;
    std::cout << "  - ws://127.0.0.1:" << port << "/ws/echo" << std::endl;
    std::cout << "  - ws://127.0.0.1:" << port << "/ws/uppercase" << std::endl;
    std::cout << "  - ws://127.0.0.1:" << port << "/ws/reverse" << std::endl;
    std::cout << "  - ws://127.0.0.1:" << port << "/ws/json" << std::endl;
    std::cout << std::endl;
    std::cout << "Press Ctrl+C to stop..." << std::endl;
    std::cout << "SERVER_READY" << std::endl;  // Signal for test harness

    // Run the server
    int result = app.run_unified("127.0.0.1", port);

    if (result != 0) {
        std::cerr << "Server failed with error code: " << result << std::endl;
        return 1;
    }

    return 0;
}
