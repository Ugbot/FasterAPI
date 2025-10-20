#pragma once

#include "../protocol/message.h"
#include <functional>
#include <memory>
#include <string>
#include <atomic>

namespace fasterapi {
namespace mcp {

/**
 * Transport types supported by MCP
 */
enum class TransportType {
    STDIO,      // Standard input/output (local subprocess)
    SSE,        // HTTP with Server-Sent Events
    STREAMABLE, // HTTP with streaming support
    WEBSOCKET   // WebSocket (bidirectional)
};

/**
 * Transport state
 */
enum class TransportState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    DISCONNECTING,
    ERROR
};

/**
 * Abstract transport interface for MCP communication.
 *
 * All transports must implement:
 * - Connection management
 * - Message sending/receiving
 * - Error handling
 */
class Transport {
public:
    using MessageCallback = std::function<void(const std::string& message)>;
    using ErrorCallback = std::function<void(const std::string& error)>;
    using StateCallback = std::function<void(TransportState state)>;

    virtual ~Transport() = default;

    /**
     * Connect the transport.
     *
     * @return 0 on success, negative error code on failure
     */
    virtual int connect() = 0;

    /**
     * Disconnect the transport.
     *
     * @return 0 on success, negative error code on failure
     */
    virtual int disconnect() = 0;

    /**
     * Send a message.
     *
     * @param message JSON-RPC message as string
     * @return 0 on success, negative error code on failure
     */
    virtual int send(const std::string& message) = 0;

    /**
     * Receive a message (blocking).
     *
     * @param timeout_ms Timeout in milliseconds (0 = no timeout)
     * @return Message string, or nullopt on timeout/error
     */
    virtual std::optional<std::string> receive(uint32_t timeout_ms = 0) = 0;

    /**
     * Set message callback for async reception.
     *
     * @param callback Function to call when message received
     */
    virtual void set_message_callback(MessageCallback callback) = 0;

    /**
     * Set error callback.
     *
     * @param callback Function to call on error
     */
    virtual void set_error_callback(ErrorCallback callback) = 0;

    /**
     * Set state change callback.
     *
     * @param callback Function to call on state change
     */
    virtual void set_state_callback(StateCallback callback) = 0;

    /**
     * Get current transport state.
     */
    virtual TransportState get_state() const = 0;

    /**
     * Check if transport is connected.
     */
    virtual bool is_connected() const = 0;

    /**
     * Get transport type.
     */
    virtual TransportType get_type() const = 0;

    /**
     * Get transport name.
     */
    virtual std::string get_name() const = 0;
};

/**
 * Transport factory for creating transports by type
 */
class TransportFactory {
public:
    /**
     * Create a STDIO transport.
     *
     * @param command Command to execute (optional, for client mode)
     * @param args Command arguments
     * @return Unique pointer to transport
     */
    static std::unique_ptr<Transport> create_stdio(
        const std::string& command = "",
        const std::vector<std::string>& args = {}
    );

    /**
     * Create an SSE transport.
     *
     * @param url Server URL
     * @param auth_token Optional authentication token
     * @return Unique pointer to transport
     */
    static std::unique_ptr<Transport> create_sse(
        const std::string& url,
        const std::string& auth_token = ""
    );

    /**
     * Create a streamable HTTP transport.
     *
     * @param url Server URL
     * @param auth_token Optional authentication token
     * @return Unique pointer to transport
     */
    static std::unique_ptr<Transport> create_streamable(
        const std::string& url,
        const std::string& auth_token = ""
    );

    /**
     * Create a WebSocket transport.
     *
     * @param url WebSocket URL
     * @param auth_token Optional authentication token
     * @return Unique pointer to transport
     */
    static std::unique_ptr<Transport> create_websocket(
        const std::string& url,
        const std::string& auth_token = ""
    );
};

} // namespace mcp
} // namespace fasterapi
