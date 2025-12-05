#pragma once

#include "quic_varint.h"
#include <cstdint>
#include <cstring>
#include <array>

namespace fasterapi {
namespace quic {

/**
 * QUIC packet types (RFC 9000 Section 17).
 */
enum class PacketType : uint8_t {
    INITIAL = 0x00,
    ZERO_RTT = 0x01,
    HANDSHAKE = 0x02,
    RETRY = 0x03,
    ONE_RTT = 0x04,  // Short header packet
};

/**
 * QUIC connection ID.
 * 
 * Max length is 20 bytes (RFC 9000).
 */
struct ConnectionID {
    uint8_t data[20];
    uint8_t length;
    
    ConnectionID() : length(0) {
        std::memset(data, 0, sizeof(data));
    }
    
    ConnectionID(const uint8_t* bytes, uint8_t len) : length(len) {
        std::memcpy(data, bytes, len);
    }
    
    bool operator==(const ConnectionID& other) const noexcept {
        return length == other.length && 
               std::memcmp(data, other.data, length) == 0;
    }
    
    bool operator!=(const ConnectionID& other) const noexcept {
        return !(*this == other);
    }
};

/**
 * QUIC long header (Initial, 0-RTT, Handshake, Retry).
 * 
 * Format (RFC 9000 Section 17.2):
 * +-+-+-+-+-+-+-+-+
 * |1|1|T T|X X X X|
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                         Version (32)                          |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | DCID Len (8)  |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |               Destination Connection ID (0..160)            ...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | SCID Len (8)  |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                 Source Connection ID (0..160)               ...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
struct LongHeader {
    PacketType type;
    uint32_t version;
    ConnectionID dest_conn_id;
    ConnectionID source_conn_id;
    uint64_t token_length;       // Only for Initial packets
    uint8_t* token;              // Only for Initial packets
    uint64_t packet_length;      // Remaining packet length
    uint64_t packet_number;      // Encrypted packet number
    
    /**
     * Parse long header from buffer.
     * 
     * @param data Input buffer
     * @param len Buffer length
     * @param out_consumed Bytes consumed
     * @return 0 on success, -1 if need more data, 1 on error
     */
    int parse(const uint8_t* data, size_t len, size_t& out_consumed) noexcept {
        if (len < 1) return -1;
        
        uint8_t first_byte = data[0];
        
        // Check for long header (bit 0x80 set)
        if ((first_byte & 0x80) == 0) {
            return 1;  // Not a long header
        }
        
        // Extract packet type
        uint8_t type_bits = (first_byte >> 4) & 0x03;
        type = static_cast<PacketType>(type_bits);
        
        size_t pos = 1;
        
        // Parse version (4 bytes)
        if (len < pos + 4) return -1;
        version = (static_cast<uint32_t>(data[pos]) << 24) |
                  (static_cast<uint32_t>(data[pos+1]) << 16) |
                  (static_cast<uint32_t>(data[pos+2]) << 8) |
                  static_cast<uint32_t>(data[pos+3]);
        pos += 4;
        
        // Parse DCID length and value
        if (len < pos + 1) return -1;
        uint8_t dcid_len = data[pos++];
        if (dcid_len > 20) return 1;  // Invalid length
        if (len < pos + dcid_len) return -1;
        dest_conn_id = ConnectionID(data + pos, dcid_len);
        pos += dcid_len;
        
        // Parse SCID length and value
        if (len < pos + 1) return -1;
        uint8_t scid_len = data[pos++];
        if (scid_len > 20) return 1;  // Invalid length
        if (len < pos + scid_len) return -1;
        source_conn_id = ConnectionID(data + pos, scid_len);
        pos += scid_len;
        
        // For Initial packets, parse token
        if (type == PacketType::INITIAL) {
            uint64_t token_len;
            int consumed = VarInt::decode(data + pos, len - pos, token_len);
            if (consumed < 0) return -1;
            pos += consumed;
            
            if (len < pos + token_len) return -1;
            token_length = token_len;
            token = const_cast<uint8_t*>(data + pos);
            pos += token_len;
        } else {
            token_length = 0;
            token = nullptr;
        }
        
        // Parse packet length
        uint64_t pkt_len;
        int consumed = VarInt::decode(data + pos, len - pos, pkt_len);
        if (consumed < 0) return -1;
        pos += consumed;
        packet_length = pkt_len;
        
        out_consumed = pos;
        return 0;
    }
    
    /**
     * Serialize long header to buffer.
     * 
     * @param out Output buffer (must be large enough)
     * @return Number of bytes written
     */
    size_t serialize(uint8_t* out) const noexcept {
        size_t pos = 0;
        
        // First byte: 1|1|TT|XXXX
        out[pos++] = 0xC0 | (static_cast<uint8_t>(type) << 4);
        
        // Version (4 bytes)
        out[pos++] = (version >> 24) & 0xFF;
        out[pos++] = (version >> 16) & 0xFF;
        out[pos++] = (version >> 8) & 0xFF;
        out[pos++] = version & 0xFF;
        
        // DCID
        out[pos++] = dest_conn_id.length;
        std::memcpy(out + pos, dest_conn_id.data, dest_conn_id.length);
        pos += dest_conn_id.length;
        
        // SCID
        out[pos++] = source_conn_id.length;
        std::memcpy(out + pos, source_conn_id.data, source_conn_id.length);
        pos += source_conn_id.length;
        
        // Token (Initial packets only)
        if (type == PacketType::INITIAL) {
            pos += VarInt::encode(token_length, out + pos);
            if (token_length > 0) {
                std::memcpy(out + pos, token, token_length);
                pos += token_length;
            }
        }
        
        // Packet length
        pos += VarInt::encode(packet_length, out + pos);
        
        return pos;
    }
};

/**
 * QUIC short header (1-RTT packets).
 * 
 * Format (RFC 9000 Section 17.3):
 * +-+-+-+-+-+-+-+-+
 * |0|1|S|R|R|K|P P|
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |               Destination Connection ID (0..160)            ...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                     Packet Number (8/16/24/32)              ...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                     Protected Payload (*)                   ...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
struct ShortHeader {
    bool spin_bit;
    bool key_phase;
    ConnectionID dest_conn_id;
    uint64_t packet_number;
    uint8_t packet_number_length;  // 1, 2, 3, or 4 bytes
    
    /**
     * Parse short header from buffer.
     * 
     * @param data Input buffer
     * @param len Buffer length
     * @param dcid_len Expected DCID length
     * @param out_consumed Bytes consumed
     * @return 0 on success, -1 if need more data, 1 on error
     */
    int parse(const uint8_t* data, size_t len, uint8_t dcid_len, size_t& out_consumed) noexcept {
        if (len < 1) return -1;
        
        uint8_t first_byte = data[0];
        
        // Check for short header (bit 0x80 clear)
        if ((first_byte & 0x80) != 0) {
            return 1;  // Not a short header
        }
        
        // Extract flags
        spin_bit = (first_byte & 0x20) != 0;
        key_phase = (first_byte & 0x04) != 0;
        packet_number_length = (first_byte & 0x03) + 1;
        
        size_t pos = 1;
        
        // Parse DCID
        if (dcid_len > 20) return 1;
        if (len < pos + dcid_len) return -1;
        dest_conn_id = ConnectionID(data + pos, dcid_len);
        pos += dcid_len;
        
        // Parse packet number
        if (len < pos + packet_number_length) return -1;
        packet_number = 0;
        for (uint8_t i = 0; i < packet_number_length; i++) {
            packet_number = (packet_number << 8) | data[pos++];
        }
        
        out_consumed = pos;
        return 0;
    }
    
    /**
     * Serialize short header to buffer.
     * 
     * @param out Output buffer (must be large enough)
     * @return Number of bytes written
     */
    size_t serialize(uint8_t* out) const noexcept {
        size_t pos = 0;
        
        // First byte: 0|1|S|R|R|K|PP
        uint8_t first = 0x40;
        if (spin_bit) first |= 0x20;
        if (key_phase) first |= 0x04;
        first |= (packet_number_length - 1) & 0x03;
        out[pos++] = first;
        
        // DCID
        std::memcpy(out + pos, dest_conn_id.data, dest_conn_id.length);
        pos += dest_conn_id.length;
        
        // Packet number
        for (int i = packet_number_length - 1; i >= 0; i--) {
            out[pos++] = (packet_number >> (i * 8)) & 0xFF;
        }
        
        return pos;
    }
};

/**
 * QUIC packet (generic wrapper).
 */
struct Packet {
    bool is_long_header;
    union {
        LongHeader long_hdr;
        ShortHeader short_hdr;
    };

    uint8_t* payload;
    size_t payload_length;

    Packet() : is_long_header(true), payload(nullptr), payload_length(0) {
        std::memset(&long_hdr, 0, sizeof(long_hdr));
    }
};

// ============================================================================
// Helper Functions (implemented in quic_packet.cpp)
// ============================================================================

/**
 * Packet number encoding/decoding helpers.
 */
uint8_t encode_packet_number_length(uint64_t pn) noexcept;
uint8_t encode_packet_number_truncated(uint64_t full_pn, uint64_t largest_acked) noexcept;
uint64_t decode_packet_number(uint64_t truncated_pn, uint64_t largest_acked, uint8_t pn_nbits) noexcept;

/**
 * Packet validation helpers.
 */
bool validate_version(uint32_t version) noexcept;
bool validate_fixed_bit(uint8_t first_byte) noexcept;
bool is_long_header(uint8_t first_byte) noexcept;

/**
 * Connection ID helpers.
 */
ConnectionID generate_connection_id(uint8_t length) noexcept;
int compare_connection_id(const ConnectionID& a, const ConnectionID& b) noexcept;

/**
 * Packet type helpers.
 */
const char* packet_type_to_string(PacketType type) noexcept;
bool packet_type_has_token(PacketType type) noexcept;
bool packet_type_has_packet_number(PacketType type) noexcept;

/**
 * Buffer size estimation.
 */
size_t estimate_long_header_size(PacketType type, uint8_t dcid_len, uint8_t scid_len, uint64_t token_len) noexcept;
size_t estimate_short_header_size(uint8_t dcid_len) noexcept;

/**
 * Packet assembly/disassembly.
 */
int parse_packet(const uint8_t* data, size_t len, uint8_t dcid_len, Packet& packet, size_t& bytes_consumed) noexcept;
int serialize_packet(const Packet& packet, uint8_t* output, size_t output_len, size_t& bytes_written) noexcept;

/**
 * Diagnostic functions.
 */
uint32_t calculate_packet_checksum(const uint8_t* data, size_t len) noexcept;
int dump_packet_header(const Packet& packet, char* buffer, size_t buffer_size) noexcept;

} // namespace quic
} // namespace fasterapi
