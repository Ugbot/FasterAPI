#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace fasterapi {
namespace webrtc {

/**
 * SDP (Session Description Protocol) parser.
 * 
 * Parses SDP offers/answers for WebRTC signaling.
 * 
 * SDP Format (RFC 4566):
 *   v=0
 *   o=- 123456 123456 IN IP4 127.0.0.1
 *   s=-
 *   t=0 0
 *   m=audio 9 UDP/TLS/RTP/SAVPF 111
 *   a=rtpmap:111 opus/48000/2
 *   ...
 * 
 * Zero-allocation, zero-copy parsing using string_view.
 */

/**
 * SDP media description.
 */
struct SDPMedia {
    std::string_view media_type;     // audio, video, application
    uint16_t port;
    std::string_view protocol;       // UDP/TLS/RTP/SAVPF, etc.
    std::vector<std::string_view> formats;  // RTP payload types
    
    // Attributes (a= lines)
    std::unordered_map<std::string, std::string> attributes;
};

/**
 * Parsed SDP session.
 */
struct SDPSession {
    // Session-level fields
    std::string_view version;       // v=
    std::string_view origin;        // o=
    std::string_view session_name;  // s=
    std::string_view connection;    // c=
    std::string_view timing;        // t=
    
    // Media descriptions (m= lines)
    std::vector<SDPMedia> media;
    
    // Session-level attributes
    std::unordered_map<std::string, std::string> attributes;
    
    /**
     * Get attribute value.
     */
    std::string get_attribute(const std::string& key) const;
    
    /**
     * Check if attribute exists.
     */
    bool has_attribute(const std::string& key) const;
};

/**
 * SDP parser.
 * 
 * Parses SDP text into structured format.
 */
class SDPParser {
public:
    /**
     * Parse SDP from text.
     * 
     * @param sdp_text SDP text (must remain valid during session use)
     * @param out_session Parsed session (views into sdp_text)
     * @return 0 on success, error code otherwise
     */
    int parse(
        std::string_view sdp_text,
        SDPSession& out_session
    ) noexcept;
    
    /**
     * Generate SDP text from session.
     * 
     * @param session Session to serialize
     * @param out_sdp Generated SDP text
     * @return 0 on success
     */
    int generate(
        const SDPSession& session,
        std::string& out_sdp
    ) const noexcept;
    
private:
    /**
     * Parse a single SDP line.
     */
    int parse_line(
        std::string_view line,
        SDPSession& session,
        SDPMedia* current_media
    ) noexcept;
    
    /**
     * Trim whitespace from string.
     */
    static std::string_view trim(std::string_view str) noexcept;
};

} // namespace webrtc
} // namespace fasterapi

