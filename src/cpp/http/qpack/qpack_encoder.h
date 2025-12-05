#pragma once

#include "qpack_static_table.h"
#include "qpack_dynamic_table.h"
#include "../huffman.h"
#include "../quic/quic_varint.h"
#include <cstdint>
#include <string_view>
#include <vector>

namespace fasterapi {
namespace qpack {

/**
 * QPACK Encoder (RFC 9204).
 * 
 * Encodes HTTP headers to QPACK format with static/dynamic table indexing
 * and Huffman compression.
 */
class QPACKEncoder {
public:
    /**
     * Constructor.
     * 
     * @param max_table_capacity Maximum dynamic table capacity
     * @param max_blocked_streams Maximum blocked streams
     */
    explicit QPACKEncoder(size_t max_table_capacity = 4096, 
                         size_t max_blocked_streams = 100)
        : dynamic_table_(max_table_capacity),
          max_blocked_streams_(max_blocked_streams),
          use_huffman_(true) {
    }
    
    /**
     * Encode header field section.
     * 
     * @param headers Header name-value pairs
     * @param header_count Number of headers
     * @param output Output buffer
     * @param output_capacity Output buffer capacity
     * @param out_encoded_len Encoded length
     * @return 0 on success, -1 on error
     */
    int encode_field_section(
        const std::pair<std::string_view, std::string_view>* headers,
        size_t header_count,
        uint8_t* output,
        size_t output_capacity,
        size_t& out_encoded_len
    ) noexcept {
        size_t pos = 0;
        
        // Encoded Field Section Prefix (RFC 9204 Section 4.5.1)
        // Required Insert Count (varint)
        pos += quic::VarInt::encode(0, output + pos);  // Simplified: no dynamic table refs
        
        // Sign bit (0) + Delta Base (varint)
        pos += quic::VarInt::encode(0, output + pos);
        
        // Encode each header
        for (size_t i = 0; i < header_count; i++) {
            const auto& [name, value] = headers[i];
            
            // Try static table lookup
            int static_idx = QPACKStaticTable::find(name, value);
            if (static_idx >= 0) {
                // Indexed Field Line (static, RFC 9204 Section 4.5.2)
                if (!encode_indexed_static(static_idx, output + pos, 
                                          output_capacity - pos, pos)) {
                    return -1;
                }
                continue;
            }
            
            // Try dynamic table lookup
            int dynamic_idx = dynamic_table_.find(name, value);
            if (dynamic_idx >= 0) {
                // Indexed Field Line (dynamic, RFC 9204 Section 4.5.3)
                if (!encode_indexed_dynamic(dynamic_idx, output + pos,
                                           output_capacity - pos, pos)) {
                    return -1;
                }
                continue;
            }
            
            // Check for name-only match in static table
            int name_idx = QPACKStaticTable::find_name(name);
            if (name_idx >= 0) {
                // Literal Field Line With Name Reference (static)
                if (!encode_literal_with_name_ref_static(
                        name_idx, value, output + pos, 
                        output_capacity - pos, pos)) {
                    return -1;
                }
                continue;
            }
            
            // Check for name-only match in dynamic table
            name_idx = dynamic_table_.find_name(name);
            if (name_idx >= 0) {
                // Literal Field Line With Name Reference (dynamic)
                if (!encode_literal_with_name_ref_dynamic(
                        name_idx, value, output + pos,
                        output_capacity - pos, pos)) {
                    return -1;
                }
                continue;
            }
            
            // No match: encode literal with literal name
            if (!encode_literal_with_literal_name(name, value, output + pos,
                                                 output_capacity - pos, pos)) {
                return -1;
            }
        }
        
        out_encoded_len = pos;
        return 0;
    }
    
    /**
     * Enable/disable Huffman encoding.
     */
    void set_huffman_encoding(bool enabled) noexcept {
        use_huffman_ = enabled;
    }
    
    /**
     * Get dynamic table reference.
     */
    QPACKDynamicTable& dynamic_table() noexcept { return dynamic_table_; }

private:
    /**
     * Encode QPACK integer (RFC 9204 Section 4.1.1).
     *
     * This is similar to QUIC varint but uses a prefix-based encoding.
     * The remaining value is encoded in subsequent bytes.
     *
     * @param value Value to encode (after subtracting prefix bits)
     * @param output Output buffer (already offset to write position)
     * @param capacity Remaining buffer capacity
     * @return Number of bytes written, or 0 on error
     */
    size_t encode_qpack_integer(uint64_t value, uint8_t* output,
                                 size_t capacity) noexcept {
        size_t written = 0;
        // Encode continuation bytes (7 bits per byte)
        while (value >= 128) {
            if (written >= capacity) return 0;
            output[written++] = (value & 0x7F) | 0x80;
            value >>= 7;
        }
        if (written >= capacity) return 0;
        output[written++] = value & 0x7F;
        return written;
    }

    /**
     * Encode indexed field (static table).
     *
     * Format: 1 1 T Index(6+)
     * where T=1 for static table (bit 6)
     * Pattern: 11TXXXXX = 111XXXXX for static
     */
    bool encode_indexed_static(int index, uint8_t* output,
                               size_t capacity, size_t& pos) noexcept {
        if (capacity < 1) return false;

        // 1 1 T=1 Index(6+)
        // Pattern: 11TXXXXX where T=1 for static table (bit 6)
        // 0xC0 = 11000000, bit 6 set means static table
        if (index < 63) {
            // Index fits in 6 bits
            output[0] = 0xC0 | index;  // 11TXXXXX where T=1 (bit 6) is part of 0xC0
            pos += 1;
        } else {
            // Need more bytes (use QPACK integer encoding with 6-bit prefix)
            output[0] = 0xC0 | 0x3F;  // All 6 index bits set
            size_t cont_bytes = encode_qpack_integer(index - 63, output + 1, capacity - 1);
            if (cont_bytes == 0) return false;
            pos += 1 + cont_bytes;
        }

        return true;
    }
    
    /**
     * Encode indexed field (dynamic table).
     *
     * Format: 1 1 T Index(6+)
     * where T=0 for dynamic table (bit 6)
     * Pattern: 110XXXXX for dynamic
     */
    bool encode_indexed_dynamic(int index, uint8_t* output,
                                size_t capacity, size_t& pos) noexcept {
        if (capacity < 1) return false;

        // 1 1 T=0 Index(6+)
        // Pattern: 11000000 | index (for dynamic table, T=0)
        if (index < 63) {
            output[0] = 0xC0 | index;
            pos += 1;
        } else {
            output[0] = 0xC0 | 0x3F;
            size_t cont_bytes = encode_qpack_integer(index - 63, output + 1, capacity - 1);
            if (cont_bytes == 0) return false;
            pos += 1 + cont_bytes;
        }

        return true;
    }
    
    /**
     * Encode literal field with name reference (static).
     *
     * Format: 0 1 N T NameIndex(4+) H ValueLength(7+) Value
     * N=0 (no dynamic table insertion), T=1 (static table)
     * Pattern: 0101XXXX for static table reference
     */
    bool encode_literal_with_name_ref_static(int name_idx, std::string_view value,
                                            uint8_t* output, size_t capacity,
                                            size_t& pos) noexcept {
        if (capacity < 2) return false;

        // First byte: 01NTXXXX where N=0, T=1
        // Pattern: 01010000 = 0x50
        size_t local_pos = 0;
        if (name_idx < 15) {
            output[0] = 0x50 | name_idx;
            local_pos = 1;
        } else {
            output[0] = 0x50 | 0x0F;
            size_t cont_bytes = encode_qpack_integer(name_idx - 15, output + 1, capacity - 1);
            if (cont_bytes == 0) return false;
            local_pos = 1 + cont_bytes;
        }

        // Encode value
        size_t value_pos = 0;
        if (!encode_string(value, output + local_pos, capacity - local_pos, value_pos)) {
            return false;
        }
        pos += local_pos + value_pos;
        return true;
    }
    
    /**
     * Encode literal field with name reference (dynamic).
     *
     * Format: 0 1 N T NameIndex(4+) H ValueLength(7+) Value
     * N=0 (no dynamic table insertion), T=0 (dynamic table)
     * Pattern: 0100XXXX for dynamic table reference
     */
    bool encode_literal_with_name_ref_dynamic(int name_idx, std::string_view value,
                                             uint8_t* output, size_t capacity,
                                             size_t& pos) noexcept {
        if (capacity < 2) return false;

        // First byte: 01NTXXXX where N=0, T=0
        // Pattern: 01000000 = 0x40
        size_t local_pos = 0;
        if (name_idx < 15) {
            output[0] = 0x40 | name_idx;
            local_pos = 1;
        } else {
            output[0] = 0x40 | 0x0F;
            size_t cont_bytes = encode_qpack_integer(name_idx - 15, output + 1, capacity - 1);
            if (cont_bytes == 0) return false;
            local_pos = 1 + cont_bytes;
        }

        size_t value_pos = 0;
        if (!encode_string(value, output + local_pos, capacity - local_pos, value_pos)) {
            return false;
        }
        pos += local_pos + value_pos;
        return true;
    }
    
    /**
     * Encode literal field with literal name.
     *
     * Format: 0 0 1 N H NameLength(3+) Name H ValueLength(7+) Value
     * N=0 (no dynamic table insertion)
     * H bit is part of the string encoding
     * Pattern: 00100000 = 0x20
     */
    bool encode_literal_with_literal_name(std::string_view name, std::string_view value,
                                         uint8_t* output, size_t capacity,
                                         size_t& pos) noexcept {
        if (capacity < 3) return false;

        // First byte: 001N0000 where N=0
        // Pattern: 00100000 = 0x20
        output[0] = 0x20;
        size_t local_pos = 1;

        // Encode name (with H bit in string encoding)
        size_t name_pos = 0;
        if (!encode_string(name, output + local_pos, capacity - local_pos, name_pos)) {
            return false;
        }
        local_pos += name_pos;

        // Encode value (with H bit in string encoding)
        size_t value_pos = 0;
        if (!encode_string(value, output + local_pos, capacity - local_pos, value_pos)) {
            return false;
        }
        pos += local_pos + value_pos;
        return true;
    }
    
    /**
     * Encode string (with optional Huffman).
     *
     * Format: H Length(7+) Data
     * H=1 for Huffman encoded, H=0 for literal
     * Length uses 7-bit prefix integer encoding
     */
    bool encode_string(std::string_view str, uint8_t* output,
                      size_t capacity, size_t& pos) noexcept {
        size_t local_pos = 0;

        if (use_huffman_) {
            // Try Huffman encoding
            size_t huffman_size = http::HuffmanEncoder::encoded_size(
                reinterpret_cast<const uint8_t*>(str.data()), str.length()
            );

            // Use Huffman if it saves space
            if (huffman_size < str.length() && capacity >= huffman_size + 2) {
                // H bit set (bit 7)
                if (huffman_size < 127) {
                    output[0] = 0x80 | huffman_size;
                    local_pos = 1;
                } else {
                    output[0] = 0x80 | 0x7F;
                    size_t cont_bytes = encode_qpack_integer(huffman_size - 127, output + 1, capacity - 1);
                    if (cont_bytes == 0) return false;
                    local_pos = 1 + cont_bytes;
                }

                size_t encoded_len;
                int result = http::HuffmanEncoder::encode(
                    reinterpret_cast<const uint8_t*>(str.data()),
                    str.length(),
                    output + local_pos,
                    capacity - local_pos,
                    encoded_len
                );
                if (result != 0) return false;

                pos += local_pos + encoded_len;
                return true;
            }
        }

        // Literal (no Huffman), H=0
        if (capacity < str.length() + 2) return false;

        if (str.length() < 127) {
            output[0] = str.length();
            local_pos = 1;
        } else {
            output[0] = 0x7F;
            size_t cont_bytes = encode_qpack_integer(str.length() - 127, output + 1, capacity - 1);
            if (cont_bytes == 0) return false;
            local_pos = 1 + cont_bytes;
        }

        if (capacity - local_pos < str.length()) return false;
        std::memcpy(output + local_pos, str.data(), str.length());
        pos += local_pos + str.length();

        return true;
    }
    
    QPACKDynamicTable dynamic_table_;
    size_t max_blocked_streams_;
    bool use_huffman_;
};

} // namespace qpack
} // namespace fasterapi
