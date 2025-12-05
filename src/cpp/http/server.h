#pragma once

#include <memory>
#include <string>
#include <functional>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <thread>
#include <cstdint>

// Forward declarations
struct HttpRequest;
struct HttpResponse;
struct WebSocketHandler;

namespace fasterapi {
namespace http {
    class Router;
    class RouteParams;
    class UnifiedServer;
}
}

/**
 * High-performance multi-protocol HTTP server.
 * 
 * Supports:
 * - HTTP/1.1 (uWebSockets)
 * - HTTP/2 (nghttp2 + ALPN)
 * - HTTP/3 (MsQuic, optional)
 * - WebSocket
 * - zstd compression
 * - Per-core event loops
 */
class HttpServer {
public:
    // Server configuration
    struct Config {
        uint16_t port = 8070;
        std::string host = "0.0.0.0";
        bool enable_h1 = true;
        bool enable_h2 = false;
        bool enable_h3 = false;
        bool enable_webtransport = false;
        uint16_t http3_port = 443;
        bool enable_compression = true;
        bool enable_websocket = true;

        // TLS configuration
        std::string cert_path;
        std::string key_path;

        // Performance settings
        uint32_t max_connections = 10000;
        uint32_t max_request_size = 16 * 1024 * 1024;  // 16MB
        uint32_t compression_threshold = 1024;  // 1KB
        uint32_t compression_level = 3;  // zstd level

        // Multi-threading configuration (HTTP/1.1 with CoroIO)
        uint16_t num_worker_threads = 0;  // 0 = auto (hardware_concurrency - 2)
        size_t worker_queue_size = 1024;   // Per-worker queue size (non-Linux platforms)
    };

    // Route handler function type (matches fasterapi::http::Router signature)
    using RouteHandler = std::function<void(HttpRequest*, HttpResponse*, const fasterapi::http::RouteParams&)>;
    
    // WebSocket handler function type
    using WebSocketHandler = std::function<void(WebSocketHandler*)>;

    /**
     * Create a new HTTP server.
     * 
     * @param config Server configuration
     */
    explicit HttpServer(const Config& config);
    
    ~HttpServer();

    // Non-copyable, movable
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    HttpServer(HttpServer&&) noexcept;
    HttpServer& operator=(HttpServer&&) noexcept;

    /**
     * Add a route handler.
     * 
     * @param method HTTP method (GET, POST, etc.)
     * @param path Route path pattern
     * @param handler Handler function
     * @return Error code (0 = success)
     */
    int add_route(const std::string& method, const std::string& path, RouteHandler handler) noexcept;

    /**
     * Add a WebSocket endpoint.
     * 
     * @param path WebSocket path
     * @param handler WebSocket handler
     * @return Error code (0 = success)
     */
    int add_websocket(const std::string& path, WebSocketHandler handler) noexcept;

    /**
     * Start the server.
     * 
     * @return Error code (0 = success)
     */
    int start() noexcept;

    /**
     * Stop the server.
     * 
     * @return Error code (0 = success)
     */
    int stop() noexcept;

    /**
     * Check if server is running.
     *
     * @return true if running, false otherwise
     */
    bool is_running() const noexcept;

    /**
     * Get registered routes map (for handler lookup).
     *
     * @return Reference to routes map (method -> path -> handler)
     */
    const std::unordered_map<std::string, std::unordered_map<std::string, RouteHandler>>& get_routes() const noexcept;

    /**
     * Set App instance for direct HTTP/1.1 handling (simplified path).
     *
     * @param app Pointer to App instance
     */
    void set_app_instance(void* app) noexcept;

    /**
     * Get router for route matching (used by App::handle_http1).
     *
     * @return Pointer to router, or nullptr if not initialized
     */
    fasterapi::http::Router* get_router() noexcept {
        return router_.get();
    }

    /**
     * Get server statistics.
     *
     * @return Statistics structure
     */
    struct Stats {
        uint64_t total_requests{0};
        uint64_t total_bytes_sent{0};
        uint64_t total_bytes_received{0};
        uint64_t active_connections{0};
        uint64_t h1_requests{0};
        uint64_t h2_requests{0};
        uint64_t h3_requests{0};
        uint64_t websocket_connections{0};
        uint64_t compressed_responses{0};
        uint64_t compression_bytes_saved{0};
    };
    
    Stats get_stats() const noexcept;

private:
    Config config_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> request_count_{0};
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<uint64_t> bytes_received_{0};
    std::atomic<uint64_t> active_connections_{0};
    
    // Protocol-specific request counters
    std::atomic<uint64_t> h1_requests_{0};
    std::atomic<uint64_t> h2_requests_{0};
    std::atomic<uint64_t> h3_requests_{0};
    std::atomic<uint64_t> websocket_connections_{0};
    
    // Compression statistics
    std::atomic<uint64_t> compressed_responses_{0};
    std::atomic<uint64_t> compression_bytes_saved_{0};
    
    // Route handlers
    std::unordered_map<std::string, std::unordered_map<std::string, RouteHandler>> routes_;
    std::unordered_map<std::string, WebSocketHandler> websocket_handlers_;
    
    // High-performance router
    std::unique_ptr<fasterapi::http::Router> router_;

    // Unified HTTP/1.1 + HTTP/2 server (replaces protocol-specific handlers)
    std::unique_ptr<fasterapi::http::UnifiedServer> unified_server_;

    /**
     * Bridge handler from UnifiedServer callback to our Router-based handlers.
     *
     * @param method HTTP method
     * @param path Request path
     * @param headers Request headers
     * @param body Request body
     * @param send_response Callback to send response
     */
    void handle_unified_request(
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body,
        std::function<void(uint16_t, const std::unordered_map<std::string, std::string>&, const std::string&)> send_response
    ) noexcept;

    /**
     * Handle incoming request.
     * 
     * @param request Request object
     * @param response Response object
     */
    void handle_request(HttpRequest* request, HttpResponse* response) noexcept;

    /**
     * Get current time in nanoseconds.
     * 
     * @return Current time in nanoseconds
     */
    uint64_t get_time_ns() const noexcept;
};