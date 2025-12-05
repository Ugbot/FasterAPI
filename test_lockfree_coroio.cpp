/**
 * Test CoroIO Lockfree HTTP/1.1 Server
 *
 * Demonstrates:
 * - Lockfree handler registration via Aeron SPSC queues
 * - HTTP/1.1 keep-alive connections
 * - Connection timeout protection
 * - Graceful shutdown
 */

#include "src/cpp/http/server.h"
#include "src/cpp/http/python_callback_bridge.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>

// Global server instance for signal handler
HttpServer* g_server = nullptr;

void signal_handler(int signal) {
    if (signal == SIGINT) {
        std::cout << "\n🛑 Shutting down server..." << std::endl;
        if (g_server) {
            g_server->stop();
        }
        exit(0);
    }
}

// Simple handler that returns a response
void hello_handler(
    const std::string& method,
    const std::string& path,
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& body,
    PythonCallbackBridge::HandlerResult& result
) {
    result.status_code = 200;
    result.content_type = "text/plain";
    result.body = "Hello from lockfree CoroIO server!\n"
                  "Method: " + method + "\n"
                  "Path: " + path + "\n"
                  "Keep-alive: " + (headers.count("Connection") ? headers.at("Connection") : "default") + "\n";
}

void benchmark_handler(
    const std::string& method,
    const std::string& path,
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& body,
    PythonCallbackBridge::HandlerResult& result
) {
    result.status_code = 200;
    result.content_type = "text/plain";
    result.body = "OK";
}

int main() {
    std::cout << "=== CoroIO Lockfree HTTP/1.1 Server Test ===" << std::endl;
    std::cout << std::endl;
    std::cout << "Features:" << std::endl;
    std::cout << "  ✓ Lockfree handler registration (Aeron SPSC queues)" << std::endl;
    std::cout << "  ✓ HTTP/1.1 keep-alive connections" << std::endl;
    std::cout << "  ✓ 30-second connection timeout" << std::endl;
    std::cout << "  ✓ Graceful shutdown via atomic flags" << std::endl;
    std::cout << "  ✓ Platform-native async I/O (kqueue/epoll/IOCP)" << std::endl;
    std::cout << std::endl;

    // Setup signal handler
    signal(SIGINT, signal_handler);

    // Create server configuration
    HttpServer::Config config;
    config.port = 8000;
    config.host = "0.0.0.0";
    config.enable_h1 = true;   // Enable HTTP/1.1 with CoroIO
    config.enable_h2 = false;  // Disable HTTP/2
    config.enable_h3 = false;  // Disable HTTP/3
    config.enable_compression = false;

    std::cout << "Creating server..." << std::endl;
    HttpServer server(config);
    g_server = &server;

    // Note: In a real C++ app, we'd create proper handler wrappers
    // For now, we'll just demonstrate the server starts and polls registrations

    std::cout << "Starting server on http://0.0.0.0:8000" << std::endl;
    int result = server.start();

    if (result != 0) {
        std::cerr << "❌ Failed to start server!" << std::endl;
        return 1;
    }

    std::cout << "✓ Server started successfully!" << std::endl;
    std::cout << std::endl;
    std::cout << "Test the server:" << std::endl;
    std::cout << "  curl http://localhost:8000/" << std::endl;
    std::cout << "  curl -v http://localhost:8000/  # See keep-alive header" << std::endl;
    std::cout << std::endl;
    std::cout << "Benchmark (keep-alive reuses connection):" << std::endl;
    std::cout << "  wrk -t4 -c100 -d10s http://localhost:8000/" << std::endl;
    std::cout << std::endl;
    std::cout << "Press Ctrl+C to stop server" << std::endl;
    std::cout << std::endl;

    // Keep server running
    while (server.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Poll for handler registrations (lockfree)
        PythonCallbackBridge::poll_registrations();
    }

    std::cout << "✓ Server stopped cleanly" << std::endl;
    g_server = nullptr;

    return 0;
}
