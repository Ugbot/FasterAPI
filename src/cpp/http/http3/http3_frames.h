#pragma once

/**
 * @file http3_frames.h
 * @brief HTTP/3 Frame Definitions (RFC 9114)
 *
 * HTTP/3 frames are encoded as:
 *   Type (varint)
 *   Length (varint)
 *   Frame Payload
 */

#include <cstdint>
#include <cstddef>
#include <cstring>
#include "../quic/quic_varint.h"

namespace fasterapi {
namespace http3 {

/**
 * HTTP/3 frame types (RFC 9114 Section 7).
 */
enum class FrameType : uint64_t {
    DATA = 0x00,              // Request/response body
    HEADERS = 0x01,           // QPACK-encoded headers
    CANCEL_PUSH = 0x03,       // Server push cancellation (deprecated in final spec)
    SETTINGS = 0x04,          // Connection settings
    PUSH_PROMISE = 0x05,      // Server push promise (deprecated in final spec)
    GOAWAY = 0x07,            // Graceful shutdown
    MAX_PUSH_ID = 0x0D,       // Maximum Push ID (deprecated in final spec)

    // WebTransport extensions (RFC 9297)
    WEBTRANSPORT_STREAM = 0x41,
};

/**
 * HTTP/3 error codes (RFC 9114 Section 8).
 */
enum class ErrorCode : uint64_t {
    NO_ERROR = 0x100,              // Graceful shutdown
    GENERAL_PROTOCOL_ERROR = 0x101,
    INTERNAL_ERROR = 0x102,
    STREAM_CREATION_ERROR = 0x103,
    CLOSED_CRITICAL_STREAM = 0x104,
    FRAME_UNEXPECTED = 0x105,
    FRAME_ERROR = 0x106,
    EXCESSIVE_LOAD = 0x107,
    ID_ERROR = 0x108,
    SETTINGS_ERROR = 0x109,
    MISSING_SETTINGS = 0x10A,
    REQUEST_REJECTED = 0x10B,
    REQUEST_CANCELLED = 0x10C,
    REQUEST_INCOMPLETE = 0x10D,
    MESSAGE_ERROR = 0x10E,
    CONNECT_ERROR = 0x10F,
    VERSION_FALLBACK = 0x110,

    // QPACK error codes (RFC 9204)
    QPACK_DECOMPRESSION_FAILED = 0x200,
    QPACK_ENCODER_STREAM_ERROR = 0x201,
    QPACK_DECODER_STREAM_ERROR = 0x202,
};

/**
 * HTTP/3 SETTINGS identifiers (RFC 9114 Section 7.2.4).
 */
enum class SettingsId : uint64_t {
    QPACK_MAX_TABLE_CAPACITY = 0x01,
    MAX_FIELD_SECTION_SIZE = 0x06,
    QPACK_BLOCKED_STREAMS = 0x07,

    // WebTransport extension
    ENABLE_WEBTRANSPORT = 0x2b603742,
    H3_DATAGRAM = 0x33,
};

/**
 * HTTP/3 unidirectional stream types (RFC 9114 Section 6.2).
 */
enum class UniStreamType : uint64_t {
    CONTROL = 0x00,           // Control stream
    PUSH = 0x01,              // Server push stream (deprecated)
    QPACK_ENCODER = 0x02,     // QPACK encoder stream
    QPACK_DECODER = 0x03,     // QPACK decoder stream

    // WebTransport extension
    WEBTRANSPORT = 0x41,
};

/**
 * HTTP/3 frame header.
 */
struct FrameHeader {
    uint64_t type;
    uint64_t length;

    /**
     * Parse frame header.
     *
     * @param data Input buffer
     * @param len Buffer length
     * @param out_consumed Bytes consumed
     * @return 0 on success, -1 if need more data
     */
    int parse(const uint8_t* data, size_t len, size_t& out_consumed) noexcept {
        size_t pos = 0;

        // Frame type
        int consumed = quic::VarInt::decode(data + pos, len - pos, type);
        if (consumed < 0) return -1;
        pos += consumed;

        // Frame length
        consumed = quic::VarInt::decode(data + pos, len - pos, length);
        if (consumed < 0) return -1;
        pos += consumed;

        out_consumed = pos;
        return 0;
    }

    /**
     * Serialize frame header.
     *
     * @param out Output buffer
     * @return Number of bytes written
     */
    size_t serialize(uint8_t* out) const noexcept {
        size_t pos = 0;
        pos += quic::VarInt::encode(type, out + pos);
        pos += quic::VarInt::encode(length, out + pos);
        return pos;
    }
};

/**
 * HTTP/3 DATA frame (RFC 9114 Section 7.2.1).
 *
 * Conveys arbitrary variable-length data associated with a request or response.
 */
struct DataFrame {
    uint64_t length;
    const uint8_t* data;

    int parse(const uint8_t* data_buf, size_t len, size_t& out_consumed) noexcept {
        FrameHeader header;
        size_t header_len;

        if (header.parse(data_buf, len, header_len) != 0) {
            return -1;
        }

        if (header.type != static_cast<uint64_t>(FrameType::DATA)) {
            return 1;  // Wrong frame type
        }

        if (len < header_len + header.length) {
            return -1;  // Need more data
        }

        length = header.length;
        data = data_buf + header_len;
        out_consumed = header_len + length;
        return 0;
    }

    size_t serialize(uint8_t* out) const noexcept {
        FrameHeader header{static_cast<uint64_t>(FrameType::DATA), length};
        size_t pos = header.serialize(out);
        std::memcpy(out + pos, data, length);
        return pos + length;
    }
};

/**
 * HTTP/3 HEADERS frame (RFC 9114 Section 7.2.2).
 *
 * Carries QPACK-compressed HTTP header fields.
 */
struct HeadersFrame {
    uint64_t length;
    const uint8_t* encoded_headers;  // QPACK-encoded

    int parse(const uint8_t* data_buf, size_t len, size_t& out_consumed) noexcept {
        FrameHeader header;
        size_t header_len;

        if (header.parse(data_buf, len, header_len) != 0) {
            return -1;
        }

        if (header.type != static_cast<uint64_t>(FrameType::HEADERS)) {
            return 1;
        }

        if (len < header_len + header.length) {
            return -1;
        }

        length = header.length;
        encoded_headers = data_buf + header_len;
        out_consumed = header_len + length;
        return 0;
    }

    size_t serialize(uint8_t* out) const noexcept {
        FrameHeader header{static_cast<uint64_t>(FrameType::HEADERS), length};
        size_t pos = header.serialize(out);
        std::memcpy(out + pos, encoded_headers, length);
        return pos + length;
    }
};

/**
 * HTTP/3 SETTINGS frame (RFC 9114 Section 7.2.4).
 */
struct SettingsFrame {
    static constexpr size_t MAX_SETTINGS = 16;

    struct Setting {
        uint64_t id;
        uint64_t value;
    };

    Setting settings[MAX_SETTINGS];
    size_t count;

    int parse(const uint8_t* data_buf, size_t len, size_t& out_consumed) noexcept {
        FrameHeader header;
        size_t header_len;

        if (header.parse(data_buf, len, header_len) != 0) {
            return -1;
        }

        if (header.type != static_cast<uint64_t>(FrameType::SETTINGS)) {
            return 1;
        }

        if (len < header_len + header.length) {
            return -1;
        }

        // Parse settings
        count = 0;
        size_t pos = header_len;
        size_t end = header_len + header.length;

        while (pos < end && count < MAX_SETTINGS) {
            int consumed = quic::VarInt::decode(data_buf + pos, end - pos, settings[count].id);
            if (consumed < 0) return -1;
            pos += consumed;

            consumed = quic::VarInt::decode(data_buf + pos, end - pos, settings[count].value);
            if (consumed < 0) return -1;
            pos += consumed;

            count++;
        }

        out_consumed = end;
        return 0;
    }

    size_t serialize(uint8_t* out) const noexcept {
        // First calculate payload size
        uint8_t payload[256];
        size_t payload_len = 0;

        for (size_t i = 0; i < count; i++) {
            payload_len += quic::VarInt::encode(settings[i].id, payload + payload_len);
            payload_len += quic::VarInt::encode(settings[i].value, payload + payload_len);
        }

        // Write header
        FrameHeader header{static_cast<uint64_t>(FrameType::SETTINGS), payload_len};
        size_t pos = header.serialize(out);

        // Write payload
        std::memcpy(out + pos, payload, payload_len);
        return pos + payload_len;
    }
};

/**
 * HTTP/3 GOAWAY frame (RFC 9114 Section 7.2.6).
 */
struct GoawayFrame {
    uint64_t stream_id;  // Last client-initiated bidirectional stream ID

    int parse(const uint8_t* data_buf, size_t len, size_t& out_consumed) noexcept {
        FrameHeader header;
        size_t header_len;

        if (header.parse(data_buf, len, header_len) != 0) {
            return -1;
        }

        if (header.type != static_cast<uint64_t>(FrameType::GOAWAY)) {
            return 1;
        }

        if (len < header_len + header.length) {
            return -1;
        }

        int consumed = quic::VarInt::decode(data_buf + header_len, header.length, stream_id);
        if (consumed < 0) return -1;

        out_consumed = header_len + header.length;
        return 0;
    }

    size_t serialize(uint8_t* out) const noexcept {
        uint8_t payload[8];
        size_t payload_len = quic::VarInt::encode(stream_id, payload);

        FrameHeader header{static_cast<uint64_t>(FrameType::GOAWAY), payload_len};
        size_t pos = header.serialize(out);
        std::memcpy(out + pos, payload, payload_len);
        return pos + payload_len;
    }
};

/**
 * Utility: Append varint to buffer.
 */
inline void append_varint(uint8_t*& buf, uint64_t value) {
    buf += quic::VarInt::encode(value, buf);
}

} // namespace http3
} // namespace fasterapi
