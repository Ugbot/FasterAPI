/**
 * WebSocket Echo Server for Autobahn Testsuite
 *
 * A minimal RFC 6455 compliant echo server for running the Autobahn
 * WebSocket protocol compliance testsuite.
 *
 * Build:
 *   cmake --build build --target websocket_echo_server
 *
 * Run:
 *   DYLD_LIBRARY_PATH=build/lib ./build/tests/websocket_echo_server
 *
 * Test with Autobahn:
 *   cd tests/autobahn && wstest -m fuzzingclient -s fuzzingclient.json
 */

#include "../../src/cpp/http/app.h"
#include <iostream>
#include <csignal>
#include <atomic>

using namespace fasterapi;

static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_messages_echoed{0};
static std::atomic<uint64_t> g_bytes_echoed{0};

void signal_handler(int) {
    std::cout << "\nShutting down..." << std::endl;
    g_running = false;
}

int main(int argc, char* argv[]) {
    uint16_t port = 9001;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::cout << "=== FasterAPI WebSocket Echo Server ===" << std::endl;
    std::cout << "Autobahn Testsuite Compliance Testing" << std::endl;
    std::cout << std::endl;

    App::Config config;
    config.pure_cpp_mode = true;
    config.enable_docs = false;

    App app(config);

    // Health check endpoint
    app.get("/health", [](Request& req, Response& res) {
        uint64_t msgs = g_messages_echoed.load(std::memory_order_relaxed);
        uint64_t bytes = g_bytes_echoed.load(std::memory_order_relaxed);

        char json[256];
        snprintf(json, sizeof(json),
            R"({"status":"ok","messages_echoed":%llu,"bytes_echoed":%llu})",
            static_cast<unsigned long long>(msgs),
            static_cast<unsigned long long>(bytes));
        res.json(json);
    });

    // WebSocket echo endpoint - echoes back all text and binary messages
    app.websocket("/", [](http::WebSocketConnection& ws) {
        std::cout << "[Echo] New connection" << std::endl;

        // Echo text messages back as-is
        ws.on_text_message = [&ws](const std::string& msg) {
            g_messages_echoed.fetch_add(1, std::memory_order_relaxed);
            g_bytes_echoed.fetch_add(msg.size(), std::memory_order_relaxed);
            ws.send_text(msg);
        };

        // Echo binary messages back as-is
        ws.on_binary_message = [&ws](const uint8_t* data, size_t len) {
            g_messages_echoed.fetch_add(1, std::memory_order_relaxed);
            g_bytes_echoed.fetch_add(len, std::memory_order_relaxed);
            ws.send_binary(data, len);
        };

        ws.on_close = [](uint16_t code, const char* reason) {
            std::cout << "[Echo] Connection closed (code=" << code << ")" << std::endl;
        };
    });

    std::cout << "Listening on ws://0.0.0.0:" << port << std::endl;
    std::cout << "Health check: http://localhost:" << port << "/health" << std::endl;
    std::cout << std::endl;
    std::cout << "Run Autobahn tests:" << std::endl;
    std::cout << "  cd tests/autobahn" << std::endl;
    std::cout << "  wstest -m fuzzingclient -s fuzzingclient.json" << std::endl;
    std::cout << std::endl;
    std::cout << "Press Ctrl+C to stop..." << std::endl;

    return app.run_unified("0.0.0.0", port);
}
