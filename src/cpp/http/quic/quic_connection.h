#pragma once

#include "quic_packet.h"
#include "quic_stream.h"
#include "quic_flow_control.h"
#include "quic_congestion.h"
#include "quic_ack_tracker.h"
#include <unordered_map>
#include <memory>
#include <vector>
#include <atomic>
#include <chrono>

namespace fasterapi {
namespace quic {

/**
 * QUIC connection state (RFC 9000 Section 10).
 */
enum class ConnectionState {
    IDLE,
    HANDSHAKE,
    ESTABLISHED,
    CLOSING,
    DRAINING,
    CLOSED,
};

/**
 * QUIC connection.
 * 
 * Manages:
 * - Multiple streams
 * - Flow control (connection-level)
 * - Congestion control
 * - Loss detection
 * - Packet numbering
 */
class QUICConnection {
public:
    /**
     * Constructor.
     * 
     * @param is_server true if server-side connection
     * @param local_conn_id Local connection ID
     * @param peer_conn_id Peer connection ID
     */
    QUICConnection(bool is_server, const ConnectionID& local_conn_id,
                   const ConnectionID& peer_conn_id)
        : is_server_(is_server),
          state_(ConnectionState::IDLE),
          local_conn_id_(local_conn_id),
          peer_conn_id_(peer_conn_id),
          next_stream_id_(is_server ? 1 : 0),  // Server uses odd, client even
          flow_control_(16 * 1024 * 1024),  // 16MB connection window
          congestion_control_(),
          ack_tracker_() {
    }
    
    /**
     * Get connection state.
     */
    ConnectionState state() const noexcept { return state_; }
    
    /**
     * Check if connection is established.
     */
    bool is_established() const noexcept {
        return state_ == ConnectionState::ESTABLISHED;
    }
    
    /**
     * Check if connection is closed.
     */
    bool is_closed() const noexcept {
        return state_ == ConnectionState::CLOSED;
    }
    
    /**
     * Get local connection ID.
     */
    const ConnectionID& local_conn_id() const noexcept { return local_conn_id_; }
    
    /**
     * Get peer connection ID.
     */
    const ConnectionID& peer_conn_id() const noexcept { return peer_conn_id_; }
    
    /**
     * Create new stream.
     * 
     * @param is_bidirectional true for bidirectional stream
     * @return Stream ID, or 0 on error
     */
    uint64_t create_stream(bool is_bidirectional = true) noexcept {
        if (!is_established()) {
            return 0;  // Cannot create streams until established
        }
        
        // Generate stream ID based on type
        // Bit 0: 0=client, 1=server
        // Bit 1: 0=bidi, 1=uni
        uint64_t stream_id = next_stream_id_;
        next_stream_id_ += 4;  // Increment by 4 to maintain type bits
        
        // Create stream
        auto stream = std::make_unique<QUICStream>(stream_id, is_server_);
        streams_[stream_id] = std::move(stream);
        
        return stream_id;
    }
    
    /**
     * Get stream by ID.
     * 
     * @param stream_id Stream ID
     * @return Stream pointer or nullptr
     */
    QUICStream* get_stream(uint64_t stream_id) noexcept {
        auto it = streams_.find(stream_id);
        return (it != streams_.end()) ? it->second.get() : nullptr;
    }
    
    /**
     * Write data to stream.
     * 
     * @param stream_id Stream ID
     * @param data Data to write
     * @param length Data length
     * @return Number of bytes written, or -1 on error
     */
    ssize_t write_stream(uint64_t stream_id, const uint8_t* data, size_t length) noexcept {
        QUICStream* stream = get_stream(stream_id);
        if (!stream) return -1;
        
        // Check connection-level flow control
        if (!flow_control_.can_send(length)) {
            return -1;  // Flow control blocked
        }
        
        // Write to stream
        ssize_t written = stream->write(data, length);
        if (written > 0) {
            flow_control_.add_sent_data(written);
        }
        
        return written;
    }
    
    /**
     * Read data from stream.
     * 
     * @param stream_id Stream ID
     * @param data Output buffer
     * @param max_length Maximum bytes to read
     * @return Number of bytes read, or -1 on error
     */
    ssize_t read_stream(uint64_t stream_id, uint8_t* data, size_t max_length) noexcept {
        QUICStream* stream = get_stream(stream_id);
        if (!stream) return -1;
        
        return stream->read(data, max_length);
    }
    
    /**
     * Close stream.
     * 
     * @param stream_id Stream ID
     */
    void close_stream(uint64_t stream_id) noexcept {
        auto it = streams_.find(stream_id);
        if (it != streams_.end()) {
            it->second->close_send();
        }
    }
    
    /**
     * Process received packet.
     *
     * @param data Packet data
     * @param length Packet length
     * @param now Current time (microseconds)
     * @return 0 on success, -1 on error
     */
    int process_packet(const uint8_t* data, size_t length, uint64_t now) noexcept;
    
    /**
     * Generate packets to send.
     *
     * @param output Output buffer
     * @param capacity Output capacity
     * @param now Current time (microseconds)
     * @return Number of bytes written
     */
    size_t generate_packets(uint8_t* output, size_t capacity, uint64_t now) noexcept;
    
    /**
     * Get flow control reference.
     */
    FlowControl& flow_control() noexcept { return flow_control_; }
    
    /**
     * Get congestion control reference.
     */
    NewRenoCongestionControl& congestion_control() noexcept { 
        return congestion_control_;
    }
    
    /**
     * Get ACK tracker reference.
     */
    AckTracker& ack_tracker() noexcept { return ack_tracker_; }
    
    /**
     * Get stream count.
     */
    size_t stream_count() const noexcept { return streams_.size(); }

    /**
     * Initialize connection (call after construction).
     */
    void initialize() noexcept;

    /**
     * Close connection.
     *
     * @param error_code Error code
     * @param reason Reason phrase (optional)
     */
    void close(uint64_t error_code = 0, const char* reason = nullptr) noexcept;

    /**
     * Check for idle timeout.
     *
     * @param now Current time (microseconds)
     * @return true if timed out
     */
    bool check_idle_timeout(uint64_t now) noexcept;

    /**
     * Complete connection closure.
     */
    void complete_close() noexcept;

private:
    static constexpr size_t kMaxPacketSize = 1200;  // Conservative MTU

    /**
     * Frame processing helper.
     */
    int process_frame(uint64_t frame_type, const uint8_t* data,
                     size_t length, uint64_t now) noexcept;

    /**
     * Handle received STREAM frame.
     */
    void handle_stream_frame(const StreamFrame& frame) noexcept;

    /**
     * Handle CONNECTION_CLOSE frame.
     */
    void handle_connection_close(const ConnectionCloseFrame& frame) noexcept;

    /**
     * Handle RESET_STREAM frame.
     */
    int handle_reset_stream(const uint8_t* data, size_t length) noexcept;

    /**
     * Handle STOP_SENDING frame.
     */
    int handle_stop_sending(const uint8_t* data, size_t length) noexcept;

    /**
     * Generate STREAM packet.
     */
    size_t generate_stream_packet(QUICStream* stream, uint8_t* output,
                                  size_t capacity, uint64_t now) noexcept;

    /**
     * Generate ACK packet.
     */
    size_t generate_ack_packet(uint8_t* output, size_t capacity,
                               uint64_t now) noexcept;

    /**
     * Generate CONNECTION_CLOSE packet.
     */
    size_t generate_close_packet(uint8_t* output, size_t capacity,
                                 uint64_t now) noexcept;

    /**
     * Clean up closed streams.
     */
    void cleanup_closed_streams() noexcept;

    /**
     * Check if we should send an ACK.
     */
    bool should_send_ack() const noexcept;

    /**
     * Get current time in microseconds.
     */
    uint64_t get_current_time_us() const noexcept;

    bool is_server_;
    ConnectionState state_;
    ConnectionID local_conn_id_;
    ConnectionID peer_conn_id_;

    // Stream management
    std::unordered_map<uint64_t, std::unique_ptr<QUICStream>> streams_;
    uint64_t next_stream_id_;
    size_t max_streams_{1000};  // Max concurrent streams

    // Flow and congestion control
    FlowControl flow_control_;
    NewRenoCongestionControl congestion_control_;
    AckTracker ack_tracker_;

    // Connection close
    uint64_t close_error_code_{0};
    char close_reason_[256];
    size_t close_reason_length_{0};
    bool close_frame_sent_{false};

    // Timestamps
    uint64_t last_activity_time_{0};
    uint64_t idle_timeout_us_{30000000};  // 30 seconds
    uint64_t draining_start_time_{0};
    uint64_t last_ack_sent_time_{0};
    uint64_t largest_received_packet_{0};
};

} // namespace quic
} // namespace fasterapi
