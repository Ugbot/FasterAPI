#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include <memory>
#include <cstdint>

// Forward declarations for uWebSockets
#ifdef FA_USE_UWEBSOCKETS
namespace uWS {
    struct HttpResponse;
}
#endif

/**
 * HTTP response object with streaming and compression support.
 * 
 * Features:
 * - Streaming response support
 * - Automatic zstd compression
 * - JSON serialization
 * - File serving
 * - Chunked transfer encoding
 * - HTTP/2 server push
 */
class HttpResponse {
public:
    // HTTP status codes
    enum class Status {
        OK = 200,
        CREATED = 201,
        ACCEPTED = 202,
        NO_CONTENT = 204,
        MOVED_PERMANENTLY = 301,
        FOUND = 302,
        NOT_MODIFIED = 304,
        BAD_REQUEST = 400,
        UNAUTHORIZED = 401,
        FORBIDDEN = 403,
        NOT_FOUND = 404,
        METHOD_NOT_ALLOWED = 405,
        CONFLICT = 409,
        UNPROCESSABLE_ENTITY = 422,
        TOO_MANY_REQUESTS = 429,
        INTERNAL_SERVER_ERROR = 500,
        NOT_IMPLEMENTED = 501,
        BAD_GATEWAY = 502,
        SERVICE_UNAVAILABLE = 503
    };

    // Response types
    enum class Type {
        JSON,
        TEXT,
        HTML,
        BINARY,
        STREAM,
        FILE
    };

    /**
     * Create a new HTTP response.
     */
    HttpResponse();
    
    ~HttpResponse();

    // Non-copyable, movable
    HttpResponse(const HttpResponse&) = delete;
    HttpResponse& operator=(const HttpResponse&) = delete;
    HttpResponse(HttpResponse&&) noexcept;
    HttpResponse& operator=(HttpResponse&&) noexcept;

    /**
     * Set HTTP status code.
     * 
     * @param status HTTP status code
     * @return Reference to this response
     */
    HttpResponse& status(Status status) noexcept;

    /**
     * Set response header.
     * 
     * @param name Header name
     * @param value Header value
     * @return Reference to this response
     */
    HttpResponse& header(const std::string& name, const std::string& value) noexcept;

    /**
     * Set content type.
     * 
     * @param content_type Content type (e.g., "application/json")
     * @return Reference to this response
     */
    HttpResponse& content_type(const std::string& content_type) noexcept;

    /**
     * Send JSON response.
     * 
     * @param data JSON data (will be serialized)
     * @return Reference to this response
     */
    HttpResponse& json(const std::string& data) noexcept;

    /**
     * Send text response.
     * 
     * @param text Text content
     * @return Reference to this response
     */
    HttpResponse& text(const std::string& text) noexcept;

    /**
     * Send HTML response.
     * 
     * @param html HTML content
     * @return Reference to this response
     */
    HttpResponse& html(const std::string& html) noexcept;

    /**
     * Send binary response.
     * 
     * @param data Binary data
     * @return Reference to this response
     */
    HttpResponse& binary(const std::vector<uint8_t>& data) noexcept;

    /**
     * Send file response.
     * 
     * @param file_path Path to file
     * @return Reference to this response
     */
    HttpResponse& file(const std::string& file_path) noexcept;

    /**
     * Start streaming response.
     * 
     * @param content_type Content type for stream
     * @return Reference to this response
     */
    HttpResponse& stream(const std::string& content_type = "application/octet-stream") noexcept;

    /**
     * Write data to stream.
     * 
     * @param data Data to write
     * @return Reference to this response
     */
    HttpResponse& write(const std::string& data) noexcept;

    /**
     * Write binary data to stream.
     * 
     * @param data Binary data to write
     * @return Reference to this response
     */
    HttpResponse& write(const std::vector<uint8_t>& data) noexcept;

    /**
     * End streaming response.
     * 
     * @return Reference to this response
     */
    HttpResponse& end() noexcept;

    /**
     * Enable compression for this response.
     * 
     * @param enable Enable compression
     * @return Reference to this response
     */
    HttpResponse& compress(bool enable = true) noexcept;

    /**
     * Set compression level.
     * 
     * @param level Compression level (1-22 for zstd)
     * @return Reference to this response
     */
    HttpResponse& compression_level(int level) noexcept;

    /**
     * Redirect to another URL.
     * 
     * @param url URL to redirect to
     * @param permanent Use permanent redirect (301) instead of temporary (302)
     * @return Reference to this response
     */
    HttpResponse& redirect(const std::string& url, bool permanent = false) noexcept;

    /**
     * Set cookie.
     * 
     * @param name Cookie name
     * @param value Cookie value
     * @param options Cookie options (path, domain, expires, etc.)
     * @return Reference to this response
     */
    HttpResponse& cookie(const std::string& name, const std::string& value, 
                        const std::unordered_map<std::string, std::string>& options = {}) noexcept;

    /**
     * Clear cookie.
     * 
     * @param name Cookie name
     * @param path Cookie path
     * @return Reference to this response
     */
    HttpResponse& clear_cookie(const std::string& name, const std::string& path = "/") noexcept;

    /**
     * Send the response.
     * 
     * @return Error code (0 = success)
     */
    int send() noexcept;

    /**
     * Check if response has been sent.
     * 
     * @return true if sent, false otherwise
     */
    bool is_sent() const noexcept;

    /**
     * Get response size.
     * 
     * @return Response size in bytes
     */
    uint64_t get_size() const noexcept;

    /**
     * Get compression ratio.
     * 
     * @return Compression ratio (0.0 = no compression, 1.0 = 100% compression)
     */
    double get_compression_ratio() const noexcept;

    /**
     * Constructor for uWebSockets integration.
     * 
     * @param res uWebSockets response object
     */
#ifdef FA_USE_UWEBSOCKETS
    HttpResponse(uWS::HttpResponse* res) noexcept;
#endif

private:
    Status status_;
    Type type_;
    std::unordered_map<std::string, std::string> headers_;
    std::string content_type_;
    std::string body_;
    std::vector<uint8_t> binary_body_;
    bool is_streaming_;
    bool is_sent_;
    bool compression_enabled_;
    int compression_level_;
    uint64_t original_size_;
    uint64_t compressed_size_;
    
    // Streaming callback
    std::function<void(const std::string&)> stream_callback_;
    
    // File serving
    std::string file_path_;
    
    // Cookies
    std::vector<std::string> cookies_;
    
    // uWebSockets response object
#ifdef FA_USE_UWEBSOCKETS
    uWS::HttpResponse* uws_response_;
#endif

    /**
     * Apply compression to response body.
     * 
     * @return Error code (0 = success)
     */
    int apply_compression() noexcept;

    /**
     * Serialize headers to string.
     * 
     * @return Headers as string
     */
    std::string serialize_headers() const noexcept;

    /**
     * Get status text for status code.
     * 
     * @param status HTTP status code
     * @return Status text
     */
    std::string get_status_text(Status status) const noexcept;
};