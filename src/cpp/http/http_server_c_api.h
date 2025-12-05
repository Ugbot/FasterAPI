#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/**
 * C API for FasterAPI HTTP Server
 *
 * This provides a pure C interface to the C++ HttpServer class,
 * enabling ctypes bindings from Python without requiring C++.
 *
 * All functions are thread-safe and use the lockfree CoroIO architecture.
 */

/**
 * Opaque handle to HTTP server instance.
 */
typedef void* HttpServerHandle;

/**
 * Error codes.
 */
#define HTTP_OK 0
#define HTTP_ERROR_INVALID_ARGUMENT 1
#define HTTP_ERROR_ALREADY_RUNNING 2
#define HTTP_ERROR_NOT_RUNNING 3
#define HTTP_ERROR_START_FAILED 4
#define HTTP_ERROR_STOP_FAILED 5
#define HTTP_ERROR_OUT_OF_MEMORY 6

/**
 * Initialize the HTTP library.
 *
 * Must be called before any other functions.
 *
 * @return HTTP_OK on success, error code otherwise
 */
int http_lib_init();

/**
 * Connect a RouteRegistry to the Python callback bridge.
 *
 * This enables metadata-aware parameter extraction from registered routes.
 * Must be called after both the HTTP library and Cython module are loaded.
 *
 * @param registry_ptr Pointer to RouteRegistry instance (from Cython module)
 * @return HTTP_OK on success, error code otherwise
 */
int http_connect_route_registry(void* registry_ptr);

/**
 * Create a new HTTP server.
 *
 * @param port Server port (TCP for HTTP/1.1 and HTTP/2)
 * @param host Server host (e.g., "0.0.0.0" or "127.0.0.1")
 * @param enable_h2 Enable HTTP/2 over TLS with ALPN
 * @param enable_h3 Enable HTTP/3 over QUIC (UDP)
 * @param enable_webtransport Enable WebTransport over HTTP/3
 * @param http3_port UDP port for HTTP/3 (default 443)
 * @param enable_compression Enable zstd compression
 * @param error_out [out] Error code if creation fails
 * @return Server handle on success, NULL on failure
 */
HttpServerHandle http_server_create(
    uint16_t port,
    const char* host,
    bool enable_h2,
    bool enable_h3,
    bool enable_webtransport,
    uint16_t http3_port,
    bool enable_compression,
    int* error_out
);

/**
 * Add a route handler.
 *
 * Handler registration is lockfree - this function pushes to a queue
 * that the event loop polls. The handler becomes active after the next poll.
 *
 * @param handle Server handle
 * @param method HTTP method (e.g., "GET", "POST")
 * @param path Route path (e.g., "/", "/api/users")
 * @param handler_id Unique handler ID (used by Python to identify callback)
 * @param error_out [out] Error code if operation fails
 * @return HTTP_OK on success, error code otherwise
 */
int http_add_route(
    HttpServerHandle handle,
    const char* method,
    const char* path,
    uint32_t handler_id,
    int* error_out
);

/**
 * Add a WebSocket endpoint.
 *
 * @param handle Server handle
 * @param path WebSocket path
 * @param handler_id Unique handler ID
 * @param error_out [out] Error code if operation fails
 * @return HTTP_OK on success, error code otherwise
 */
int http_add_websocket(
    HttpServerHandle handle,
    const char* path,
    uint32_t handler_id,
    int* error_out
);

/**
 * Register WebSocket handler metadata for Python handler lookup.
 *
 * This stores the module.function info for a WebSocket path so that
 * when a WebSocket connection is made, the worker can import and call
 * the correct Python handler.
 *
 * @param path WebSocket path (e.g., "/ws/echo")
 * @param module_name Python module name (e.g., "myapp.handlers")
 * @param function_name Python function name (e.g., "ws_echo_handler")
 */
void http_register_websocket_handler_metadata(
    const char* path,
    const char* module_name,
    const char* function_name
);

/**
 * Start the HTTP server.
 *
 * This spawns a background thread running the CoroIO event loop.
 * Returns immediately after starting the thread.
 *
 * @param handle Server handle
 * @param error_out [out] Error code if start fails
 * @return HTTP_OK on success, error code otherwise
 */
int http_server_start(HttpServerHandle handle, int* error_out);

/**
 * Stop the HTTP server.
 *
 * Gracefully shuts down the event loop and closes all connections.
 * Blocks until shutdown is complete.
 *
 * @param handle Server handle
 * @param error_out [out] Error code if stop fails
 * @return HTTP_OK on success, error code otherwise
 */
int http_server_stop(HttpServerHandle handle, int* error_out);

/**
 * Check if server is running.
 *
 * @param handle Server handle
 * @return true if running, false otherwise
 */
bool http_server_is_running(HttpServerHandle handle);

/**
 * Destroy HTTP server and free resources.
 *
 * Server must be stopped before calling this.
 *
 * @param handle Server handle
 * @return HTTP_OK on success, error code otherwise
 */
int http_server_destroy(HttpServerHandle handle);

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
);

/**
 * Get handler from RouteRegistry.
 *
 * This retrieves the Python callable handler stored in RouteRegistry metadata.
 * Used by Server._sync_routes_from_registry() to get handlers for routes
 * registered via FastAPI decorators.
 *
 * @param registry_ptr Pointer to RouteRegistry instance
 * @param method HTTP method
 * @param path Route path pattern
 * @return Python callable object (PyObject*) or NULL if not found
 */
void* http_get_route_handler(
    void* registry_ptr,
    const char* method,
    const char* path
);

/**
 * Initialize ProcessPoolExecutor for multiprocessing Python handler execution.
 *
 * This creates a pool of Python worker processes for true multi-core parallelism.
 * Call this instead of initializing SubinterpreterExecutor for better performance.
 *
 * @param num_workers Number of worker processes (0 = auto-detect CPU cores)
 * @param python_executable Path to Python executable (e.g., "python3.13")
 * @param project_dir Project directory to add to sys.path
 * @return HTTP_OK on success, error code otherwise
 */
int http_init_process_pool_executor(
    uint32_t num_workers,
    const char* python_executable,
    const char* project_dir
);

/**
 * Register route metadata for parameter extraction.
 *
 * This registers parameter information for a route, enabling automatic parameter
 * extraction from URLs. Called by Python API when routes are registered.
 *
 * @param method HTTP method (e.g., "GET", "POST")
 * @param path Route path pattern (e.g., "/user/{user_id}")
 * @param param_metadata_json JSON string containing parameter metadata
 * @return HTTP_OK on success, error code otherwise
 */
int http_register_route_metadata(
    const char* method,
    const char* path,
    const char* param_metadata_json
);

#ifdef __cplusplus
}
#endif
