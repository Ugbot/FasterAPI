/**
 * FasterAPI HTTP Server - C interface for ctypes binding.
 *
 * High-performance HTTP server with:
 * - HTTP/1.1, HTTP/2, HTTP/3 support
 * - WebSocket support
 * - zstd compression
 * - Per-core event loops
 * - Zero-copy operations
 *
 * All exported functions use C linkage and void* pointers for FFI safety.
 * Implementation focuses on maximum performance with lock-free operations.
 *
 * Note: Compiled with -fno-exceptions, so no try/catch blocks.
 */

#include "server.h"
#include "request.h"
#include "response.h"
#include "compression.h"
#include "python_callback_bridge.h"
#include <cstring>
#include <memory>
#include <unordered_map>
#include <atomic>

extern "C" {

// Global server instance
static std::unique_ptr<HttpServer> g_server = nullptr;
static std::atomic<bool> g_initialized{false};

// ==============================================================================
// Server Management
// ==============================================================================

/**
 * Create a new HTTP server.
 *
 * @param port Server port
 * @param host Server host (null-terminated)
 * @param enable_h2 Enable HTTP/2
 * @param enable_h3 Enable HTTP/3
 * @param enable_compression Enable zstd compression
 * @param error_out Output error code (0 = success)
 * @return Server handle (cast to HttpServer*)
 */
void* http_server_create(
    uint16_t port,
    const char* host,
    bool enable_h2,
    bool enable_h3,
    bool enable_compression,
    int* error_out
) noexcept {
    if (!host || !error_out) {
        if (error_out) *error_out = 1;  // Invalid argument
        return nullptr;
    }
    
    if (g_server) {
        if (error_out) *error_out = 2;  // Server already exists
        return nullptr;
    }
    
    HttpServer::Config config;
    config.port = port;
    config.host = host;
    config.enable_h1 = true;
    config.enable_h2 = enable_h2;
    config.enable_h3 = enable_h3;
    config.enable_compression = enable_compression;
    
    g_server = std::make_unique<HttpServer>(config);
    if (!g_server) {
        if (error_out) *error_out = 3;  // Memory allocation failed
        return nullptr;
    }
    
    g_initialized.store(true);
    if (error_out) *error_out = 0;
    return g_server.get();
}

/**
 * Destroy the HTTP server.
 *
 * @param server Server handle from http_server_create
 * @return Error code (0 = success)
 */
int http_server_destroy(void* server) noexcept {
    if (!server || server != g_server.get()) {
        return 1;  // Invalid server handle
    }
    
    g_server.reset();
    g_initialized.store(false);
    return 0;
}

/**
 * Start the HTTP server.
 *
 * @param server Server handle
 * @param error_out Output error code (0 = success)
 * @return Error code (0 = success)
 */
int http_server_start(void* server, int* error_out) noexcept {
    if (!server || !error_out) {
        if (error_out) *error_out = 1;
        return 1;
    }
    
    auto* s = reinterpret_cast<HttpServer*>(server);
    int result = s->start();
    
    if (error_out) *error_out = result;
    return result;
}

/**
 * Stop the HTTP server.
 *
 * @param server Server handle
 * @param error_out Output error code (0 = success)
 * @return Error code (0 = success)
 */
int http_server_stop(void* server, int* error_out) noexcept {
    if (!server || !error_out) {
        if (error_out) *error_out = 1;
        return 1;
    }
    
    auto* s = reinterpret_cast<HttpServer*>(server);
    int result = s->stop();
    
    if (error_out) *error_out = result;
    return result;
}

/**
 * Check if server is running.
 *
 * @param server Server handle
 * @return true if running, false otherwise
 */
bool http_server_is_running(void* server) noexcept {
    if (!server) return false;
    
    auto* s = reinterpret_cast<HttpServer*>(server);
    return s->is_running();
}

// ==============================================================================
// Route Management
// ==============================================================================

/**
 * Add a route handler.
 *
 * @param server Server handle
 * @param method HTTP method (null-terminated)
 * @param path Route path (null-terminated)
 * @param handler_id Handler ID for Python callback
 * @param error_out Output error code (0 = success)
 * @return Error code (0 = success)
 */
int http_add_route(
    void* server,
    const char* method,
    const char* path,
    uint32_t handler_id,
    int* error_out
) noexcept {
    if (!server || !method || !path || !error_out) {
        if (error_out) *error_out = 1;
        return 1;
    }
    
    auto* s = reinterpret_cast<HttpServer*>(server);
    
    // Create handler that will call Python callback
    HttpServer::RouteHandler handler = [handler_id](HttpRequest* req, HttpResponse* res) {
        // This will be implemented to call Python callback
        // For now, just return a simple response
        res->status(HttpResponse::Status::OK)
           .content_type("application/json")
           .json("{\"message\":\"Hello from FasterAPI\",\"handler_id\":" + std::to_string(handler_id) + "}");
        res->send();
    };
    
    int result = s->add_route(method, path, std::move(handler));
    
    if (error_out) *error_out = result;
    return result;
}

/**
 * Add a WebSocket endpoint.
 *
 * @param server Server handle
 * @param path WebSocket path (null-terminated)
 * @param handler_id Handler ID for Python callback
 * @param error_out Output error code (0 = success)
 * @return Error code (0 = success)
 */
int http_add_websocket(
    void* server,
    const char* path,
    uint32_t handler_id,
    int* error_out
) noexcept {
    if (!server || !path || !error_out) {
        if (error_out) *error_out = 1;
        return 1;
    }
    
    auto* s = reinterpret_cast<HttpServer*>(server);
    
    // Create WebSocket handler that will call Python callback
    HttpServer::WebSocketHandler handler = [handler_id](WebSocketHandler* ws) {
        // This will be implemented to call Python callback
        // For now, just echo messages
        // ws->send("Echo: " + message);
    };
    
    int result = s->add_websocket(path, std::move(handler));
    
    if (error_out) *error_out = result;
    return result;
}

// ==============================================================================
// Statistics
// ==============================================================================

/**
 * Get server statistics.
 *
 * @param server Server handle
 * @param out_stats Output stats structure
 * @return Error code (0 = success)
 */
int http_server_stats(void* server, void* out_stats) noexcept {
    if (!server || !out_stats) return 1;
    
    auto* s = reinterpret_cast<HttpServer*>(server);
    auto stats = s->get_stats();
    
    // Copy stats to output structure
    // This will be implemented with proper structure definition
    return 0;
}

// ==============================================================================
// Library Initialization
// ==============================================================================

/**
 * Initialize the HTTP library.
 *
 * Called once at library load time.
 *
 * @return Error code (0 = success)
 */
int http_lib_init() noexcept {
    if (g_initialized.load()) {
        return 0;  // Already initialized
    }

    // Initialize Python callback bridge
    PythonCallbackBridge::initialize();

    // Initialize any other global resources
    g_initialized.store(true);
    return 0;
}

/**
 * Shutdown the HTTP library.
 *
 * Called at library unload time.
 *
 * @return Error code (0 = success)
 */
int http_lib_shutdown() noexcept {
    if (g_server) {
        g_server->stop();
        g_server.reset();
    }

    // Cleanup Python callback bridge
    PythonCallbackBridge::cleanup();

    g_initialized.store(false);
    return 0;
}

/**
 * Register Python handler callback.
 *
 * This is called from Python (via ctypes) to associate a Python callable
 * with a handler ID. The PythonCallbackBridge stores this mapping.
 *
 * @param method HTTP method
 * @param path Route path
 * @param handler_id Handler ID (matches ID passed to http_add_route)
 * @param py_callable Python callable object (PyObject*)
 */
void http_register_python_handler(
    const char* method,
    const char* path,
    int handler_id,
    void* py_callable
) noexcept {
    if (!method || !path || !py_callable) {
        return;
    }

    // Register with PythonCallbackBridge (lockfree queue)
    PythonCallbackBridge::register_handler(
        std::string(method),
        std::string(path),
        handler_id,
        py_callable
    );
}

}  // extern "C"