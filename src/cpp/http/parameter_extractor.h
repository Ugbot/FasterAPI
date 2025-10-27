/**
 * Parameter Extractor
 *
 * High-performance parameter extraction for FastAPI-compatible routing.
 * Extracts path parameters, query parameters, and parses request bodies.
 *
 * Features:
 * - Zero-copy string_view operations where possible
 * - Pre-compiled route patterns for fast matching
 * - URL decoding with minimal allocations
 * - Query parameter parsing
 *
 * Performance targets:
 * - Path param extraction: < 100ns per parameter
 * - Query param parsing: < 500ns for typical requests
 * - URL decoding: Zero allocation for ASCII strings
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <optional>

namespace fasterapi {
namespace http {

/**
 * Represents a path parameter extracted from a route pattern.
 *
 * Example: "/items/{item_id}/details" → PathParam{name="item_id", position=1}
 */
struct PathParam {
    std::string name;        // Parameter name (e.g., "item_id")
    size_t position;         // Position in path segments (0-indexed)

    PathParam() = default;
    PathParam(std::string n, size_t pos) : name(std::move(n)), position(pos) {}
};

/**
 * Compiled route pattern for fast parameter extraction.
 *
 * Pre-processes route patterns at registration time to enable
 * fast parameter extraction during request handling.
 */
class CompiledRoutePattern {
public:
    CompiledRoutePattern() = default;
    explicit CompiledRoutePattern(const std::string& pattern);

    /**
     * Extract parameter values from a request path.
     *
     * @param path Request path (e.g., "/items/123/details")
     * @return Map of parameter name to value, or empty if path doesn't match
     *
     * Example:
     *   pattern = "/items/{item_id}/details"
     *   path = "/items/123/details"
     *   returns: {"item_id": "123"}
     */
    std::unordered_map<std::string, std::string> extract(std::string_view path) const noexcept;

    /**
     * Check if a path matches this pattern (without extracting values).
     */
    bool matches(std::string_view path) const noexcept;

    /**
     * Get parameter definitions.
     */
    const std::vector<PathParam>& get_params() const noexcept { return params_; }

    /**
     * Get the original pattern string.
     */
    const std::string& get_pattern() const noexcept { return pattern_; }

private:
    std::string pattern_;                 // Original pattern
    std::vector<std::string> segments_;   // Path segments (split by '/')
    std::vector<PathParam> params_;       // Parameter metadata
    size_t segment_count_;                // Number of segments (for fast validation)

    void compile() noexcept;
};

/**
 * High-performance parameter extractor.
 *
 * Provides zero-copy parameter extraction from HTTP requests.
 */
class ParameterExtractor {
public:
    /**
     * Extract path parameters from URL pattern.
     *
     * @param pattern Route pattern (e.g., "/items/{item_id}")
     * @return Vector of parameter names in order
     *
     * Example:
     *   extract_path_params("/users/{user_id}/posts/{post_id}")
     *   → ["user_id", "post_id"]
     */
    static std::vector<std::string> extract_path_params(const std::string& pattern) noexcept;

    /**
     * Extract a single path parameter value from a request path.
     *
     * @param path Request path
     * @param pattern Route pattern
     * @param param_name Parameter name to extract
     * @return Parameter value, or empty string if not found
     *
     * Example:
     *   get_path_param("/items/123", "/items/{item_id}", "item_id") → "123"
     */
    static std::string get_path_param(
        std::string_view path,
        const std::string& pattern,
        const std::string& param_name
    ) noexcept;

    /**
     * Extract all query parameters from URL.
     *
     * @param url Full URL or query string
     * @return Map of query parameter name to value
     *
     * Example:
     *   get_query_params("?q=search&limit=10")
     *   → {"q": "search", "limit": "10"}
     *
     * Handles:
     * - URL decoding (%20 → space, etc.)
     * - Multiple values for same key (last value wins)
     * - Empty values (key without =)
     */
    static std::unordered_map<std::string, std::string> get_query_params(
        std::string_view url
    ) noexcept;

    /**
     * Decode URL-encoded string.
     *
     * @param encoded URL-encoded string
     * @return Decoded string
     *
     * Handles:
     * - Percent encoding (%XX)
     * - Plus to space (+ → space)
     *
     * Performance: Zero allocation if string contains no encoded characters.
     */
    static std::string url_decode(std::string_view encoded) noexcept;

    /**
     * Split path into segments.
     *
     * @param path Path string
     * @return Vector of path segments (empty segments removed)
     *
     * Example:
     *   split_path("/items/123/details") → ["items", "123", "details"]
     */
    static std::vector<std::string_view> split_path(std::string_view path) noexcept;

    /**
     * Check if a string is a path parameter placeholder.
     *
     * @param segment Path segment
     * @return true if segment is "{param_name}" format
     */
    static bool is_path_param(std::string_view segment) noexcept;

    /**
     * Extract parameter name from placeholder.
     *
     * @param placeholder Placeholder string ("{param_name}")
     * @return Parameter name ("param_name"), or empty if invalid
     */
    static std::string_view extract_param_name(std::string_view placeholder) noexcept;

private:
    /**
     * Convert hex digit to integer.
     *
     * @param c Hex character ('0'-'9', 'A'-'F', 'a'-'f')
     * @return Integer value, or -1 if invalid
     */
    static int hex_to_int(char c) noexcept;
};

} // namespace http
} // namespace fasterapi
