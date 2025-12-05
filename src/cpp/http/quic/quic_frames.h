#pragma once

#include "quic_varint.h"
#include <cstdint>
#include <cstring>

namespace fasterapi {
namespace quic {

/**
 * QUIC frame types (RFC 9000 Section 19).
 */
enum class FrameType : uint64_t {
    PADDING = 0x00,
    PING = 0x01,
    ACK = 0x02,
    ACK_ECN = 0x03,
    RESET_STREAM = 0x04,
    STOP_SENDING = 0x05,
    CRYPTO = 0x06,
    NEW_TOKEN = 0x07,
    STREAM = 0x08,          // Base value, flags in low bits
    MAX_DATA = 0x10,
    MAX_STREAM_DATA = 0x11,
    MAX_STREAMS_BIDI = 0x12,
    MAX_STREAMS_UNI = 0x13,
    DATA_BLOCKED = 0x14,
    STREAM_DATA_BLOCKED = 0x15,
    STREAMS_BLOCKED_BIDI = 0x16,
    STREAMS_BLOCKED_UNI = 0x17,
    NEW_CONNECTION_ID = 0x18,
    RETIRE_CONNECTION_ID = 0x19,
    PATH_CHALLENGE = 0x1A,
    PATH_RESPONSE = 0x1B,
    CONNECTION_CLOSE = 0x1C,
    CONNECTION_CLOSE_APP = 0x1D,
    HANDSHAKE_DONE = 0x1E,
    DATAGRAM = 0x30,           // RFC 9221 (base value)
    DATAGRAM_WITH_LEN = 0x31,  // RFC 9221 (with length field)
};

/**
 * STREAM frame.
 * 
 * Format:
 *   0b00001XXX
 *   XXX bits: OFF|LEN|FIN
 */
struct StreamFrame {
    uint64_t stream_id;
    uint64_t offset;        // Only if OFF bit set
    uint64_t length;        // Only if LEN bit set
    bool fin;               // FIN bit
    const uint8_t* data;
    
    static constexpr uint8_t FLAG_FIN = 0x01;
    static constexpr uint8_t FLAG_LEN = 0x02;
    static constexpr uint8_t FLAG_OFF = 0x04;
    
    /**
     * Parse STREAM frame.
     * 
     * @param data Input buffer
     * @param len Buffer length
     * @param out_consumed Bytes consumed
     * @return 0 on success, -1 if need more data, 1 on error
     */
    int parse(const uint8_t* data_buf, size_t len, size_t& out_consumed) noexcept {
        if (len == 0) return -1;
        
        uint8_t type_byte = data_buf[0];
        uint8_t flags = type_byte & 0x07;
        
        fin = (flags & FLAG_FIN) != 0;
        bool has_length = (flags & FLAG_LEN) != 0;
        bool has_offset = (flags & FLAG_OFF) != 0;
        
        size_t pos = 1;
        
        // Stream ID
        int consumed = VarInt::decode(data_buf + pos, len - pos, stream_id);
        if (consumed < 0) return -1;
        pos += consumed;
        
        // Offset (if present)
        if (has_offset) {
            consumed = VarInt::decode(data_buf + pos, len - pos, offset);
            if (consumed < 0) return -1;
            pos += consumed;
        } else {
            offset = 0;
        }
        
        // Length (if present)
        if (has_length) {
            consumed = VarInt::decode(data_buf + pos, len - pos, length);
            if (consumed < 0) return -1;
            pos += consumed;
        } else {
            // Length extends to end of packet
            length = len - pos;
        }
        
        // Data
        if (len < pos + length) return -1;
        data = data_buf + pos;
        pos += length;
        
        out_consumed = pos;
        return 0;
    }
    
    /**
     * Serialize STREAM frame.
     * 
     * @param out Output buffer
     * @return Number of bytes written
     */
    size_t serialize(uint8_t* out) const noexcept {
        size_t pos = 0;
        
        // Type byte with flags
        uint8_t type_byte = 0x08;
        if (fin) type_byte |= FLAG_FIN;
        if (length > 0) type_byte |= FLAG_LEN;
        if (offset > 0) type_byte |= FLAG_OFF;
        out[pos++] = type_byte;
        
        // Stream ID
        pos += VarInt::encode(stream_id, out + pos);
        
        // Offset (if present)
        if (offset > 0) {
            pos += VarInt::encode(offset, out + pos);
        }
        
        // Length (if present)
        if (length > 0) {
            pos += VarInt::encode(length, out + pos);
        }
        
        // Data
        std::memcpy(out + pos, data, length);
        pos += length;
        
        return pos;
    }
};

/**
 * ACK frame.
 * 
 * Format (RFC 9000 Section 19.3):
 *   Type (i) = 0x02 or 0x03
 *   Largest Acknowledged (i)
 *   ACK Delay (i)
 *   ACK Range Count (i)
 *   First ACK Range (i)
 *   ACK Range (..) ...
 */
struct AckRange {
    uint64_t gap;      // Gap from previous range
    uint64_t length;   // Length of this range
};

struct AckFrame {
    uint64_t largest_acked;
    uint64_t ack_delay;
    uint64_t first_ack_range;
    AckRange ranges[64];  // Max 64 ranges
    size_t range_count;
    
    /**
     * Parse ACK frame.
     * 
     * @param data Input buffer
     * @param len Buffer length
     * @param out_consumed Bytes consumed
     * @return 0 on success, -1 if need more data, 1 on error
     */
    int parse(const uint8_t* data, size_t len, size_t& out_consumed) noexcept {
        if (len == 0) return -1;
        
        size_t pos = 1;  // Skip type byte
        
        // Largest acknowledged
        int consumed = VarInt::decode(data + pos, len - pos, largest_acked);
        if (consumed < 0) return -1;
        pos += consumed;
        
        // ACK delay
        consumed = VarInt::decode(data + pos, len - pos, ack_delay);
        if (consumed < 0) return -1;
        pos += consumed;
        
        // ACK range count
        uint64_t range_cnt;
        consumed = VarInt::decode(data + pos, len - pos, range_cnt);
        if (consumed < 0) return -1;
        pos += consumed;
        
        if (range_cnt > 64) return 1;  // Too many ranges
        range_count = static_cast<size_t>(range_cnt);
        
        // First ACK range
        consumed = VarInt::decode(data + pos, len - pos, first_ack_range);
        if (consumed < 0) return -1;
        pos += consumed;
        
        // Additional ACK ranges
        for (size_t i = 0; i < range_count; i++) {
            consumed = VarInt::decode(data + pos, len - pos, ranges[i].gap);
            if (consumed < 0) return -1;
            pos += consumed;
            
            consumed = VarInt::decode(data + pos, len - pos, ranges[i].length);
            if (consumed < 0) return -1;
            pos += consumed;
        }
        
        out_consumed = pos;
        return 0;
    }
    
    /**
     * Serialize ACK frame.
     * 
     * @param out Output buffer
     * @return Number of bytes written
     */
    size_t serialize(uint8_t* out) const noexcept {
        size_t pos = 0;
        
        // Type
        out[pos++] = 0x02;
        
        // Largest acknowledged
        pos += VarInt::encode(largest_acked, out + pos);
        
        // ACK delay
        pos += VarInt::encode(ack_delay, out + pos);
        
        // ACK range count
        pos += VarInt::encode(range_count, out + pos);
        
        // First ACK range
        pos += VarInt::encode(first_ack_range, out + pos);
        
        // Additional ranges
        for (size_t i = 0; i < range_count; i++) {
            pos += VarInt::encode(ranges[i].gap, out + pos);
            pos += VarInt::encode(ranges[i].length, out + pos);
        }
        
        return pos;
    }
};

/**
 * CRYPTO frame (for TLS handshake data).
 */
struct CryptoFrame {
    uint64_t offset;
    uint64_t length;
    const uint8_t* data;
    
    int parse(const uint8_t* data_buf, size_t len, size_t& out_consumed) noexcept {
        if (len == 0) return -1;
        
        size_t pos = 1;  // Skip type byte
        
        // Offset
        int consumed = VarInt::decode(data_buf + pos, len - pos, offset);
        if (consumed < 0) return -1;
        pos += consumed;
        
        // Length
        consumed = VarInt::decode(data_buf + pos, len - pos, length);
        if (consumed < 0) return -1;
        pos += consumed;
        
        // Data
        if (len < pos + length) return -1;
        data = data_buf + pos;
        pos += length;
        
        out_consumed = pos;
        return 0;
    }
    
    size_t serialize(uint8_t* out) const noexcept {
        size_t pos = 0;
        
        out[pos++] = 0x06;  // CRYPTO type
        pos += VarInt::encode(offset, out + pos);
        pos += VarInt::encode(length, out + pos);
        std::memcpy(out + pos, data, length);
        pos += length;
        
        return pos;
    }
};

/**
 * CONNECTION_CLOSE frame.
 */
struct ConnectionCloseFrame {
    uint64_t error_code;
    uint64_t frame_type;     // Only for transport-level errors
    uint64_t reason_length;
    const char* reason_phrase;

    int parse(const uint8_t* data, size_t len, bool is_app_error, size_t& out_consumed) noexcept {
        if (len == 0) return -1;

        size_t pos = 1;  // Skip type byte

        // Error code
        int consumed = VarInt::decode(data + pos, len - pos, error_code);
        if (consumed < 0) return -1;
        pos += consumed;

        // Frame type (only for transport errors)
        if (!is_app_error) {
            consumed = VarInt::decode(data + pos, len - pos, frame_type);
            if (consumed < 0) return -1;
            pos += consumed;
        }

        // Reason phrase length
        consumed = VarInt::decode(data + pos, len - pos, reason_length);
        if (consumed < 0) return -1;
        pos += consumed;

        // Reason phrase
        if (len < pos + reason_length) return -1;
        reason_phrase = reinterpret_cast<const char*>(data + pos);
        pos += reason_length;

        out_consumed = pos;
        return 0;
    }
};

/**
 * DATAGRAM frame (RFC 9221).
 *
 * Used for unreliable, unordered delivery of application data.
 * Perfect for WebTransport datagrams.
 *
 * Format:
 *   Type (i) = 0x30 or 0x31
 *   [Length (i)]  // Only if type = 0x31
 *   Datagram Data (..)
 */
struct DatagramFrame {
    uint64_t length;          // Only present if type = 0x31
    const uint8_t* data;

    /**
     * Parse DATAGRAM frame.
     *
     * @param data_buf Input buffer
     * @param len Buffer length
     * @param out_consumed Bytes consumed
     * @return 0 on success, -1 if need more data, 1 on error
     */
    int parse(const uint8_t* data_buf, size_t len, size_t& out_consumed) noexcept {
        if (len == 0) return -1;

        uint8_t type_byte = data_buf[0];
        bool has_length = (type_byte == 0x31);

        size_t pos = 1;

        // Length (if present)
        if (has_length) {
            int consumed = VarInt::decode(data_buf + pos, len - pos, length);
            if (consumed < 0) return -1;
            pos += consumed;

            // Verify length
            if (len < pos + length) return -1;
        } else {
            // Length extends to end of packet
            length = len - pos;
        }

        // Data
        data = data_buf + pos;
        pos += length;

        out_consumed = pos;
        return 0;
    }

    /**
     * Serialize DATAGRAM frame.
     *
     * @param out Output buffer
     * @param with_length Include length field (type 0x31 vs 0x30)
     * @return Number of bytes written
     */
    size_t serialize(uint8_t* out, bool with_length = true) const noexcept {
        size_t pos = 0;

        // Type byte
        out[pos++] = with_length ? 0x31 : 0x30;

        // Length (if with_length)
        if (with_length) {
            pos += VarInt::encode(length, out + pos);
        }

        // Data
        std::memcpy(out + pos, data, length);
        pos += length;

        return pos;
    }
};

} // namespace quic
} // namespace fasterapi
