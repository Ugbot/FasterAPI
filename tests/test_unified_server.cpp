/**
 * Test Unified HTTP Server with TLS/ALPN
 *
 * Demonstrates multi-protocol server with:
 * - TLS on port 443 with ALPN (HTTP/2 and HTTP/1.1)
 * - Cleartext HTTP/1.1 on port 8080
 */

#include <iostream>
#include <csignal>
#include <atomic>
#include "src/cpp/http/unified_server.h"

using namespace fasterapi::http;
using namespace fasterapi::net;

std::atomic<bool> shutdown_requested{false};

void signal_handler(int signal) {
    std::cout << "\n[Test] Shutdown signal received (" << signal << ")" << std::endl;
    shutdown_requested.store(true);
}

int main(int argc, char* argv[]) {
    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "=== FasterAPI Unified HTTP Server Test ===" << std::endl;
    std::cout << std::endl;

    // Configure server
    UnifiedServerConfig config;

    // TLS configuration (port 443)
    config.enable_tls = true;
    config.tls_port = 8443;  // Use 8443 for testing (no sudo needed)
    config.host = "127.0.0.1";
    config.cert_file = "certs/server.crt";
    config.key_file = "certs/server.key";
    config.alpn_protocols = {"h2", "http/1.1"};  // HTTP/2 and HTTP/1.1

    // Cleartext HTTP/1.1 (port 8080)
    config.enable_http1_cleartext = true;
    config.http1_port = 8080;

    // Single worker for testing
    config.num_workers = 1;

    std::cout << "Configuration:" << std::endl;
    std::cout << "  - TLS (ALPN): https://127.0.0.1:8443 (protocols: h2, http/1.1)" << std::endl;
    std::cout << "  - Cleartext:  http://127.0.0.1:8080 (HTTP/1.1 only)" << std::endl;
    std::cout << std::endl;

    // Create server
    UnifiedServer server(config);

    // Set request handler
    server.set_request_handler([](
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body,
        std::function<void(uint16_t, const std::unordered_map<std::string, std::string>&, const std::string&)> send_response
    ) {
        std::cout << "[Request] " << method << " " << path << std::endl;

        // Print some headers
        auto it = headers.find("user-agent");
        if (it != headers.end()) {
            std::cout << "  User-Agent: " << it->second << std::endl;
        }

        // Build response
        std::unordered_map<std::string, std::string> response_headers;
        response_headers["content-type"] = "text/plain";
        response_headers["server"] = "FasterAPI/1.0";

        std::string response_body = "Hello from FasterAPI Unified Server!\n";
        response_body += "Method: " + method + "\n";
        response_body += "Path: " + path + "\n";

        send_response(200, response_headers, response_body);
    });

    std::cout << "Starting server..." << std::endl;
    std::cout << std::endl;
    std::cout << "Test with:" << std::endl;
    std::cout << "  HTTP/1.1 cleartext:  curl http://127.0.0.1:8080/" << std::endl;
    std::cout << "  HTTP/1.1 over TLS:   curl -k --http1.1 https://127.0.0.1:8443/" << std::endl;
    std::cout << "  HTTP/2 over TLS:     curl -k --http2 https://127.0.0.1:8443/" << std::endl;
    std::cout << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    std::cout << std::endl;

    // Start server (blocks)
    int result = server.start();

    if (result < 0) {
        std::cerr << "Error: Failed to start server: " << server.get_error() << std::endl;
        return 1;
    }

    std::cout << "Server stopped gracefully" << std::endl;
    return 0;
}
