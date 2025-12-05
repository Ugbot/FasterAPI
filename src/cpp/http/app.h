/**
 * @file app.h
 * @brief High-level C++ API for FasterAPI - FastAPI-style fluent interface
 *
 * This provides a clean, modern C++ API for building HTTP applications
 * with a similar developer experience to Python's FastAPI.
 *
 * Example:
 * @code
 * #include "app.h"
 *
 * int main() {
 *     auto app = fasterapi::App();
 *
 *     app.get("/", [](auto& req, auto& res) {
 *         res.json({{"message", "Hello World"}});
 *     });
 *
 *     app.get("/users/{id}", [](auto& req, auto& res) {
 *         auto id = req.path_param("id");
 *         res.json({{"user_id", id}});
 *     });
 *
 *     app.run("0.0.0.0", 8000);
 * }
 * @endcode
 */

#pragma once

#include "server.h"
#include "request.h"
#include "response.h"
#include "router.h"
#include "websocket.h"
#include "sse.h"
#include "http1_connection.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <optional>

namespace fasterapi {

// Forward declarations
class App;
class RouteBuilder;
class Middleware;

/**
 * Request context - wraps HttpRequest with convenience methods.
 */
class Request {
public:
    explicit Request(HttpRequest* req, const http::RouteParams& params);

    // Method accessors
    std::string method() const;
    std::string path() const;
    std::string query_string() const;

    // Headers
    std::string header(const std::string& name) const;
    std::map<std::string, std::string> headers() const;

    // Parameters
    std::string path_param(const std::string& name) const;
    std::string query_param(const std::string& name) const;
    std::optional<std::string> path_param_optional(const std::string& name) const;
    std::optional<std::string> query_param_optional(const std::string& name) const;

    // Body
    std::string body() const;
    std::string json_body() const;

    // Client info
    std::string client_ip() const;
    std::string user_agent() const;

    // Internal request object
    HttpRequest* raw() const { return req_; }

private:
    HttpRequest* req_;
    const http::RouteParams& params_;
};

/**
 * Response context - wraps HttpResponse with convenience methods.
 */
class Response {
public:
    explicit Response(HttpResponse* res);

    // Status
    Response& status(int code);
    Response& ok() { return status(200); }
    Response& created() { return status(201); }
    Response& no_content() { return status(204); }
    Response& bad_request() { return status(400); }
    Response& unauthorized() { return status(401); }
    Response& forbidden() { return status(403); }
    Response& not_found() { return status(404); }
    Response& internal_error() { return status(500); }

    // Headers
    Response& header(const std::string& name, const std::string& value);
    Response& content_type(const std::string& type);

    // Body
    Response& send(const std::string& body);
    Response& json(const std::string& json_str);
    Response& html(const std::string& html_str);
    Response& text(const std::string& text_str);
    Response& file(const std::string& path);

    // Convenience JSON builders
    Response& json(const std::vector<std::pair<std::string, std::string>>& pairs);
    Response& json(std::initializer_list<std::pair<const char*, const char*>> pairs);

    // CORS
    Response& cors(const std::string& origin = "*");

    // Cookies
    Response& cookie(
        const std::string& name,
        const std::string& value,
        int max_age = -1,
        const std::string& path = "/",
        bool http_only = false,
        bool secure = false,
        const std::string& same_site = ""
    );

    // Redirect
    Response& redirect(const std::string& url, int code = 302);

    // Stream
    Response& stream_start();
    Response& stream_chunk(const std::string& chunk);
    Response& stream_end();

    // Internal response object
    HttpResponse* raw() const { return res_; }

private:
    HttpResponse* res_;
    bool sent_{false};
};

/**
 * Handler function type - takes Request and Response by reference.
 */
using Handler = std::function<void(Request&, Response&)>;

/**
 * WebSocket handler type.
 */
using WSHandler = std::function<void(http::WebSocketConnection&)>;

/**
 * SSE handler type.
 */
using SSEHandler = std::function<void(http::SSEConnection&)>;

/**
 * Middleware function type - can modify request/response or call next.
 */
using MiddlewareFunc = std::function<void(Request&, Response&, std::function<void()> next)>;

/**
 * Route configuration builder - allows chaining configuration methods.
 */
class RouteBuilder {
public:
    RouteBuilder(App* app, const std::string& method, const std::string& path);

    // Add tags/metadata
    RouteBuilder& tag(const std::string& tag);
    RouteBuilder& summary(const std::string& summary);
    RouteBuilder& description(const std::string& desc);

    // Response models (for OpenAPI generation)
    RouteBuilder& response_model(int status_code, const std::string& schema);

    // Middleware
    RouteBuilder& use(MiddlewareFunc middleware);

    // Authentication
    RouteBuilder& require_auth();
    RouteBuilder& require_role(const std::string& role);

    // Rate limiting
    RouteBuilder& rate_limit(int requests_per_minute);

    // Finally, set the handler
    void handler(Handler h);
    void operator()(Handler h) { handler(h); }

private:
    App* app_;
    std::string method_;
    std::string path_;
    std::vector<std::string> tags_;
    std::string summary_;
    std::string description_;
    std::vector<MiddlewareFunc> middleware_;
    std::map<int, std::string> response_models_;
};

/**
 * High-level FasterAPI application - FastAPI-style C++ interface.
 *
 * Provides a clean, fluent API for building HTTP applications:
 *
 * @code
 * auto app = fasterapi::App();
 *
 * // Simple routes
 * app.get("/", [](auto& req, auto& res) {
 *     res.json({{"message", "Hello"}});
 * });
 *
 * // Path parameters
 * app.get("/users/{id}", [](auto& req, auto& res) {
 *     auto id = req.path_param("id");
 *     res.json({{"user", id}});
 * });
 *
 * // Route builder for configuration
 * app.route("POST", "/api/data")
 *    .tag("API")
 *    .summary("Create data")
 *    .require_auth()
 *    .rate_limit(100)
 *    .handler([](auto& req, auto& res) {
 *        // Handler logic
 *    });
 *
 * // WebSocket
 * app.websocket("/ws", [](auto& ws) {
 *     ws.on_text_message = [&](const std::string& msg) {
 *         ws.send_text("Echo: " + msg);
 *     };
 * });
 *
 * // Server-Sent Events
 * app.sse("/events", [](auto& sse) {
 *     for (int i = 0; i < 10; i++) {
 *         sse.send("Event " + std::to_string(i));
 *         std::this_thread::sleep_for(std::chrono::seconds(1));
 *     }
 * });
 *
 * // Middleware
 * app.use([](auto& req, auto& res, auto next) {
 *     std::cout << req.method() << " " << req.path() << std::endl;
 *     next();
 * });
 *
 * // Mount sub-apps
 * auto api_v1 = fasterapi::App();
 * api_v1.get("/status", ...);
 * app.mount("/api/v1", api_v1);
 *
 * // Run server
 * app.run("0.0.0.0", 8000);
 * @endcode
 */
class App {
    // RouteBuilder needs access to private members
    friend class RouteBuilder;

public:
    /**
     * Application configuration.
     */
    struct Config {
        std::string title = "FasterAPI Application";
        std::string version = "1.0.0";
        std::string description;

        // Server settings
        bool enable_http2 = false;
        bool enable_http3 = false;
        bool enable_webtransport = false;
        uint16_t http3_port = 443;
        bool enable_compression = true;
        bool enable_cors = false;
        std::string cors_origin = "*";

        // TLS settings
        std::string cert_path;
        std::string key_path;

        // Performance
        uint32_t max_connections = 10000;
        uint32_t max_request_size = 16 * 1024 * 1024;  // 16MB

        // Timeouts (milliseconds)
        uint32_t request_timeout = 30000;   // 30s
        uint32_t keepalive_timeout = 60000; // 60s

        // Documentation
        bool enable_docs = true;
        std::string docs_url = "/docs";
        std::string openapi_url = "/openapi.json";

        // Pure C++ mode - disables Python/ZMQ bridge entirely
        // When true:
        // - No ProcessPoolExecutor is created
        // - No ZMQ sockets are initialized
        // - All handlers must be C++ (no Python callbacks)
        // - Uses UnifiedServer with pure_cpp_mode enabled
        bool pure_cpp_mode = false;
    };

    /**
     * Create a new FasterAPI application.
     */
    explicit App(const Config& config);

    /**
     * Create a new FasterAPI application with default configuration.
     */
    App();

    ~App();

    // Non-copyable, movable
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) noexcept;
    App& operator=(App&&) noexcept;

    // =========================================================================
    // Route Registration - HTTP Methods
    // =========================================================================

    /**
     * Register a GET route.
     *
     * @param path Route path (supports {param} and *wildcard)
     * @param handler Request handler
     */
    App& get(const std::string& path, Handler handler);

    /**
     * Register a POST route.
     */
    App& post(const std::string& path, Handler handler);

    /**
     * Register a PUT route.
     */
    App& put(const std::string& path, Handler handler);

    /**
     * Register a DELETE route.
     */
    App& del(const std::string& path, Handler handler);

    /**
     * Register a PATCH route.
     */
    App& patch(const std::string& path, Handler handler);

    /**
     * Register a HEAD route.
     */
    App& head(const std::string& path, Handler handler);

    /**
     * Register an OPTIONS route.
     */
    App& options(const std::string& path, Handler handler);

    /**
     * Register a route with builder pattern for advanced configuration.
     *
     * @param method HTTP method
     * @param path Route path
     * @return RouteBuilder for chaining configuration
     *
     * Example:
     * @code
     * app.route("POST", "/users")
     *    .tag("Users")
     *    .summary("Create user")
     *    .require_auth()
     *    .handler([](auto& req, auto& res) { ... });
     * @endcode
     */
    RouteBuilder route(const std::string& method, const std::string& path);

    // =========================================================================
    // WebSocket & SSE
    // =========================================================================

    /**
     * Register a WebSocket endpoint.
     *
     * @param path WebSocket path
     * @param handler WebSocket handler
     */
    App& websocket(const std::string& path, WSHandler handler);

    /**
     * Register a Server-Sent Events endpoint.
     *
     * @param path SSE path
     * @param handler SSE handler
     */
    App& sse(const std::string& path, SSEHandler handler);

    // =========================================================================
    // Middleware
    // =========================================================================

    /**
     * Add global middleware.
     *
     * Middleware is executed in order of registration for all routes.
     * Call next() to continue to the next middleware or route handler.
     *
     * @param middleware Middleware function
     */
    App& use(MiddlewareFunc middleware);

    /**
     * Add middleware for specific path prefix.
     *
     * @param path Path prefix (e.g., "/api")
     * @param middleware Middleware function
     */
    App& use(const std::string& path, MiddlewareFunc middleware);

    // =========================================================================
    // Static Files
    // =========================================================================

    /**
     * Serve static files from a directory.
     *
     * @param url_path URL path prefix (e.g., "/static")
     * @param directory_path Filesystem directory path
     */
    App& static_files(const std::string& url_path, const std::string& directory_path);

    // =========================================================================
    // Sub-Applications
    // =========================================================================

    /**
     * Mount a sub-application at a path prefix.
     *
     * @param path Prefix path (e.g., "/api/v1")
     * @param sub_app Sub-application to mount
     */
    App& mount(const std::string& path, App& sub_app);

    // =========================================================================
    // Server Control
    // =========================================================================

    /**
     * Start the server and run until stopped.
     *
     * @param host Host to bind (e.g., "0.0.0.0", "127.0.0.1")
     * @param port Port to listen on
     * @return Error code (0 = success)
     */
    int run(const std::string& host = "0.0.0.0", uint16_t port = 8000);

    /**
     * Start the server in background (non-blocking).
     *
     * @param host Host to bind
     * @param port Port to listen on
     * @return Error code (0 = success)
     */
    int start(const std::string& host = "0.0.0.0", uint16_t port = 8000);

    /**
     * Run using UnifiedServer (multi-protocol: HTTP/1.1, HTTP/2, HTTP/3).
     *
     * This is the preferred method for production pure C++ servers.
     * Supports:
     * - HTTP/1.1 over cleartext (port configurable, default 8080)
     * - HTTP/1.1 + HTTP/2 over TLS with ALPN (port 443)
     * - HTTP/3 over QUIC (UDP port 443)
     * - WebSocket over any HTTP protocol
     *
     * In pure_cpp_mode (config.pure_cpp_mode = true):
     * - No Python/ZMQ bridges are initialized
     * - All WebSocket handlers run in C++ only
     * - Maximum performance, zero Python overhead
     *
     * @param host Host to bind (e.g., "0.0.0.0")
     * @param port Cleartext HTTP/1.1 port (default 8080)
     * @return Error code (0 = success)
     */
    int run_unified(const std::string& host = "0.0.0.0", uint16_t port = 8080);

    /**
     * Stop the server.
     *
     * @return Error code (0 = success)
     */
    int stop();

    /**
     * Check if server is running.
     */
    bool is_running() const;

    // =========================================================================
    // Introspection & Documentation
    // =========================================================================

    /**
     * Get all registered routes.
     */
    std::vector<std::pair<std::string, std::string>> routes() const;

    /**
     * Generate OpenAPI specification.
     *
     * @return OpenAPI JSON string
     */
    std::string openapi_spec() const;

    /**
     * Get application configuration.
     */
    const Config& config() const { return config_; }

    /**
     * Get server statistics.
     */
    HttpServer::Stats stats() const;

    // =========================================================================
    // Internal HTTP/1.1 Handler (called by UnifiedServer)
    // =========================================================================

    /**
     * Handle HTTP/1.1 request directly - used by UnifiedServer.
     * This bypasses HttpServer and calls router/middleware/handlers directly.
     *
     * @param method HTTP method
     * @param path Request path
     * @param headers Request headers
     * @param body Request body
     * @return Http1Response to send to client
     */
    http::Http1Response handle_http1(
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body
    ) noexcept;

private:
    Config config_;
    std::unique_ptr<HttpServer> server_;
    std::vector<MiddlewareFunc> global_middleware_;
    std::map<std::string, std::vector<MiddlewareFunc>> path_middleware_;
    std::map<std::string, std::string> static_paths_;

    // WebSocket handlers stored for transfer to UnifiedServer in run_unified()
    std::map<std::string, WSHandler> websocket_handlers_;

    // Route metadata for OpenAPI generation
    struct RouteMetadata {
        std::string method;
        std::string path;
        std::vector<std::string> tags;
        std::string summary;
        std::string description;
        std::map<int, std::string> response_models;
        // NOTE: middleware removed - not needed for OpenAPI generation
        // Storing std::function objects here caused destructor crashes due to
        // complex captured state and potential double-frees
    };
    std::vector<RouteMetadata> route_metadata_;

    /**
     * Internal method to register a route with full configuration.
     */
    void register_route(
        const std::string& method,
        const std::string& path,
        Handler handler,
        const RouteMetadata& metadata = RouteMetadata{},
        const std::vector<MiddlewareFunc>& route_middleware = {}
    );

    /**
     * Wrap user handler with middleware chain.
     */
    HttpServer::RouteHandler wrap_handler(
        Handler user_handler,
        const std::vector<MiddlewareFunc>& route_middleware
    );

    /**
     * Initialize default routes (docs, OpenAPI, health check).
     */
    void init_default_routes();

    /**
     * Build simple JSON object from key-value pairs.
     */
    static std::string build_json(const std::vector<std::pair<std::string, std::string>>& pairs);
};

} // namespace fasterapi
