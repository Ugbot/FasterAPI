/**
 * FasterAPI SSE (Server-Sent Events) - C interface for ctypes binding.
 *
 * Implements the SSE protocol (text/event-stream) for real-time
 * server-to-client push notifications.
 *
 * Features:
 * - Event streaming with automatic keep-alive
 * - Named events with custom types
 * - Event ID tracking for reconnection
 * - Automatic retry hints for clients
 * - Zero-copy where possible
 * - Backpressure handling
 *
 * All exported functions use C linkage and void* pointers for FFI safety.
 * Implementation focuses on maximum performance with lock-free operations.
 *
 * Note: Compiled with -fno-exceptions, so no try/catch blocks.
 *
 * Spec: https://html.spec.whatwg.org/multipage/server-sent-events.html
 */

#include "sse.h"
#include <cstring>
#include <memory>
#include <unordered_map>
#include <mutex>

namespace {
    // Connection registry for managing SSE connections
    std::unordered_map<uint64_t, std::unique_ptr<fasterapi::http::SSEConnection>> g_connections;
    std::mutex g_connections_mutex;
    std::atomic<uint64_t> g_next_id{1};
}

extern "C" {

// ==============================================================================
// SSE Connection Management
// ==============================================================================

/**
 * Create a new SSE connection.
 *
 * @param connection_id Unique connection ID (use 0 for auto-generation)
 * @return Connection handle (cast to SSEConnection*), or nullptr on error
 */
void* sse_create(uint64_t connection_id) noexcept {
    using namespace fasterapi::http;

    // Auto-generate ID if not provided
    if (connection_id == 0) {
        connection_id = g_next_id.fetch_add(1, std::memory_order_relaxed);
    }

    // Create connection
    auto conn = std::make_unique<SSEConnection>(connection_id);

    if (!conn) {
        return nullptr;
    }

    auto* conn_ptr = conn.get();

    // Register connection
    {
        std::lock_guard<std::mutex> lock(g_connections_mutex);
        g_connections[connection_id] = std::move(conn);
    }

    return conn_ptr;
}

/**
 * Destroy an SSE connection.
 *
 * @param sse Connection handle from sse_create
 */
void sse_destroy(void* sse) noexcept {
    if (!sse) return;

    auto* conn = reinterpret_cast<fasterapi::http::SSEConnection*>(sse);
    uint64_t conn_id = conn->get_id();

    // Unregister and destroy
    std::lock_guard<std::mutex> lock(g_connections_mutex);
    g_connections.erase(conn_id);
}

/**
 * Send an SSE event.
 *
 * @param sse Connection handle
 * @param data Event data (null-terminated, required)
 * @param event Event type (null-terminated, optional - can be nullptr)
 * @param id Event ID (null-terminated, optional - can be nullptr)
 * @param retry Retry time in milliseconds (use -1 for no retry field)
 * @return 0 on success, error code otherwise
 *
 * Example:
 *   sse_send(conn, "Hello World", nullptr, nullptr, -1);
 *   sse_send(conn, "{\"msg\":\"hi\"}", "chat", "123", 3000);
 */
int sse_send(
    void* sse,
    const char* data,
    const char* event,
    const char* id,
    int retry
) noexcept {
    if (!sse || !data) return 1;

    auto* conn = reinterpret_cast<fasterapi::http::SSEConnection*>(sse);
    std::string data_str(data);

    return conn->send(data_str, event, id, retry);
}

/**
 * Send an SSE comment (ignored by client, useful for keep-alive).
 *
 * @param sse Connection handle
 * @param comment Comment text (null-terminated)
 * @return 0 on success, error code otherwise
 */
int sse_send_comment(void* sse, const char* comment) noexcept {
    if (!sse || !comment) return 1;

    auto* conn = reinterpret_cast<fasterapi::http::SSEConnection*>(sse);
    std::string comment_str(comment);

    return conn->send_comment(comment_str);
}

/**
 * Send a keep-alive ping.
 *
 * Sends a comment to keep connection alive.
 *
 * @param sse Connection handle
 * @return 0 on success, error code otherwise
 */
int sse_ping(void* sse) noexcept {
    if (!sse) return 1;

    auto* conn = reinterpret_cast<fasterapi::http::SSEConnection*>(sse);
    return conn->ping();
}

/**
 * Close the SSE connection.
 *
 * @param sse Connection handle
 * @return 0 on success, error code otherwise
 */
int sse_close(void* sse) noexcept {
    if (!sse) return 1;

    auto* conn = reinterpret_cast<fasterapi::http::SSEConnection*>(sse);
    return conn->close();
}

/**
 * Check if connection is open.
 *
 * @param sse Connection handle
 * @return true if open, false otherwise
 */
bool sse_is_open(void* sse) noexcept {
    if (!sse) return false;

    auto* conn = reinterpret_cast<fasterapi::http::SSEConnection*>(sse);
    return conn->is_open();
}

/**
 * Get number of events sent.
 *
 * @param sse Connection handle
 * @return Number of events sent
 */
uint64_t sse_events_sent(void* sse) noexcept {
    if (!sse) return 0;

    auto* conn = reinterpret_cast<fasterapi::http::SSEConnection*>(sse);
    return conn->events_sent();
}

/**
 * Get total bytes sent.
 *
 * @param sse Connection handle
 * @return Number of bytes sent
 */
uint64_t sse_bytes_sent(void* sse) noexcept {
    if (!sse) return 0;

    auto* conn = reinterpret_cast<fasterapi::http::SSEConnection*>(sse);
    return conn->bytes_sent();
}

/**
 * Get connection ID.
 *
 * @param sse Connection handle
 * @return Connection ID
 */
uint64_t sse_get_id(void* sse) noexcept {
    if (!sse) return 0;

    auto* conn = reinterpret_cast<fasterapi::http::SSEConnection*>(sse);
    return conn->get_id();
}

/**
 * Set last event ID (for reconnection support).
 *
 * @param sse Connection handle
 * @param id Event ID (null-terminated)
 * @return 0 on success
 */
int sse_set_last_event_id(void* sse, const char* id) noexcept {
    if (!sse || !id) return 1;

    auto* conn = reinterpret_cast<fasterapi::http::SSEConnection*>(sse);
    std::string id_str(id);
    conn->set_last_event_id(id_str);

    return 0;
}

/**
 * Get last event ID.
 *
 * @param sse Connection handle
 * @param out_buffer Output buffer for ID
 * @param buffer_size Size of output buffer
 * @return 0 on success, 1 if buffer too small
 */
int sse_get_last_event_id(void* sse, char* out_buffer, size_t buffer_size) noexcept {
    if (!sse || !out_buffer || buffer_size == 0) return 1;

    auto* conn = reinterpret_cast<fasterapi::http::SSEConnection*>(sse);
    const std::string& id = conn->get_last_event_id();

    if (id.length() + 1 > buffer_size) {
        return 1;  // Buffer too small
    }

    std::strncpy(out_buffer, id.c_str(), buffer_size);
    out_buffer[buffer_size - 1] = '\0';  // Ensure null termination

    return 0;
}

// ==============================================================================
// Library Initialization
// ==============================================================================

/**
 * Initialize the SSE library.
 *
 * Called once at library load time.
 *
 * @return Error code (0 = success)
 */
int sse_lib_init() noexcept {
    // Initialize any global resources
    return 0;
}

/**
 * Shutdown the SSE library.
 *
 * Called at library unload time.
 *
 * @return Error code (0 = success)
 */
int sse_lib_shutdown() noexcept {
    // Clean up all connections
    std::lock_guard<std::mutex> lock(g_connections_mutex);
    g_connections.clear();
    return 0;
}

}  // extern "C"
