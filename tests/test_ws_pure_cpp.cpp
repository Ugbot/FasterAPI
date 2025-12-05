/**
 * Pure C++ WebSocket E2E Test Server
 *
 * This server uses ONLY C++ WebSocket handlers, bypassing ZMQ entirely.
 * Used to isolate whether the kqueue event issue exists in pure C++ mode.
 */

#include "../src/cpp/http/unified_server.h"
#include "../src/cpp/core/logger.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <cstring>

static std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running = false;
}

int main(int argc, char** argv) {
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize logger
    fasterapi::Logger::init();
    fasterapi::Logger::set_level(fasterapi::LogLevel::DEBUG);

    std::cout << "=== Pure C++ WebSocket Test Server ===" << std::endl;
    std::cout << "Starting on port 8600..." << std::endl;

    // Create unified server config
    fasterapi::http::UnifiedServerConfig config;
    config.host = "127.0.0.1";
    config.http1_port = 8600;
    config.enable_tls = false;
    config.enable_http1_cleartext = true;
    config.num_workers = 1;  // Single threaded for easier debugging

    fasterapi::http::UnifiedServer server(config);

    // Register pure C++ WebSocket handler - no ZMQ involved
    server.add_websocket_handler("/ws/echo", [](fasterapi::http::WebSocketConnection& ws) {
        std::cout << "[WS] New connection on /ws/echo (id=" << ws.get_id() << ")" << std::endl;

        // Set up callbacks - these run in C++ only, no ZMQ
        ws.on_text_message = [&ws](const std::string& msg) {
            std::cout << "[WS] Received text message: \"" << msg << "\"" << std::endl;
            std::string response = "Echo: " + msg;
            int result = ws.send_text(response);
            if (result == 0) {
                std::cout << "[WS] Sent response: \"" << response << "\"" << std::endl;
            } else {
                std::cerr << "[WS] Error sending response: " << result << std::endl;
            }
        };

        ws.on_binary_message = [&ws](const uint8_t* data, size_t len) {
            std::cout << "[WS] Received binary message: " << len << " bytes" << std::endl;
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

    // Register HTTP health endpoint using set_request_handler
    server.set_request_handler([](
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body,
        std::function<void(uint16_t, const std::unordered_map<std::string, std::string>&, const std::string&)> send_response
    ) {
        if (method == "GET" && path == "/health") {
            std::unordered_map<std::string, std::string> resp_headers;
            resp_headers["Content-Type"] = "application/json";
            send_response(200, resp_headers, R"({"status":"ok","mode":"pure_cpp"})");
        } else {
            std::unordered_map<std::string, std::string> resp_headers;
            resp_headers["Content-Type"] = "text/plain";
            send_response(404, resp_headers, "Not Found");
        }
    });

    // Start server
    int result = server.start();
    if (result != 0) {
        std::cerr << "Failed to start server: " << result << std::endl;
        return 1;
    }

    std::cout << "Server started. Listening on http://127.0.0.1:8600" << std::endl;
    std::cout << "WebSocket endpoint: ws://127.0.0.1:8600/ws/echo" << std::endl;
    std::cout << "Press Ctrl+C to stop..." << std::endl;

    // Run until interrupted
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\nStopping server..." << std::endl;
    server.stop();

    return 0;
}
