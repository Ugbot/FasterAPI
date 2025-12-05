#pragma once

#include "quic_frames.h"
#include "../../core/ring_buffer.h"
#include <cstdint>
#include <atomic>
#include <memory>

namespace fasterapi {
namespace quic {

/**
 * QUIC stream state (RFC 9000 Section 3).
 */
enum class StreamState {
    IDLE,
    OPEN,
    SEND_CLOSED,
    RECV_CLOSED,
    CLOSED,
    RESET,
};

/**
 * QUIC stream type based on stream ID.
 * 
 * Stream ID encoding (RFC 9000 Section 2.1):
 *   Bit 0: 0 = client-initiated, 1 = server-initiated
 *   Bit 1: 0 = bidirectional, 1 = unidirectional
 */
enum class StreamType {
    CLIENT_BIDI = 0x00,  // 0b00
    SERVER_BIDI = 0x01,  // 0b01
    CLIENT_UNI = 0x02,   // 0b10
    SERVER_UNI = 0x03,   // 0b11
};

/**
 * QUIC stream.
 * 
 * Pre-allocated from ObjectPool to avoid malloc in hot path.
 */
class QUICStream {
public:
    QUICStream(uint64_t stream_id, bool is_server)
        : stream_id_(stream_id),
          state_(StreamState::IDLE),
          send_offset_(0),
          recv_offset_(0),
          max_send_offset_(1024 * 1024),  // 1MB initial send window
          max_recv_offset_(1024 * 1024),  // 1MB initial recv window
          fin_sent_(false),
          fin_received_(false),
          send_buffer_(64 * 1024),  // 64KB send buffer
          recv_buffer_(64 * 1024)   // 64KB receive buffer
    {
        // Determine stream type from ID
        uint8_t type_bits = stream_id & 0x03;
        type_ = static_cast<StreamType>(type_bits);

        is_bidirectional_ = (type_bits & 0x02) == 0;
        is_client_initiated_ = (type_bits & 0x01) == 0;
        is_server_initiated_ = !is_client_initiated_;
    }
    
    /**
     * Get stream ID.
     */
    uint64_t stream_id() const noexcept { return stream_id_; }
    
    /**
     * Get stream type.
     */
    StreamType type() const noexcept { return type_; }
    
    /**
     * Check if bidirectional.
     */
    bool is_bidirectional() const noexcept { return is_bidirectional_; }
    
    /**
     * Get current state.
     */
    StreamState state() const noexcept { return state_; }
    
    /**
     * Check if stream is closed.
     */
    bool is_closed() const noexcept { 
        return state_ == StreamState::CLOSED || state_ == StreamState::RESET;
    }
    
    /**
     * Write data to stream (application -> QUIC).
     * 
     * @param data Data to write
     * @param length Data length
     * @return Number of bytes written, or -1 if flow control blocked
     */
    ssize_t write(const uint8_t* data, size_t length) noexcept {
        if (state_ == StreamState::SEND_CLOSED || state_ == StreamState::CLOSED) {
            return -1;  // Cannot send on closed stream
        }
        
        // Check flow control
        if (send_offset_ + length > max_send_offset_) {
            // Flow control blocked
            length = max_send_offset_ - send_offset_;
            if (length == 0) return -1;
        }
        
        // Write to send buffer
        ssize_t written = send_buffer_.write(data, length);
        if (written > 0) {
            send_offset_ += written;
        }
        
        return written;
    }
    
    /**
     * Read data from stream (QUIC -> application).
     * 
     * @param data Output buffer
     * @param max_length Maximum bytes to read
     * @return Number of bytes read, or -1 on error
     */
    ssize_t read(uint8_t* data, size_t max_length) noexcept {
        if (state_ == StreamState::RECV_CLOSED || state_ == StreamState::CLOSED) {
            return 0;  // No more data
        }
        
        return recv_buffer_.read(data, max_length);
    }
    
    /**
     * Receive STREAM frame data.
     * 
     * @param frame STREAM frame
     * @return 0 on success, -1 on error
     */
    int receive_data(const StreamFrame& frame) noexcept {
        // Check flow control
        if (frame.offset + frame.length > max_recv_offset_) {
            return -1;  // Flow control violation
        }
        
        // Handle out-of-order delivery
        if (frame.offset == recv_offset_) {
            // In-order data
            recv_buffer_.write(frame.data, frame.length);
            recv_offset_ += frame.length;
        } else if (frame.offset > recv_offset_) {
            // Out-of-order: store for later
            // TODO: Implement reassembly buffer
            return -1;  // Simplified: reject out-of-order for now
        }
        // Ignore duplicate/old data
        
        // Handle FIN
        if (frame.fin) {
            fin_received_ = true;
            if (state_ == StreamState::OPEN) {
                state_ = StreamState::RECV_CLOSED;
            } else if (state_ == StreamState::SEND_CLOSED) {
                state_ = StreamState::CLOSED;
            }
        }
        
        return 0;
    }
    
    /**
     * Get next STREAM frame to send.
     * 
     * @param max_frame_size Maximum frame size
     * @param out_frame Output frame
     * @return true if frame available, false if nothing to send
     */
    bool get_next_frame(size_t max_frame_size, StreamFrame& out_frame) noexcept {
        if (send_buffer_.available() == 0 && !should_send_fin()) {
            return false;  // Nothing to send
        }
        
        // Calculate how much we can send
        size_t available = send_buffer_.available();
        size_t to_send = available < max_frame_size ? available : max_frame_size;
        
        if (to_send > 0) {
            out_frame.stream_id = stream_id_;
            out_frame.offset = send_offset_ - available;
            out_frame.length = to_send;
            out_frame.fin = (available == to_send) && should_send_fin();
            
            // Data will be read from send_buffer_ by caller
            return true;
        } else if (should_send_fin()) {
            // Send FIN-only frame
            out_frame.stream_id = stream_id_;
            out_frame.offset = send_offset_;
            out_frame.length = 0;
            out_frame.fin = true;
            fin_sent_ = true;
            
            if (state_ == StreamState::OPEN) {
                state_ = StreamState::SEND_CLOSED;
            } else if (state_ == StreamState::RECV_CLOSED) {
                state_ = StreamState::CLOSED;
            }
            
            return true;
        }
        
        return false;
    }
    
    /**
     * Update send flow control window.
     * 
     * @param max_offset New maximum offset
     */
    void update_send_window(uint64_t max_offset) noexcept {
        max_send_offset_ = max_offset;
    }
    
    /**
     * Update receive flow control window.
     * 
     * @param max_offset New maximum offset
     */
    void update_recv_window(uint64_t max_offset) noexcept {
        max_recv_offset_ = max_offset;
    }
    
    /**
     * Close stream for sending.
     */
    void close_send() noexcept {
        if (state_ == StreamState::IDLE || state_ == StreamState::OPEN) {
            state_ = StreamState::SEND_CLOSED;
        } else if (state_ == StreamState::RECV_CLOSED) {
            state_ = StreamState::CLOSED;
        }
    }
    
    /**
     * Reset stream.
     */
    void reset() noexcept {
        state_ = StreamState::RESET;
        send_buffer_.clear();
        recv_buffer_.clear();
    }
    
    /**
     * Get send buffer reference (for zero-copy reads).
     */
    core::RingBuffer& send_buffer() noexcept { return send_buffer_; }
    
    /**
     * Get receive buffer reference (for zero-copy writes).
     */
    core::RingBuffer& recv_buffer() noexcept { return recv_buffer_; }

private:
    uint64_t stream_id_;
    StreamType type_;
    StreamState state_;
    
    bool is_bidirectional_;
    bool is_client_initiated_;
    bool is_server_initiated_;
    
    // Flow control
    uint64_t send_offset_;      // Bytes we've sent
    uint64_t recv_offset_;      // Bytes we've received
    uint64_t max_send_offset_;  // Max bytes we can send (peer window)
    uint64_t max_recv_offset_;  // Max bytes peer can send (our window)
    
    bool fin_sent_;
    bool fin_received_;
    
    // Buffers (using pre-allocated ring buffers)
    core::RingBuffer send_buffer_;
    core::RingBuffer recv_buffer_;
    
    bool should_send_fin() const noexcept {
        return !fin_sent_ && (state_ == StreamState::SEND_CLOSED || 
                             state_ == StreamState::CLOSED);
    }
};

} // namespace quic
} // namespace fasterapi
