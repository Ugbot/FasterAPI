/**
 * HTTP Compression Middleware
 *
 * Automatic response compression based on Accept-Encoding header.
 * Supports gzip, brotli, and zstd algorithms.
 *
 * Features:
 * - Accept-Encoding parsing with quality values
 * - Content-Type based compression decisions
 * - Minimum size threshold (default: 256 bytes)
 * - Algorithm preference (brotli > zstd > gzip > deflate)
 */

#pragma once

#include "compression.h"
#include "http1_connection.h"
#include "response.h"
#include <string>
#include <string_view>
#include <map>
#include <unordered_map>
#include <optional>
#include <memory>
#include <functional>

namespace fasterapi {
namespace http {

/**
 * Compression middleware configuration
 */
struct CompressionConfig {
    bool enabled = true;                              // Master switch for compression
    size_t min_size = 256;                            // Minimum response size to compress
    CompressionLevel level = CompressionLevel::DEFAULT;  // Compression level

    // Algorithm preferences (in order of preference)
    // First algorithm in this list that client accepts will be used
    std::vector<CompressionAlgorithm> preferred_algorithms = {
        CompressionAlgorithm::BROTLI,  // Best compression ratio
        CompressionAlgorithm::ZSTD,    // Best speed/ratio balance
        CompressionAlgorithm::GZIP,    // Universal support
        CompressionAlgorithm::DEFLATE  // Fallback
    };

    // Content types to compress (partial match - "text/" matches "text/html")
    std::vector<std::string> compressible_types = {
        "text/",
        "application/json",
        "application/javascript",
        "application/xml",
        "application/xhtml+xml",
        "application/atom+xml",
        "application/rss+xml",
        "application/x-javascript",
        "image/svg+xml"
    };

    // Content types to never compress (even if matching above)
    std::vector<std::string> skip_types = {
        "image/jpeg",
        "image/png",
        "image/gif",
        "image/webp",
        "video/",
        "audio/",
        "application/gzip",
        "application/zip",
        "application/x-tar",
        "application/pdf"  // Already compressed
    };

    CompressionConfig() = default;
};

/**
 * Compression middleware
 *
 * Applies automatic compression to HTTP responses based on:
 * - Client's Accept-Encoding header
 * - Response Content-Type
 * - Response size
 *
 * Usage:
 *   CompressionMiddleware middleware;
 *   middleware.apply(request_headers, response);
 */
class CompressionMiddleware {
public:
    /**
     * Create middleware with default configuration
     */
    CompressionMiddleware() = default;

    /**
     * Create middleware with custom configuration
     */
    explicit CompressionMiddleware(const CompressionConfig& config)
        : config_(config) {}

    /**
     * Apply compression to response
     *
     * Modifies response in place:
     * - Compresses body if appropriate
     * - Adds Content-Encoding header
     * - Updates Content-Length (handled by connection layer)
     *
     * @param request_headers Request headers (needs Accept-Encoding)
     * @param response Response to potentially compress
     * @return true if compression was applied, false otherwise
     */
    bool apply(
        const std::unordered_map<std::string, std::string>& request_headers,
        Http1Response& response
    ) noexcept {
        if (!config_.enabled) {
            return false;
        }

        // Check if response already has Content-Encoding
        if (response.headers.count("Content-Encoding") > 0 ||
            response.headers.count("content-encoding") > 0) {
            return false;
        }

        // Check response size
        if (response.body.size() < config_.min_size) {
            return false;
        }

        // Get Content-Type from response
        std::string content_type;
        auto ct_it = response.headers.find("Content-Type");
        if (ct_it == response.headers.end()) {
            ct_it = response.headers.find("content-type");
        }
        if (ct_it != response.headers.end()) {
            content_type = ct_it->second;
        }

        // Check if content type is compressible
        if (!should_compress_type(content_type)) {
            return false;
        }

        // Get Accept-Encoding header
        std::string accept_encoding;
        auto ae_it = request_headers.find("Accept-Encoding");
        if (ae_it == request_headers.end()) {
            ae_it = request_headers.find("accept-encoding");
        }
        if (ae_it == request_headers.end()) {
            return false;  // Client doesn't accept compression
        }
        accept_encoding = ae_it->second;

        // Find best matching algorithm
        CompressionAlgorithm algo = select_algorithm(accept_encoding);
        if (algo == CompressionAlgorithm::NONE) {
            return false;  // No acceptable algorithm
        }

        // Compress the body
        auto result = compress(
            response.body.data(),
            response.body.size(),
            algo,
            config_.level
        );

        if (!result.success) {
            return false;  // Compression failed
        }

        // Only use compression if it actually reduced size
        if (result.compressed_size >= response.body.size()) {
            return false;  // Compression didn't help
        }

        // Update response body
        response.body.assign(result.data.begin(), result.data.end());

        // Add Content-Encoding header
        const char* encoding = encoding_header_value(algo);
        response.headers["Content-Encoding"] = encoding;

        // Add Vary header to indicate response varies by Accept-Encoding
        auto vary_it = response.headers.find("Vary");
        if (vary_it == response.headers.end()) {
            response.headers["Vary"] = "Accept-Encoding";
        } else if (vary_it->second.find("Accept-Encoding") == std::string::npos) {
            vary_it->second += ", Accept-Encoding";
        }

        return true;
    }

    /**
     * Apply compression to HttpResponse (high-level API).
     *
     * Works with the App-level HttpResponse type.
     *
     * @param request_headers Request headers (needs Accept-Encoding)
     * @param response HttpResponse to potentially compress
     * @return true if compression was applied, false otherwise
     */
    bool apply(
        const std::unordered_map<std::string, std::string>& request_headers,
        HttpResponse& response
    ) noexcept {
        return apply_impl(request_headers, response);
    }

    /**
     * Apply compression to HttpResponse (std::map overload for convenience).
     *
     * Accepts std::map for compatibility with Request::headers().
     */
    bool apply(
        const std::map<std::string, std::string>& request_headers,
        HttpResponse& response
    ) noexcept {
        std::unordered_map<std::string, std::string> headers_copy(
            request_headers.begin(), request_headers.end());
        return apply_impl(headers_copy, response);
    }

    /**
     * Get configuration (for introspection/testing)
     */
    const CompressionConfig& config() const noexcept {
        return config_;
    }

    /**
     * Set configuration
     */
    void set_config(const CompressionConfig& config) noexcept {
        config_ = config;
    }

private:
    bool apply_impl(
        const std::unordered_map<std::string, std::string>& request_headers,
        HttpResponse& response
    ) noexcept {
        if (!config_.enabled) {
            return false;
        }

        // Check if response already has Content-Encoding
        const auto& headers = response.get_headers();
        if (headers.count("Content-Encoding") > 0 ||
            headers.count("content-encoding") > 0) {
            return false;
        }

        // Check response size
        const std::string& body = response.get_body();
        if (body.size() < config_.min_size) {
            return false;
        }

        // Get Content-Type from response
        std::string content_type;
        auto ct_it = headers.find("Content-Type");
        if (ct_it == headers.end()) {
            ct_it = headers.find("content-type");
        }
        if (ct_it != headers.end()) {
            content_type = ct_it->second;
        }

        // Check if content type is compressible
        if (!should_compress_type(content_type)) {
            return false;
        }

        // Get Accept-Encoding header
        std::string accept_encoding;
        auto ae_it = request_headers.find("Accept-Encoding");
        if (ae_it == request_headers.end()) {
            ae_it = request_headers.find("accept-encoding");
        }
        if (ae_it == request_headers.end()) {
            return false;  // Client doesn't accept compression
        }
        accept_encoding = ae_it->second;

        // Find best matching algorithm
        CompressionAlgorithm algo = select_algorithm(accept_encoding);
        if (algo == CompressionAlgorithm::NONE) {
            return false;  // No acceptable algorithm
        }

        // Compress the body
        auto result = compress(
            body.data(),
            body.size(),
            algo,
            config_.level
        );

        if (!result.success) {
            return false;  // Compression failed
        }

        // Only use compression if it actually reduced size
        if (result.compressed_size >= body.size()) {
            return false;  // Compression didn't help
        }

        // Update response body with compressed data
        response.body(std::string(result.data.begin(), result.data.end()));

        // Add Content-Encoding header
        const char* encoding = encoding_header_value(algo);
        response.header("Content-Encoding", encoding);

        // Add Vary header to indicate response varies by Accept-Encoding
        auto vary_it = headers.find("Vary");
        if (vary_it == headers.end()) {
            response.header("Vary", "Accept-Encoding");
        } else if (vary_it->second.find("Accept-Encoding") == std::string::npos) {
            response.header("Vary", vary_it->second + ", Accept-Encoding");
        }

        return true;
    }

    CompressionConfig config_;

    /**
     * Check if content type should be compressed
     */
    bool should_compress_type(const std::string& content_type) const noexcept {
        if (content_type.empty()) {
            return false;  // Unknown type - don't compress
        }

        // Extract base content type (remove charset, etc.)
        std::string_view base_type = content_type;
        auto semicolon = base_type.find(';');
        if (semicolon != std::string_view::npos) {
            base_type = base_type.substr(0, semicolon);
        }
        // Trim whitespace
        while (!base_type.empty() && base_type.back() == ' ') {
            base_type.remove_suffix(1);
        }

        // Check skip list first
        for (const auto& skip : config_.skip_types) {
            if (base_type.find(skip) != std::string_view::npos) {
                return false;
            }
        }

        // Check compressible list
        for (const auto& compressible : config_.compressible_types) {
            // Partial match (e.g., "text/" matches "text/html")
            if (base_type.find(compressible) != std::string_view::npos) {
                return true;
            }
        }

        return false;
    }

    /**
     * Select best compression algorithm based on Accept-Encoding
     */
    CompressionAlgorithm select_algorithm(const std::string& accept_encoding) const noexcept {
        // Parse Accept-Encoding into algorithm -> quality map
        auto client_prefs = parse_accept_encoding_full(accept_encoding);

        // Find first preferred algorithm that client accepts
        for (auto algo : config_.preferred_algorithms) {
            auto it = client_prefs.find(algo);
            if (it != client_prefs.end() && it->second > 0.0f) {
                return algo;
            }
        }

        return CompressionAlgorithm::NONE;
    }

    /**
     * Parse Accept-Encoding header with quality values
     *
     * Returns map of algorithm -> quality (0.0 to 1.0)
     */
    static std::unordered_map<CompressionAlgorithm, float>
    parse_accept_encoding_full(const std::string& accept_encoding) noexcept {
        std::unordered_map<CompressionAlgorithm, float> result;

        // Tokenize by comma
        size_t pos = 0;
        while (pos < accept_encoding.size()) {
            // Skip whitespace
            while (pos < accept_encoding.size() && accept_encoding[pos] == ' ') {
                pos++;
            }

            // Find end of this encoding spec
            size_t end = accept_encoding.find(',', pos);
            if (end == std::string::npos) {
                end = accept_encoding.size();
            }

            // Parse this encoding: "gzip;q=0.5" or "br"
            std::string_view spec(accept_encoding.data() + pos, end - pos);

            // Find semicolon for quality
            float quality = 1.0f;
            auto semi = spec.find(';');
            std::string_view encoding = spec;

            if (semi != std::string_view::npos) {
                encoding = spec.substr(0, semi);

                // Parse quality
                auto q_pos = spec.find("q=", semi);
                if (q_pos != std::string_view::npos) {
                    auto q_str = spec.substr(q_pos + 2);
                    // Trim any trailing whitespace
                    while (!q_str.empty() && (q_str.back() == ' ' || q_str.back() == '\t')) {
                        q_str.remove_suffix(1);
                    }
                    // Parse float
                    char* endptr = nullptr;
                    float q = std::strtof(std::string(q_str).c_str(), &endptr);
                    if (endptr != std::string(q_str).c_str()) {
                        quality = q;
                    }
                }
            }

            // Trim encoding
            while (!encoding.empty() && encoding.front() == ' ') {
                encoding.remove_prefix(1);
            }
            while (!encoding.empty() && encoding.back() == ' ') {
                encoding.remove_suffix(1);
            }

            // Map encoding string to algorithm
            CompressionAlgorithm algo = CompressionAlgorithm::NONE;
            if (encoding == "br") {
                algo = CompressionAlgorithm::BROTLI;
            } else if (encoding == "gzip") {
                algo = CompressionAlgorithm::GZIP;
            } else if (encoding == "deflate") {
                algo = CompressionAlgorithm::DEFLATE;
            } else if (encoding == "zstd") {
                algo = CompressionAlgorithm::ZSTD;
            } else if (encoding == "*") {
                // Wildcard - add all algorithms with this quality
                // but only if not already specified
                if (result.find(CompressionAlgorithm::GZIP) == result.end()) {
                    result[CompressionAlgorithm::GZIP] = quality;
                }
                if (result.find(CompressionAlgorithm::BROTLI) == result.end()) {
                    result[CompressionAlgorithm::BROTLI] = quality;
                }
                if (result.find(CompressionAlgorithm::DEFLATE) == result.end()) {
                    result[CompressionAlgorithm::DEFLATE] = quality;
                }
                if (result.find(CompressionAlgorithm::ZSTD) == result.end()) {
                    result[CompressionAlgorithm::ZSTD] = quality;
                }
            }

            if (algo != CompressionAlgorithm::NONE) {
                result[algo] = quality;
            }

            pos = end + 1;
        }

        return result;
    }
};

/**
 * Apply compression to response (convenience function)
 *
 * Uses default configuration.
 *
 * @param accept_encoding Accept-Encoding header value
 * @param response Response to compress
 * @return true if compression was applied
 */
inline bool apply_compression(
    const std::string& accept_encoding,
    Http1Response& response
) noexcept {
    std::unordered_map<std::string, std::string> headers;
    headers["Accept-Encoding"] = accept_encoding;

    CompressionMiddleware middleware;
    return middleware.apply(headers, response);
}

/**
 * Apply compression to response with custom config
 *
 * @param accept_encoding Accept-Encoding header value
 * @param response Response to compress
 * @param config Compression configuration
 * @return true if compression was applied
 */
inline bool apply_compression(
    const std::string& accept_encoding,
    Http1Response& response,
    const CompressionConfig& config
) noexcept {
    std::unordered_map<std::string, std::string> headers;
    headers["Accept-Encoding"] = accept_encoding;

    CompressionMiddleware middleware(config);
    return middleware.apply(headers, response);
}

/**
 * Apply compression to HttpResponse (convenience function)
 *
 * Uses default configuration.
 *
 * @param accept_encoding Accept-Encoding header value
 * @param response HttpResponse to compress
 * @return true if compression was applied
 */
inline bool apply_compression(
    const std::string& accept_encoding,
    HttpResponse& response
) noexcept {
    std::unordered_map<std::string, std::string> headers;
    headers["Accept-Encoding"] = accept_encoding;

    CompressionMiddleware middleware;
    return middleware.apply(headers, response);
}

/**
 * Apply compression to HttpResponse with custom config
 *
 * @param accept_encoding Accept-Encoding header value
 * @param response HttpResponse to compress
 * @param config Compression configuration
 * @return true if compression was applied
 */
inline bool apply_compression(
    const std::string& accept_encoding,
    HttpResponse& response,
    const CompressionConfig& config
) noexcept {
    std::unordered_map<std::string, std::string> headers;
    headers["Accept-Encoding"] = accept_encoding;

    CompressionMiddleware middleware(config);
    return middleware.apply(headers, response);
}

// ============================================================================
// Per-Route Compression Helpers
// ============================================================================

/**
 * Create a shared compression middleware instance.
 *
 * Useful for creating a middleware that can be shared across multiple routes
 * or used in lambda captures.
 *
 * @param config Compression configuration (defaults to browser-compatible if not specified)
 * @return Shared pointer to CompressionMiddleware
 *
 * Example:
 * @code
 * // Create middleware with browser-compatible settings
 * auto compressor = make_shared_compression_middleware();
 *
 * // Use in route handler
 * app.get("/api/data", [compressor](auto& req, auto& res) {
 *     res.json(data);
 *     // Apply compression based on Accept-Encoding
 *     compressor->apply(req.headers(), res.raw());
 * });
 * @endcode
 */
inline std::shared_ptr<CompressionMiddleware> make_shared_compression_middleware(
    const CompressionConfig& config = CompressionConfig()
) {
    return std::make_shared<CompressionMiddleware>(config);
}

/**
 * Response compression function type.
 *
 * Can be stored and invoked to compress responses. Use with make_response_compressor().
 */
using ResponseCompressor = std::function<bool(
    const std::unordered_map<std::string, std::string>& request_headers,
    Http1Response& response
)>;

/**
 * Create a response compressor function.
 *
 * Returns a callable that can be invoked to compress responses.
 * The returned function captures the middleware configuration.
 *
 * @param config Compression configuration
 * @return Callable that compresses responses
 *
 * Example:
 * @code
 * // Create a browser-compatible compressor
 * auto compress = make_response_compressor(compression_presets::browser_compatible());
 *
 * app.get("/page", [compress](auto& req, auto& res) {
 *     res.html("<html>...</html>");
 *     compress(req.headers(), res.raw());
 * });
 * @endcode
 */
inline ResponseCompressor make_response_compressor(
    const CompressionConfig& config = CompressionConfig()
) {
    auto middleware = std::make_shared<CompressionMiddleware>(config);
    return [middleware](
        const std::unordered_map<std::string, std::string>& headers,
        Http1Response& response
    ) -> bool {
        return middleware->apply(headers, response);
    };
}

/**
 * HttpResponse compression function type.
 *
 * Can be stored and invoked to compress HttpResponse objects.
 */
using HttpResponseCompressor = std::function<bool(
    const std::unordered_map<std::string, std::string>& request_headers,
    HttpResponse& response
)>;

/**
 * Create a response compressor for HttpResponse (high-level API).
 *
 * Returns a callable that can be invoked to compress HttpResponse objects.
 * This works with the App-level Response wrapper.
 *
 * @param config Compression configuration
 * @return Callable that compresses responses
 *
 * Example:
 * @code
 * // Create a browser-compatible compressor
 * auto compress = make_http_response_compressor(compression_presets::browser_compatible());
 *
 * app.get("/page", [compress](auto& req, auto& res) {
 *     res.html("<html>...</html>");
 *     compress(req.headers(), *res.raw());
 * });
 * @endcode
 */
inline HttpResponseCompressor make_http_response_compressor(
    const CompressionConfig& config = CompressionConfig()
) {
    auto middleware = std::make_shared<CompressionMiddleware>(config);
    return [middleware](
        const std::unordered_map<std::string, std::string>& headers,
        HttpResponse& response
    ) -> bool {
        return middleware->apply(headers, response);
    };
}

// ============================================================================
// Compression Presets
// ============================================================================

/**
 * Pre-configured compression settings for common use cases.
 */
namespace compression_presets {

/**
 * Browser-compatible compression (no zstd).
 *
 * Uses brotli (best ratio, widely supported) with gzip fallback.
 * Excludes zstd which browsers don't natively support.
 *
 * Use for: Browser-facing routes, HTML/CSS/JS responses
 */
inline CompressionConfig browser_compatible() {
    CompressionConfig config;
    config.preferred_algorithms = {
        CompressionAlgorithm::BROTLI,   // Modern browsers support br
        CompressionAlgorithm::GZIP,     // Universal fallback
        CompressionAlgorithm::DEFLATE   // Legacy fallback
    };
    return config;
}

/**
 * Gzip-only compression for maximum compatibility.
 *
 * Use for: Legacy clients, maximum compatibility requirements
 */
inline CompressionConfig gzip_only() {
    CompressionConfig config;
    config.preferred_algorithms = {
        CompressionAlgorithm::GZIP
    };
    return config;
}

/**
 * API-optimized compression with all algorithms.
 *
 * Includes zstd for best speed/ratio balance with programmatic clients.
 * Falls back to brotli/gzip for browser clients.
 *
 * Use for: API endpoints consumed by SDKs, CLI tools, or other servers
 */
inline CompressionConfig api_optimized() {
    CompressionConfig config;
    config.preferred_algorithms = {
        CompressionAlgorithm::ZSTD,     // Best speed/ratio for APIs
        CompressionAlgorithm::BROTLI,
        CompressionAlgorithm::GZIP,
        CompressionAlgorithm::DEFLATE
    };
    return config;
}

/**
 * Fast compression for high-throughput scenarios.
 *
 * Uses fastest compression level with gzip for broad compatibility.
 */
inline CompressionConfig fast() {
    CompressionConfig config;
    config.level = CompressionLevel::FASTEST;
    config.preferred_algorithms = {
        CompressionAlgorithm::GZIP,
        CompressionAlgorithm::DEFLATE
    };
    return config;
}

/**
 * Best compression ratio (slower).
 *
 * Uses highest compression level with brotli preferred.
 */
inline CompressionConfig best_ratio() {
    CompressionConfig config;
    config.level = CompressionLevel::BEST;
    config.preferred_algorithms = {
        CompressionAlgorithm::BROTLI,
        CompressionAlgorithm::GZIP
    };
    return config;
}

} // namespace compression_presets

} // namespace http
} // namespace fasterapi
