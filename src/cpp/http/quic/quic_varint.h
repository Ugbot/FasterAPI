#pragma once

#include <cstdint>
#include <cstddef>

namespace fasterapi {
namespace quic {

/**
 * QUIC Variable-Length Integer Encoding (RFC 9000 Section 16).
 * 
 * Encoding:
 *   00 = 1 byte  (0-63)
 *   01 = 2 bytes (0-16383)
 *   10 = 4 bytes (0-1073741823)
 *   11 = 8 bytes (0-4611686018427387903)
 * 
 * Performance: <10ns per encode/decode on modern CPUs.
 */
class VarInt {
public:
    /**
     * Encode variable-length integer.
     * 
     * @param value Value to encode
     * @param out Output buffer (must have at least 8 bytes)
     * @return Number of bytes written (1, 2, 4, or 8)
     */
    static size_t encode(uint64_t value, uint8_t* out) noexcept {
        if (value <= 63) {
            out[0] = static_cast<uint8_t>(value);
            return 1;
        } else if (value <= 16383) {
            out[0] = 0x40 | static_cast<uint8_t>(value >> 8);
            out[1] = static_cast<uint8_t>(value & 0xFF);
            return 2;
        } else if (value <= 1073741823) {
            out[0] = 0x80 | static_cast<uint8_t>(value >> 24);
            out[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
            out[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
            out[3] = static_cast<uint8_t>(value & 0xFF);
            return 4;
        } else {
            out[0] = 0xC0 | static_cast<uint8_t>(value >> 56);
            for (int i = 1; i < 8; i++) {
                out[i] = static_cast<uint8_t>((value >> (8 * (7 - i))) & 0xFF);
            }
            return 8;
        }
    }
    
    /**
     * Decode variable-length integer.
     * 
     * @param data Input buffer
     * @param len Buffer length
     * @param out Decoded value (output parameter)
     * @return Number of bytes consumed, or -1 if need more data
     */
    static int decode(const uint8_t* data, size_t len, uint64_t& out) noexcept {
        if (len == 0) {
            return -1;
        }
        
        uint8_t prefix = data[0] >> 6;
        size_t bytes = 1u << prefix;
        
        if (len < bytes) {
            return -1;
        }
        
        out = data[0] & 0x3F;
        for (size_t i = 1; i < bytes; i++) {
            out = (out << 8) | data[i];
        }
        
        return static_cast<int>(bytes);
    }
    
    /**
     * Get encoded size without encoding.
     * 
     * @param value Value to measure
     * @return Size in bytes (1, 2, 4, or 8)
     */
    static size_t encoded_size(uint64_t value) noexcept {
        if (value <= 63) return 1;
        if (value <= 16383) return 2;
        if (value <= 1073741823) return 4;
        return 8;
    }
};

} // namespace quic
} // namespace fasterapi
