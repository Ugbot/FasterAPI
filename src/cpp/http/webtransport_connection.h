#pragma once

#include "quic/quic_connection.h"
#include "quic/quic_stream.h"
#include "quic/quic_frames.h"
#include "../core/ring_buffer.h"
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <atomic>

namespace fasterapi {
namespace http {

/**
 * WebTransport connection (RFC 9297).
 *
 * Implements WebTransport protocol over HTTP/3 and QUIC:
 * - Bidirectional streams (reliable, ordered)
 * - Unidirectional streams (reliable, ordered, one-way)
 * - Datagrams (unreliable, unordered)
 *
 * Features:
 * - Zero-copy data transfer using ring buffers
 * - Pre-allocated stream pools (no malloc in hot path)
 * - Python callback support
 * - RFC 9297 compliant
 *
 * Usage:
 *   auto wt = std::make_unique<WebTransportConnection>(std::move(quic_conn));
 *   wt->on_stream_data([](uint64_t stream_id, const uint8_t* data, size_t len) {
 *       // Handle bidirectional stream data
 *   });
 *
 *   uint64_t stream_id = wt->open_stream();
 *   wt->send_stream(stream_id, data, len);
 *
 *   wt->send_datagram(data, len);
 */
class WebTransportConnection {
public:
    /**
     * Callback for bidirectional stream data.
     *
     * @param stream_id Stream ID
     * @param data Data buffer
     * @param length Data length
     */
    using StreamDataCallback = std::function<void(uint64_t stream_id, const uint8_t* data, size_t length)>;

    /**
     * Callback for unidirectional stream data.
     *
     * @param stream_id Stream ID
     * @param data Data buffer
     * @param length Data length
     */
    using UnidirectionalDataCallback = std::function<void(uint64_t stream_id, const uint8_t* data, size_t length)>;

    /**
     * Callback for datagram received.
     *
     * @param data Datagram data
     * @param length Datagram length
     */
    using DatagramCallback = std::function<void(const uint8_t* data, size_t length)>;

    /**
     * Callback for stream opened by peer.
     *
     * @param stream_id Stream ID
     * @param is_bidirectional True if bidirectional, false if unidirectional
     */
    using StreamOpenedCallback = std::function<void(uint64_t stream_id, bool is_bidirectional)>;

    /**
     * Callback for stream closed.
     *
     * @param stream_id Stream ID
     */
    using StreamClosedCallback = std::function<void(uint64_t stream_id)>;

    /**
     * Callback for connection closed.
     *
     * @param error_code Error code (0 = normal close)
     * @param reason Reason phrase
     */
    using ConnectionClosedCallback = std::function<void(uint64_t error_code, const char* reason)>;

    /**
     * WebTransport session state.
     */
    enum class State {
        CONNECTING,     // Establishing session (HTTP/3 CONNECT in progress)
        CONNECTED,      // Session established
        CLOSING,        // Graceful shutdown initiated
        CLOSED,         // Session closed
    };

    /**
     * Constructor.
     *
     * @param quic_conn Underlying QUIC connection (ownership transferred)
     */
    explicit WebTransportConnection(std::unique_ptr<quic::QUICConnection> quic_conn) noexcept;

    /**
     * Destructor.
     */
    ~WebTransportConnection() noexcept;

    // No copy/move (manages underlying QUIC connection)
    WebTransportConnection(const WebTransportConnection&) = delete;
    WebTransportConnection& operator=(const WebTransportConnection&) = delete;

    /**
     * Initialize WebTransport session.
     *
     * Server-side: automatically accepts incoming CONNECT request.
     * Client-side: sends CONNECT request to establish session.
     *
     * @return 0 on success, -1 on error
     */
    int initialize() noexcept;

    /**
     * Connect to WebTransport endpoint (client-side).
     *
     * Sends HTTP/3 CONNECT request with :protocol = "webtransport".
     *
     * @param url WebTransport URL (e.g., "https://example.com/wt")
     * @return 0 on success, -1 on error
     */
    int connect(const char* url) noexcept;

    /**
     * Accept incoming WebTransport session (server-side).
     *
     * @return 0 on success, -1 on error
     */
    int accept() noexcept;

    /**
     * Open bidirectional stream.
     *
     * Creates new stream for bidirectional communication.
     *
     * @return Stream ID on success, 0 on error
     */
    uint64_t open_stream() noexcept;

    /**
     * Send data on bidirectional stream.
     *
     * @param stream_id Stream ID
     * @param data Data to send
     * @param length Data length
     * @return Number of bytes written, or -1 on error
     */
    ssize_t send_stream(uint64_t stream_id, const uint8_t* data, size_t length) noexcept;

    /**
     * Close bidirectional stream.
     *
     * Sends FIN on stream (graceful close).
     *
     * @param stream_id Stream ID
     * @return 0 on success, -1 on error
     */
    int close_stream(uint64_t stream_id) noexcept;

    /**
     * Open unidirectional stream.
     *
     * Creates new stream for one-way communication (send-only).
     *
     * @return Stream ID on success, 0 on error
     */
    uint64_t open_unidirectional_stream() noexcept;

    /**
     * Send data on unidirectional stream.
     *
     * @param stream_id Stream ID
     * @param data Data to send
     * @param length Data length
     * @return Number of bytes written, or -1 on error
     */
    ssize_t send_unidirectional(uint64_t stream_id, const uint8_t* data, size_t length) noexcept;

    /**
     * Close unidirectional stream.
     *
     * @param stream_id Stream ID
     * @return 0 on success, -1 on error
     */
    int close_unidirectional_stream(uint64_t stream_id) noexcept;

    /**
     * Send datagram.
     *
     * Sends unreliable, unordered datagram over QUIC.
     * May be dropped or reordered.
     *
     * @param data Datagram data
     * @param length Datagram length (max ~1200 bytes)
     * @return 0 on success, -1 on error
     */
    int send_datagram(const uint8_t* data, size_t length) noexcept;

    /**
     * Process incoming QUIC packet.
     *
     * Call this when data arrives from the network.
     * Dispatches callbacks as needed.
     *
     * @param data Packet data
     * @param length Packet length
     * @param now_us Current time (microseconds)
     * @return 0 on success, -1 on error
     */
    int process_datagram(const uint8_t* data, size_t length, uint64_t now_us) noexcept;

    /**
     * Generate outgoing QUIC packets.
     *
     * Call this to generate data to send over the network.
     *
     * @param output Output buffer
     * @param capacity Output capacity
     * @param now_us Current time (microseconds)
     * @return Number of bytes written
     */
    size_t generate_datagrams(uint8_t* output, size_t capacity, uint64_t now_us) noexcept;

    /**
     * Set callback for bidirectional stream data.
     *
     * @param callback Callback function
     */
    void on_stream_data(StreamDataCallback callback) noexcept {
        stream_data_callback_ = callback;
    }

    /**
     * Set callback for unidirectional stream data.
     *
     * @param callback Callback function
     */
    void on_unidirectional_data(UnidirectionalDataCallback callback) noexcept {
        unidirectional_data_callback_ = callback;
    }

    /**
     * Set callback for datagram received.
     *
     * @param callback Callback function
     */
    void on_datagram(DatagramCallback callback) noexcept {
        datagram_callback_ = callback;
    }

    /**
     * Set callback for stream opened by peer.
     *
     * @param callback Callback function
     */
    void on_stream_opened(StreamOpenedCallback callback) noexcept {
        stream_opened_callback_ = callback;
    }

    /**
     * Set callback for stream closed.
     *
     * @param callback Callback function
     */
    void on_stream_closed(StreamClosedCallback callback) noexcept {
        stream_closed_callback_ = callback;
    }

    /**
     * Set callback for connection closed.
     *
     * @param callback Callback function
     */
    void on_connection_closed(ConnectionClosedCallback callback) noexcept {
        connection_closed_callback_ = callback;
    }

    /**
     * Check if connection is closed.
     */
    bool is_closed() const noexcept {
        return state_ == State::CLOSED;
    }

    /**
     * Check if connection is connected.
     */
    bool is_connected() const noexcept {
        return state_ == State::CONNECTED;
    }

    /**
     * Get current state.
     */
    State state() const noexcept {
        return state_;
    }

    /**
     * Close connection.
     *
     * @param error_code Error code (0 = normal close)
     * @param reason Reason phrase (optional)
     */
    void close(uint64_t error_code = 0, const char* reason = nullptr) noexcept;

    /**
     * Get statistics.
     *
     * @return Statistics (key-value pairs)
     */
    std::unordered_map<std::string, uint64_t> get_stats() const noexcept;

    /**
     * Get underlying QUIC connection.
     */
    quic::QUICConnection* quic_connection() noexcept {
        return quic_conn_.get();
    }

private:
    /**
     * Process incoming stream data.
     */
    void process_stream_data(uint64_t stream_id, const uint8_t* data, size_t length) noexcept;

    /**
     * Process incoming datagram frame.
     */
    void process_datagram_frame(const quic::DatagramFrame& frame) noexcept;

    /**
     * Handle stream opened by peer.
     */
    void handle_peer_stream_opened(uint64_t stream_id) noexcept;

    /**
     * Handle stream closed.
     */
    void handle_stream_closed(uint64_t stream_id) noexcept;

    /**
     * Check if stream is bidirectional.
     */
    bool is_bidirectional_stream(uint64_t stream_id) const noexcept {
        // Bit 1 of stream ID: 0 = bidi, 1 = uni
        return (stream_id & 0x02) == 0;
    }

    /**
     * Check if stream is initiated by peer.
     */
    bool is_peer_initiated_stream(uint64_t stream_id) const noexcept {
        // Bit 0 of stream ID: 0 = client, 1 = server
        bool is_server_stream = (stream_id & 0x01) != 0;
        return quic_conn_->is_established() && (is_server_ != is_server_stream);
    }

    /**
     * Send HTTP/3 CONNECT request (client-side).
     */
    int send_connect_request(const char* url) noexcept;

    /**
     * Send HTTP/3 CONNECT response (server-side).
     */
    int send_connect_response(uint64_t stream_id, int status_code) noexcept;

    /**
     * Enqueue outgoing datagram.
     */
    int enqueue_datagram(const uint8_t* data, size_t length) noexcept;

    // State
    State state_;
    bool is_server_;

    // Underlying QUIC connection
    std::unique_ptr<quic::QUICConnection> quic_conn_;

    // Session stream (HTTP/3 CONNECT stream)
    uint64_t session_stream_id_;

    // Callbacks
    StreamDataCallback stream_data_callback_;
    UnidirectionalDataCallback unidirectional_data_callback_;
    DatagramCallback datagram_callback_;
    StreamOpenedCallback stream_opened_callback_;
    StreamClosedCallback stream_closed_callback_;
    ConnectionClosedCallback connection_closed_callback_;

    // Stream tracking
    std::unordered_map<uint64_t, bool> active_streams_;  // stream_id -> is_bidirectional

    // Datagram queue (for outgoing datagrams)
    struct DatagramItem {
        uint8_t data[1500];  // Max MTU
        size_t length;
    };
    std::vector<DatagramItem> pending_datagrams_;
    size_t max_pending_datagrams_{256};

    // Statistics
    std::atomic<uint64_t> total_streams_opened_{0};
    std::atomic<uint64_t> total_datagrams_sent_{0};
    std::atomic<uint64_t> total_datagrams_received_{0};
    std::atomic<uint64_t> total_bytes_sent_{0};
    std::atomic<uint64_t> total_bytes_received_{0};
};

} // namespace http
} // namespace fasterapi
