#pragma once

#include "qpack_static_table.h"
#include "qpack_dynamic_table.h"
#include "../huffman.h"
#include "../quic/quic_varint.h"
#include <cstdint>
#include <string_view>
#include <vector>
#include <string>

namespace fasterapi {
namespace qpack {

/**
 * QPACK Decoder (RFC 9204).
 * 
 * Decodes QPACK-encoded header field sections.
 */
class QPACKDecoder {
public:
    /**
     * Maximum headers to decode (prevents DoS).
     */
    static constexpr size_t kMaxHeaders = 256;
    
    /**
     * Maximum header size (prevents DoS).
     */
    static constexpr size_t kMaxHeaderSize = 8192;
    
    /**
     * Constructor.
     * 
     * @param max_table_capacity Maximum dynamic table capacity
     */
    explicit QPACKDecoder(size_t max_table_capacity = 4096)
        : dynamic_table_(max_table_capacity) {
    }
    
    /**
     * Decode header field section.
     *
     * @param input Encoded data
     * @param input_len Input length
     * @param out_headers Output headers (must have space for kMaxHeaders)
     * @param out_count Number of headers decoded
     * @return 0 on success, -1 on error
     */
    int decode_field_section(
        const uint8_t* input,
        size_t input_len,
        std::pair<std::string, std::string>* out_headers,
        size_t& out_count
    ) noexcept {
        size_t pos = 0;
        out_count = 0;

        if (input_len == 0) return -1;

        // Decode prefix (RFC 9204 Section 4.5.1)
        uint64_t required_insert_count;
        size_t consumed = 0;
        if (!decode_prefix_int(input + pos, input_len - pos, 8,
                               required_insert_count, consumed)) {
            return -1;
        }
        pos += consumed;

        // Decode Delta Base (sign bit + varint)
        if (pos >= input_len) return -1;
        bool delta_base_sign = (input[pos] & 0x80) != 0;
        uint64_t delta_base_value;
        if (!decode_prefix_int(input + pos, input_len - pos, 7,
                               delta_base_value, consumed)) {
            return -1;
        }
        pos += consumed;

        // Decode field lines
        while (pos < input_len && out_count < kMaxHeaders) {
            uint8_t first_byte = input[pos];

            if ((first_byte & 0x80) != 0) {
                // 1xxxxxxx: Indexed field line (RFC 9204 Section 4.5.2)
                // Format: 1 T Index(6+)
                // Bit 7 = 1, Bit 6 = T
                bool is_static = (first_byte & 0x40) != 0;  // T bit

                if (!decode_indexed(input + pos, input_len - pos,
                                   is_static, out_headers[out_count], consumed)) {
                    return -1;
                }
                pos += consumed;
                out_count++;
            } else if ((first_byte & 0x40) != 0) {
                // 01xxxxxx: Literal Field Line with Name Reference
                bool is_static = (first_byte & 0x10) != 0;  // T bit

                if (!decode_literal_with_name_ref(input + pos, input_len - pos,
                                                 is_static, out_headers[out_count], consumed)) {
                    return -1;
                }
                pos += consumed;
                out_count++;
            } else if ((first_byte & 0x20) != 0) {
                // 001xxxxx: Literal Field Line with Literal Name
                if (!decode_literal_with_literal_name(input + pos, input_len - pos,
                                                     out_headers[out_count], consumed)) {
                    return -1;
                }
                pos += consumed;
                out_count++;
            } else {
                // 000xxxxx: Other instruction types (post-base, etc.)
                // Simplified: not implemented
                return -1;
            }
        }

        return 0;
    }
    
    /**
     * Get dynamic table reference.
     */
    QPACKDynamicTable& dynamic_table() noexcept { return dynamic_table_; }

private:
    /**
     * Decode QPACK prefix integer (RFC 9204 Section 4.1.1).
     *
     * This is similar to HPACK integer encoding but different from QUIC varint.
     *
     * @param input Input buffer
     * @param len Input length
     * @param prefix_bits Number of prefix bits (1-8)
     * @param out_value Decoded value
     * @param out_consumed Bytes consumed
     * @return true on success, false on error
     */
    bool decode_prefix_int(const uint8_t* input, size_t len, uint8_t prefix_bits,
                          uint64_t& out_value, size_t& out_consumed) noexcept {
        if (len == 0 || prefix_bits == 0 || prefix_bits > 8) return false;

        uint64_t max_prefix = (1ULL << prefix_bits) - 1;
        uint64_t prefix_mask = max_prefix;

        out_value = input[0] & prefix_mask;
        out_consumed = 1;

        if (out_value < max_prefix) {
            // Value fits in prefix
            return true;
        }

        // Multi-byte encoding: value = max_prefix + continuation bytes
        uint64_t m = 0;
        uint8_t byte;

        do {
            if (out_consumed >= len) return false;  // Need more data
            if (m >= 63) return false;  // Overflow protection

            byte = input[out_consumed];
            out_consumed++;

            out_value += (byte & 0x7F) << m;
            m += 7;
        } while ((byte & 0x80) != 0);

        return true;
    }

    /**
     * Decode indexed field line.
     *
     * Format: 1 1 T Index(6+)
     * T=1 for static table, T=0 for dynamic table
     */
    bool decode_indexed(const uint8_t* input, size_t len, bool is_static,
                       std::pair<std::string, std::string>& out_header,
                       size_t& out_consumed) noexcept {
        if (len == 0) return false;

        // Extract T bit (bit 6)
        bool table_bit = (input[0] & 0x40) != 0;

        // Decode index with 6-bit prefix
        uint64_t index;
        if (!decode_prefix_int(input, len, 6, index, out_consumed)) {
            return false;
        }

        if (table_bit) {
            // Static table
            const StaticEntry* entry = QPACKStaticTable::get(index);
            if (!entry) return false;

            out_header.first = std::string(entry->name);
            out_header.second = std::string(entry->value);
        } else {
            // Dynamic table
            const DynamicEntry* entry = dynamic_table_.get(index);
            if (!entry) return false;

            out_header.first = entry->name;
            out_header.second = entry->value;
        }

        return true;
    }
    
    /**
     * Decode literal with name reference.
     *
     * Format: 0 1 N T NameIndex(4+) H ValueLength(7+) Value
     * N=0 (not adding to dynamic table)
     * T=1 for static, T=0 for dynamic
     */
    bool decode_literal_with_name_ref(const uint8_t* input, size_t len,
                                     bool is_static,
                                     std::pair<std::string, std::string>& out_header,
                                     size_t& out_consumed) noexcept {
        if (len == 0) return false;

        // Extract T bit (bit 4)
        bool table_bit = (input[0] & 0x10) != 0;

        // Decode name index with 4-bit prefix
        uint64_t name_idx;
        size_t consumed;
        if (!decode_prefix_int(input, len, 4, name_idx, consumed)) {
            return false;
        }

        // Get name from appropriate table
        if (table_bit) {
            // Static table
            const StaticEntry* entry = QPACKStaticTable::get(name_idx);
            if (!entry) return false;
            out_header.first = std::string(entry->name);
        } else {
            // Dynamic table
            const DynamicEntry* entry = dynamic_table_.get(name_idx);
            if (!entry) return false;
            out_header.first = entry->name;
        }

        // Decode value string
        std::string value;
        size_t value_consumed;
        if (!decode_string(input + consumed, len - consumed, value, value_consumed)) {
            return false;
        }
        out_header.second = std::move(value);

        out_consumed = consumed + value_consumed;
        return true;
    }
    
    /**
     * Decode literal with literal name.
     *
     * Format: 0 0 1 N H NameLength(3+) Name H ValueLength(7+) Value
     * N=0 (not adding to dynamic table)
     */
    bool decode_literal_with_literal_name(const uint8_t* input, size_t len,
                                         std::pair<std::string, std::string>& out_header,
                                         size_t& out_consumed) noexcept {
        if (len == 0) return false;

        size_t pos = 1;  // Skip first byte (pattern already checked)

        // Decode name string
        std::string name;
        size_t name_consumed;
        if (!decode_string(input + pos, len - pos, name, name_consumed)) {
            return false;
        }
        pos += name_consumed;
        out_header.first = std::move(name);

        // Decode value string
        std::string value;
        size_t value_consumed;
        if (!decode_string(input + pos, len - pos, value, value_consumed)) {
            return false;
        }
        pos += value_consumed;
        out_header.second = std::move(value);

        out_consumed = pos;
        return true;
    }
    
    /**
     * Decode string (with optional Huffman decoding).
     *
     * Format: H Length(7+) Data
     * H=1 for Huffman-encoded, H=0 for literal
     */
    bool decode_string(const uint8_t* input, size_t len,
                      std::string& out_str, size_t& out_consumed) noexcept {
        if (len == 0) return false;

        uint8_t first_byte = input[0];
        bool is_huffman = (first_byte & 0x80) != 0;

        // Decode string length with 7-bit prefix
        uint64_t str_len;
        size_t consumed;
        if (!decode_prefix_int(input, len, 7, str_len, consumed)) {
            return false;
        }

        // Check size limits
        if (str_len > kMaxHeaderSize) return false;
        if (len < consumed + str_len) return false;

        if (is_huffman) {
            // Huffman-encoded
            uint8_t decoded_buf[kMaxHeaderSize];
            size_t decoded_len;

            if (http::HuffmanDecoder::decode(input + consumed, str_len,
                                            decoded_buf, sizeof(decoded_buf),
                                            decoded_len) != 0) {
                return false;
            }

            out_str.assign(reinterpret_cast<char*>(decoded_buf), decoded_len);
        } else {
            // Literal string
            out_str.assign(reinterpret_cast<const char*>(input + consumed), str_len);
        }

        out_consumed = consumed + str_len;
        return true;
    }
    
    QPACKDynamicTable dynamic_table_;
};

} // namespace qpack
} // namespace fasterapi
