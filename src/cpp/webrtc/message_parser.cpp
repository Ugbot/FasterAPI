#include "message_parser.h"
#include <sstream>
#include <cstring>

// Include simdjson only in .cpp to avoid header pollution
#include <simdjson.h>

namespace fasterapi {
namespace webrtc {

RTCMessageParser::RTCMessageParser() {
    // Create simdjson parser
    parser_ = new simdjson::ondemand::parser();
}

RTCMessageParser::~RTCMessageParser() {
    if (parser_) {
        delete static_cast<simdjson::ondemand::parser*>(parser_);
    }
}

int RTCMessageParser::parse(
    const char* json_data,
    size_t len,
    RTCMessage& out_message
) noexcept {
    if (!json_data || len == 0) {
        return 1;
    }
    
    auto* parser = static_cast<simdjson::ondemand::parser*>(parser_);
    
    // Parse with simdjson (zero-copy where possible)
    simdjson::padded_string padded(json_data, len);
    simdjson::ondemand::document doc;
    auto error = parser->iterate(padded).get(doc);
    
    if (error) {
        return 1;
    }
    
    // Parse message type
    std::string_view type_view;
    error = doc["type"].get_string().get(type_view);
    if (error) {
        return 1;
    }
    
    if (type_view == "offer") {
        out_message.type = RTCMessageType::OFFER;
        
        // Parse SDP
        std::string_view sdp_view;
        if (!doc["sdp"].get_string().get(sdp_view)) {
            out_message.sdp = std::string(sdp_view);
        }
        
    } else if (type_view == "answer") {
        out_message.type = RTCMessageType::ANSWER;
        
        // Parse SDP
        std::string_view sdp_view;
        if (!doc["sdp"].get_string().get(sdp_view)) {
            out_message.sdp = std::string(sdp_view);
        }
        
    } else if (type_view == "ice-candidate") {
        out_message.type = RTCMessageType::ICE_CANDIDATE;
        
        // Parse candidate (simplified for now)
        out_message.candidate = "{}";
        
    } else {
        out_message.type = RTCMessageType::UNKNOWN;
    }
    
    // Parse optional from/to fields
    std::string_view from_view;
    if (!doc["from"].get_string().get(from_view)) {
        out_message.from_peer = std::string(from_view);
    }
    
    std::string_view to_view;
    if (!doc["target"].get_string().get(to_view)) {
        out_message.to_peer = std::string(to_view);
    }
    
    return 0;
}

int RTCMessageParser::generate(
    const RTCMessage& message,
    std::string& out_json
) const noexcept {
    std::ostringstream oss;
    
    oss << "{";
    
    // Message type
    oss << R"("type":")";
    switch (message.type) {
        case RTCMessageType::OFFER:         oss << "offer"; break;
        case RTCMessageType::ANSWER:        oss << "answer"; break;
        case RTCMessageType::ICE_CANDIDATE: oss << "ice-candidate"; break;
        default:                             oss << "unknown"; break;
    }
    oss << "\"";
    
    // From/to peers
    if (!message.from_peer.empty()) {
        oss << R"(,"from":")" << message.from_peer << "\"";
    }
    
    if (!message.to_peer.empty()) {
        oss << R"(,"target":")" << message.to_peer << "\"";
    }
    
    // SDP (for offer/answer)
    if (!message.sdp.empty()) {
        oss << R"(,"sdp":")" << message.sdp << "\"";
    }
    
    // Candidate (for ICE)
    if (!message.candidate.empty()) {
        oss << R"(,"candidate":)" << message.candidate;
    }
    
    oss << "}";
    
    out_json = oss.str();
    return 0;
}

} // namespace webrtc
} // namespace fasterapi

