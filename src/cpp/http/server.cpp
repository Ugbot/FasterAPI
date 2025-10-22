#include "server.h"
#include "request.h"
#include "response.h"
#include "router.h"
#include "http1_coroio_handler.h"
#include <thread>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <atomic>
#include <unordered_map>
#include <functional>

using namespace fasterapi::http;

class Http2Handler {
public:
    Http2Handler(HttpServer* server) : server_(server) {}
    
    int start(uint16_t port, const std::string& host) {
        // HTTP/2 implementation would go here
        // For now, just return success
        return 0;
    }
    
private:
    HttpServer* server_;
};

class Http3Handler {
public:
    Http3Handler(HttpServer* server) : server_(server) {}
    
    int start(uint16_t port, const std::string& host) {
        // HTTP/3 implementation would go here
        // For now, just return success
        return 0;
    }
    
private:
    HttpServer* server_;
};

class CompressionHandler {
public:
    CompressionHandler(const HttpServer::Config& config) : config_(config) {}
    
    bool should_compress(const std::string& content_type, uint64_t size) {
        if (!config_.enable_compression) return false;
        if (size < config_.compression_threshold) return false;
        
        // Check if content type is compressible
        return content_type.find("text/") == 0 || 
               content_type == "application/json" ||
               content_type == "application/javascript";
    }
    
    std::string compress(const std::string& data) {
        // Simple compression placeholder
        // Real implementation would use zstd
        return data;
    }
    
private:
    HttpServer::Config config_;
};

HttpServer::HttpServer(const Config& config) 
    : config_(config), num_cores_(std::thread::hardware_concurrency()) {
    
    // Initialize protocol handlers
    h1_handler_ = std::make_unique<Http1CoroioHandler>(this);
    h2_handler_ = std::make_unique<Http2Handler>(this);
    h3_handler_ = std::make_unique<Http3Handler>(this);
    compression_handler_ = std::make_unique<CompressionHandler>(config);
    
    // Initialize high-performance router
    router_ = std::make_unique<Router>();
}

HttpServer::~HttpServer() {
    stop();
}

HttpServer::HttpServer(HttpServer&& other) noexcept
    : config_(other.config_),
      running_(other.running_.load()),
      request_count_(other.request_count_.load()),
      bytes_sent_(other.bytes_sent_.load()),
      bytes_received_(other.bytes_received_.load()),
      active_connections_(other.active_connections_.load()),
      h1_requests_(other.h1_requests_.load()),
      h2_requests_(other.h2_requests_.load()),
      h3_requests_(other.h3_requests_.load()),
      websocket_connections_(other.websocket_connections_.load()),
      compressed_responses_(other.compressed_responses_.load()),
      compression_bytes_saved_(other.compression_bytes_saved_.load()),
      routes_(std::move(other.routes_)),
      websocket_handlers_(std::move(other.websocket_handlers_)),
      server_threads_(std::move(other.server_threads_)),
      num_cores_(other.num_cores_),
      h1_handler_(std::move(other.h1_handler_)),
      h2_handler_(std::move(other.h2_handler_)),
      h3_handler_(std::move(other.h3_handler_)),
      compression_handler_(std::move(other.compression_handler_)) {
}

HttpServer& HttpServer::operator=(HttpServer&& other) noexcept {
    if (this != &other) {
        stop();
        
        config_ = other.config_;
        running_ = other.running_.load();
        request_count_ = other.request_count_.load();
        bytes_sent_ = other.bytes_sent_.load();
        bytes_received_ = other.bytes_received_.load();
        active_connections_ = other.active_connections_.load();
        h1_requests_ = other.h1_requests_.load();
        h2_requests_ = other.h2_requests_.load();
        h3_requests_ = other.h3_requests_.load();
        websocket_connections_ = other.websocket_connections_.load();
        compressed_responses_ = other.compressed_responses_.load();
        compression_bytes_saved_ = other.compression_bytes_saved_.load();
        routes_ = std::move(other.routes_);
        websocket_handlers_ = std::move(other.websocket_handlers_);
        server_threads_ = std::move(other.server_threads_);
        num_cores_ = other.num_cores_;
        h1_handler_ = std::move(other.h1_handler_);
        h2_handler_ = std::move(other.h2_handler_);
        h3_handler_ = std::move(other.h3_handler_);
        compression_handler_ = std::move(other.compression_handler_);
    }
    return *this;
}

int HttpServer::add_route(const std::string& method, const std::string& path, RouteHandler handler) noexcept {
    if (running_.load()) {
        return 1;  // Cannot add routes while running
    }
    
    routes_[method][path] = std::move(handler);
    return 0;
}

int HttpServer::add_websocket(const std::string& path, WebSocketHandler handler) noexcept {
    if (running_.load()) {
        return 1;  // Cannot add routes while running
    }
    
    websocket_handlers_[path] = std::move(handler);
    return 0;
}

int HttpServer::start() noexcept {
    if (running_.load()) {
        return 1;  // Already running
    }
    
    // Start HTTP/1.1 server
    if (config_.enable_h1) {
        int result = start_h1_server();
        if (result != 0) return result;
    }
    
    // Start HTTP/2 server
    if (config_.enable_h2) {
        int result = start_h2_server();
        if (result != 0) return result;
    }
    
    // Start HTTP/3 server
    if (config_.enable_h3) {
        int result = start_h3_server();
        if (result != 0) return result;
    }
    
    running_.store(true);
    return 0;
}

int HttpServer::stop() noexcept {
    if (!running_.load()) {
        return 0;  // Already stopped
    }
    
    // Stop all server threads
    for (auto& thread : server_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    server_threads_.clear();
    
    running_.store(false);
    return 0;
}

bool HttpServer::is_running() const noexcept {
    return running_.load();
}

const std::unordered_map<std::string, std::unordered_map<std::string, HttpServer::RouteHandler>>&
HttpServer::get_routes() const noexcept {
    return routes_;
}

HttpServer::Stats HttpServer::get_stats() const noexcept {
    Stats stats;
    stats.total_requests = request_count_.load();
    stats.total_bytes_sent = bytes_sent_.load();
    stats.total_bytes_received = bytes_received_.load();
    stats.active_connections = active_connections_.load();
    stats.h1_requests = h1_requests_.load();
    stats.h2_requests = h2_requests_.load();
    stats.h3_requests = h3_requests_.load();
    stats.websocket_connections = websocket_connections_.load();
    stats.compressed_responses = compressed_responses_.load();
    stats.compression_bytes_saved = compression_bytes_saved_.load();
    return stats;
}

int HttpServer::start_h1_server() noexcept {
    if (!h1_handler_) {
        return 1;
    }

    // Start HTTP/1.1 multi-threaded server with EventLoopPool
    // Linux: Each worker uses SO_REUSEPORT for kernel-level load balancing
    // Non-Linux: Acceptor thread + lockfree queue distribution
    return h1_handler_->start(
        config_.port,
        config_.host,
        config_.num_worker_threads,
        config_.worker_queue_size
    );
}

int HttpServer::start_h2_server() noexcept {
    if (!h2_handler_) {
        return 1;
    }
    
    // Start HTTP/2 server in a separate thread
    server_threads_.emplace_back([this]() {
        h2_handler_->start(config_.port + 1, config_.host);  // Use different port
    });
    
    return 0;
}

int HttpServer::start_h3_server() noexcept {
    if (!h3_handler_) {
        return 1;
    }
    
    // Start HTTP/3 server in a separate thread
    server_threads_.emplace_back([this]() {
        h3_handler_->start(config_.port + 2, config_.host);  // Use different port
    });
    
    return 0;
}

void HttpServer::handle_request(HttpRequest* request, HttpResponse* response) noexcept {
    request_count_.fetch_add(1);
    
    // Determine protocol and increment counter
    const std::string& protocol = request->get_protocol();
    if (protocol == "HTTP/1.1") {
        h1_requests_.fetch_add(1);
    } else if (protocol == "HTTP/2.0") {
        h2_requests_.fetch_add(1);
    } else if (protocol == "HTTP/3.0") {
        h3_requests_.fetch_add(1);
    }
    
    // Find matching route handler
    const std::string& method = request->get_method() == HttpRequest::Method::GET ? "GET" : "POST";
    const std::string& path = request->get_path();
    
    auto method_routes = routes_.find(method);
    if (method_routes != routes_.end()) {
        auto route_handler = method_routes->second.find(path);
        if (route_handler != method_routes->second.end()) {
            route_handler->second(request, response);
            return;
        }
    }
    
    // No route found, return 404
    response->status(HttpResponse::Status::NOT_FOUND)
           .content_type("application/json")
           .json("{\"error\":\"Not Found\"}")
           .send();
}

uint64_t HttpServer::get_time_ns() const noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}
