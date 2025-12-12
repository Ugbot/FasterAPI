/**
 * Ultra-Fast Benchmark Server
 *
 * Maximum performance HTTP server using zero-allocation path.
 * Uses UltraFastCallback to write responses directly to pooled buffers.
 *
 * Build:
 *   cmake --build build --target ultrafast_benchmark
 *
 * Run:
 *   ./build/examples/ultrafast_benchmark
 *
 * Benchmark:
 *   wrk -c16 -d5s -t2 http://localhost:8080/plaintext
 *   wrk -c16 -d5s -t2 -s benchmarks/tfb/pipeline.lua http://localhost:8080/plaintext -- 16
 */

#include "../src/cpp/http/unified_server.h"
#include "../src/cpp/http/http1_connection.h"
#include <iostream>
#include <csignal>
#include <cstring>

using namespace fasterapi::http;

// Thread-local cached date header (updated once per second)
thread_local char t_date_buf[64] = "Date: Thu, 01 Jan 1970 00:00:00 GMT\r\n";
thread_local size_t t_date_len = 37;
thread_local time_t t_last_time = 0;

static inline void update_date() noexcept {
    time_t now = time(nullptr);
    if (now != t_last_time) {
        t_last_time = now;
        struct tm tm_buf;
        gmtime_r(&now, &tm_buf);
        t_date_len = strftime(t_date_buf, sizeof(t_date_buf),
            "Date: %a, %d %b %Y %H:%M:%S GMT\r\n", &tm_buf);
    }
}

// Ultra-fast request handler - writes directly to buffer, zero allocations
size_t ultrafast_handler(const Http1RequestView& req, FastResponseWriter& w) {
    // Route based on path
    if (req.path == "/plaintext") {
        // TechEmpower plaintext response
        static constexpr char body[] = "Hello, World!";
        static constexpr size_t body_len = sizeof(body) - 1;
        
        w.write_status_200();
        w.write_content_type_text();
        w.write_content_length(body_len);
        w.write_connection_keepalive();
        update_date();
        w.write(t_date_buf, t_date_len);
        w.write_headers_end();
        w.write(body, body_len);
        
        return w.size;
    }
    
    if (req.path == "/json") {
        // TechEmpower JSON response
        static constexpr char body[] = R"({"message":"Hello, World!"})";
        static constexpr size_t body_len = sizeof(body) - 1;
        
        w.write_status_200();
        w.write_content_type_json();
        w.write_content_length(body_len);
        w.write_connection_keepalive();
        update_date();
        w.write(t_date_buf, t_date_len);
        w.write_headers_end();
        w.write(body, body_len);
        
        return w.size;
    }
    
    // 404 for unknown paths
    static constexpr char not_found[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: keep-alive\r\n\r\nNot Found";
    w.write(not_found, sizeof(not_found) - 1);
    return w.size;
}

static UnifiedServer* g_server = nullptr;

void signal_handler(int) {
    std::cout << "\nShutting down..." << std::endl;
    if (g_server) {
        g_server->stop();
    }
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::cout << "=== Ultra-Fast Benchmark Server ===" << std::endl;
    std::cout << "Zero-allocation HTTP/1.1 with buffer pooling" << std::endl;
    std::cout << std::endl;

    UnifiedServerConfig config;
    config.host = "127.0.0.1";
    config.http1_port = 8080;
    config.enable_http1_cleartext = true;
    config.enable_tls = false;
    config.enable_http3 = false;
    config.pure_cpp_mode = true;
    config.num_workers = 0;  // Auto-detect CPU count
    config.use_reuseport = true;

    UnifiedServer server(config);
    g_server = &server;

    // Set the ultra-fast callback
    server.set_ultra_fast_callback(ultrafast_handler);

    std::cout << "Starting server on http://127.0.0.1:8080" << std::endl;
    std::cout << "Endpoints: /plaintext, /json" << std::endl;
    std::cout << std::endl;
    std::cout << "Press Ctrl+C to stop..." << std::endl;

    int result = server.start();
    
    g_server = nullptr;
    return result;
}
