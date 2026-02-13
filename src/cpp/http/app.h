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
#include "coro_unified_server.h"
#include "../core/coro_task.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <regex>
#include <type_traits>

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
    bool content_type_set_{false};
};

/**
 * Handler function type - takes Request and Response by reference.
 */
using Handler = std::function<void(Request&, Response&)>;

/**
 * Async handler function type - returns a coroutine task.
 *
 * Use this for handlers that need to `co_await` async operations.
 * The coroutine will be suspended when it yields and resumed when
 * the async operation completes.
 *
 * Example:
 * @code
 * app.get_async("/users/{id}", [](Request& req, Response& res) -> core::coro_task<void> {
 *     auto id = req.path_param("id");
 *     auto user = co_await fetch_user_async(id);  // Yields here
 *     res.json(user.to_json());
 *     co_return;
 * });
 * @endcode
 */
using AsyncHandler = std::function<core::coro_task<void>(Request&, Response&)>;

/**
 * WebSocket handler type.
 */
using WSHandler = std::function<void(http::WebSocketConnection&)>;

/**
 * SSE handler type (synchronous, for use with UnifiedServer).
 */
using SSEHandler = std::function<void(http::SSEConnection&)>;

/**
 * Middleware function type - can modify request/response or call next.
 */
using MiddlewareFunc = std::function<void(Request&, Response&, std::function<void()> next)>;

/**
 * Async middleware function type - compatible with coroutines.
 *
 * Async middleware can co_await async operations and must call next() to continue
 * the middleware chain. If next() is not called, the chain is short-circuited.
 *
 * Example:
 * @code
 * app.use_async([](Request& req, Response& res, auto next) -> core::coro_task<void> {
 *     auto token = req.header("Authorization");
 *     auto user = co_await validate_token_async(token);
 *     if (!user) {
 *         res.unauthorized().json({{"error", "Invalid token"}});
 *         co_return;  // Short-circuit - don't call next
 *     }
 *     co_await next();  // Continue to next middleware/handler
 * });
 * @endcode
 */
using AsyncMiddlewareFunc = std::function<
    core::coro_task<void>(Request&, Response&, std::function<core::coro_task<void>()> next)
>;

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
        uint16_t tls_port = 0;  // 0 = same as main port (TLS on main port)

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

        // Coroutine server configuration (for run_coro)
        // num_io_threads: I/O dispatcher threads (1-2 recommended)
        // num_workers: Worker pool size for coroutine execution (0 = auto = CPU count)
        size_t num_io_threads = 1;
        size_t num_workers = 0;
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
    // IMPORTANT: These sync route methods are for handlers that return void.
    // For coroutine handlers that use co_await and return coro_task<void>,
    // use the *_async variants instead (get_async, post_async, etc.)
    //
    // Using the wrong method type will result in silent failures or empty
    // responses. When in doubt, check your handler's return type:
    //   - void               -> use get(), post(), put(), del(), patch()
    //   - coro_task<void>    -> use get_async(), post_async(), put_async(), etc.

    /**
     * Register a GET route with a synchronous handler.
     *
     * @param path Route path (supports {param} and *wildcard)
     * @param handler Request handler (must return void)
     *
     * IMPORTANT: For async handlers that use co_await and return coro_task<void>,
     * use get_async() instead. Using get() with an async handler will cause
     * silent failures.
     */
    App& get(const std::string& path, Handler handler);

    /**
     * Register a POST route with a synchronous handler.
     *
     * IMPORTANT: For async handlers, use post_async() instead.
     */
    App& post(const std::string& path, Handler handler);

    /**
     * Register a PUT route with a synchronous handler.
     *
     * IMPORTANT: For async handlers, use put_async() instead.
     */
    App& put(const std::string& path, Handler handler);

    /**
     * Register a DELETE route with a synchronous handler.
     *
     * IMPORTANT: For async handlers, use del_async() instead.
     */
    App& del(const std::string& path, Handler handler);

    /**
     * Register a PATCH route with a synchronous handler.
     *
     * IMPORTANT: For async handlers, use patch_async() instead.
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
    // Async Route Registration - Coroutine Handlers
    // =========================================================================

    /**
     * Register an async GET route.
     *
     * Async handlers can use co_await for non-blocking I/O operations.
     * The coroutine will be suspended when it yields and resumed when
     * the async operation completes.
     *
     * @param path Route path (supports {param} and *wildcard)
     * @param handler Async request handler (returns coro_task<void>)
     *
     * Example:
     * @code
     * app.get_async("/users/{id}", [](auto& req, auto& res) -> core::coro_task<void> {
     *     auto id = req.path_param("id");
     *     auto user = co_await db.get_user_async(id);
     *     res.json(user.to_json());
     *     co_return;
     * });
     * @endcode
     */
    App& get_async(const std::string& path, AsyncHandler handler);

    /**
     * Register an async POST route.
     */
    App& post_async(const std::string& path, AsyncHandler handler);

    /**
     * Register an async PUT route.
     */
    App& put_async(const std::string& path, AsyncHandler handler);

    /**
     * Register an async DELETE route.
     */
    App& del_async(const std::string& path, AsyncHandler handler);

    /**
     * Register an async PATCH route.
     */
    App& patch_async(const std::string& path, AsyncHandler handler);

    /**
     * Check if a route has an async handler registered.
     *
     * @param method HTTP method (GET, POST, etc.)
     * @param path Request path
     * @return True if an async handler exists for this route
     */
    bool has_async_route(const std::string& method, const std::string& path) const;

    /**
     * Dispatch to async handler and return coroutine task.
     *
     * Called by UnifiedServer/Http2Server when handling async routes.
     * The caller is responsible for keeping the coroutine alive until completion.
     *
     * @param req Request object
     * @param res Response object (will be populated by handler)
     * @return Coroutine task that executes the async handler
     */
    core::coro_task<void> dispatch_async(Request& req, Response& res);

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

    /**
     * Register a coroutine SSE endpoint (for use with CoroUnifiedServer).
     *
     * The handler receives the I/O dispatcher, file descriptor, and request.
     * It should stream events by writing directly to the fd via io.async_write().
     * SSE headers are automatically sent before the handler is invoked.
     *
     * @param path SSE path
     * @param handler Coroutine SSE handler
     */
    App& sse_coro(const std::string& path, http::CoroSSEHandler handler);

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

    /**
     * Add global async middleware.
     *
     * Async middleware can use co_await for non-blocking operations.
     * Works with both sync and async route handlers.
     *
     * @param middleware Async middleware function
     */
    App& use_async(AsyncMiddlewareFunc middleware);

    /**
     * Add async middleware for specific path prefix.
     *
     * @param path Path prefix (e.g., "/api")
     * @param middleware Async middleware function
     */
    App& use_async(const std::string& path, AsyncMiddlewareFunc middleware);

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
     * Run using CoroUnifiedServer (coroutine-based architecture).
     *
     * This is the preferred method for production servers using async coroutines.
     * Implements the Seastar-inspired architecture:
     * - 1-2 I/O threads for event dispatch
     * - N worker threads executing coroutines
     * - True async with zero-allocation per-request
     *
     * Features:
     * - Lower CPU usage when idle (no per-thread event loops)
     * - Coroutines yield on blocking I/O instead of blocking threads
     * - Better scalability for high-concurrency workloads
     *
     * Configure via App::Config:
     * - num_io_threads: I/O dispatcher threads (default 1, max 2 recommended)
     * - num_workers: Worker pool size (default 0 = auto-detect CPU count)
     *
     * @param host Host to bind (e.g., "0.0.0.0")
     * @param port Cleartext HTTP/1.1 port (default 8080)
     * @return Error code (0 = success)
     */
    int run_coro(const std::string& host = "0.0.0.0", uint16_t port = 8080);

    /**
     * Set ultra-fast callback for maximum performance plaintext routes.
     * 
     * This bypasses all routing and middleware for a single callback that
     * writes directly to a pre-allocated buffer. Use for TechEmpower-style
     * benchmarks where every nanosecond counts.
     * 
     * @param callback Function that handles requests and writes responses
     */
    void set_ultra_fast_callback(http::Http1Connection::UltraFastCallback callback);

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
    
    /**
     * Handle HTTP/1.1 request with zero-copy view - used by UnifiedServer fast path.
     * This avoids string allocations by using string_view into the connection buffer.
     *
     * @param view Zero-copy request view (valid only during this call)
     * @return Http1Response to send to client
     */
    http::Http1Response handle_http1_fast(const http::Http1RequestView& view) noexcept;

    /**
     * Handle HTTP/2 request - used by UnifiedServer for HTTP/2 streams.
     * Routes through same handlers as HTTP/1, but uses callback for response.
     * This ensures HTTP handlers work transparently with both HTTP/1 and HTTP/2.
     *
     * @param method HTTP method
     * @param path Request path
     * @param headers Request headers
     * @param body Request body
     * @param send_response Callback to send response (status, headers, body)
     */
    void handle_http2_request(
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body,
        std::function<void(uint16_t, const std::unordered_map<std::string, std::string>&, const std::string&)> send_response
    ) noexcept;

private:
    Config config_;
    std::unique_ptr<HttpServer> server_;
    std::vector<MiddlewareFunc> global_middleware_;
    std::map<std::string, std::vector<MiddlewareFunc>> path_middleware_;
    std::vector<AsyncMiddlewareFunc> async_global_middleware_;
    std::map<std::string, std::vector<AsyncMiddlewareFunc>> async_path_middleware_;
    std::map<std::string, std::string> static_paths_;

    // WebSocket handlers stored for transfer to UnifiedServer in run_unified()
    std::map<std::string, WSHandler> websocket_handlers_;

    // Coroutine SSE handlers stored for transfer to CoroUnifiedServer in run_coro()
    std::map<std::string, http::CoroSSEHandler> coro_sse_handlers_;

    // Ultra-fast callback for maximum performance (bypasses routing)
    http::Http1Connection::UltraFastCallback ultra_fast_callback_ = nullptr;

    // =========================================================================
    // Async Route Storage
    // =========================================================================

    /**
     * Async route entry - stores compiled regex and handler.
     */
    struct AsyncRouteEntry {
        std::regex pattern;
        std::string path_template;  // Original path with {params}
        AsyncHandler handler;
        std::vector<std::string> param_names;  // Parameter names in order
    };

    /**
     * Async routes by method (GET, POST, PUT, DELETE, PATCH).
     * Routes are tried in order of registration.
     */
    std::map<std::string, std::vector<AsyncRouteEntry>> async_routes_;

    /**
     * Register an async route internally.
     *
     * Converts path template like "/users/{id}" to regex and stores handler.
     */
    void register_async_route(const std::string& method, const std::string& path, AsyncHandler handler);

    /**
     * Match async route and extract parameters.
     *
     * @param method HTTP method
     * @param path Request path
     * @param[out] handler Matched handler (if found)
     * @param[out] params Extracted route parameters
     * @return True if route matched
     */
    bool match_async_route(
        const std::string& method,
        const std::string& path,
        AsyncHandler* handler,
        http::RouteParams* params
    ) const;

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
     * Execute async middleware chain as a coroutine.
     *
     * Recursively calls each middleware, awaiting the next() function.
     * When all middleware are exhausted, calls the final handler.
     *
     * @param request Request object
     * @param response Response object
     * @param middleware Vector of async middleware functions
     * @param final_handler Handler to call after all middleware
     * @return Coroutine task that completes when chain is done
     */
    core::coro_task<void> execute_async_middleware_chain(
        Request& request,
        Response& response,
        const std::vector<AsyncMiddlewareFunc>& middleware,
        std::function<core::coro_task<void>()> final_handler
    );

    /**
     * Wrap sync middleware for use in async middleware chain.
     *
     * Converts a synchronous MiddlewareFunc to an AsyncMiddlewareFunc.
     * The sync middleware's next() call triggers co_await on the async next().
     *
     * @param sync_mw Synchronous middleware to wrap
     * @return AsyncMiddlewareFunc that wraps the sync middleware
     */
    static AsyncMiddlewareFunc wrap_sync_middleware(const MiddlewareFunc& sync_mw);

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
