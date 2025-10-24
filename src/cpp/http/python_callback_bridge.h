#pragma once

#include <Python.h>
#include <string>
#include <unordered_map>
#include <functional>
#include "../core/lockfree_queue.h"
#include "../core/result.h"
#include "../core/future.h"

/**
 * Bridge between C++ HTTP server and Python route handlers.
 *
 * This allows C++ coroutines to invoke Python callbacks with minimal overhead.
 * Uses PyGILState API for thread-safe Python calls from C++ threads.
 *
 * LOCKFREE DESIGN:
 * - Python thread pushes handler registrations to lockfree queue
 * - Event loop thread polls queue and updates routing table
 * - No mutexes, no contention, <50ns latency
 */
class PythonCallbackBridge {
public:
    /**
     * Python handler result containing response data.
     */
    struct HandlerResult {
        int status_code = 200;
        std::string content_type = "text/plain";
        std::string body;
        std::unordered_map<std::string, std::string> headers;
    };

    /**
     * Serialized request data (no Python objects - safe for inter-interpreter passing).
     * Used to pass request data to sub-interpreters without GIL conflicts.
     */
    struct SerializedRequest {
        std::string method;
        std::string path;
        std::unordered_map<std::string, std::string> headers;
        std::string body;
        int handler_id;
        PyObject* callable;  // Handler callable (must be used in same interpreter it was created)
    };

    /**
     * Handler registration message (passed via lockfree queue).
     */
    struct HandlerRegistration {
        std::string method;
        std::string path;
        int handler_id;
        PyObject* callable;
    };

    /**
     * Initialize the bridge.
     */
    static void initialize();

    /**
     * Register a Python callable for a route.
     *
     * LOCKFREE: Pushes to queue, returns immediately.
     * Registration becomes visible on event loop thread after poll.
     *
     * @param method HTTP method (GET, POST, etc.)
     * @param path Route path
     * @param handler_id Unique handler ID
     * @param callable Python callable object (as void pointer from Cython)
     */
    static void register_handler(
        const std::string& method,
        const std::string& path,
        int handler_id,
        void* callable
    );

    /**
     * Poll registration queue and update handler map.
     *
     * MUST be called from event loop thread before accepting connections.
     * Drains queue and updates routing table.
     */
    static void poll_registrations();

    /**
     * Invoke Python handler for a request (SYNCHRONOUS - DEPRECATED).
     *
     * This blocks the calling thread while Python executes.
     * Use invoke_handler_async() for non-blocking execution.
     *
     * @param method HTTP method
     * @param path Request path
     * @param headers Request headers
     * @param body Request body
     * @return Handler result with response data
     */
    static HandlerResult invoke_handler(
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body
    );

    /**
     * Invoke Python handler asynchronously using sub-interpreters.
     *
     * This submits the handler to SubinterpreterExecutor and returns immediately.
     * The handler executes in a sub-interpreter with its own GIL (true parallelism).
     *
     * @param method HTTP method
     * @param path Request path
     * @param headers Request headers
     * @param body Request body
     * @return Future with handler result
     */
    static fasterapi::core::future<fasterapi::core::result<HandlerResult>> invoke_handler_async(
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body
    ) noexcept;

    /**
     * Cleanup all registered handlers.
     */
    static void cleanup();

private:
    // Lockfree queue for handler registrations (Python thread -> Event loop thread)
    // Capacity: 1024 pending registrations (more than enough for typical apps)
    static fasterapi::core::AeronSPSCQueue<HandlerRegistration> registration_queue_;

    // Map of "METHOD:path" -> (handler_id, PyObject* callable)
    // Only accessed by event loop thread after polling queue
    static std::unordered_map<std::string, std::pair<int, PyObject*>> handlers_;
};
