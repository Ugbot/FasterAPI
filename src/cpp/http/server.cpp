#include "server.h"
#include "request.h"
#include "response.h"
#include "router.h"
#include "unified_server.h"
#include "core/logger.h"
#include <thread>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <memory>
#include <atomic>
#include <unordered_map>
#include <functional>

using namespace fasterapi::http;

HttpServer::HttpServer(const Config& config) : config_(config) {
    // Initialize high-performance router (exception-free allocation)
    router_.reset(new (std::nothrow) Router());

    if (!router_) {
        LOG_ERROR("Server", "FATAL: Failed to allocate Router");
        // Server will be non-functional without router
    }

    // UnifiedServer is created in start() after routes are registered
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
      router_(std::move(other.router_)),
      unified_server_(std::move(other.unified_server_)) {
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
        router_ = std::move(other.router_);
        unified_server_ = std::move(other.unified_server_);
    }
    return *this;
}

int HttpServer::add_route(const std::string& method, const std::string& path, RouteHandler handler) noexcept {
    if (running_.load()) {
        return 1;  // Cannot add routes while running
    }

    if (!router_) {
        return -1;  // Router failed to allocate
    }

    // Add to router for efficient matching
    int result = router_->add_route(method, path, handler);
    if (result != 0) {
        return result;
    }

    // Also store in routes map for introspection
    routes_[method][path] = handler;

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

    if (!router_) {
        LOG_ERROR("Server", "Cannot start server - router not initialized");
        return -1;
    }

    // Map HttpServer::Config to UnifiedServerConfig
    UnifiedServerConfig unified_config;
    unified_config.host = config_.host;
    unified_config.http1_port = config_.port;
    unified_config.enable_http1_cleartext = config_.enable_h1;

    // Enable TLS if HTTP/2 requested
    // UnifiedServer will auto-generate certificates if none provided
    unified_config.enable_tls = config_.enable_h2;
    if (unified_config.enable_tls) {
        // Pass certificate paths if provided (otherwise UnifiedServer auto-generates)
        unified_config.cert_file = config_.cert_path;
        unified_config.key_file = config_.key_path;
        unified_config.tls_port = config_.port + 1;  // Use separate port for HTTPS
        // Configure ALPN protocols for HTTP/2
        unified_config.alpn_protocols = {"h2", "http/1.1"};
    }

    // Worker configuration
    unified_config.num_workers = config_.num_worker_threads;

    // Create UnifiedServer with exception-free allocation
    unified_server_.reset(new (std::nothrow) UnifiedServer(unified_config));
    if (!unified_server_) {
        LOG_ERROR("Server", "FATAL: Failed to allocate UnifiedServer (out of memory)");
        return -1;
    }

    // Register bridge handler that connects UnifiedServer to our Router
    auto bridge_handler = [this](
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body,
        std::function<void(uint16_t, const std::unordered_map<std::string, std::string>&, const std::string&)> send_response
    ) {
        this->handle_unified_request(method, path, headers, body, send_response);
    };

    unified_server_->set_request_handler(bridge_handler);

    // Start the unified server (blocking call - should be in separate thread)
    // TODO: Consider making this non-blocking or running in thread pool
    int result = unified_server_->start();
    if (result == 0) {
        running_.store(true);
    }

    return result;
}

int HttpServer::stop() noexcept {
    if (!running_.load()) {
        return 0;  // Already stopped
    }

    if (unified_server_) {
        unified_server_->stop();
    }

    running_.store(false);
    return 0;
}

bool HttpServer::is_running() const noexcept {
    if (unified_server_) {
        return unified_server_->is_running();
    }
    return running_.load();
}

const std::unordered_map<std::string, std::unordered_map<std::string, HttpServer::RouteHandler>>&
HttpServer::get_routes() const noexcept {
    return routes_;
}

void HttpServer::set_app_instance(void* app) noexcept {
    if (unified_server_) {
        unified_server_->set_app_instance(app);
    }
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

void HttpServer::handle_unified_request(
    const std::string& method,
    const std::string& path,
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& body,
    std::function<void(uint16_t, const std::unordered_map<std::string, std::string>&, const std::string&)> send_response
) noexcept {
    LOG_DEBUG("Server", "handle_unified_request: %s %s", method.c_str(), path.c_str());

    // Increment request counter
    request_count_.fetch_add(1);

    // Determine protocol from headers (if available)
    auto protocol_it = headers.find(":protocol");
    if (protocol_it != headers.end()) {
        const std::string& protocol = protocol_it->second;
        if (protocol == "HTTP/1.1") {
            h1_requests_.fetch_add(1);
        } else if (protocol == "HTTP/2.0" || protocol == "h2") {
            h2_requests_.fetch_add(1);
        }
    }

    // Create HttpRequest from parsed data
    HttpRequest request = HttpRequest::from_parsed_data(method, path, headers, body);
    LOG_DEBUG("Server", "Created HttpRequest");

    // Create HttpResponse
    HttpResponse response;
    LOG_DEBUG("Server", "Created HttpResponse");

    // Match route and get handler
    fasterapi::http::RouteParams params;
    fasterapi::http::RouteHandler handler = router_->match(method, path, params);

    if (handler) {
        LOG_DEBUG("Server", "Found handler, calling it...");
        // Call user's route handler with path parameters
        handler(&request, &response, params);
        LOG_DEBUG("Server", "Handler returned");
    } else {
        LOG_INFO("Server", "No handler found for %s %s, returning 404", method.c_str(), path.c_str());
        // No route found - return 404
        response.status(HttpResponse::Status::NOT_FOUND)
               .content_type("application/json")
               .json("{\"error\":\"Not Found\"}")
               .send();
    }

    // Extract response data and send via UnifiedServer callback
    uint16_t status_code = static_cast<uint16_t>(response.get_status_code());
    const auto& response_headers = response.get_headers();
    const auto& response_body = response.get_body();

    LOG_DEBUG("Server", "Sending response: %d body_size=%zu", status_code, response_body.size());
    send_response(status_code, response_headers, response_body);
    LOG_DEBUG("Server", "Response sent");
}


uint64_t HttpServer::get_time_ns() const noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}
