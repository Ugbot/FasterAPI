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
    class Http1CoroioHandler;
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

    // Route handler function type
    using RouteHandler = std::function<void(HttpRequest*, HttpResponse*)>;
    
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
    
    // High-performance router (new implementation)
    std::unique_ptr<fasterapi::http::Router> router_;
    
    // Server threads (one per core)
    std::vector<std::thread> server_threads_;
    uint32_t num_cores_;
    
    // Protocol handlers
    std::unique_ptr<fasterapi::http::Http1CoroioHandler> h1_handler_;
    std::unique_ptr<class Http2Handler> h2_handler_;
    std::unique_ptr<class Http3Handler> h3_handler_;
    std::unique_ptr<class CompressionHandler> compression_handler_;

    /**
     * Initialize protocol handlers.
     * 
     * @return Error code (0 = success)
     */
    int init_handlers() noexcept;

    /**
     * Start HTTP/1.1 server.
     * 
     * @return Error code (0 = success)
     */
    int start_h1_server() noexcept;

    /**
     * Start HTTP/2 server.
     * 
     * @return Error code (0 = success)
     */
    int start_h2_server() noexcept;

    /**
     * Start HTTP/3 server.
     * 
     * @return Error code (0 = success)
     */
    int start_h3_server() noexcept;

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