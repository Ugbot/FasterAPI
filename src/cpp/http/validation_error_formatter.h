/**
 * Validation Error Formatter
 *
 * Formats validation errors as FastAPI-compatible 422 responses.
 * Uses C++ string building for maximum performance.
 *
 * Features:
 * - FastAPI-compatible JSON error format
 * - Efficient JSON generation (no library overhead)
 * - Location path formatting (["body", "field", "nested"])
 * - Standard HTTP 422 status code
 * - Content-Type: application/json headers
 *
 * Performance: < 1μs to format typical validation errors
 */

#pragma once

#include "schema_validator.h"
#include <string>

namespace fasterapi {
namespace http {

/**
 * Format validation errors as FastAPI-compatible JSON.
 *
 * Converts ValidationResult into a JSON string matching FastAPI's format:
 * {
 *   "detail": [
 *     {
 *       "loc": ["body", "field"],
 *       "msg": "error message",
 *       "type": "error_type"
 *     }
 *   ]
 * }
 */
class ValidationErrorFormatter {
public:
    /**
     * Format validation errors as JSON string.
     *
     * @param result ValidationResult containing errors
     * @return JSON string with error details
     */
    static std::string format_as_json(const ValidationResult& result) noexcept;

    /**
     * Format validation errors as complete HTTP 422 response.
     *
     * Includes:
     * - HTTP/1.1 422 Unprocessable Entity status line
     * - Content-Type: application/json header
     * - Content-Length header
     * - Empty line
     * - JSON body
     *
     * @param result ValidationResult containing errors
     * @return Complete HTTP response string
     */
    static std::string format_as_http_response(const ValidationResult& result) noexcept;

    /**
     * Format a single validation error as JSON object.
     *
     * Internal helper for formatting individual errors.
     *
     * @param error ValidationError to format
     * @return JSON object string (without surrounding braces)
     */
    static std::string format_single_error(const ValidationError& error) noexcept;

    /**
     * Format location array as JSON.
     *
     * Examples:
     * - ["body"] → ["body"]
     * - ["body", "user", "age"] → ["body","user","age"]
     *
     * @param loc Location vector
     * @return JSON array string
     */
    static std::string format_location(const std::vector<std::string>& loc) noexcept;

    /**
     * Escape string for JSON.
     *
     * Escapes special characters:
     * - " → \"
     * - \ → \\
     * - newline → \n
     * - etc.
     *
     * @param str String to escape
     * @return Escaped string
     */
    static std::string escape_json_string(const std::string& str) noexcept;
};

} // namespace http
} // namespace fasterapi
