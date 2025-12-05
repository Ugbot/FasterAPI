/**
 * CoroIO HTTP/1.1 Benchmark Server
 *
 * High-performance lockfree HTTP server for benchmarking.
 * Returns simple JSON responses like TechEmpower tests.
 */

#include "src/cpp/http/http1_coroio_handler.h"
#include "src/cpp/http/server.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <csignal>

std::atomic<bool> g_shutdown_requested{false};
std::atomic<uint64_t> g_request_count{0};

void signal_handler(int signum) {
    std::cout << "\n🛑 Shutdown requested (signal " << signum << ")" << std::endl;
    g_shutdown_requested.store(true);
}

int main(int argc, char* argv[]) {
    std::cout << "==================================================================" << std::endl;
    std::cout << "🚀 CoroIO HTTP/1.1 Benchmark Server" << std::endl;
    std::cout << "==================================================================" << std::endl;
    std::cout << std::endl;

    std::cout << "Features:" << std::endl;
    std::cout << "  ✓ Lockfree architecture (no mutexes)" << std::endl;
    std::cout << "  ✓ HTTP/1.1 keep-alive connections" << std::endl;
    std::cout << "  ✓ Platform-native async I/O (kqueue/epoll/IOCP)" << std::endl;
    std::cout << "  ✓ C++20 coroutines via CoroIO" << std::endl;
    std::cout << std::endl;

    // Parse command line args
    uint16_t port = 8000;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }

    // Install signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Create server config
    HttpServer::Config config;
    config.port = port;
    config.host = "0.0.0.0";
    config.enable_h1 = true;
    config.enable_h2 = false;
    config.enable_h3 = false;
    config.enable_compression = false;  // Disable for pure benchmark

    // Create server
    HttpServer server(config);

    // Add routes for TechEmpower-style tests

    // JSON serialization test
    server.add_route("GET", "/json", [](HttpRequest* req, HttpResponse* res) {
        g_request_count.fetch_add(1, std::memory_order_relaxed);
        res->status(HttpResponse::Status::OK)
           .content_type("application/json")
           .body("{\"message\":\"Hello, World!\"}");
        res->send();
    });

    // Plaintext test
    server.add_route("GET", "/plaintext", [](HttpRequest* req, HttpResponse* res) {
        g_request_count.fetch_add(1, std::memory_order_relaxed);
        res->status(HttpResponse::Status::OK)
           .content_type("text/plain")
           .body("Hello, World!");
        res->send();
    });

    // Root path
    server.add_route("GET", "/", [](HttpRequest* req, HttpResponse* res) {
        g_request_count.fetch_add(1, std::memory_order_relaxed);
        res->status(HttpResponse::Status::OK)
           .content_type("text/html")
           .body("<html><body><h1>CoroIO Benchmark Server</h1>"
                 "<p>Endpoints:</p>"
                 "<ul>"
                 "<li><a href=\"/json\">/json</a> - JSON test</li>"
                 "<li><a href=\"/plaintext\">/plaintext</a> - Plaintext test</li>"
                 "</ul></body></html>");
        res->send();
    });

    // Start server
    std::cout << "Starting server on http://0.0.0.0:" << port << std::endl;
    int result = server.start();
    if (result != 0) {
        std::cerr << "❌ Failed to start server: " << result << std::endl;
        return 1;
    }

    std::cout << "✓ Server started successfully!" << std::endl;
    std::cout << std::endl;
    std::cout << "Test endpoints:" << std::endl;
    std::cout << "  curl http://localhost:" << port << "/" << std::endl;
    std::cout << "  curl http://localhost:" << port << "/json" << std::endl;
    std::cout << "  curl http://localhost:" << port << "/plaintext" << std::endl;
    std::cout << std::endl;
    std::cout << "Benchmark commands:" << std::endl;
    std::cout << "  # Apache Bench (10k requests, 100 concurrent)" << std::endl;
    std::cout << "  ab -n 10000 -c 100 http://localhost:" << port << "/plaintext" << std::endl;
    std::cout << std::endl;
    std::cout << "  # Simple sequential test" << std::endl;
    std::cout << "  for i in {1..100}; do curl -s http://localhost:" << port << "/plaintext > /dev/null; done" << std::endl;
    std::cout << std::endl;
    std::cout << "Press Ctrl+C to stop server" << std::endl;
    std::cout << std::endl;

    // Track stats
    auto start_time = std::chrono::steady_clock::now();
    uint64_t last_count = 0;

    // Keep server running
    while (!g_shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));

        // Print stats every 5 seconds if there were requests
        uint64_t current_count = g_request_count.load(std::memory_order_relaxed);
        if (current_count > last_count) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();

            if (elapsed > 0) {
                double avg_rps = static_cast<double>(current_count) / elapsed;
                std::cout << "📊 Stats: " << current_count << " requests"
                          << " (" << static_cast<uint64_t>(avg_rps) << " req/s average)" << std::endl;
            }
            last_count = current_count;
        }
    }

    // Stop server
    std::cout << std::endl;
    std::cout << "Stopping server..." << std::endl;
    server.stop();

    // Print final stats
    auto end_time = std::chrono::steady_clock::now();
    auto total_time = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();
    uint64_t final_count = g_request_count.load();

    std::cout << std::endl;
    std::cout << "==================================================================" << std::endl;
    std::cout << "📊 Final Statistics" << std::endl;
    std::cout << "==================================================================" << std::endl;
    std::cout << "Total requests:  " << final_count << std::endl;
    std::cout << "Total time:      " << total_time << " seconds" << std::endl;

    if (total_time > 0) {
        double avg_rps = static_cast<double>(final_count) / total_time;
        std::cout << "Average RPS:     " << static_cast<uint64_t>(avg_rps) << " requests/second" << std::endl;
    }

    std::cout << "==================================================================" << std::endl;
    std::cout << "✅ Server stopped cleanly" << std::endl;

    return 0;
}
