// QUIC connection implementation (RFC 9000)
// Production-quality connection orchestration

#include "quic_connection.h"
#include <cstring>
#include <algorithm>

namespace fasterapi {
namespace quic {

// ============================================================================
// Connection Lifecycle Management
// ============================================================================

/**
 * Initialize connection (called after construction).
 */
void QUICConnection::initialize() noexcept {
    state_ = ConnectionState::HANDSHAKE;
    last_activity_time_ = get_current_time_us();
    idle_timeout_us_ = 30000000;  // 30 seconds default
}

/**
 * Close connection gracefully.
 *
 * @param error_code Error code to send
 * @param reason Reason phrase (optional)
 */
void QUICConnection::close(uint64_t error_code, const char* reason) noexcept {
    if (state_ == ConnectionState::CLOSED) {
        return;  // Already closed
    }

    // Transition to CLOSING state
    state_ = ConnectionState::CLOSING;
    close_error_code_ = error_code;

    if (reason) {
        size_t reason_len = std::strlen(reason);
        if (reason_len > 255) reason_len = 255;
        std::memcpy(close_reason_, reason, reason_len);
        close_reason_length_ = reason_len;
    } else {
        close_reason_length_ = 0;
    }

    // Generate CONNECTION_CLOSE frame (will be sent by generate_packets)
    close_frame_sent_ = false;
}

/**
 * Complete connection closure.
 */
void QUICConnection::complete_close() noexcept {
    // Clean up all streams
    streams_.clear();

    // Transition to CLOSED
    state_ = ConnectionState::CLOSED;
}

/**
 * Check for idle timeout.
 *
 * @param now Current time (microseconds)
 * @return true if connection has timed out
 */
bool QUICConnection::check_idle_timeout(uint64_t now) noexcept {
    if (state_ == ConnectionState::CLOSED) {
        return true;
    }

    uint64_t elapsed = now - last_activity_time_;
    if (elapsed > idle_timeout_us_) {
        close(0x01, "idle_timeout");  // Error code 0x01 = NO_ERROR
        return true;
    }

    return false;
}

// ============================================================================
// Packet Processing
// ============================================================================

/**
 * Process received packet.
 *
 * @param data Packet data
 * @param length Packet length
 * @param now Current time (microseconds)
 * @return 0 on success, -1 on error
 */
int QUICConnection::process_packet(const uint8_t* data, size_t length, uint64_t now) noexcept {
    if (!data || length == 0) {
        return -1;
    }

    if (state_ == ConnectionState::CLOSED || state_ == ConnectionState::DRAINING) {
        return -1;  // Don't process packets in terminal states
    }

    // Update activity timestamp
    last_activity_time_ = now;

    // Parse packet header
    Packet packet;
    size_t header_consumed = 0;
    int parse_result = 0;

    // Determine packet type
    if ((data[0] & 0x80) != 0) {
        // Long header
        packet.is_long_header = true;
        parse_result = packet.long_hdr.parse(data, length, header_consumed);
    } else {
        // Short header
        packet.is_long_header = false;
        parse_result = packet.short_hdr.parse(data, length, peer_conn_id_.length, header_consumed);
    }

    if (parse_result != 0) {
        return -1;  // Parse error
    }

    // Validate connection IDs
    if (packet.is_long_header) {
        if (packet.long_hdr.dest_conn_id != local_conn_id_) {
            return -1;  // Wrong connection
        }
    } else {
        if (packet.short_hdr.dest_conn_id != local_conn_id_) {
            return -1;  // Wrong connection
        }
    }

    // Extract payload (TODO: decrypt when crypto is implemented)
    size_t payload_offset = header_consumed;
    size_t payload_length = length - payload_offset;

    // Process frames in payload
    size_t pos = 0;
    while (pos < payload_length) {
        uint64_t frame_type;
        int consumed = VarInt::decode(data + payload_offset + pos,
                                     payload_length - pos, frame_type);
        if (consumed < 0) break;

        size_t frame_start = pos;
        pos += consumed;

        // Dispatch frame
        int frame_result = process_frame(frame_type,
                                         data + payload_offset + frame_start,
                                         payload_length - frame_start,
                                         now);

        if (frame_result < 0) {
            // Frame processing error
            break;
        }

        pos = frame_start + frame_result;
    }

    // Update connection state based on packets received
    if (state_ == ConnectionState::HANDSHAKE) {
        // Simplified: assume connection established after first packet
        // TODO: Proper TLS handshake when crypto is implemented
        state_ = ConnectionState::ESTABLISHED;
    }

    return 0;
}

/**
 * Process individual frame.
 *
 * @param frame_type Frame type
 * @param data Frame data (including type byte)
 * @param length Available data length
 * @param now Current time (microseconds)
 * @return Bytes consumed, or -1 on error
 */
int QUICConnection::process_frame(uint64_t frame_type, const uint8_t* data,
                                  size_t length, uint64_t now) noexcept {
    size_t consumed = 0;

    // STREAM frames (0x08-0x0F)
    if (frame_type >= 0x08 && frame_type <= 0x0F) {
        StreamFrame stream_frame;
        if (stream_frame.parse(data, length, consumed) == 0) {
            handle_stream_frame(stream_frame);
            return static_cast<int>(consumed);
        }
        return -1;
    }

    // ACK frames (0x02-0x03)
    if (frame_type == 0x02 || frame_type == 0x03) {
        AckFrame ack_frame;
        if (ack_frame.parse(data, length, consumed) == 0) {
            ack_tracker_.on_ack_received(ack_frame, now, congestion_control_);
            return static_cast<int>(consumed);
        }
        return -1;
    }

    // PADDING frame (0x00)
    if (frame_type == 0x00) {
        // Consume all consecutive padding bytes
        consumed = 1;
        while (consumed < length && data[consumed] == 0x00) {
            consumed++;
        }
        return static_cast<int>(consumed);
    }

    // PING frame (0x01)
    if (frame_type == 0x01) {
        // PING is ack-eliciting but has no payload
        return 1;
    }

    // CONNECTION_CLOSE frame (0x1C-0x1D)
    if (frame_type == 0x1C || frame_type == 0x1D) {
        ConnectionCloseFrame close_frame;
        if (close_frame.parse(data, length, frame_type == 0x1D, consumed) == 0) {
            handle_connection_close(close_frame);
            return static_cast<int>(consumed);
        }
        return -1;
    }

    // RESET_STREAM frame (0x04)
    if (frame_type == 0x04) {
        return handle_reset_stream(data, length);
    }

    // STOP_SENDING frame (0x05)
    if (frame_type == 0x05) {
        return handle_stop_sending(data, length);
    }

    // MAX_DATA frame (0x10)
    if (frame_type == 0x10) {
        uint64_t max_data;
        int result = VarInt::decode(data + 1, length - 1, max_data);
        if (result > 0) {
            flow_control_.update_peer_max_data(max_data);
            return 1 + result;
        }
        return -1;
    }

    // MAX_STREAM_DATA frame (0x11)
    if (frame_type == 0x11) {
        uint64_t stream_id, max_stream_data;
        size_t pos = 1;
        int result = VarInt::decode(data + pos, length - pos, stream_id);
        if (result < 0) return -1;
        pos += result;

        result = VarInt::decode(data + pos, length - pos, max_stream_data);
        if (result < 0) return -1;
        pos += result;

        // Update stream flow control
        QUICStream* stream = get_stream(stream_id);
        if (stream) {
            stream->update_send_window(max_stream_data);
        }

        return static_cast<int>(pos);
    }

    // Unknown/unimplemented frame - skip it
    // For robustness, we should handle unknown frames gracefully
    return 1;
}

/**
 * Handle received STREAM frame.
 */
void QUICConnection::handle_stream_frame(const StreamFrame& frame) noexcept {
    // Get or create stream
    QUICStream* stream = get_stream(frame.stream_id);

    if (!stream) {
        // Peer-initiated stream - create it
        if (streams_.size() >= max_streams_) {
            // Too many streams - ignore (should send STREAMS_BLOCKED)
            return;
        }

        auto new_stream = std::make_unique<QUICStream>(frame.stream_id, is_server_);
        stream = new_stream.get();
        streams_[frame.stream_id] = std::move(new_stream);
    }

    // Check connection-level flow control
    if (!flow_control_.can_receive(frame.offset, frame.length)) {
        // Flow control violation - should close connection
        close(0x03, "flow_control_error");
        return;
    }

    // Deliver data to stream
    if (stream->receive_data(frame) == 0) {
        // Update connection flow control
        flow_control_.add_recv_data(frame.length);
    }
}

/**
 * Handle CONNECTION_CLOSE frame.
 */
void QUICConnection::handle_connection_close(const ConnectionCloseFrame& frame) noexcept {
    // Peer is closing connection
    if (state_ == ConnectionState::ESTABLISHED ||
        state_ == ConnectionState::HANDSHAKE) {
        // Enter DRAINING state (don't send our own CLOSE)
        state_ = ConnectionState::DRAINING;
        draining_start_time_ = get_current_time_us();
    }
}

/**
 * Handle RESET_STREAM frame.
 */
int QUICConnection::handle_reset_stream(const uint8_t* data, size_t length) noexcept {
    uint64_t stream_id, error_code, final_size;
    size_t pos = 1;

    int result = VarInt::decode(data + pos, length - pos, stream_id);
    if (result < 0) return -1;
    pos += result;

    result = VarInt::decode(data + pos, length - pos, error_code);
    if (result < 0) return -1;
    pos += result;

    result = VarInt::decode(data + pos, length - pos, final_size);
    if (result < 0) return -1;
    pos += result;

    // Reset the stream
    QUICStream* stream = get_stream(stream_id);
    if (stream) {
        stream->reset();
    }

    return static_cast<int>(pos);
}

/**
 * Handle STOP_SENDING frame.
 */
int QUICConnection::handle_stop_sending(const uint8_t* data, size_t length) noexcept {
    uint64_t stream_id, error_code;
    size_t pos = 1;

    int result = VarInt::decode(data + pos, length - pos, stream_id);
    if (result < 0) return -1;
    pos += result;

    result = VarInt::decode(data + pos, length - pos, error_code);
    if (result < 0) return -1;
    pos += result;

    // Stop sending on the stream
    QUICStream* stream = get_stream(stream_id);
    if (stream) {
        stream->close_send();
    }

    return static_cast<int>(pos);
}

// ============================================================================
// Packet Generation
// ============================================================================

/**
 * Generate packets to send.
 *
 * @param output Output buffer
 * @param capacity Output capacity
 * @param now Current time (microseconds)
 * @return Number of bytes written
 */
size_t QUICConnection::generate_packets(uint8_t* output, size_t capacity, uint64_t now) noexcept {
    if (!output || capacity < kMaxPacketSize) {
        return 0;
    }

    if (state_ == ConnectionState::CLOSED || state_ == ConnectionState::DRAINING) {
        return 0;  // Don't generate packets in terminal states
    }

    size_t total_written = 0;

    // If in CLOSING state, send CONNECTION_CLOSE frame
    if (state_ == ConnectionState::CLOSING && !close_frame_sent_) {
        size_t written = generate_close_packet(output, capacity, now);
        if (written > 0) {
            total_written += written;
            close_frame_sent_ = true;

            // After sending close, enter DRAINING
            state_ = ConnectionState::DRAINING;
            draining_start_time_ = now;
        }
        return total_written;
    }

    // Check congestion control
    if (!congestion_control_.can_send(kMaxPacketSize)) {
        return 0;  // Congestion window full
    }

    // Generate ACK frame if needed
    if (should_send_ack()) {
        size_t written = generate_ack_packet(output + total_written,
                                             capacity - total_written, now);
        if (written > 0) {
            total_written += written;
            last_ack_sent_time_ = now;
        }
    }

    // Generate STREAM frames for pending data
    for (auto& [stream_id, stream] : streams_) {
        if (total_written + kMaxPacketSize > capacity) {
            break;  // Not enough space
        }

        if (stream->is_closed()) {
            continue;
        }

        // Check if stream has data to send
        if (stream->send_buffer().available() == 0) {
            continue;
        }

        // Generate packet for this stream
        size_t written = generate_stream_packet(stream.get(),
                                                output + total_written,
                                                capacity - total_written,
                                                now);
        if (written > 0) {
            total_written += written;
        }
    }

    // Clean up closed streams
    cleanup_closed_streams();

    return total_written;
}

/**
 * Generate STREAM packet.
 */
size_t QUICConnection::generate_stream_packet(QUICStream* stream, uint8_t* output,
                                              size_t capacity, uint64_t now) noexcept {
    if (!stream || capacity < kMaxPacketSize) {
        return 0;
    }

    // Calculate max frame size (leave room for headers)
    size_t max_frame_size = kMaxPacketSize - 50;  // Conservative estimate

    // Get next frame from stream
    StreamFrame frame;
    if (!stream->get_next_frame(max_frame_size, frame)) {
        return 0;  // No data to send
    }

    // Build short header packet
    ShortHeader hdr;
    hdr.dest_conn_id = peer_conn_id_;
    hdr.packet_number = ack_tracker_.next_packet_number();
    hdr.packet_number_length = 4;
    hdr.spin_bit = false;
    hdr.key_phase = false;

    // Serialize header
    size_t hdr_len = hdr.serialize(output);

    // Read data from stream buffer
    uint8_t frame_buffer[kMaxPacketSize];
    frame.data = frame_buffer;
    ssize_t data_read = stream->send_buffer().read(frame_buffer, frame.length);
    if (data_read <= 0) {
        return 0;
    }
    frame.length = static_cast<size_t>(data_read);

    // Serialize frame
    size_t frame_len = frame.serialize(output + hdr_len);

    // Record sent packet
    size_t packet_len = hdr_len + frame_len;
    ack_tracker_.on_packet_sent(hdr.packet_number, packet_len, true, now);
    congestion_control_.on_packet_sent(packet_len);

    return packet_len;
}

/**
 * Generate ACK packet.
 */
size_t QUICConnection::generate_ack_packet(uint8_t* output, size_t capacity,
                                           uint64_t now) noexcept {
    if (capacity < kMaxPacketSize) {
        return 0;
    }

    // Build short header
    ShortHeader hdr;
    hdr.dest_conn_id = peer_conn_id_;
    hdr.packet_number = ack_tracker_.next_packet_number();
    hdr.packet_number_length = 4;
    hdr.spin_bit = false;
    hdr.key_phase = false;

    size_t hdr_len = hdr.serialize(output);

    // Build ACK frame
    AckFrame ack;
    ack.largest_acked = largest_received_packet_;
    ack.ack_delay = 0;  // Simplified
    ack.first_ack_range = 0;  // Simplified: single packet ack
    ack.range_count = 0;

    size_t frame_len = ack.serialize(output + hdr_len);

    // Record sent packet (ACKs are not ack-eliciting)
    size_t packet_len = hdr_len + frame_len;
    ack_tracker_.on_packet_sent(hdr.packet_number, packet_len, false, now);

    return packet_len;
}

/**
 * Generate CONNECTION_CLOSE packet.
 */
size_t QUICConnection::generate_close_packet(uint8_t* output, size_t capacity,
                                             uint64_t now) noexcept {
    if (capacity < kMaxPacketSize) {
        return 0;
    }

    // Build short header
    ShortHeader hdr;
    hdr.dest_conn_id = peer_conn_id_;
    hdr.packet_number = ack_tracker_.next_packet_number();
    hdr.packet_number_length = 4;
    hdr.spin_bit = false;
    hdr.key_phase = false;

    size_t pos = hdr.serialize(output);

    // CONNECTION_CLOSE frame
    output[pos++] = 0x1C;  // CONNECTION_CLOSE (transport)
    pos += VarInt::encode(close_error_code_, output + pos);
    pos += VarInt::encode(0, output + pos);  // Frame type (none)
    pos += VarInt::encode(close_reason_length_, output + pos);

    if (close_reason_length_ > 0) {
        std::memcpy(output + pos, close_reason_, close_reason_length_);
        pos += close_reason_length_;
    }

    // Record sent packet
    ack_tracker_.on_packet_sent(hdr.packet_number, pos, true, now);

    return pos;
}

// ============================================================================
// Stream Management
// ============================================================================

/**
 * Clean up closed streams.
 */
void QUICConnection::cleanup_closed_streams() noexcept {
    // Remove streams that are fully closed and drained
    auto it = streams_.begin();
    while (it != streams_.end()) {
        QUICStream* stream = it->second.get();

        if (stream->is_closed() &&
            stream->send_buffer().available() == 0 &&
            stream->recv_buffer().available() == 0) {
            it = streams_.erase(it);
        } else {
            ++it;
        }
    }
}

/**
 * Check if we should send an ACK.
 */
bool QUICConnection::should_send_ack() const noexcept {
    // Simplified: send ACK if we've received packets and haven't acked recently
    uint64_t now = get_current_time_us();
    return (largest_received_packet_ > 0) &&
           (now - last_ack_sent_time_ > 25000);  // 25ms max ack delay
}

/**
 * Get current time in microseconds.
 */
uint64_t QUICConnection::get_current_time_us() const noexcept {
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

} // namespace quic
} // namespace fasterapi
