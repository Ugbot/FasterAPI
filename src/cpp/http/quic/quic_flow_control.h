#pragma once

#include <cstdint>
#include <atomic>

namespace fasterapi {
namespace quic {

/**
 * QUIC flow control (RFC 9000 Section 4).
 * 
 * Flow control operates at two levels:
 * 1. Per-stream: MAX_STREAM_DATA frames
 * 2. Connection-wide: MAX_DATA frames
 */
class FlowControl {
public:
    /**
     * Constructor.
     * 
     * @param initial_window Initial flow control window size
     */
    explicit FlowControl(uint64_t initial_window = 1024 * 1024)  // 1MB default
        : max_data_(initial_window),
          sent_data_(0),
          recv_data_(0),
          recv_max_data_(initial_window) {
    }
    
    /**
     * Check if we can send data (respects peer's window).
     * 
     * @param bytes Number of bytes to send
     * @return true if allowed, false if blocked
     */
    bool can_send(uint64_t bytes) const noexcept {
        return sent_data_ + bytes <= max_data_;
    }
    
    /**
     * Record sent data.
     * 
     * @param bytes Number of bytes sent
     */
    void add_sent_data(uint64_t bytes) noexcept {
        sent_data_ += bytes;
    }
    
    /**
     * Check if we should receive data (respects our window).
     * 
     * @param offset Offset in the stream
     * @param bytes Number of bytes
     * @return true if allowed, false if flow control violation
     */
    bool can_receive(uint64_t offset, uint64_t bytes) const noexcept {
        return offset + bytes <= recv_max_data_;
    }
    
    /**
     * Record received data.
     * 
     * @param bytes Number of bytes received
     */
    void add_recv_data(uint64_t bytes) noexcept {
        recv_data_ += bytes;
    }
    
    /**
     * Update peer's max data (from MAX_DATA frame).
     * 
     * @param new_max New maximum
     */
    void update_peer_max_data(uint64_t new_max) noexcept {
        if (new_max > max_data_) {
            max_data_ = new_max;
        }
    }
    
    /**
     * Update our max data (advertise to peer).
     * 
     * @param new_max New maximum
     */
    void update_recv_max_data(uint64_t new_max) noexcept {
        recv_max_data_ = new_max;
    }
    
    /**
     * Get current peer max data.
     */
    uint64_t peer_max_data() const noexcept { return max_data_; }
    
    /**
     * Get sent data.
     */
    uint64_t sent_data() const noexcept { return sent_data_; }
    
    /**
     * Get received data.
     */
    uint64_t recv_data() const noexcept { return recv_data_; }
    
    /**
     * Get our advertised max data.
     */
    uint64_t recv_max_data() const noexcept { return recv_max_data_; }
    
    /**
     * Check if we're blocked (should send BLOCKED frame).
     */
    bool is_blocked() const noexcept {
        return sent_data_ >= max_data_;
    }
    
    /**
     * Get available send window.
     */
    uint64_t available_window() const noexcept {
        if (sent_data_ >= max_data_) return 0;
        return max_data_ - sent_data_;
    }
    
    /**
     * Auto-increment receive window (when data consumed by application).
     * 
     * @param consumed_bytes Bytes consumed
     * @return New recv_max_data to advertise
     */
    uint64_t auto_increment_window(uint64_t consumed_bytes) noexcept {
        // Simple strategy: increment by consumed amount
        recv_max_data_ += consumed_bytes;
        return recv_max_data_;
    }

private:
    uint64_t max_data_;        // Maximum we can send (peer's window)
    uint64_t sent_data_;       // Total sent
    uint64_t recv_data_;       // Total received
    uint64_t recv_max_data_;   // Maximum peer can send (our window)
};

/**
 * Per-stream flow control.
 */
class StreamFlowControl {
public:
    explicit StreamFlowControl(uint64_t initial_window = 256 * 1024)  // 256KB default
        : max_stream_data_(initial_window),
          sent_offset_(0),
          recv_offset_(0),
          recv_max_offset_(initial_window) {
    }
    
    bool can_send(uint64_t bytes) const noexcept {
        return sent_offset_ + bytes <= max_stream_data_;
    }
    
    void add_sent_data(uint64_t bytes) noexcept {
        sent_offset_ += bytes;
    }
    
    bool can_receive(uint64_t offset, uint64_t bytes) const noexcept {
        return offset + bytes <= recv_max_offset_;
    }
    
    void add_recv_data(uint64_t bytes) noexcept {
        recv_offset_ += bytes;
    }
    
    void update_peer_max_stream_data(uint64_t new_max) noexcept {
        if (new_max > max_stream_data_) {
            max_stream_data_ = new_max;
        }
    }
    
    void update_recv_max_offset(uint64_t new_max) noexcept {
        recv_max_offset_ = new_max;
    }
    
    uint64_t peer_max_stream_data() const noexcept { return max_stream_data_; }
    uint64_t sent_offset() const noexcept { return sent_offset_; }
    uint64_t recv_offset() const noexcept { return recv_offset_; }
    uint64_t recv_max_offset() const noexcept { return recv_max_offset_; }
    
    bool is_blocked() const noexcept {
        return sent_offset_ >= max_stream_data_;
    }
    
    uint64_t available_window() const noexcept {
        if (sent_offset_ >= max_stream_data_) return 0;
        return max_stream_data_ - sent_offset_;
    }
    
    uint64_t auto_increment_window(uint64_t consumed_bytes) noexcept {
        recv_max_offset_ += consumed_bytes;
        return recv_max_offset_;
    }

private:
    uint64_t max_stream_data_;   // Maximum we can send on this stream
    uint64_t sent_offset_;       // Offset we've sent up to
    uint64_t recv_offset_;       // Offset we've received up to
    uint64_t recv_max_offset_;   // Maximum peer can send on this stream
};

} // namespace quic
} // namespace fasterapi
