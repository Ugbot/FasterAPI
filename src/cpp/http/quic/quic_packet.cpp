// QUIC packet implementation
// RFC 9000 compliant packet parsing and serialization

#include "quic_packet.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace fasterapi {
namespace quic {

// ============================================================================
// Packet Number Encoding/Decoding Helpers (RFC 9000 Section 17.1)
// ============================================================================

/**
 * Determine minimum bytes needed to encode a packet number.
 *
 * @param pn Packet number to encode
 * @return Number of bytes (1, 2, 3, or 4)
 */
uint8_t encode_packet_number_length(uint64_t pn) noexcept {
    if (pn < 0x100) return 1;
    if (pn < 0x10000) return 2;
    if (pn < 0x1000000) return 3;
    return 4;
}

/**
 * Encode truncated packet number for short header.
 *
 * Determines the minimum number of bytes needed to encode the packet number
 * such that it can be reconstructed from the largest acknowledged packet.
 *
 * @param full_pn Full packet number
 * @param largest_acked Largest acknowledged packet number
 * @return Number of bytes needed (1-4)
 */
uint8_t encode_packet_number_truncated(uint64_t full_pn, uint64_t largest_acked) noexcept {
    // Calculate the difference
    uint64_t diff = full_pn - largest_acked;

    // Determine bytes needed to represent difference with safety margin
    // We need at least twice the difference to ensure reconstruction
    uint64_t range = diff * 4;

    if (range < 0x80) return 1;        // 7-bit space
    if (range < 0x4000) return 2;      // 14-bit space
    if (range < 0x200000) return 3;    // 21-bit space
    return 4;                          // 28-bit space
}

/**
 * Decode and reconstruct full packet number from truncated value.
 *
 * Implements the packet number reconstruction algorithm from RFC 9000 Appendix A.3.
 *
 * @param truncated_pn Truncated packet number from wire
 * @param largest_acked Largest acknowledged packet number
 * @param pn_nbits Number of bits in truncated packet number (8, 16, 24, or 32)
 * @return Reconstructed full packet number
 */
uint64_t decode_packet_number(uint64_t truncated_pn,
                              uint64_t largest_acked,
                              uint8_t pn_nbits) noexcept {
    uint64_t expected_pn = largest_acked + 1;
    uint64_t pn_win = 1ULL << pn_nbits;
    uint64_t pn_hwin = pn_win / 2;
    uint64_t pn_mask = pn_win - 1;

    // The incoming packet number should be greater than expected_pn - pn_hwin
    // and less than or equal to expected_pn + pn_hwin
    //
    // This means we can't just strip the trailing bits from expected_pn and add
    // the truncated_pn because that might yield a value outside the window.
    //
    // To avoid that, we find the candidate value by stripping the trailing bits
    // from expected_pn and adding the truncated_pn, then adjust if needed.

    uint64_t candidate_pn = (expected_pn & ~pn_mask) | truncated_pn;

    // If the candidate is too far below expected, add a window
    if (candidate_pn <= expected_pn - pn_hwin &&
        candidate_pn < (1ULL << 62) - pn_win) {
        return candidate_pn + pn_win;
    }

    // If the candidate is too far above expected, subtract a window
    if (candidate_pn > expected_pn + pn_hwin && candidate_pn >= pn_win) {
        return candidate_pn - pn_win;
    }

    return candidate_pn;
}

// ============================================================================
// Packet Validation Helpers
// ============================================================================

/**
 * Validate QUIC version number.
 *
 * @param version Version to validate
 * @return true if version is supported
 */
bool validate_version(uint32_t version) noexcept {
    // Version 1 (RFC 9000)
    if (version == 0x00000001) return true;

    // Version negotiation (all zeros)
    if (version == 0x00000000) return true;

    // Reserved versions for forcing negotiation (0x?a?a?a?a)
    if ((version & 0x0F0F0F0F) == 0x0A0A0A0A) return true;

    return false;
}

/**
 * Validate fixed bit in packet header.
 *
 * The fixed bit (0x40) MUST be set to 1. If it's 0, the packet should be dropped.
 *
 * @param first_byte First byte of packet
 * @return true if fixed bit is valid
 */
bool validate_fixed_bit(uint8_t first_byte) noexcept {
    return (first_byte & 0x40) != 0;
}

/**
 * Check if packet is a long header packet.
 *
 * @param first_byte First byte of packet
 * @return true if long header
 */
bool is_long_header(uint8_t first_byte) noexcept {
    return (first_byte & 0x80) != 0;
}

// ============================================================================
// Connection ID Helpers
// ============================================================================

/**
 * Generate random connection ID.
 *
 * @param length Desired length (0-20 bytes)
 * @return Generated connection ID
 */
ConnectionID generate_connection_id(uint8_t length) noexcept {
    ConnectionID cid;
    cid.length = std::min(length, static_cast<uint8_t>(20));

    // Generate random bytes (simple implementation, should use crypto RNG in production)
    for (uint8_t i = 0; i < cid.length; i++) {
        cid.data[i] = static_cast<uint8_t>(rand() & 0xFF);
    }

    return cid;
}

/**
 * Compare two connection IDs.
 *
 * @param a First connection ID
 * @param b Second connection ID
 * @return 0 if equal, <0 if a < b, >0 if a > b
 */
int compare_connection_id(const ConnectionID& a, const ConnectionID& b) noexcept {
    if (a.length != b.length) {
        return static_cast<int>(a.length) - static_cast<int>(b.length);
    }
    return std::memcmp(a.data, b.data, a.length);
}

// ============================================================================
// Packet Type Helpers
// ============================================================================

/**
 * Get string representation of packet type.
 *
 * @param type Packet type
 * @return String name
 */
const char* packet_type_to_string(PacketType type) noexcept {
    switch (type) {
        case PacketType::INITIAL: return "Initial";
        case PacketType::ZERO_RTT: return "0-RTT";
        case PacketType::HANDSHAKE: return "Handshake";
        case PacketType::RETRY: return "Retry";
        case PacketType::ONE_RTT: return "1-RTT";
        default: return "Unknown";
    }
}

/**
 * Check if packet type requires token field.
 *
 * @param type Packet type
 * @return true if token field is present
 */
bool packet_type_has_token(PacketType type) noexcept {
    return type == PacketType::INITIAL;
}

/**
 * Check if packet type has packet number.
 *
 * @param type Packet type
 * @return true if packet has packet number field
 */
bool packet_type_has_packet_number(PacketType type) noexcept {
    return type != PacketType::RETRY;
}

// ============================================================================
// Buffer Size Estimation
// ============================================================================

/**
 * Estimate maximum header size for long header packet.
 *
 * @param type Packet type
 * @param dcid_len Destination connection ID length
 * @param scid_len Source connection ID length
 * @param token_len Token length (for Initial packets)
 * @return Maximum header size in bytes
 */
size_t estimate_long_header_size(PacketType type,
                                 uint8_t dcid_len,
                                 uint8_t scid_len,
                                 uint64_t token_len) noexcept {
    size_t size = 0;
    size += 1;                                    // First byte
    size += 4;                                    // Version
    size += 1 + dcid_len;                         // DCID
    size += 1 + scid_len;                         // SCID

    if (type == PacketType::INITIAL) {
        size += VarInt::encoded_size(token_len);  // Token length
        size += token_len;                        // Token
    }

    size += 8;                                    // Packet length (max varint)
    size += 4;                                    // Packet number (max)

    return size;
}

/**
 * Estimate maximum header size for short header packet.
 *
 * @param dcid_len Destination connection ID length
 * @return Maximum header size in bytes
 */
size_t estimate_short_header_size(uint8_t dcid_len) noexcept {
    size_t size = 0;
    size += 1;          // First byte
    size += dcid_len;   // DCID
    size += 4;          // Packet number (max)
    return size;
}

// ============================================================================
// Packet Assembly and Disassembly
// ============================================================================

/**
 * Parse complete packet from buffer.
 *
 * @param data Input buffer
 * @param len Buffer length
 * @param dcid_len Expected DCID length for short headers (from connection state)
 * @param packet Output packet
 * @param bytes_consumed Number of bytes consumed from buffer
 * @return 0 on success, -1 if need more data, >0 on error
 */
int parse_packet(const uint8_t* data,
                size_t len,
                uint8_t dcid_len,
                Packet& packet,
                size_t& bytes_consumed) noexcept {
    if (len < 1) return -1;

    uint8_t first_byte = data[0];

    // Validate fixed bit
    if (!validate_fixed_bit(first_byte)) {
        return 1;  // Invalid packet, drop it
    }

    size_t header_consumed = 0;
    int result;

    if (is_long_header(first_byte)) {
        // Parse long header
        packet.is_long_header = true;
        result = packet.long_hdr.parse(data, len, header_consumed);
        if (result != 0) return result;

        // Validate version
        if (!validate_version(packet.long_hdr.version)) {
            return 2;  // Unsupported version
        }

        // Payload starts after header
        packet.payload = const_cast<uint8_t*>(data + header_consumed);

        // For long headers, packet_length tells us the payload size
        if (len < header_consumed + packet.long_hdr.packet_length) {
            return -1;  // Need more data
        }

        packet.payload_length = packet.long_hdr.packet_length;
        bytes_consumed = header_consumed + packet.long_hdr.packet_length;

    } else {
        // Parse short header
        packet.is_long_header = false;
        result = packet.short_hdr.parse(data, len, dcid_len, header_consumed);
        if (result != 0) return result;

        // Payload is everything after the header
        packet.payload = const_cast<uint8_t*>(data + header_consumed);
        packet.payload_length = len - header_consumed;
        bytes_consumed = len;
    }

    return 0;
}

/**
 * Serialize complete packet to buffer.
 *
 * @param packet Packet to serialize
 * @param output Output buffer (must be large enough)
 * @param output_len Length of output buffer
 * @param bytes_written Number of bytes written (output)
 * @return 0 on success, -1 on insufficient buffer
 */
int serialize_packet(const Packet& packet,
                    uint8_t* output,
                    size_t output_len,
                    size_t& bytes_written) noexcept {
    size_t pos = 0;

    if (packet.is_long_header) {
        // Serialize long header
        size_t header_size = packet.long_hdr.serialize(output);
        pos += header_size;

        // Check buffer space
        if (pos + packet.payload_length > output_len) {
            return -1;
        }

        // Copy payload
        if (packet.payload && packet.payload_length > 0) {
            std::memcpy(output + pos, packet.payload, packet.payload_length);
            pos += packet.payload_length;
        }

    } else {
        // Serialize short header
        size_t header_size = packet.short_hdr.serialize(output);
        pos += header_size;

        // Check buffer space
        if (pos + packet.payload_length > output_len) {
            return -1;
        }

        // Copy payload
        if (packet.payload && packet.payload_length > 0) {
            std::memcpy(output + pos, packet.payload, packet.payload_length);
            pos += packet.payload_length;
        }
    }

    bytes_written = pos;
    return 0;
}

// ============================================================================
// Diagnostic Functions
// ============================================================================

/**
 * Calculate checksum for packet integrity verification (not part of QUIC spec).
 *
 * This is a simple utility for testing/debugging.
 *
 * @param data Buffer to checksum
 * @param len Buffer length
 * @return Simple checksum value
 */
uint32_t calculate_packet_checksum(const uint8_t* data, size_t len) noexcept {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
        sum = (sum << 1) | (sum >> 31);  // Rotate left
    }
    return sum;
}

/**
 * Dump packet header in human-readable format for debugging.
 *
 * @param packet Packet to dump
 * @param buffer Output buffer
 * @param buffer_size Size of output buffer
 * @return Number of bytes written to buffer
 */
int dump_packet_header(const Packet& packet, char* buffer, size_t buffer_size) noexcept {
    int written = 0;

    if (packet.is_long_header) {
        written = snprintf(buffer, buffer_size,
            "Long Header Packet:\n"
            "  Type: %s\n"
            "  Version: 0x%08X\n"
            "  DCID Length: %u\n"
            "  SCID Length: %u\n"
            "  Packet Length: %llu\n"
            "  Packet Number: %llu\n"
            "  Payload Length: %zu\n",
            packet_type_to_string(packet.long_hdr.type),
            packet.long_hdr.version,
            packet.long_hdr.dest_conn_id.length,
            packet.long_hdr.source_conn_id.length,
            (unsigned long long)packet.long_hdr.packet_length,
            (unsigned long long)packet.long_hdr.packet_number,
            packet.payload_length
        );
    } else {
        written = snprintf(buffer, buffer_size,
            "Short Header Packet:\n"
            "  Type: 1-RTT\n"
            "  DCID Length: %u\n"
            "  Packet Number: %llu\n"
            "  Packet Number Length: %u\n"
            "  Spin Bit: %d\n"
            "  Key Phase: %d\n"
            "  Payload Length: %zu\n",
            packet.short_hdr.dest_conn_id.length,
            (unsigned long long)packet.short_hdr.packet_number,
            packet.short_hdr.packet_number_length,
            packet.short_hdr.spin_bit ? 1 : 0,
            packet.short_hdr.key_phase ? 1 : 0,
            packet.payload_length
        );
    }

    return written;
}

} // namespace quic
} // namespace fasterapi
