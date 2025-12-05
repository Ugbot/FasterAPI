/**
 * Simple test program to verify CoroIO HTTP server is working
 */

#include "src/cpp/http/http1_coroio_handler.h"
#include "src/cpp/http/server.h"
#include "src/cpp/http/request.h"
#include "src/cpp/http/response.h"
#include <iostream>
#include <thread>
#include <chrono>

using namespace fasterapi::http;

int main() {
    std::cout << "================================================\n";
    std::cout << "Testing CoroIO HTTP Server\n";
    std::cout << "================================================\n\n";

    // Create server configuration
    HttpServer::Config config;
    config.port = 8000;
    config.host = "0.0.0.0";
    config.enable_h1 = true;
    config.enable_h2 = false;
    config.enable_h3 = false;

    std::cout << "Creating HTTP server...\n";
    HttpServer server(config);

    // Add a simple test route
    std::cout << "Adding test routes...\n";

    // Root endpoint
    server.add_route("GET", "/", [](HttpRequest* req, HttpResponse* res) {
        res->status(HttpResponse::Status::OK)
           .content_type("application/json")
           .json("{\"message\":\"Hello from CoroIO!\"}")
           .send();
    });

    // Health check endpoint
    server.add_route("GET", "/health", [](HttpRequest* req, HttpResponse* res) {
        res->status(HttpResponse::Status::OK)
           .content_type("application/json")
           .json("{\"status\":\"healthy\"}")
           .send();
    });

    // Benchmark endpoint
    server.add_route("GET", "/benchmark", [](HttpRequest* req, HttpResponse* res) {
        res->status(HttpResponse::Status::OK)
           .content_type("application/json")
           .json("{\"hello\":\"world\"}")
           .send();
    });

    std::cout << "\nStarting HTTP server on " << config.host << ":" << config.port << "...\n";
    int result = server.start();

    if (result != 0) {
        std::cerr << "Failed to start server: " << result << "\n";
        return 1;
    }

    std::cout << "Server started successfully!\n";
    std::cout << "\nTest endpoints:\n";
    std::cout << "  curl http://localhost:8000/\n";
    std::cout << "  curl http://localhost:8000/health\n";
    std::cout << "  curl http://localhost:8000/benchmark\n";
    std::cout << "\nPress Ctrl+C to stop the server.\n\n";

    // Wait for the server to run
    try {
        while (server.is_running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    } catch (...) {
        std::cout << "\nShutting down...\n";
    }

    // Stop the server
    server.stop();
    std::cout << "Server stopped.\n";

    return 0;
}
