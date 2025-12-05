/**
 * Parameter Extractor Implementation
 */

#include "parameter_extractor.h"
#include "core/logger.h"
#include <algorithm>
#include <cctype>

namespace fasterapi {
namespace http {

// ============================================================================
// CompiledRoutePattern Implementation
// ============================================================================

CompiledRoutePattern::CompiledRoutePattern(const std::string& pattern)
    : pattern_(pattern), segment_count_(0) {
    compile();
}

void CompiledRoutePattern::compile() noexcept {
    // Split pattern into segments
    auto segments_views = ParameterExtractor::split_path(pattern_);

    for (size_t i = 0; i < segments_views.size(); ++i) {
        std::string segment(segments_views[i]);
        segments_.push_back(segment);

        // Check if this segment is a path parameter
        if (ParameterExtractor::is_path_param(segment)) {
            auto param_name = ParameterExtractor::extract_param_name(segment);
            if (!param_name.empty()) {
                params_.emplace_back(std::string(param_name), i);
            }
        }
    }

    segment_count_ = segments_.size();
}

bool CompiledRoutePattern::matches(std::string_view path) const noexcept {
    auto path_segments = ParameterExtractor::split_path(path);

    // Quick check: segment count must match
    if (path_segments.size() != segment_count_) {
        return false;
    }

    // Check each segment
    for (size_t i = 0; i < segment_count_; ++i) {
        const auto& pattern_segment = segments_[i];

        // Parameter placeholders match anything
        if (ParameterExtractor::is_path_param(pattern_segment)) {
            continue;
        }

        // Static segments must match exactly
        if (pattern_segment != path_segments[i]) {
            return false;
        }
    }

    return true;
}

std::unordered_map<std::string, std::string> CompiledRoutePattern::extract(std::string_view path) const noexcept {
    std::unordered_map<std::string, std::string> result;

    auto path_segments = ParameterExtractor::split_path(path);

    // Check if path matches pattern
    if (!matches(path)) {
        return result;  // Empty map indicates no match
    }

    // Extract parameter values
    for (const auto& param : params_) {
        if (param.position < path_segments.size()) {
            result[param.name] = std::string(path_segments[param.position]);
        }
    }

    return result;
}

// ============================================================================
// ParameterExtractor Implementation
// ============================================================================

std::vector<std::string> ParameterExtractor::extract_path_params(const std::string& pattern) noexcept {
    std::vector<std::string> params;

    auto segments = split_path(pattern);
    for (const auto& segment : segments) {
        if (is_path_param(segment)) {
            auto param_name = extract_param_name(segment);
            if (!param_name.empty()) {
                params.emplace_back(param_name);
            }
        }
    }

    return params;
}

std::string ParameterExtractor::get_path_param(
    std::string_view path,
    const std::string& pattern,
    const std::string& param_name
) noexcept {
    CompiledRoutePattern compiled(pattern);
    auto params = compiled.extract(path);

    auto it = params.find(param_name);
    if (it != params.end()) {
        return it->second;
    }

    return "";
}

std::unordered_map<std::string, std::string> ParameterExtractor::get_query_params(
    std::string_view url
) noexcept {
    std::unordered_map<std::string, std::string> params;

    // Find query string start
    size_t query_start = url.find('?');
    if (query_start == std::string_view::npos) {
        return params;  // No query string
    }

    // Extract query string (skip the '?')
    std::string_view query = url.substr(query_start + 1);

    // Parse key=value pairs separated by '&'
    size_t pos = 0;
    while (pos < query.size()) {
        // Find next '&' or end of string
        size_t next_amp = query.find('&', pos);
        if (next_amp == std::string_view::npos) {
            next_amp = query.size();
        }

        // Extract key=value pair
        std::string_view pair = query.substr(pos, next_amp - pos);

        // Split on '='
        size_t eq_pos = pair.find('=');
        if (eq_pos != std::string_view::npos) {
            std::string_view key = pair.substr(0, eq_pos);
            std::string_view value = pair.substr(eq_pos + 1);

            // URL decode key and value
            std::string decoded_key = url_decode(key);
            std::string decoded_value = url_decode(value);

            params[decoded_key] = decoded_value;
        } else {
            // Key without value (e.g., "?flag")
            std::string decoded_key = url_decode(pair);
            params[decoded_key] = "";
        }

        pos = next_amp + 1;
    }

    return params;
}

std::string ParameterExtractor::url_decode(std::string_view encoded) noexcept {
    std::string decoded;
    decoded.reserve(encoded.size());  // Decoded string won't be larger

    for (size_t i = 0; i < encoded.size(); ++i) {
        char c = encoded[i];

        if (c == '%' && i + 2 < encoded.size()) {
            // Percent-encoded character
            int high = hex_to_int(encoded[i + 1]);
            int low = hex_to_int(encoded[i + 2]);

            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 2;  // Skip the two hex digits
            } else {
                // Invalid encoding, keep as-is
                decoded.push_back(c);
            }
        } else if (c == '+') {
            // Plus sign represents space in query strings
            decoded.push_back(' ');
        } else {
            decoded.push_back(c);
        }
    }

    return decoded;
}

std::vector<std::string_view> ParameterExtractor::split_path(std::string_view path) noexcept {
    std::vector<std::string_view> segments;

    size_t start = 0;
    size_t end = 0;

    // Skip leading slash
    if (!path.empty() && path[0] == '/') {
        start = 1;
    }

    while (start < path.size()) {
        // Find next '/' or end of string
        end = path.find('/', start);
        if (end == std::string_view::npos) {
            end = path.size();
        }

        // Extract segment
        if (end > start) {
            std::string_view segment = path.substr(start, end - start);
            if (!segment.empty()) {
                segments.push_back(segment);
            }
        }

        start = end + 1;
    }

    return segments;
}

bool ParameterExtractor::is_path_param(std::string_view segment) noexcept {
    if (segment.size() < 3) {
        return false;  // Minimum is "{x}"
    }

    return segment.front() == '{' && segment.back() == '}';
}

std::string_view ParameterExtractor::extract_param_name(std::string_view placeholder) noexcept {
    if (!is_path_param(placeholder)) {
        return "";
    }

    // Remove surrounding braces
    return placeholder.substr(1, placeholder.size() - 2);
}

int ParameterExtractor::hex_to_int(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    } else if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    } else if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

} // namespace http
} // namespace fasterapi
