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
 * Create a new HTTP server.
 *
 * @param port Server port
 * @param host Server host (e.g., "0.0.0.0" or "127.0.0.1")
 * @param enable_h2 Enable HTTP/2
 * @param enable_h3 Enable HTTP/3
 * @param enable_compression Enable zstd compression
 * @param error_out [out] Error code if creation fails
 * @return Server handle on success, NULL on failure
 */
HttpServerHandle http_server_create(
    uint16_t port,
    const char* host,
    bool enable_h2,
    bool enable_h3,
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

#ifdef __cplusplus
}
#endif
