#include "http3_parser.h"
#include <cstring>

namespace fasterapi {
namespace http {

// ============================================================================
// HTTP3Parser Implementation
// ============================================================================

HTTP3Parser::HTTP3Parser() {
    reset();
}

void HTTP3Parser::reset() noexcept {
    current_frame_type_ = 0;
    current_frame_length_ = 0;
    in_frame_ = false;
}

int HTTP3Parser::parse_frame_header(
    const uint8_t* data,
    size_t len,
    HTTP3FrameHeader& out_header,
    size_t& out_consumed
) noexcept {
    if (!data || len == 0) {
        return -1;
    }
    
    size_t pos = 0;
    
    // Parse frame type (varint)
    uint64_t frame_type;
    size_t type_consumed;
    
    if (parse_varint(data + pos, len - pos, frame_type, type_consumed) != 0) {
        return -1;  // Need more data
    }
    
    pos += type_consumed;
    
    // Parse frame length (varint)
    uint64_t frame_length;
    size_t length_consumed;
    
    if (parse_varint(data + pos, len - pos, frame_length, length_consumed) != 0) {
        return -1;  // Need more data
    }
    
    pos += length_consumed;
    
    // Fill output
    out_header.type = static_cast<HTTP3FrameType>(frame_type);
    out_header.length = frame_length;
    out_consumed = pos;
    
    return 0;
}

int HTTP3Parser::parse_settings(
    const uint8_t* data,
    size_t len,
    HTTP3Settings& out_settings
) noexcept {
    size_t pos = 0;
    
    while (pos < len) {
        // Parse setting ID (varint)
        uint64_t setting_id;
        size_t id_consumed;
        
        if (parse_varint(data + pos, len - pos, setting_id, id_consumed) != 0) {
            return 1;
        }
        
        pos += id_consumed;
        
        // Parse setting value (varint)
        uint64_t setting_value;
        size_t value_consumed;
        
        if (parse_varint(data + pos, len - pos, setting_value, value_consumed) != 0) {
            return 1;
        }
        
        pos += value_consumed;
        
        // Store setting based on ID
        switch (setting_id) {
            case 0x06:  // SETTINGS_MAX_HEADER_LIST_SIZE
                out_settings.max_header_list_size = setting_value;
                break;
            case 0x01:  // SETTINGS_QPACK_MAX_TABLE_CAPACITY
                out_settings.qpack_max_table_capacity = setting_value;
                break;
            case 0x07:  // SETTINGS_QPACK_BLOCKED_STREAMS
                out_settings.qpack_blocked_streams = setting_value;
                break;
            default:
                // Unknown setting, ignore
                break;
        }
    }
    
    return 0;
}

int HTTP3Parser::parse_varint(
    const uint8_t* data,
    size_t len,
    uint64_t& out_value,
    size_t& out_consumed
) noexcept {
    if (len == 0) {
        return -1;
    }
    
    // QUIC varint encoding (RFC 9000 Section 16)
    // First 2 bits indicate length:
    // 00 = 1 byte
    // 01 = 2 bytes
    // 10 = 4 bytes
    // 11 = 8 bytes
    
    uint8_t first_byte = data[0];
    uint8_t length_bits = (first_byte & 0xC0) >> 6;
    
    size_t bytes_needed;
    switch (length_bits) {
        case 0: bytes_needed = 1; break;
        case 1: bytes_needed = 2; break;
        case 2: bytes_needed = 4; break;
        case 3: bytes_needed = 8; break;
        default: return 1;  // Invalid
    }
    
    if (len < bytes_needed) {
        return -1;  // Need more data
    }
    
    // Decode value
    uint64_t value = first_byte & 0x3F;  // Remove length bits
    
    for (size_t i = 1; i < bytes_needed; ++i) {
        value = (value << 8) | data[i];
    }
    
    out_value = value;
    out_consumed = bytes_needed;
    
    return 0;
}

// ============================================================================
// QPACKDecoder Implementation (Simplified)
// ============================================================================

QPACKDecoder::QPACKDecoder(size_t max_table_size)
    : max_table_size_(max_table_size) {
}

int QPACKDecoder::decode(
    const uint8_t* data,
    size_t len,
    std::vector<std::pair<std::string_view, std::string_view>>& out_headers,
    size_t max_headers
) noexcept {
    // Simplified QPACK decoder
    // Full QPACK is complex (separate encoder/decoder streams)
    // For now, implement basic indexed headers
    
    size_t pos = 0;
    
    while (pos < len && out_headers.size() < max_headers) {
        uint8_t first_byte = data[pos];
        
        if (first_byte & 0x80) {
            // Indexed header
            // Similar to HPACK, but with different static table
            // TODO: Implement full QPACK static table
            pos++;
        } else {
            // Literal header
            // TODO: Implement literal decoding
            break;
        }
    }
    
    return 0;
}

} // namespace http
} // namespace fasterapi

