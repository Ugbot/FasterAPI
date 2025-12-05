#pragma once

#include "quic/quic_varint.h"
#include "qpack/qpack_decoder.h"
#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fasterapi {
namespace http {

/**
 * HTTP/3 frame types (RFC 9114 Section 7.2).
 */
enum class HTTP3FrameType : uint64_t {
    DATA = 0x00,
    HEADERS = 0x01,
    CANCEL_PUSH = 0x03,
    SETTINGS = 0x04,
    PUSH_PROMISE = 0x05,
    GOAWAY = 0x07,
    MAX_PUSH_ID = 0x0D,
};

/**
 * HTTP/3 frame header.
 */
struct HTTP3FrameHeader {
    HTTP3FrameType type;
    uint64_t length;
};

/**
 * HTTP/3 SETTINGS frame parameters (RFC 9114 Section 7.2.4).
 */
struct HTTP3Settings {
    uint64_t qpack_max_table_capacity{4096};
    uint64_t max_header_list_size{16384};
    uint64_t qpack_blocked_streams{100};
};

/**
 * HTTP/3 frame parser.
 * 
 * Parses HTTP/3 frames from QUIC stream data.
 * Zero-allocation, uses QPACK decoder for headers.
 */
class HTTP3Parser {
public:
    HTTP3Parser();
    
    /**
     * Parse frame header.
     * 
     * @param data Input buffer
     * @param len Buffer length
     * @param out_header Parsed frame header
     * @param out_consumed Bytes consumed
     * @return 0 on success, -1 if need more data, 1 on error
     */
    int parse_frame_header(
        const uint8_t* data,
        size_t len,
        HTTP3FrameHeader& out_header,
        size_t& out_consumed
    ) noexcept;
    
    /**
     * Parse SETTINGS frame payload.
     * 
     * @param data Frame payload
     * @param len Payload length
     * @param out_settings Parsed settings
     * @return 0 on success, 1 on error
     */
    int parse_settings(
        const uint8_t* data,
        size_t len,
        HTTP3Settings& out_settings
    ) noexcept;
    
    /**
     * Parse HEADERS frame (uses QPACK decoder).
     * 
     * @param data Frame payload (QPACK-encoded headers)
     * @param len Payload length
     * @param out_headers Output headers
     * @param out_count Number of headers decoded
     * @return 0 on success, -1 on error
     */
    int parse_headers(
        const uint8_t* data,
        size_t len,
        std::pair<std::string, std::string>* out_headers,
        size_t& out_count
    ) noexcept {
        return qpack_decoder_.decode_field_section(data, len, out_headers, out_count);
    }
    
    /**
     * Reset parser state.
     */
    void reset() noexcept;
    
    /**
     * Get QPACK decoder reference.
     */
    qpack::QPACKDecoder& qpack_decoder() noexcept { return qpack_decoder_; }

private:
    // Parser state
    uint64_t current_frame_type_{0};
    uint64_t current_frame_length_{0};
    bool in_frame_{false};
    
    // QPACK decoder for HEADERS frames
    qpack::QPACKDecoder qpack_decoder_;
};

} // namespace http
} // namespace fasterapi
