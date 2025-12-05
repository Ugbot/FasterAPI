/**
 * Pure C++ test of the HTTP server with CoroIO
 * This tests the server WITHOUT Python callbacks to isolate the issue
 */

#include "src/cpp/http/http1_coroio_handler.h"
#include "src/cpp/http/server.h"
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    std::cout << "======================================================================" << std::endl;
    std::cout << "FasterAPI Pure C++ HTTP Server Test" << std::endl;
    std::cout << "======================================================================" << std::endl;
    std::cout << std::endl;

    // Create server config
    HttpServer::Config config;
    config.port = 9000;
    config.host = "0.0.0.0";
    config.enable_h1 = true;
    config.enable_h2 = false;
    config.enable_h3 = false;
    config.enable_compression = false;
    config.enable_websocket = false;

    std::cout << "Creating HTTP server..." << std::endl;
    HttpServer server(config);
    std::cout << "✓ Server created!" << std::endl;
    std::cout << std::endl;

    std::cout << "Starting server on port 8000..." << std::endl;
    int result = server.start();
    if (result != 0) {
        std::cerr << "✗ Failed to start server: error code " << result << std::endl;
        return 1;
    }
    std::cout << "✓ Server started!" << std::endl;
    std::cout << "  Running: " << (server.is_running() ? "YES" : "NO") << std::endl;
    std::cout << std::endl;

    std::cout << "======================================================================" << std::endl;
    std::cout << "Server is running on http://0.0.0.0:9000" << std::endl;
    std::cout << "Test with: curl http://localhost:9000/" << std::endl;
    std::cout << "Press Ctrl+C to stop..." << std::endl;
    std::cout << "======================================================================" << std::endl;
    std::cout << std::endl;

    // Keep running
    while (server.is_running()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        auto stats = server.get_stats();
        if (stats.total_requests > 0) {
            std::cout << "\rRequests: " << stats.total_requests
                      << ", Connections: " << stats.active_connections
                      << ", Bytes sent: " << stats.total_bytes_sent
                      << std::flush;
        }
    }

    std::cout << std::endl;
    std::cout << "Server stopped." << std::endl;

    return 0;
}
