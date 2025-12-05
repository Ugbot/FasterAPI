#pragma once

#include <cstdint>

namespace fasterapi {
namespace python {

/**
 * Message types for IPC protocol.
 * Shared by both shared memory and ZeroMQ implementations.
 */
enum class MessageType : uint8_t {
    REQUEST = 1,
    RESPONSE = 2,
    SHUTDOWN = 3,

    // WebSocket events
    WS_CONNECT = 10,     // WebSocket connection opened
    WS_MESSAGE = 11,     // WebSocket message received
    WS_DISCONNECT = 12,  // WebSocket connection closed

    // WebSocket responses from Python
    WS_SEND = 20,        // Send message to client
    WS_CLOSE = 21        // Close connection
};

/**
 * Kwargs/body serialization format identifiers.
 * Used for format detection and backward compatibility.
 */
enum class PayloadFormat : uint8_t {
    FORMAT_JSON = 0,         // Legacy JSON format (default)
    FORMAT_BINARY_TLV = 1,   // Custom TLV binary format (~26x faster)
    FORMAT_MSGPACK = 2,      // MessagePack format (~5x faster)
};

/**
 * Request message header.
 * Binary-compatible format for IPC communication.
 *
 * Note: For backward compatibility, kwargs_format defaults to 0 (JSON).
 * The Python side should check for binary format magic byte 0xFA first.
 */
struct MessageHeader {
    MessageType type;
    uint32_t request_id;
    uint32_t total_length;      // Total message size (header + payload)
    uint32_t module_name_len;
    uint32_t function_name_len;
    uint32_t kwargs_len;        // Renamed from kwargs_json_len (format-agnostic)
    PayloadFormat kwargs_format; // NEW: 0=JSON, 1=BINARY_TLV, 2=MSGPACK
} __attribute__((packed));

/**
 * Response message header.
 * Binary-compatible format for IPC communication.
 *
 * Note: For backward compatibility, body_format defaults to 0 (JSON).
 * The C++ side should check for binary format magic byte 0xFA first.
 */
struct ResponseHeader {
    MessageType type;
    uint32_t request_id;
    uint32_t total_length;      // Total message size (header + payload)
    uint16_t status_code;       // HTTP-style status code (200, 500, etc.)
    uint32_t body_len;          // Renamed from body_json_len (format-agnostic)
    uint32_t error_message_len;
    uint8_t success;            // 1 = success, 0 = error
    PayloadFormat body_format;  // NEW: 0=JSON, 1=BINARY_TLV, 2=MSGPACK
} __attribute__((packed));

/**
 * WebSocket message header.
 * Used for WS_CONNECT, WS_MESSAGE, WS_DISCONNECT events.
 */
struct WebSocketMessageHeader {
    MessageType type;
    uint64_t connection_id;     // Unique WebSocket connection ID
    uint32_t total_length;      // Total message size (header + payload)
    uint32_t path_len;          // Length of path string
    uint32_t payload_len;       // Length of message payload
    uint8_t is_binary;          // 1 = binary, 0 = text
} __attribute__((packed));

/**
 * WebSocket response header.
 * Used for WS_SEND, WS_CLOSE responses from Python.
 */
struct WebSocketResponseHeader {
    MessageType type;
    uint64_t connection_id;     // Target WebSocket connection ID
    uint32_t total_length;      // Total message size (header + payload)
    uint32_t payload_len;       // Length of message payload
    uint16_t close_code;        // Close code (for WS_CLOSE)
    uint8_t is_binary;          // 1 = binary, 0 = text
} __attribute__((packed));

}  // namespace python
}  // namespace fasterapi
