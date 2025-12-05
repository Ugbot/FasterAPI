/**
 * HTTP/1.1 Connection Handler
 *
 * Manages HTTP/1.0 and HTTP/1.1 connections with:
 * - Request parsing (HTTP1Parser)
 * - Response generation
 * - Keep-alive (persistent connections)
 * - Python callback integration
 * - Event loop integration
 *
 * Supports both cleartext and TLS connections.
 */

#pragma once

#include "http1_parser.h"
#include "../core/result.h"
#include <string>
#include <unordered_map>
#include <functional>
#include <vector>

namespace fasterapi {
namespace http {

/**
 * HTTP/1.1 Connection State
 */
enum class Http1State {
    READING_REQUEST,     // Parsing request headers
    READING_BODY,        // Reading request body
    PROCESSING,          // Calling Python handler
    WRITING_RESPONSE,    // Sending response
    KEEPALIVE,           // Connection kept alive, ready for next request
    CLOSING,             // Connection closing
    ERROR                // Error state
};

/**
 * HTTP/1.1 Response
 */
struct Http1Response {
    uint16_t status = 200;
    std::string status_message = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    // WebSocket upgrade flag
    bool websocket_upgrade = false;
    std::string websocket_path;  // Path for WebSocket handler lookup

    // Helper: Add header
    void add_header(const std::string& name, const std::string& value) {
        headers[name] = value;
    }

    // Helper: Set content type
    void set_content_type(const std::string& type) {
        headers["Content-Type"] = type;
    }

    // Helper: Mark as WebSocket upgrade response
    void mark_websocket_upgrade(const std::string& path) {
        websocket_upgrade = true;
        websocket_path = path;
    }
};

/**
 * HTTP/1.1 Connection Handler
 *
 * Manages a single HTTP/1.1 connection lifecycle.
 * Supports keep-alive for connection reuse.
 *
 * Usage:
 *   Http1Connection conn(socket_fd);
 *   conn.set_request_callback(my_handler);
 *
 *   // In event loop:
 *   conn.process_input(data, len);  // Parse request
 *   conn.process_output();           // Send response
 */
class Http1Connection {
public:
    /**
     * Request callback type
     *
     * Called when a complete HTTP request is received.
     * Handler should fill in the response and return.
     *
     * @param method HTTP method string
     * @param path URL path
     * @param headers Request headers
     * @param body Request body
     * @return Response to send to client
     */
    using RequestCallback = std::function<Http1Response(
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body
    )>;

    /**
     * Create HTTP/1.1 connection
     *
     * @param socket_fd Socket file descriptor (ownership not transferred)
     */
    explicit Http1Connection(int socket_fd);

    /**
     * Destructor
     */
    ~Http1Connection();

    // Non-copyable, movable
    Http1Connection(const Http1Connection&) = delete;
    Http1Connection& operator=(const Http1Connection&) = delete;
    Http1Connection(Http1Connection&& other) noexcept;
    Http1Connection& operator=(Http1Connection&& other) noexcept;

    /**
     * Set request callback
     *
     * Called when a complete request is received.
     *
     * @param callback Handler function
     */
    void set_request_callback(RequestCallback callback) {
        request_callback_ = std::move(callback);
    }

    /**
     * Process incoming data
     *
     * Parse HTTP request from incoming data.
     * May trigger request callback if request is complete.
     *
     * @param data Incoming data buffer
     * @param len Data length
     * @return Number of bytes consumed, or -1 on error
     */
    core::result<size_t> process_input(const uint8_t* data, size_t len) noexcept;

    /**
     * Get output data to send
     *
     * Returns pointer to response data that needs to be sent.
     *
     * @param out_data Output buffer pointer (output)
     * @param out_len Output buffer length (output)
     * @return true if data available, false if none
     */
    bool get_output(const uint8_t** out_data, size_t* out_len) noexcept;

    /**
     * Commit sent output
     *
     * Call after sending data returned by get_output().
     * Advances the output pointer.
     *
     * @param len Number of bytes sent
     */
    void commit_output(size_t len) noexcept;

    /**
     * Get connection state
     */
    Http1State get_state() const noexcept {
        return state_;
    }

    /**
     * Check if connection should be kept alive
     */
    bool should_keep_alive() const noexcept {
        return keep_alive_ && state_ != Http1State::ERROR && state_ != Http1State::CLOSING;
    }

    /**
     * Check if connection has data to send
     */
    bool has_pending_output() const noexcept {
        return output_offset_ < output_buffer_.size();
    }

    /**
     * Reset connection for next request (keep-alive)
     */
    void reset_for_next_request() noexcept;

    /**
     * Get error message (if in ERROR state)
     */
    const std::string& get_error() const noexcept {
        return error_message_;
    }

    /**
     * Check if connection is pending WebSocket upgrade.
     * Returns true if the last response was a 101 Switching Protocols.
     */
    bool is_websocket_upgrade() const noexcept {
        return pending_websocket_upgrade_;
    }

    /**
     * Get WebSocket path (for handler lookup after upgrade).
     */
    const std::string& get_websocket_path() const noexcept {
        return pending_websocket_path_;
    }

private:
    /**
     * Parse request from input buffer
     */
    core::result<void> parse_request() noexcept;

    /**
     * Handle complete request (invoke callback)
     */
    core::result<void> handle_request() noexcept;

    /**
     * Build HTTP response
     */
    void build_response(const Http1Response& response) noexcept;

    /**
     * Build response status line
     */
    std::string build_status_line(uint16_t status, const std::string& message) const;

    /**
     * Should connection be kept alive based on request?
     */
    bool should_keep_alive_from_request(const HTTP1Request& request) const noexcept;

    int socket_fd_;
    Http1State state_ = Http1State::READING_REQUEST;

    // Parsing
    HTTP1Parser parser_;
    std::vector<uint8_t> input_buffer_;  // Accumulates partial requests
    HTTP1Request current_request_;
    size_t body_bytes_read_ = 0;

    // Response
    std::vector<uint8_t> output_buffer_;
    size_t output_offset_ = 0;

    // Keep-alive
    bool keep_alive_ = true;
    size_t requests_served_ = 0;

    // Callback
    RequestCallback request_callback_;

    // Error tracking
    std::string error_message_;

    // WebSocket upgrade tracking
    bool pending_websocket_upgrade_ = false;
    std::string pending_websocket_path_;
};

} // namespace http
} // namespace fasterapi
