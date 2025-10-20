#include "sdp_parser.h"
#include <sstream>
#include <algorithm>

namespace fasterapi {
namespace webrtc {

// ============================================================================
// SDPSession Implementation
// ============================================================================

std::string SDPSession::get_attribute(const std::string& key) const {
    auto it = attributes.find(key);
    if (it != attributes.end()) {
        return it->second;
    }
    return "";
}

bool SDPSession::has_attribute(const std::string& key) const {
    return attributes.find(key) != attributes.end();
}

// ============================================================================
// SDPParser Implementation
// ============================================================================

int SDPParser::parse(
    std::string_view sdp_text,
    SDPSession& out_session
) noexcept {
    if (sdp_text.empty()) {
        return 1;
    }
    
    SDPMedia* current_media = nullptr;
    
    // Parse line by line
    size_t pos = 0;
    size_t line_start = 0;
    
    while (pos <= sdp_text.length()) {
        // Find end of line
        if (pos == sdp_text.length() || sdp_text[pos] == '\n') {
            // Extract line
            size_t line_end = pos;
            if (line_end > line_start && sdp_text[line_end - 1] == '\r') {
                line_end--;
            }
            
            std::string_view line = sdp_text.substr(line_start, line_end - line_start);
            
            // Parse line
            if (!line.empty()) {
                if (parse_line(line, out_session, current_media) != 0) {
                    return 1;
                }
                
                // Check if we started a new media section
                if (line[0] == 'm' && line[1] == '=') {
                    if (!out_session.media.empty()) {
                        current_media = &out_session.media.back();
                    }
                }
            }
            
            line_start = pos + 1;
        }
        
        pos++;
    }
    
    return 0;
}

int SDPParser::parse_line(
    std::string_view line,
    SDPSession& session,
    SDPMedia* current_media
) noexcept {
    if (line.length() < 2 || line[1] != '=') {
        return 1;  // Invalid format
    }
    
    char type = line[0];
    std::string_view value = trim(line.substr(2));
    
    switch (type) {
        case 'v':  // Version
            session.version = value;
            break;
        
        case 'o':  // Origin
            session.origin = value;
            break;
        
        case 's':  // Session name
            session.session_name = value;
            break;
        
        case 'c':  // Connection
            session.connection = value;
            break;
        
        case 't':  // Timing
            session.timing = value;
            break;
        
        case 'm':  // Media
        {
            // Parse media line: m=<type> <port> <proto> <fmt> ...
            SDPMedia media;
            
            size_t space1 = value.find(' ');
            if (space1 == std::string_view::npos) return 1;
            
            media.media_type = trim(value.substr(0, space1));
            
            size_t space2 = value.find(' ', space1 + 1);
            if (space2 == std::string_view::npos) return 1;
            
            // Parse port
            std::string port_str(trim(value.substr(space1 + 1, space2 - space1 - 1)));
            media.port = std::stoi(port_str);
            
            size_t space3 = value.find(' ', space2 + 1);
            if (space3 == std::string_view::npos) {
                media.protocol = trim(value.substr(space2 + 1));
            } else {
                media.protocol = trim(value.substr(space2 + 1, space3 - space2 - 1));
                
                // Parse formats
                std::string_view formats = trim(value.substr(space3 + 1));
                size_t fmt_pos = 0;
                while (fmt_pos < formats.length()) {
                    size_t next_space = formats.find(' ', fmt_pos);
                    if (next_space == std::string_view::npos) {
                        media.formats.push_back(trim(formats.substr(fmt_pos)));
                        break;
                    } else {
                        media.formats.push_back(trim(formats.substr(fmt_pos, next_space - fmt_pos)));
                        fmt_pos = next_space + 1;
                    }
                }
            }
            
            session.media.push_back(media);
            break;
        }
        
        case 'a':  // Attribute
        {
            // Parse attribute: a=<name>:<value> or a=<flag>
            size_t colon = value.find(':');
            
            if (colon == std::string_view::npos) {
                // Flag attribute (no value)
                if (current_media) {
                    current_media->attributes[std::string(value)] = "";
                } else {
                    session.attributes[std::string(value)] = "";
                }
            } else {
                // Name:value attribute
                std::string name(trim(value.substr(0, colon)));
                std::string attr_value(trim(value.substr(colon + 1)));
                
                if (current_media) {
                    current_media->attributes[name] = attr_value;
                } else {
                    session.attributes[name] = attr_value;
                }
            }
            break;
        }
        
        default:
            // Unknown line type, skip
            break;
    }
    
    return 0;
}

int SDPParser::generate(
    const SDPSession& session,
    std::string& out_sdp
) const noexcept {
    std::ostringstream oss;
    
    // Session-level fields
    oss << "v=" << session.version << "\r\n";
    oss << "o=" << session.origin << "\r\n";
    oss << "s=" << session.session_name << "\r\n";
    if (!session.connection.empty()) {
        oss << "c=" << session.connection << "\r\n";
    }
    oss << "t=" << session.timing << "\r\n";
    
    // Session attributes
    for (const auto& [key, value] : session.attributes) {
        if (value.empty()) {
            oss << "a=" << key << "\r\n";
        } else {
            oss << "a=" << key << ":" << value << "\r\n";
        }
    }
    
    // Media descriptions
    for (const auto& media : session.media) {
        oss << "m=" << media.media_type << " " << media.port << " " << media.protocol;
        for (const auto& fmt : media.formats) {
            oss << " " << fmt;
        }
        oss << "\r\n";
        
        // Media attributes
        for (const auto& [key, value] : media.attributes) {
            if (value.empty()) {
                oss << "a=" << key << "\r\n";
            } else {
                oss << "a=" << key << ":" << value << "\r\n";
            }
        }
    }
    
    out_sdp = oss.str();
    return 0;
}

std::string_view SDPParser::trim(std::string_view str) noexcept {
    // Trim leading whitespace
    size_t start = 0;
    while (start < str.length() && (str[start] == ' ' || str[start] == '\t')) {
        start++;
    }
    
    // Trim trailing whitespace
    size_t end = str.length();
    while (end > start && (str[end - 1] == ' ' || str[end - 1] == '\t')) {
        end--;
    }
    
    return str.substr(start, end - start);
}

} // namespace webrtc
} // namespace fasterapi

