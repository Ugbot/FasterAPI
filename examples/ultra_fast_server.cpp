/**
 * Ultra-Fast Benchmark Server
 * 
 * Uses zero-allocation callback path for maximum performance.
 * This is a specialized server for benchmarking only.
 */

#include "../src/cpp/http/unified_server.h"
#include "../src/cpp/http/http1_connection.h"
#include <iostream>
#include <csignal>
#include <atomic>
#include <random>
#include <array>

using namespace fasterapi::http;

static std::atomic<bool> g_running{true};

// TechEmpower World table simulation (10000 rows)
static std::array<int, 10001> g_world_table;

// Thread-local RNG
thread_local std::mt19937 t_rng{std::random_device{}()};
thread_local std::uniform_int_distribution<int> t_id_dist{1, 10000};

// Pre-computed JSON response for /json endpoint
static constexpr char JSON_RESPONSE[] = 
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 27\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "{\"message\":\"Hello, World!\"}";

// Pre-computed plaintext response
static constexpr char PLAINTEXT_RESPONSE[] = 
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 13\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, World!";

// Ultra-fast request handler - zero allocations
static size_t ultra_fast_handler(const Http1RequestView& view, FastResponseWriter& writer) {
    // Match path
    if (view.path == "/json") {
        writer.write(JSON_RESPONSE, sizeof(JSON_RESPONSE) - 1);
        return writer.size;
    }
    
    if (view.path == "/plaintext") {
        writer.write(PLAINTEXT_RESPONSE, sizeof(PLAINTEXT_RESPONSE) - 1);
        return writer.size;
    }
    
    if (view.path == "/db") {
        // Single database query
        int id = t_id_dist(t_rng);
        int randomNumber = g_world_table[id];
        
        // Build JSON manually
        char body[64];
        char* p = body;
        memcpy(p, "{\"id\":", 6); p += 6;
        auto [end1, ec1] = std::to_chars(p, p + 10, id);
        p = end1;
        memcpy(p, ",\"randomNumber\":", 16); p += 16;
        auto [end2, ec2] = std::to_chars(p, p + 10, randomNumber);
        p = end2;
        *p++ = '}';
        size_t body_len = p - body;
        
        writer.write_status_200();
        writer.write_content_type_json();
        writer.write_content_length(body_len);
        writer.write_connection_keepalive();
        writer.write_headers_end();
        writer.write(body, body_len);
        return writer.size;
    }
    
    // 404 for unknown paths
    static constexpr char NOT_FOUND[] = 
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 9\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "Not Found";
    writer.write(NOT_FOUND, sizeof(NOT_FOUND) - 1);
    return writer.size;
}

void signal_handler(int) {
    std::cout << "\nShutting down..." << std::endl;
    g_running = false;
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize world table
    std::mt19937 init_rng{42};
    std::uniform_int_distribution<int> init_dist{1, 10000};
    for (int i = 1; i <= 10000; i++) {
        g_world_table[i] = init_dist(init_rng);
    }
    
    std::cout << "=== Ultra-Fast Benchmark Server ===" << std::endl;
    std::cout << "Zero-allocation request handling" << std::endl;
    std::cout << std::endl;
    
    // Configure server
    UnifiedServerConfig config;
    config.host = "127.0.0.1";
    config.http1_port = 8080;
    config.enable_http1_cleartext = true;
    config.pure_cpp_mode = true;
    config.num_workers = 0;  // Auto-detect
    config.use_reuseport = true;
    
    UnifiedServer server(config);
    
    // Set ultra-fast callback via a custom connection setup
    // We need to hook into the connection creation
    // For now, use a shim that sets the ultra_fast_callback
    
    std::cout << "Starting on http://127.0.0.1:8080" << std::endl;
    std::cout << "Endpoints: /json, /plaintext, /db" << std::endl;
    std::cout << std::endl;
    
    // Start server - but we need to modify how connections are created
    // to use ultra_fast_callback instead of fast_request_callback
    
    // For now, let's just test the build compiles
    std::cout << "Note: Ultra-fast callback requires server modification." << std::endl;
    std::cout << "Use pure_cpp_server for now with the existing fast path." << std::endl;
    
    return 0;
}
