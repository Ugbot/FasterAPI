/**
 * @file app.cpp
 * @brief Implementation of high-level FasterAPI application interface
 */

#include "app.h"
#include "unified_server.h"
#include "core/logger.h"
#include <sstream>
#include <algorithm>
#include <iostream>
#include <cstdio>

namespace fasterapi {

// =============================================================================
// Request Implementation
// =============================================================================

Request::Request(HttpRequest* req, const http::RouteParams& params)
    : req_(req), params_(params) {}

std::string Request::method() const {
    // Convert Method enum to string
    auto m = req_->get_method();
    switch (m) {
        case HttpRequest::Method::GET: return "GET";
        case HttpRequest::Method::POST: return "POST";
        case HttpRequest::Method::PUT: return "PUT";
        case HttpRequest::Method::DELETE: return "DELETE";
        case HttpRequest::Method::PATCH: return "PATCH";
        case HttpRequest::Method::HEAD: return "HEAD";
        case HttpRequest::Method::OPTIONS: return "OPTIONS";
        case HttpRequest::Method::CONNECT: return "CONNECT";
        case HttpRequest::Method::TRACE: return "TRACE";
        default: return "UNKNOWN";
    }
}

std::string Request::path() const {
    return req_->get_path();
}

std::string Request::query_string() const {
    return req_->get_query();
}

std::string Request::header(const std::string& name) const {
    return req_->get_header(name);
}

std::map<std::string, std::string> Request::headers() const {
    const auto& headers = req_->get_headers();
    return std::map<std::string, std::string>(headers.begin(), headers.end());
}

std::string Request::path_param(const std::string& name) const {
    return params_.get(name);
}

std::string Request::query_param(const std::string& name) const {
    return req_->get_query_param(name);
}

std::optional<std::string> Request::path_param_optional(const std::string& name) const {
    auto value = params_.get(name);
    if (value.empty()) return std::nullopt;
    return value;
}

std::optional<std::string> Request::query_param_optional(const std::string& name) const {
    auto value = req_->get_query_param(name);
    if (value.empty()) return std::nullopt;
    return value;
}

std::string Request::body() const {
    return req_->get_body();
}

std::string Request::json_body() const {
    // TODO: Parse and validate JSON
    return req_->get_body();
}

std::string Request::client_ip() const {
    // Try X-Forwarded-For first, then X-Real-IP, then direct connection
    auto forwarded = header("X-Forwarded-For");
    if (!forwarded.empty()) {
        // Take first IP from comma-separated list
        auto comma = forwarded.find(',');
        if (comma != std::string::npos) {
            return forwarded.substr(0, comma);
        }
        return forwarded;
    }

    auto real_ip = header("X-Real-IP");
    if (!real_ip.empty()) {
        return real_ip;
    }

    // TODO: Get actual client IP from connection
    return "0.0.0.0";
}

std::string Request::user_agent() const {
    return header("User-Agent");
}

// =============================================================================
// Response Implementation
// =============================================================================

Response::Response(HttpResponse* res) : res_(res) {}

Response& Response::status(int code) {
    res_->status(static_cast<HttpResponse::Status>(code));
    return *this;
}

Response& Response::header(const std::string& name, const std::string& value) {
    res_->header(name, value);
    return *this;
}

Response& Response::content_type(const std::string& type) {
    res_->content_type(type);
    return *this;
}

Response& Response::send(const std::string& body) {
    res_->content_type("text/plain");
    res_->text(body);
    res_->send();
    sent_ = true;
    return *this;
}

Response& Response::json(const std::string& json_str) {
    LOG_DEBUG("App", "json(string) called with: %s", json_str.c_str());
    content_type("application/json");
    LOG_DEBUG("App", "calling send()");
    send(json_str);
    LOG_DEBUG("App", "send() returned");
    return *this;
}

Response& Response::json(const std::vector<std::pair<std::string, std::string>>& pairs) {
    std::ostringstream oss;
    oss << "{";
    for (size_t i = 0; i < pairs.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << pairs[i].first << "\":\"" << pairs[i].second << "\"";
    }
    oss << "}";
    return json(oss.str());
}

Response& Response::json(std::initializer_list<std::pair<const char*, const char*>> pairs) {
    LOG_DEBUG("App", "json(initializer_list) called with %zu pairs", pairs.size());
    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (const auto& pair : pairs) {
        if (!first) oss << ",";
        first = false;
        oss << "\"" << pair.first << "\":\"" << pair.second << "\"";
    }
    oss << "}";
    LOG_DEBUG("App", "json(initializer_list) built JSON: %s", oss.str().c_str());
    return json(oss.str());
}

Response& Response::html(const std::string& html_str) {
    content_type("text/html; charset=utf-8");
    send(html_str);
    return *this;
}

Response& Response::text(const std::string& text_str) {
    content_type("text/plain; charset=utf-8");
    send(text_str);
    return *this;
}

Response& Response::file(const std::string& path) {
    // TODO: Implement file serving with proper MIME type detection
    res_->file(path);
    sent_ = true;
    return *this;
}

Response& Response::cors(const std::string& origin) {
    header("Access-Control-Allow-Origin", origin);
    header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, PATCH, OPTIONS");
    header("Access-Control-Allow-Headers", "Content-Type, Authorization");
    return *this;
}

Response& Response::cookie(
    const std::string& name,
    const std::string& value,
    int max_age,
    const std::string& path,
    bool http_only,
    bool secure,
    const std::string& same_site
) {
    std::ostringstream cookie;
    cookie << name << "=" << value;

    if (max_age >= 0) {
        cookie << "; Max-Age=" << max_age;
    }

    if (!path.empty()) {
        cookie << "; Path=" << path;
    }

    if (http_only) {
        cookie << "; HttpOnly";
    }

    if (secure) {
        cookie << "; Secure";
    }

    if (!same_site.empty()) {
        cookie << "; SameSite=" << same_site;
    }

    header("Set-Cookie", cookie.str());
    return *this;
}

Response& Response::redirect(const std::string& url, int code) {
    status(code);
    header("Location", url);
    send("");
    return *this;
}

Response& Response::stream_start() {
    header("Transfer-Encoding", "chunked");
    res_->stream(); // Initialize streaming mode
    return *this;
}

Response& Response::stream_chunk(const std::string& chunk) {
    // Format as HTTP chunked encoding: "{size in hex}\r\n{data}\r\n"
    if (!chunk.empty()) {
        // Convert size to hex
        char size_buf[32];
        snprintf(size_buf, sizeof(size_buf), "%zx\r\n", chunk.size());

        // Send chunk header
        res_->write(size_buf);

        // Send chunk data
        res_->write(chunk);

        // Send chunk trailer
        res_->write("\r\n");
    }
    return *this;
}

Response& Response::stream_end() {
    // Send final chunk: "0\r\n\r\n"
    res_->write("0\r\n\r\n");
    res_->end();
    return *this;
}

// =============================================================================
// RouteBuilder Implementation
// =============================================================================

RouteBuilder::RouteBuilder(App* app, const std::string& method, const std::string& path)
    : app_(app), method_(method), path_(path) {}

RouteBuilder& RouteBuilder::tag(const std::string& tag) {
    tags_.push_back(tag);
    return *this;
}

RouteBuilder& RouteBuilder::summary(const std::string& summary) {
    summary_ = summary;
    return *this;
}

RouteBuilder& RouteBuilder::description(const std::string& desc) {
    description_ = desc;
    return *this;
}

RouteBuilder& RouteBuilder::response_model(int status_code, const std::string& schema) {
    response_models_[status_code] = schema;
    return *this;
}

RouteBuilder& RouteBuilder::use(MiddlewareFunc middleware) {
    middleware_.push_back(middleware);
    return *this;
}

RouteBuilder& RouteBuilder::require_auth() {
    // Add authentication middleware
    middleware_.push_back([](Request& req, Response& res, auto next) {
        auto auth = req.header("Authorization");
        if (auth.empty()) {
            res.unauthorized().json({{"error", "Authentication required"}});
            return;
        }
        next();
    });
    return *this;
}

RouteBuilder& RouteBuilder::require_role(const std::string& role) {
    // Add role-based authorization middleware
    middleware_.push_back([role](Request& req, Response& res, auto next) {
        // TODO: Implement role checking logic
        next();
    });
    return *this;
}

RouteBuilder& RouteBuilder::rate_limit(int requests_per_minute) {
    // Add rate limiting middleware
    middleware_.push_back([requests_per_minute](Request& req, Response& res, auto next) {
        // TODO: Implement rate limiting logic
        next();
    });
    return *this;
}

void RouteBuilder::handler(Handler h) {
    App::RouteMetadata metadata;
    metadata.method = method_;
    metadata.path = path_;
    metadata.tags = tags_;
    metadata.summary = summary_;
    metadata.description = description_;
    metadata.response_models = response_models_;

    // Pass middleware as separate parameter instead of in metadata
    app_->register_route(method_, path_, h, metadata, middleware_);
}

// =============================================================================
// App Implementation
// =============================================================================

App::App() : App(Config{}) {}

App::App(const Config& config) : config_(config) {
    // Create server configuration
    HttpServer::Config server_config;
    server_config.host = "0.0.0.0";
    server_config.port = 8000;
    server_config.enable_h1 = true;
    server_config.enable_h2 = config_.enable_http2;
    server_config.enable_h3 = config_.enable_http3;
    server_config.enable_webtransport = config_.enable_webtransport;
    server_config.http3_port = config_.http3_port;
    server_config.enable_compression = config_.enable_compression;
    server_config.cert_path = config_.cert_path;
    server_config.key_path = config_.key_path;
    server_config.max_connections = config_.max_connections;
    server_config.max_request_size = config_.max_request_size;

    // Exception-free allocation using std::nothrow
    server_.reset(new (std::nothrow) HttpServer(server_config));
    if (!server_) {
        LOG_ERROR("App", "Failed to allocate HttpServer (out of memory)");
        // Constructor failure - server will be non-functional
        // User should check is_running() before using
    }
    // Note: set_app_instance will be called in start() when server is actually started

    // Initialize default routes if enabled
    if (config_.enable_docs) {
        init_default_routes();
    }
}

App::~App() {
    if (is_running()) {
        stop();
    }

    // CRITICAL: Destroy server FIRST before clearing route metadata
    // The server owns the router which owns handlers that may reference route_metadata_
    // We must ensure all handlers are destroyed before route_metadata_ is cleared
    if (server_) {
        server_->set_app_instance(nullptr);
    }

    // Explicitly destroy server_ to ensure proper cleanup order
    // This destroys the router and all route handlers
    // BEFORE route_metadata_ and other App members are destroyed
    server_.reset();

    // NOW it's safe to clear route metadata
    // All handlers that might reference this data have been destroyed
    route_metadata_.clear();
    global_middleware_.clear();
    path_middleware_.clear();
}

App::App(App&&) noexcept = default;
App& App::operator=(App&&) noexcept = default;

// Route registration methods

App& App::get(const std::string& path, Handler handler) {
    register_route("GET", path, handler);
    return *this;
}

App& App::post(const std::string& path, Handler handler) {
    register_route("POST", path, handler);
    return *this;
}

App& App::put(const std::string& path, Handler handler) {
    register_route("PUT", path, handler);
    return *this;
}

App& App::del(const std::string& path, Handler handler) {
    register_route("DELETE", path, handler);
    return *this;
}

App& App::patch(const std::string& path, Handler handler) {
    register_route("PATCH", path, handler);
    return *this;
}

App& App::head(const std::string& path, Handler handler) {
    register_route("HEAD", path, handler);
    return *this;
}

App& App::options(const std::string& path, Handler handler) {
    register_route("OPTIONS", path, handler);
    return *this;
}

RouteBuilder App::route(const std::string& method, const std::string& path) {
    return RouteBuilder(this, method, path);
}

App& App::websocket(const std::string& path, WSHandler handler) {
    // Store handler for run_unified() to transfer to UnifiedServer
    websocket_handlers_[path] = handler;

    // Create connection ID counter (static to persist across calls)
    static std::atomic<uint64_t> connection_id_counter{0};

    // Wrap WebSocket handler for HttpServer
    auto wrapped = [handler](WebSocketHandler* ws_raw) {
        // Create connection ID
        uint64_t conn_id = connection_id_counter.fetch_add(1, std::memory_order_relaxed);

        // Create WebSocketConnection wrapper (exception-free allocation)
        http::WebSocketConnection::Config config;
        std::unique_ptr<http::WebSocketConnection> ws_conn(
            new (std::nothrow) http::WebSocketConnection(conn_id, config)
        );
        if (!ws_conn) {
            LOG_ERROR("App", "Failed to allocate WebSocketConnection (out of memory)");
            return; // Graceful failure - connection will not be established
        }

        // Set up bidirectional connection between raw handler and our wrapper
        // The raw WebSocketHandler provides the actual I/O, our wrapper provides the API

        // Call user's handler with our clean API
        handler(*ws_conn);

        // The WebSocketConnection will remain alive until the handler returns
        // or the connection is explicitly closed
    };

    server_->add_websocket(path, wrapped);
    return *this;
}

App& App::sse(const std::string& path, SSEHandler handler) {
    // Create connection ID counter (static to persist across calls)
    static std::atomic<uint64_t> sse_connection_id_counter{0};

    // Register SSE endpoint as a regular GET route with special handling
    register_route("GET", path, [handler](Request& req, Response& res) {
        // Set SSE headers
        res.header("Content-Type", "text/event-stream");
        res.header("Cache-Control", "no-cache");
        res.header("Connection", "keep-alive");
        res.header("X-Accel-Buffering", "no"); // Disable nginx buffering

        // Create connection ID
        uint64_t conn_id = sse_connection_id_counter.fetch_add(1, std::memory_order_relaxed);

        // Create SSE connection (exception-free allocation)
        std::unique_ptr<http::SSEConnection> sse_conn(
            new (std::nothrow) http::SSEConnection(conn_id)
        );
        if (!sse_conn) {
            LOG_ERROR("App", "Failed to allocate SSEConnection (out of memory)");
            res.internal_error().json({{"error", "Failed to establish SSE connection"}});
            return; // Graceful failure
        }

        // Check for Last-Event-ID header (for client reconnection)
        auto last_event_id = req.header("Last-Event-ID");
        if (!last_event_id.empty()) {
            sse_conn->set_last_event_id(last_event_id);
        }

        // Start streaming response
        res.stream_start();

        // Call user's handler - this will block while sending events
        handler(*sse_conn);

        // Connection closed - either by handler or client
        res.stream_end();
    });
    return *this;
}

App& App::use(MiddlewareFunc middleware) {
    global_middleware_.push_back(middleware);
    return *this;
}

App& App::use(const std::string& path, MiddlewareFunc middleware) {
    path_middleware_[path].push_back(middleware);
    return *this;
}

App& App::static_files(const std::string& url_path, const std::string& directory_path) {
    static_paths_[url_path] = directory_path;

    // Register wildcard route for static files
    std::string pattern = url_path;
    if (pattern.back() != '/') pattern += "/";
    pattern += "*path";

    register_route("GET", pattern, [directory_path](Request& req, Response& res) {
        auto path = req.path_param("path");
        auto full_path = directory_path + "/" + path;
        res.file(full_path);
    });

    return *this;
}

App& App::mount(const std::string& path, App& sub_app) {
    // Normalize path (ensure it starts with / and doesn't end with /)
    std::string prefix = path;
    if (prefix.empty() || prefix[0] != '/') {
        prefix = "/" + prefix;
    }
    if (prefix.length() > 1 && prefix.back() == '/') {
        prefix.pop_back();
    }

    // Get all routes from sub-app
    auto sub_routes = sub_app.routes();

    // Register each route with the prefix
    for (const auto& [method, route_path] : sub_routes) {
        std::string full_path = prefix + route_path;

        // Copy the route metadata if available
        // For now, we'll re-register as a simple route
        // In production, we'd want to copy middleware and metadata too

        // Find the route in sub_app's server and copy it
        // Since we don't have direct access to handlers, we need a different approach:
        // Store the sub_app reference and forward requests to it

        register_route(method, full_path, [&sub_app, route_path](Request& req, Response& res) {
            // Forward to sub-app's handler
            // This is a simplification - in production we'd need to properly
            // extract and call the sub-app's handler

            // For now, just document that this needs sub-app handler extraction
            res.internal_error().json({
                {"error", "Sub-app mounting requires handler extraction"},
                {"todo", "Implement handler forwarding"}
            });
        });
    }

    // Copy middleware from sub-app
    // TODO: Properly merge middleware chains

    return *this;
}

int App::run(const std::string& host, uint16_t port) {
    int result = start(host, port);
    if (result != 0) {
        return result;
    }

    LOG_INFO("App", "FasterAPI application started on http://%s:%d", host.c_str(), port);
    if (config_.enable_docs) {
        LOG_INFO("App", "Documentation: http://%s:%d%s", host.c_str(), port, config_.docs_url.c_str());
        LOG_INFO("App", "OpenAPI spec: http://%s:%d%s", host.c_str(), port, config_.openapi_url.c_str());
    }

    // Block forever (until signal)
    while (is_running()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}

int App::start(const std::string& host, uint16_t port) {
    // Update server config
    // TODO: Expose method to update host/port on server
    return server_->start();
}

int App::stop() {
    return server_->stop();
}

bool App::is_running() const {
    return server_->is_running();
}

std::vector<std::pair<std::string, std::string>> App::routes() const {
    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(route_metadata_.size());
    for (const auto& meta : route_metadata_) {
        result.emplace_back(meta.method, meta.path);
    }
    return result;
}

std::string App::openapi_spec() const {
    // Generate OpenAPI 3.0 specification
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"openapi\": \"3.0.0\",\n";
    oss << "  \"info\": {\n";
    oss << "    \"title\": \"" << config_.title << "\",\n";
    oss << "    \"version\": \"" << config_.version << "\",\n";
    oss << "    \"description\": \"" << config_.description << "\"\n";
    oss << "  },\n";
    oss << "  \"paths\": {\n";

    bool first_path = true;
    for (const auto& meta : route_metadata_) {
        if (!first_path) oss << ",\n";
        first_path = false;

        oss << "    \"" << meta.path << "\": {\n";
        oss << "      \"" << meta.method << "\": {\n";

        if (!meta.summary.empty()) {
            oss << "        \"summary\": \"" << meta.summary << "\",\n";
        }

        if (!meta.description.empty()) {
            oss << "        \"description\": \"" << meta.description << "\",\n";
        }

        if (!meta.tags.empty()) {
            oss << "        \"tags\": [";
            for (size_t i = 0; i < meta.tags.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << "\"" << meta.tags[i] << "\"";
            }
            oss << "],\n";
        }

        oss << "        \"responses\": {}\n";
        oss << "      }\n";
        oss << "    }";
    }

    oss << "\n  }\n";
    oss << "}\n";

    return oss.str();
}

HttpServer::Stats App::stats() const {
    return server_->get_stats();
}

// Private methods

void App::register_route(
    const std::string& method,
    const std::string& path,
    Handler handler,
    const RouteMetadata& metadata,
    const std::vector<MiddlewareFunc>& route_middleware
) {
    LOG_DEBUG("Router", "Registering: %s %s", method.c_str(), path.c_str());

    // Copy metadata fields BEFORE any operations that might reallocate route_metadata_
    // to avoid potential reference invalidation
    auto tags = metadata.tags;
    auto summary = metadata.summary;
    auto description = metadata.description;
    auto response_models = metadata.response_models;

    // Store metadata for OpenAPI generation
    RouteMetadata meta;
    meta.method = method;
    meta.path = path;
    meta.tags = std::move(tags);
    meta.summary = std::move(summary);
    meta.description = std::move(description);
    meta.response_models = std::move(response_models);
    route_metadata_.emplace_back(std::move(meta));

    // Wrap handler with middleware
    auto wrapped = wrap_handler(handler, route_middleware);

    // Register with server
    int result = server_->add_route(method, path, wrapped);
    if (result != 0) {
        LOG_ERROR("Router", "add_route returned %d for %s %s", result, method.c_str(), path.c_str());
    }
}

HttpServer::RouteHandler App::wrap_handler(
    Handler user_handler,
    const std::vector<MiddlewareFunc>& route_middleware
) {
    // Capture middleware and config by value to avoid accessing App members during lambda destruction
    auto global_mw = global_middleware_;
    auto path_mw = path_middleware_;
    bool enable_cors = config_.enable_cors;
    std::string cors_origin = config_.cors_origin;

    return [user_handler, route_middleware, global_mw, path_mw, enable_cors, cors_origin](
        HttpRequest* req,
        HttpResponse* res,
        const http::RouteParams& params
    ) {
        // RouteParams now passed from Router via HttpServer bridge

        // Create wrapper objects
        Request request(req, params);
        Response response(res);

        // Build middleware chain
        std::vector<MiddlewareFunc> all_middleware;

        // Add global middleware
        all_middleware.insert(all_middleware.end(),
            global_mw.begin(), global_mw.end());

        // Add path-specific middleware
        for (const auto& [prefix, middleware_list] : path_mw) {
            if (request.path().find(prefix) == 0) {
                all_middleware.insert(all_middleware.end(),
                    middleware_list.begin(), middleware_list.end());
            }
        }

        // Add route-specific middleware
        all_middleware.insert(all_middleware.end(),
            route_middleware.begin(), route_middleware.end());

        // Execute middleware chain
        size_t middleware_index = 0;
        std::function<void()> next;

        next = [&]() {
            LOG_DEBUG("App", "next() middleware_index=%zu total=%zu", middleware_index, all_middleware.size());
            if (middleware_index < all_middleware.size()) {
                auto& middleware = all_middleware[middleware_index++];
                LOG_DEBUG("App", "Calling middleware %zu", middleware_index - 1);
                middleware(request, response, next);
                LOG_DEBUG("App", "Middleware %zu returned", middleware_index - 1);
            } else {
                // All middleware executed, call the actual handler
                LOG_DEBUG("App", "Calling user handler");
                LOG_DEBUG("App", "user_handler valid: %s", (user_handler ? "yes" : "no"));
                LOG_DEBUG("App", "request ptr: %p", (void*)&request);
                LOG_DEBUG("App", "response ptr: %p", (void*)&response);
                LOG_DEBUG("App", "req_ ptr: %p", (void*)req);
                LOG_DEBUG("App", "res_ ptr: %p", (void*)res);
                LOG_DEBUG("App", "About to call user_handler function...");
                user_handler(request, response);
                LOG_DEBUG("App", "User handler returned successfully");
            }
        };

        // Start middleware chain
        LOG_DEBUG("App", "Starting middleware chain");
        next();
        LOG_DEBUG("App", "Middleware chain complete");

        // CORS handling if enabled
        if (enable_cors) {
            response.cors(cors_origin);
        }
    };
}

void App::init_default_routes() {
    // Health check endpoint
    get("/health", [](Request& req, Response& res) {
        res.json({{"status", "healthy"}});
    });

    // OpenAPI specification endpoint
    get(config_.openapi_url, [this](Request& req, Response& res) {
        res.content_type("application/json");
        res.send(openapi_spec());
    });

    // Documentation endpoint (Swagger UI)
    get(config_.docs_url, [this](Request& req, Response& res) {
        std::string html = R"(
<!DOCTYPE html>
<html>
<head>
    <title>)" + config_.title + R"( - API Documentation</title>
    <link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/swagger-ui-dist@5/swagger-ui.css">
</head>
<body>
    <div id="swagger-ui"></div>
    <script src="https://cdn.jsdelivr.net/npm/swagger-ui-dist@5/swagger-ui-bundle.js"></script>
    <script>
        SwaggerUIBundle({
            url: ')" + config_.openapi_url + R"(',
            dom_id: '#swagger-ui'
        });
    </script>
</body>
</html>
        )";
        res.html(html);
    });
}

std::string App::build_json(const std::vector<std::pair<std::string, std::string>>& pairs) {
    std::ostringstream oss;
    oss << "{";
    for (size_t i = 0; i < pairs.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << pairs[i].first << "\":\"" << pairs[i].second << "\"";
    }
    oss << "}";
    return oss.str();
}

// =============================================================================
// HTTP/1.1 Direct Handler (for UnifiedServer)
// =============================================================================

http::Http1Response App::handle_http1(
    const std::string& method,
    const std::string& path,
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& body
) noexcept {
    // Create HttpRequest from parsed data (stack-allocated, no heap allocation)
    HttpRequest req = HttpRequest::from_parsed_data(method, path, headers, body);

    // Match route using router
    http::RouteParams params;
    auto* router = server_->get_router();
    if (!router) {
        // Router not initialized - return 500
        http::Http1Response response;
        response.status = 500;
        response.status_message = "Internal Server Error";
        response.headers["Content-Type"] = "application/json";
        response.body = "{\"error\":\"Router not initialized\"}";
        return response;
    }

    auto handler = router->match(method, path, params);

    // If no route found, return 404
    if (!handler) {
        http::Http1Response response;
        response.status = 404;
        response.status_message = "Not Found";
        response.headers["Content-Type"] = "application/json";
        response.body = "{\"error\":\"Not Found\"}";
        return response;
    }

    // Create HttpResponse object (stack-allocated, no heap allocation)
    HttpResponse res;

    // Create wrapper objects
    Request request(&req, params);
    Response response(&res);

    // Build middleware chain (adapted from wrap_handler)
    std::vector<MiddlewareFunc> all_middleware;

    // Add global middleware
    all_middleware.insert(all_middleware.end(),
        global_middleware_.begin(), global_middleware_.end());

    // Add path-specific middleware
    for (const auto& [prefix, middleware_list] : path_middleware_) {
        if (path.find(prefix) == 0) {
            all_middleware.insert(all_middleware.end(),
                middleware_list.begin(), middleware_list.end());
        }
    }

    // Execute middleware chain
    size_t middleware_index = 0;
    std::function<void()> next;

    next = [&]() {
        if (middleware_index < all_middleware.size()) {
            auto& middleware = all_middleware[middleware_index++];
            middleware(request, response, next);
        } else {
            // All middleware executed, call the route handler
            handler(&req, &res, params);
        }
    };

    // Start middleware chain
    next();

    // CORS handling if enabled
    if (config_.enable_cors) {
        response.cors(config_.cors_origin);
    }

    // Extract response data and build Http1Response
    http::Http1Response http1_response;
    http1_response.status = static_cast<uint16_t>(res.get_status_code());

    // Get status message
    switch (res.get_status_code()) {
        case HttpResponse::Status::OK: http1_response.status_message = "OK"; break;
        case HttpResponse::Status::CREATED: http1_response.status_message = "Created"; break;
        case HttpResponse::Status::NO_CONTENT: http1_response.status_message = "No Content"; break;
        case HttpResponse::Status::BAD_REQUEST: http1_response.status_message = "Bad Request"; break;
        case HttpResponse::Status::UNAUTHORIZED: http1_response.status_message = "Unauthorized"; break;
        case HttpResponse::Status::FORBIDDEN: http1_response.status_message = "Forbidden"; break;
        case HttpResponse::Status::NOT_FOUND: http1_response.status_message = "Not Found"; break;
        case HttpResponse::Status::INTERNAL_SERVER_ERROR: http1_response.status_message = "Internal Server Error"; break;
        default: http1_response.status_message = "OK"; break;
    }

    http1_response.headers = res.get_headers();
    http1_response.body = res.get_body();

    return http1_response;
}

// =============================================================================
// UnifiedServer Integration (Multi-protocol Pure C++ Mode)
// =============================================================================

int App::run_unified(const std::string& host, uint16_t port) {
    // Create UnifiedServer configuration from App config
    http::UnifiedServerConfig server_config;
    server_config.host = host;
    server_config.http1_port = port;
    server_config.enable_http1_cleartext = true;
    server_config.pure_cpp_mode = config_.pure_cpp_mode;

    // TLS configuration
    server_config.enable_tls = !config_.cert_path.empty() && !config_.key_path.empty();
    if (server_config.enable_tls) {
        server_config.cert_file = config_.cert_path;
        server_config.key_file = config_.key_path;
        server_config.tls_port = 443;  // Standard HTTPS port
    }

    // HTTP/2 and HTTP/3 configuration
    server_config.enable_http3 = config_.enable_http3;
    server_config.http3_port = config_.http3_port;
    server_config.enable_webtransport = config_.enable_webtransport;

    // Worker configuration
    server_config.num_workers = 0;  // Auto-detect CPU count
    server_config.use_reuseport = true;

    // Log mode
    if (config_.pure_cpp_mode) {
        LOG_INFO("App", "Starting in pure C++ mode (no Python/ZMQ bridges)");
    } else {
        LOG_INFO("App", "Starting with Python/ZMQ bridge enabled");
    }

    // Create UnifiedServer
    http::UnifiedServer unified_server(server_config);

    // Set this App instance for direct HTTP/1.1 handling
    unified_server.set_app_instance(this);

    // Transfer WebSocket handlers to UnifiedServer
    for (const auto& [ws_path, ws_handler] : websocket_handlers_) {
        // Convert WSHandler to UnifiedServer's WebSocketHandler type
        // Both use std::function<void(http::WebSocketConnection&)>
        unified_server.add_websocket_handler(ws_path, ws_handler);
        LOG_INFO("App", "Registered WebSocket handler: %s", ws_path.c_str());
    }

    LOG_INFO("App", "FasterAPI application starting on http://%s:%d", host.c_str(), port);
    if (server_config.enable_tls) {
        LOG_INFO("App", "TLS enabled on port %d (HTTP/1.1 + HTTP/2)", server_config.tls_port);
    }
    if (server_config.enable_http3) {
        LOG_INFO("App", "HTTP/3 enabled on UDP port %d", server_config.http3_port);
    }
    if (config_.enable_docs) {
        LOG_INFO("App", "Documentation: http://%s:%d%s", host.c_str(), port, config_.docs_url.c_str());
    }

    // Start the unified server (blocks until stopped)
    int result = unified_server.start();
    if (result != 0) {
        LOG_ERROR("App", "UnifiedServer failed to start: %s", unified_server.get_error().c_str());
        return result;
    }

    return 0;
}

} // namespace fasterapi
