#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <array>
#include <vector>
#include <utility>

namespace fasterapi {
namespace http {

/**
 * Zero-allocation HTTP/3 (QUIC) frame parser.
 * 
 * HTTP/3 uses QUIC as transport and QPACK for header compression.
 * 
 * Based on:
 * - HTTP/3 Spec: RFC 9114
 * - QPACK Spec: RFC 9204
 * - Algorithm concepts from MsQuic and nghttp3
 * 
 * Adapted for:
 * - Zero allocations (stack only)
 * - Zero-copy (string_view)
 * - No callbacks
 * - Inline hot paths
 * 
 * Performance targets:
 * - Parse frame: <100ns
 * - QPACK decode: <500ns per header
 * - Zero allocations
 */

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
    MAX_PUSH_ID = 0x0d,
    // QPACK frames
    QPACK_ENCODER = 0x02,
    QPACK_DECODER = 0x03,
};

/**
 * HTTP/3 frame header.
 */
struct HTTP3FrameHeader {
    HTTP3FrameType type;
    uint64_t length;
    
    // Frame payload follows
};

/**
 * HTTP/3 SETTINGS frame parameters.
 */
struct HTTP3Settings {
    uint64_t max_header_list_size{0};
    uint64_t qpack_max_table_capacity{0};
    uint64_t qpack_blocked_streams{0};
    
    // Additional settings as needed
};

/**
 * HTTP/3 frame parser.
 * 
 * Parses HTTP/3 frames from QUIC stream data.
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
     * Parse variable-length integer (RFC 9000 Section 16).
     * 
     * QUIC uses variable-length integers for efficiency.
     * 
     * @param data Input buffer
     * @param len Buffer length
     * @param out_value Decoded integer
     * @param out_consumed Bytes consumed
     * @return 0 on success, -1 if need more data
     */
    static int parse_varint(
        const uint8_t* data,
        size_t len,
        uint64_t& out_value,
        size_t& out_consumed
    ) noexcept;
    
    /**
     * Reset parser state.
     */
    void reset() noexcept;
    
private:
    // Parser state
    uint64_t current_frame_type_{0};
    uint64_t current_frame_length_{0};
    bool in_frame_{false};
};

/**
 * QPACK decoder (simplified).
 * 
 * QPACK is similar to HPACK but designed for QUIC's
 * unordered stream delivery.
 * 
 * For now, we implement a simplified version.
 * Full QPACK is complex due to dynamic table updates
 * on separate streams.
 */
class QPACKDecoder {
public:
    QPACKDecoder(size_t max_table_size = 4096);
    
    /**
     * Decode QPACK-encoded headers.
     * 
     * @param data QPACK-encoded data
     * @param len Data length
     * @param out_headers Output headers
     * @param max_headers Max headers to decode
     * @return 0 on success, error code otherwise
     */
    int decode(
        const uint8_t* data,
        size_t len,
        std::vector<std::pair<std::string_view, std::string_view>>& out_headers,
        size_t max_headers = 100
    ) noexcept;
    
private:
    size_t max_table_size_;
    
    // QPACK uses a dynamic table similar to HPACK
    // For simplicity, we use a basic implementation
};

} // namespace http
} // namespace fasterapi

