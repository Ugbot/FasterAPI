#pragma once

#include <string>
#include <string_view>
#include <cstdint>

namespace fasterapi {
namespace webrtc {

/**
 * WebRTC signaling message parser using simdjson.
 * 
 * Parses JSON signaling messages with zero-copy where possible.
 * Uses simdjson for SIMD-accelerated parsing.
 * 
 * Message types:
 * - offer: {"type":"offer", "sdp":"..."}
 * - answer: {"type":"answer", "sdp":"..."}
 * - ice-candidate: {"type":"ice-candidate", "candidate":{...}}
 * 
 * Performance target: <500ns per message parse
 */

/**
 * WebRTC message type.
 */
enum class RTCMessageType : uint8_t {
    OFFER,
    ANSWER,
    ICE_CANDIDATE,
    UNKNOWN
};

/**
 * Parsed WebRTC signaling message.
 */
struct RTCMessage {
    RTCMessageType type;
    std::string from_peer;
    std::string to_peer;
    std::string sdp;           // For offer/answer
    std::string candidate;     // For ICE candidate (JSON)
    
    RTCMessage() : type(RTCMessageType::UNKNOWN) {}
};

/**
 * WebRTC message parser using simdjson.
 */
class RTCMessageParser {
public:
    RTCMessageParser();
    ~RTCMessageParser();
    
    /**
     * Parse WebRTC signaling message.
     * 
     * Uses simdjson for fast, zero-copy parsing.
     * 
     * @param json_data JSON message data
     * @param len Data length
     * @param out_message Parsed message
     * @return 0 on success, error code otherwise
     */
    int parse(
        const char* json_data,
        size_t len,
        RTCMessage& out_message
    ) noexcept;
    
    /**
     * Generate JSON message.
     * 
     * @param message Message to serialize
     * @param out_json Output JSON string
     * @return 0 on success
     */
    int generate(
        const RTCMessage& message,
        std::string& out_json
    ) const noexcept;
    
private:
    // simdjson parser (opaque pointer to avoid header dependency)
    void* parser_;
};

} // namespace webrtc
} // namespace fasterapi

