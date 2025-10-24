#pragma once

#include "http2_frame.h"
#include "../core/result.h"
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace fasterapi {
namespace http2 {

/**
 * HTTP/2 Stream States (RFC 7540 Section 5.1)
 *
 * Stream state machine:
 *                          +--------+
 *                  send PP |        | recv PP
 *                 ,--------|  idle  |--------.
 *                /         |        |         \
 *               v          +--------+          v
 *        +----------+          |           +----------+
 *        |          |          | send H /  |          |
 * ,------| reserved |          | recv H    | reserved |------.
 * |      | (local)  |          |           | (remote) |      |
 * |      +----------+          v           +----------+      |
 * |          |             +--------+             |          |
 * |          |     recv ES |        | send ES     |          |
 * |   send H |     ,-------|  open  |-------.     | recv H   |
 * |          |    /        |        |        \    |          |
 * |          v   v         +--------+         v   v          |
 * |      +----------+          |           +----------+      |
 * |      |   half   |          |           |   half   |      |
 * |      |  closed  |          | send R /  |  closed  |      |
 * |      | (remote) |          | recv R    | (local)  |      |
 * |      +----------+          |           +----------+      |
 * |           |                |                 |           |
 * |           | send ES /      |       recv ES / |           |
 * |           | send R /       v        send R / |           |
 * |           | recv R     +--------+   recv R   |           |
 * | send R /  `----------->|        |<-----------'  send R / |
 * | recv R                 | closed |               recv R   |
 * `----------------------->|        |<----------------------'
 *                          +--------+
 *
 * send:   endpoint sends this frame
 * recv:   endpoint receives this frame
 * H:      HEADERS frame (with or without END_STREAM flag)
 * PP:     PUSH_PROMISE
 * ES:     END_STREAM flag
 * R:      RST_STREAM frame
 */
enum class StreamState : uint8_t {
    IDLE = 0,              // Not yet used
    RESERVED_LOCAL = 1,    // Reserved by local PUSH_PROMISE
    RESERVED_REMOTE = 2,   // Reserved by remote PUSH_PROMISE
    OPEN = 3,              // Open for both sides
    HALF_CLOSED_LOCAL = 4, // Local sent END_STREAM
    HALF_CLOSED_REMOTE = 5,// Remote sent END_STREAM
    CLOSED = 6             // Stream closed
};

/**
 * HTTP/2 Stream.
 *
 * Represents a single request-response exchange.
 * Handles state machine, flow control, and data buffering.
 */
class Http2Stream {
public:
    // Allow StreamManager to access private members
    friend class StreamManager;

    /**
     * Create stream.
     *
     * @param stream_id Stream ID (must be odd for client-initiated, even for server push)
     * @param initial_window_size Initial flow control window (default 65535)
     */
    explicit Http2Stream(uint32_t stream_id, uint32_t initial_window_size = 65535);

    // Stream ID
    uint32_t id() const noexcept { return stream_id_; }

    // Current state
    StreamState state() const noexcept { return state_; }

    /**
     * State transitions.
     */
    void on_headers_sent(bool end_stream) noexcept;
    void on_headers_received(bool end_stream) noexcept;
    void on_data_sent(bool end_stream) noexcept;
    void on_data_received(bool end_stream) noexcept;
    void on_rst_stream() noexcept;
    void on_push_promise_sent() noexcept;
    void on_push_promise_received() noexcept;

    /**
     * Check if stream is closed.
     */
    bool is_closed() const noexcept { return state_ == StreamState::CLOSED; }

    /**
     * Check if stream can send data.
     */
    bool can_send() const noexcept {
        return state_ == StreamState::OPEN || state_ == StreamState::HALF_CLOSED_REMOTE;
    }

    /**
     * Check if stream can receive data.
     */
    bool can_receive() const noexcept {
        return state_ == StreamState::OPEN || state_ == StreamState::HALF_CLOSED_LOCAL;
    }

    /**
     * Flow control - send window.
     *
     * Amount of data we're allowed to send before receiving WINDOW_UPDATE.
     */
    int32_t send_window() const noexcept { return send_window_; }

    /**
     * Flow control - receive window.
     *
     * Amount of data we can receive before sending WINDOW_UPDATE.
     */
    int32_t recv_window() const noexcept { return recv_window_; }

    /**
     * Update send window (when WINDOW_UPDATE received).
     *
     * @param increment Window increment (must be positive)
     * @return Success or error
     */
    core::result<void> update_send_window(int32_t increment) noexcept;

    /**
     * Update receive window (when we send WINDOW_UPDATE).
     *
     * @param increment Window increment (must be positive)
     * @return Success or error
     */
    core::result<void> update_recv_window(int32_t increment) noexcept;

    /**
     * Consume send window (when DATA frame sent).
     *
     * @param size Data size
     * @return Success or error if window would become negative
     */
    core::result<void> consume_send_window(uint32_t size) noexcept;

    /**
     * Consume receive window (when DATA frame received).
     *
     * @param size Data size
     * @return Success or error if window would become negative
     */
    core::result<void> consume_recv_window(uint32_t size) noexcept;

    /**
     * Priority information (RFC 7540 Section 5.3).
     */
    bool has_priority() const noexcept { return has_priority_; }
    const PrioritySpec& priority() const noexcept { return priority_; }
    void set_priority(const PrioritySpec& spec) noexcept {
        priority_ = spec;
        has_priority_ = true;
    }

    /**
     * Request headers (after HPACK decoding).
     */
    const std::unordered_map<std::string, std::string>& request_headers() const noexcept {
        return request_headers_;
    }

    void add_request_header(std::string name, std::string value) {
        request_headers_[std::move(name)] = std::move(value);
    }

    /**
     * Request body data.
     */
    const std::string& request_body() const noexcept { return request_body_; }

    void append_request_body(const uint8_t* data, size_t len) {
        request_body_.append(reinterpret_cast<const char*>(data), len);
    }

    void append_request_body(std::string data) {
        request_body_.append(std::move(data));
    }

    /**
     * Response status code.
     */
    uint16_t response_status() const noexcept { return response_status_; }
    void set_response_status(uint16_t status) noexcept { response_status_ = status; }

    /**
     * Response headers.
     */
    const std::unordered_map<std::string, std::string>& response_headers() const noexcept {
        return response_headers_;
    }

    void add_response_header(std::string name, std::string value) {
        response_headers_[std::move(name)] = std::move(value);
    }

    /**
     * Response body data.
     */
    const std::string& response_body() const noexcept { return response_body_; }

    void set_response_body(std::string body) {
        response_body_ = std::move(body);
    }

    void append_response_body(std::string data) {
        response_body_.append(std::move(data));
    }

    /**
     * Error code (if RST_STREAM sent/received).
     */
    ErrorCode error_code() const noexcept { return error_code_; }
    void set_error_code(ErrorCode code) noexcept { error_code_ = code; }

private:
    uint32_t stream_id_;
    StreamState state_{StreamState::IDLE};

    // Flow control windows
    int32_t send_window_;  // Data we can send
    int32_t recv_window_;  // Data we can receive

    // Priority
    bool has_priority_{false};
    PrioritySpec priority_;

    // Request data
    std::unordered_map<std::string, std::string> request_headers_;
    std::string request_body_;

    // Response data
    uint16_t response_status_{200};
    std::unordered_map<std::string, std::string> response_headers_;
    std::string response_body_;

    // Error handling
    ErrorCode error_code_{ErrorCode::NO_ERROR};
};

/**
 * HTTP/2 Stream Manager.
 *
 * Manages all active streams for a connection.
 */
class StreamManager {
public:
    /**
     * Create stream manager.
     *
     * @param initial_window_size Initial flow control window for new streams
     */
    explicit StreamManager(uint32_t initial_window_size = 65535);

    /**
     * Create new stream.
     *
     * @param stream_id Stream ID
     * @return Stream pointer or error if stream already exists
     */
    core::result<Http2Stream*> create_stream(uint32_t stream_id) noexcept;

    /**
     * Get stream by ID.
     *
     * @param stream_id Stream ID
     * @return Stream pointer or nullptr if not found
     */
    Http2Stream* get_stream(uint32_t stream_id) noexcept;

    /**
     * Remove stream (when closed and processed).
     *
     * @param stream_id Stream ID
     */
    void remove_stream(uint32_t stream_id) noexcept;

    /**
     * Get number of active streams.
     */
    size_t stream_count() const noexcept { return streams_.size(); }

    /**
     * Update initial window size for all streams (SETTINGS_INITIAL_WINDOW_SIZE).
     *
     * @param new_size New initial window size
     */
    void update_initial_window_size(uint32_t new_size) noexcept;

    /**
     * Get initial window size.
     */
    uint32_t initial_window_size() const noexcept { return initial_window_size_; }

private:
    std::unordered_map<uint32_t, Http2Stream> streams_;
    uint32_t initial_window_size_;
};

} // namespace http2
} // namespace fasterapi
