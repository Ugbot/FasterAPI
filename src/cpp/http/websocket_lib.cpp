/**
 * FasterAPI WebSocket - C interface for ctypes binding.
 *
 * High-performance WebSocket implementation with:
 * - Text and binary message support
 * - Automatic ping/pong handling
 * - Permessage-deflate compression
 * - Fragmentation support
 * - Close handshake
 *
 * All exported functions use C linkage and void* pointers for FFI safety.
 * Implementation focuses on maximum performance with lock-free operations.
 *
 * Note: Compiled with -fno-exceptions, so no try/catch blocks.
 */

#include "websocket.h"
#include <cstring>
#include <memory>
#include <unordered_map>
#include <mutex>

namespace {
    // Connection registry for managing WebSocket connections
    std::unordered_map<uint64_t, std::unique_ptr<fasterapi::http::WebSocketConnection>> g_connections;
    std::mutex g_connections_mutex;
    std::atomic<uint64_t> g_next_id{1};
}

extern "C" {

// ==============================================================================
// WebSocket Connection Management
// ==============================================================================

/**
 * Create a new WebSocket connection.
 *
 * @param connection_id Unique connection ID (use 0 for auto-generation)
 * @return Connection handle (cast to WebSocketConnection*), or nullptr on error
 */
void* ws_create(uint64_t connection_id) noexcept {
    using namespace fasterapi::http;

    // Auto-generate ID if not provided
    if (connection_id == 0) {
        connection_id = g_next_id.fetch_add(1, std::memory_order_relaxed);
    }

    // Create connection with default config
    WebSocketConnection::Config config;
    auto conn = std::make_unique<WebSocketConnection>(connection_id, config);

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
 * Destroy a WebSocket connection.
 *
 * @param ws Connection handle from ws_create
 */
void ws_destroy(void* ws) noexcept {
    if (!ws) return;

    auto* conn = reinterpret_cast<fasterapi::http::WebSocketConnection*>(ws);
    uint64_t conn_id = conn->get_id();

    // Unregister and destroy
    std::lock_guard<std::mutex> lock(g_connections_mutex);
    g_connections.erase(conn_id);
}

/**
 * Send text message.
 *
 * @param ws Connection handle
 * @param message Null-terminated text message
 * @return 0 on success, error code otherwise
 */
int ws_send_text(void* ws, const char* message) noexcept {
    if (!ws || !message) return -1;

    auto* conn = reinterpret_cast<fasterapi::http::WebSocketConnection*>(ws);
    std::string msg(message);

    return conn->send_text(msg);
}

/**
 * Send binary message.
 *
 * @param ws Connection handle
 * @param data Binary data
 * @param length Data length
 * @return 0 on success, error code otherwise
 */
int ws_send_binary(void* ws, const uint8_t* data, size_t length) noexcept {
    if (!ws || !data) return -1;

    auto* conn = reinterpret_cast<fasterapi::http::WebSocketConnection*>(ws);
    return conn->send_binary(data, length);
}

/**
 * Send ping frame.
 *
 * @param ws Connection handle
 * @param data Optional ping data (can be nullptr)
 * @param length Data length (0 if no data)
 * @return 0 on success, error code otherwise
 */
int ws_send_ping(void* ws, const uint8_t* data, size_t length) noexcept {
    if (!ws) return -1;

    auto* conn = reinterpret_cast<fasterapi::http::WebSocketConnection*>(ws);
    return conn->send_ping(data, length);
}

/**
 * Send pong frame.
 *
 * @param ws Connection handle
 * @param data Optional pong data (can be nullptr)
 * @param length Data length (0 if no data)
 * @return 0 on success, error code otherwise
 */
int ws_send_pong(void* ws, const uint8_t* data, size_t length) noexcept {
    if (!ws) return -1;

    auto* conn = reinterpret_cast<fasterapi::http::WebSocketConnection*>(ws);
    return conn->send_pong(data, length);
}

/**
 * Close WebSocket connection.
 *
 * @param ws Connection handle
 * @param code Close code (default 1000 = normal closure)
 * @param reason Null-terminated reason string (can be nullptr)
 * @return 0 on success, error code otherwise
 */
int ws_close(void* ws, uint16_t code, const char* reason) noexcept {
    if (!ws) return -1;

    auto* conn = reinterpret_cast<fasterapi::http::WebSocketConnection*>(ws);
    return conn->close(code, reason);
}

/**
 * Check if connection is open.
 *
 * @param ws Connection handle
 * @return true if open, false otherwise
 */
bool ws_is_open(void* ws) noexcept {
    if (!ws) return false;

    auto* conn = reinterpret_cast<fasterapi::http::WebSocketConnection*>(ws);
    return conn->is_open();
}

/**
 * Get number of messages sent.
 *
 * @param ws Connection handle
 * @return Number of messages sent
 */
uint64_t ws_messages_sent(void* ws) noexcept {
    if (!ws) return 0;

    auto* conn = reinterpret_cast<fasterapi::http::WebSocketConnection*>(ws);
    return conn->messages_sent();
}

/**
 * Get number of messages received.
 *
 * @param ws Connection handle
 * @return Number of messages received
 */
uint64_t ws_messages_received(void* ws) noexcept {
    if (!ws) return 0;

    auto* conn = reinterpret_cast<fasterapi::http::WebSocketConnection*>(ws);
    return conn->messages_received();
}

/**
 * Get total bytes sent.
 *
 * @param ws Connection handle
 * @return Number of bytes sent
 */
uint64_t ws_bytes_sent(void* ws) noexcept {
    if (!ws) return 0;

    auto* conn = reinterpret_cast<fasterapi::http::WebSocketConnection*>(ws);
    return conn->bytes_sent();
}

/**
 * Get total bytes received.
 *
 * @param ws Connection handle
 * @return Number of bytes received
 */
uint64_t ws_bytes_received(void* ws) noexcept {
    if (!ws) return 0;

    auto* conn = reinterpret_cast<fasterapi::http::WebSocketConnection*>(ws);
    return conn->bytes_received();
}

/**
 * Get connection ID.
 *
 * @param ws Connection handle
 * @return Connection ID
 */
uint64_t ws_get_id(void* ws) noexcept {
    if (!ws) return 0;

    auto* conn = reinterpret_cast<fasterapi::http::WebSocketConnection*>(ws);
    return conn->get_id();
}

// ==============================================================================
// Library Initialization
// ==============================================================================

/**
 * Initialize the WebSocket library.
 *
 * Called once at library load time.
 *
 * @return Error code (0 = success)
 */
int ws_lib_init() noexcept {
    // Initialize any global resources
    return 0;
}

/**
 * Shutdown the WebSocket library.
 *
 * Called at library unload time.
 *
 * @return Error code (0 = success)
 */
int ws_lib_shutdown() noexcept {
    // Clean up all connections
    std::lock_guard<std::mutex> lock(g_connections_mutex);
    g_connections.clear();
    return 0;
}

}  // extern "C"
