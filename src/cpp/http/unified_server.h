/**
 * Unified HTTP Server with TLS/ALPN and HTTP/3
 *
 * Multi-protocol HTTP server supporting:
 * - HTTP/3 over QUIC (UDP)
 * - HTTP/2 over TLS (h2) via ALPN
 * - HTTP/1.1 over TLS via ALPN
 * - HTTP/1.1 cleartext (testing port)
 * - WebTransport (optional, over HTTP/3)
 *
 * Architecture:
 * - Port 443 TCP: TLS with ALPN → HTTP/2 OR HTTP/1.1
 * - Port 443 UDP: QUIC → HTTP/3 (optionally with WebTransport)
 * - Port 8080 TCP: Cleartext → HTTP/1.1 only
 *
 * Transparent to users - protocol selection automatic via ALPN (TLS) or QUIC (UDP)
 */

#pragma once

#include "../net/tcp_listener.h"
#include "../net/udp_listener.h"
#include "../net/tls_context.h"
#include "../net/tls_socket.h"
#include "../net/event_loop.h"
#include "http2_connection.h"
#include "http1_connection.h"
#include "http3_connection.h"
#include "webtransport_connection.h"
#include <memory>
#include <atomic>
#include <functional>
#include <thread>
#include <unordered_map>
#include <string>

namespace fasterapi {
namespace http {

/**
 * Unified HTTP Server Configuration
 */
struct UnifiedServerConfig {
    // TLS configuration (port 443)
    bool enable_tls = true;
    uint16_t tls_port = 443;
    std::string host = "0.0.0.0";

    // Certificate configuration (file-based OR memory-based)
    std::string cert_file;         // Path to certificate file
    std::string key_file;          // Path to private key file
    std::string cert_data;         // In-memory certificate (PEM)
    std::string key_data;          // In-memory private key (PEM)

    // ALPN protocols (default: HTTP/2 and HTTP/1.1)
    std::vector<std::string> alpn_protocols = {"h2", "http/1.1"};

    // Cleartext HTTP/1.1 for testing (port 8080)
    bool enable_http1_cleartext = true;
    uint16_t http1_port = 8080;

    // HTTP/3 configuration (UDP)
    bool enable_http3 = false;
    uint16_t http3_port = 443;     // UDP port for HTTP/3 (defaults to 443)

    // WebTransport configuration
    bool enable_webtransport = false;

    // Worker configuration
    uint16_t num_workers = 0;      // 0 = auto (CPU count)
    bool use_reuseport = true;

    // Pure C++ mode - disables Python/ZMQ bridge initialization
    // When true:
    // - No ProcessPoolExecutor is created
    // - No ZMQ sockets are initialized
    // - All handlers must be C++ (no Python callbacks)
    // - WebSocket handlers are looked up in s_websocket_handlers_ only
    bool pure_cpp_mode = false;

    UnifiedServerConfig() = default;
};

/**
 * HTTP Request Handler Callback
 *
 * Universal callback for both HTTP/1.1 and HTTP/2 requests.
 * Handler receives method, path, headers, body and returns response.
 */
using HttpRequestHandler = std::function<void(
    const std::string& method,
    const std::string& path,
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& body,
    std::function<void(uint16_t, const std::unordered_map<std::string, std::string>&, const std::string&)> send_response
)>;

// Forward declaration for WebSocket handler
class WebSocketConnection;

/**
 * WebSocket Handler Callback
 *
 * Pure C++ WebSocket handler - bypasses ZMQ and runs entirely in C++.
 * Use this for performance-critical WebSocket handling without Python.
 *
 * The handler is invoked when a WebSocket connection is established.
 * Set up message callbacks on the connection inside the handler:
 *
 *   ws.on_text_message = [&ws](const std::string& msg) {
 *       ws.send_text("echo: " + msg);
 *   };
 *   ws.on_binary_message = [&ws](const uint8_t* data, size_t len) {
 *       ws.send_binary(data, len);
 *   };
 *   ws.on_close = [](uint16_t code, const char* reason) { ... };
 *   ws.on_error = [](const char* error) { ... };
 */
using WebSocketHandler = std::function<void(WebSocketConnection&)>;

/**
 * Unified HTTP Server
 *
 * Production-grade multi-protocol HTTP server.
 *
 * Features:
 * - HTTP/3 over QUIC (UDP)
 * - TLS with ALPN (automatic HTTP/2 vs HTTP/1.1 selection)
 * - Cleartext HTTP/1.1 for testing
 * - WebTransport (optional, over HTTP/3)
 * - Keep-alive (HTTP/1.1)
 * - Stream multiplexing (HTTP/2, HTTP/3)
 * - Event loop integration
 * - Python callback support
 *
 * Usage:
 *   UnifiedServerConfig config;
 *   config.cert_file = "server.crt";
 *   config.key_file = "server.key";
 *   config.enable_http3 = true;  // Enable HTTP/3 on UDP port 443
 *
 *   UnifiedServer server(config);
 *   server.set_request_handler(my_handler);
 *   server.start();  // Blocks
 */
class UnifiedServer {
public:
    /**
     * Create unified HTTP server
     *
     * @param config Server configuration
     */
    explicit UnifiedServer(const UnifiedServerConfig& config);

    /**
     * Destructor - ensures clean shutdown
     */
    ~UnifiedServer();

    // Non-copyable, non-movable
    UnifiedServer(const UnifiedServer&) = delete;
    UnifiedServer& operator=(const UnifiedServer&) = delete;

    /**
     * Set request handler callback
     *
     * Called for all HTTP requests (both HTTP/1.1 and HTTP/2).
     *
     * @param handler Request handler function
     */
    void set_request_handler(HttpRequestHandler handler);

    /**
     * Register a pure C++ WebSocket handler
     *
     * This handler runs entirely in C++, bypassing Python/ZMQ.
     * Use for performance-critical WebSocket handling.
     *
     * @param path WebSocket path (e.g., "/ws/echo")
     * @param handler Callback invoked when WebSocket connection established
     */
    void add_websocket_handler(const std::string& path, WebSocketHandler handler);

    /**
     * Set App instance for direct HTTP/1.1 handling (simplified path).
     *
     * @param app Pointer to App instance
     */
    void set_app_instance(void* app);

    /**
     * Start the server (blocks until stop())
     *
     * Starts listening on configured ports and processes requests.
     *
     * @return 0 on success, -1 on error
     */
    int start();

    /**
     * Stop the server
     *
     * Gracefully shuts down listeners and closes connections.
     */
    void stop();

    /**
     * Check if server is running
     */
    bool is_running() const noexcept {
        return !shutdown_flag_.load(std::memory_order_relaxed);
    }

    /**
     * Get last error message
     */
    const std::string& get_error() const noexcept {
        return error_message_;
    }

private:
    /**
     * TLS connection handler (port 443)
     */
    static void on_tls_connection(net::TcpSocket socket, net::EventLoop* event_loop);

    /**
     * Cleartext HTTP/1.1 connection handler (port 8080)
     */
    static void on_cleartext_connection(net::TcpSocket socket, net::EventLoop* event_loop);

    /**
     * QUIC datagram handler (UDP port for HTTP/3)
     */
    static void on_quic_datagram(
        const uint8_t* data,
        size_t length,
        const struct sockaddr* addr,
        socklen_t addrlen,
        net::EventLoop* event_loop
    );

    /**
     * Handle TLS connection with ALPN protocol detection
     */
    static void handle_tls_connection(
        net::TcpSocket socket,
        net::EventLoop* event_loop,
        std::shared_ptr<net::TlsContext> tls_context
    );

    /**
     * Handle HTTP/2 connection
     */
    static void handle_http2_connection(
        int fd,
        net::EventLoop* event_loop,
        http2::Http2Connection* http2_conn
    );

    /**
     * Handle HTTP/1.1 connection
     */
    static void handle_http1_connection(
        int fd,
        net::IOEvent events,
        net::EventLoop* event_loop,
        Http1Connection* http1_conn
    );

    UnifiedServerConfig config_;
    std::shared_ptr<net::TlsContext> tls_context_;
    std::unique_ptr<net::TcpListener> tls_listener_;
    std::unique_ptr<net::TcpListener> cleartext_listener_;
    std::unique_ptr<net::UdpListener> quic_listener_;  // UDP port for HTTP/3/QUIC
    std::thread tls_thread_;  // TLS listener runs in background if both TLS+cleartext enabled
    std::thread quic_thread_;  // QUIC listener runs in background if needed
    std::atomic<bool> shutdown_flag_{false};
    std::string error_message_;

    // Global request handler
    static HttpRequestHandler s_request_handler_;

    // Pure C++ WebSocket handlers (path → handler mapping)
    static std::unordered_map<std::string, WebSocketHandler> s_websocket_handlers_;

    // Helper to look up a C++ WebSocket handler
    static WebSocketHandler* get_websocket_handler(const std::string& path);
};

} // namespace http
} // namespace fasterapi
