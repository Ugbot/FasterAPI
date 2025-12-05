#include "http3_parser.h"
#include "quic/quic_varint.h"
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
    int type_consumed = quic::VarInt::decode(data + pos, len - pos, frame_type);
    
    if (type_consumed < 0) {
        return -1;  // Need more data
    }
    
    pos += type_consumed;
    
    // Parse frame length (varint)
    uint64_t frame_length;
    int length_consumed = quic::VarInt::decode(data + pos, len - pos, frame_length);
    
    if (length_consumed < 0) {
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
        int id_consumed = quic::VarInt::decode(data + pos, len - pos, setting_id);
        
        if (id_consumed < 0) {
            return 1;
        }
        
        pos += id_consumed;
        
        // Parse setting value (varint)
        uint64_t setting_value;
        int value_consumed = quic::VarInt::decode(data + pos, len - pos, setting_value);
        
        if (value_consumed < 0) {
            return 1;
        }
        
        pos += value_consumed;
        
        // Store setting based on ID (RFC 9114 Section 7.2.4)
        switch (setting_id) {
            case 0x01:  // SETTINGS_QPACK_MAX_TABLE_CAPACITY
                out_settings.qpack_max_table_capacity = setting_value;
                break;
            case 0x06:  // SETTINGS_MAX_HEADER_LIST_SIZE
                out_settings.max_header_list_size = setting_value;
                break;
            case 0x07:  // SETTINGS_QPACK_BLOCKED_STREAMS
                out_settings.qpack_blocked_streams = setting_value;
                break;
            default:
                // Unknown setting, ignore per spec
                break;
        }
    }
    
    return 0;
}

} // namespace http
} // namespace fasterapi
