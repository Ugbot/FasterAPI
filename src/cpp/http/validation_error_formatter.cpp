/**
 * Validation Error Formatter Implementation
 */

#include "validation_error_formatter.h"
#include <sstream>

namespace fasterapi {
namespace http {

std::string ValidationErrorFormatter::escape_json_string(const std::string& str) noexcept {
    std::string result;
    result.reserve(str.size() + 10);  // Pre-allocate for common case

    for (char c : str) {
        switch (c) {
            case '"':
                result += "\\\"";
                break;
            case '\\':
                result += "\\\\";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            case '\b':
                result += "\\b";
                break;
            case '\f':
                result += "\\f";
                break;
            default:
                if (c < 32) {
                    // Escape control characters
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    result += buf;
                } else {
                    result += c;
                }
                break;
        }
    }

    return result;
}

std::string ValidationErrorFormatter::format_location(
    const std::vector<std::string>& loc
) noexcept {
    std::string result = "[";

    for (size_t i = 0; i < loc.size(); ++i) {
        if (i > 0) {
            result += ",";
        }
        result += "\"";
        result += escape_json_string(loc[i]);
        result += "\"";
    }

    result += "]";
    return result;
}

std::string ValidationErrorFormatter::format_single_error(
    const ValidationError& error
) noexcept {
    std::string result = "{";

    // "loc": [...]
    result += "\"loc\":";
    result += format_location(error.loc);
    result += ",";

    // "msg": "..."
    result += "\"msg\":\"";
    result += escape_json_string(error.msg);
    result += "\",";

    // "type": "..."
    result += "\"type\":\"";
    result += escape_json_string(error.type);
    result += "\"";

    result += "}";
    return result;
}

std::string ValidationErrorFormatter::format_as_json(
    const ValidationResult& result
) noexcept {
    if (result.valid) {
        // No errors - return empty detail array
        return "{\"detail\":[]}";
    }

    std::string json = "{\"detail\":[";

    for (size_t i = 0; i < result.errors.size(); ++i) {
        if (i > 0) {
            json += ",";
        }
        json += format_single_error(result.errors[i]);
    }

    json += "]}";
    return json;
}

std::string ValidationErrorFormatter::format_as_http_response(
    const ValidationResult& result
) noexcept {
    // Generate JSON body
    std::string json_body = format_as_json(result);

    // Build HTTP response
    std::ostringstream response;
    response << "HTTP/1.1 422 Unprocessable Entity\r\n";
    response << "Content-Type: application/json\r\n";
    response << "Content-Length: " << json_body.size() << "\r\n";
    response << "\r\n";
    response << json_body;

    return response.str();
}

} // namespace http
} // namespace fasterapi
