#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <functional>
#include <atomic>

// Forward declarations for llhttp
struct llhttp_t;
struct llhttp_settings_t;

/**
 * Zero-copy HTTP parser using llhttp.
 * 
 * Features:
 * - Zero-copy string access
 * - Streaming HTTP parsing
 * - HTTP/1.1 and HTTP/1.0 support
 * - Header parsing with case-insensitive lookup
 * - Chunked transfer encoding
 * - Keep-alive connection handling
 */
class HttpParser {
public:
    // HTTP methods
    enum class Method {
        GET,
        POST,
        PUT,
        DELETE,
        PATCH,
        HEAD,
        OPTIONS,
        TRACE,
        CONNECT
    };

    // HTTP versions
    enum class Version {
        HTTP_1_0,
        HTTP_1_1,
        HTTP_2_0,
        HTTP_3_0
    };

    // Parser state
    enum class State {
        IDLE,
        HEADERS,
        BODY,
        CHUNKED_BODY,
        COMPLETE,
        ERROR
    };

    // HTTP request/response
    struct Message {
        Method method = Method::GET;
        Version version = Version::HTTP_1_1;
        std::string path;
        std::string query;
        std::string fragment;
        int status_code = 200;
        std::string reason_phrase;
        std::unordered_map<std::string, std::string> headers;
        std::vector<uint8_t> body;
        bool is_complete = false;
        bool is_chunked = false;
        size_t content_length = 0;
        bool keep_alive = true;
    };

    // Callback types
    using OnHeadersCompleteCallback = std::function<void(const Message&)>;
    using OnBodyCallback = std::function<void(const uint8_t* data, size_t length)>;
    using OnMessageCompleteCallback = std::function<void(const Message&)>;
    using OnErrorCallback = std::function<void(const std::string& error)>;

    /**
     * Constructor.
     */
    HttpParser();

    /**
     * Destructor.
     */
    ~HttpParser();

    /**
     * Parse HTTP data.
     * 
     * @param data Input data
     * @param length Data length
     * @return Number of bytes consumed
     */
    size_t parse(const uint8_t* data, size_t length) noexcept;

    /**
     * Parse HTTP data from string.
     * 
     * @param data Input string
     * @return Number of bytes consumed
     */
    size_t parse(const std::string& data) noexcept;

    /**
     * Reset parser state.
     */
    void reset() noexcept;

    /**
     * Check if parser is in error state.
     * 
     * @return true if in error state, false otherwise
     */
    bool has_error() const noexcept;

    /**
     * Get last error message.
     * 
     * @return Error message
     */
    std::string get_last_error() const noexcept;

    /**
     * Get current parser state.
     * 
     * @return Current state
     */
    State get_state() const noexcept;

    /**
     * Get current message.
     * 
     * @return Current message
     */
    const Message& get_message() const noexcept;

    /**
     * Set headers complete callback.
     * 
     * @param callback Callback function
     */
    void set_on_headers_complete(OnHeadersCompleteCallback callback) noexcept;

    /**
     * Set body callback.
     * 
     * @param callback Callback function
     */
    void set_on_body(OnBodyCallback callback) noexcept;

    /**
     * Set message complete callback.
     * 
     * @param callback Callback function
     */
    void set_on_message_complete(OnMessageCompleteCallback callback) noexcept;

    /**
     * Set error callback.
     * 
     * @param callback Callback function
     */
    void set_on_error(OnErrorCallback callback) noexcept;

    /**
     * Get parser statistics.
     * 
     * @return Statistics map
     */
    std::unordered_map<std::string, uint64_t> get_stats() const noexcept;

    /**
     * Convert method to string.
     * 
     * @param method HTTP method
     * @return Method string
     */
    static std::string method_to_string(Method method) noexcept;

    /**
     * Convert string to method.
     * 
     * @param str Method string
     * @return HTTP method
     */
    static Method string_to_method(const std::string& str) noexcept;

    /**
     * Convert version to string.
     * 
     * @param version HTTP version
     * @return Version string
     */
    static std::string version_to_string(Version version) noexcept;

    /**
     * Convert string to version.
     * 
     * @param str Version string
     * @return HTTP version
     */
    static Version string_to_version(const std::string& str) noexcept;

private:
    void* parser_;  // Opaque pointer to avoid incomplete type issues
    void* settings_;  // Opaque pointer to avoid incomplete type issues
    Message current_message_;
    State current_state_;
    std::string last_error_;
    
    // Callbacks
    OnHeadersCompleteCallback on_headers_complete_;
    OnBodyCallback on_body_;
    OnMessageCompleteCallback on_message_complete_;
    OnErrorCallback on_error_;
    
    // Statistics
    std::atomic<uint64_t> total_parses_;
    std::atomic<uint64_t> successful_parses_;
    std::atomic<uint64_t> failed_parses_;
    std::atomic<uint64_t> total_bytes_parsed_;
    
    /**
     * Initialize llhttp parser.
     * 
     * @return Error code (0 = success)
     */
    int initialize_parser() noexcept;
    
    /**
     * Set error message.
     * 
     * @param error Error message
     */
    void set_error(const std::string& error) noexcept;
    
    /**
     * Update statistics.
     * 
     * @param success Whether parse was successful
     * @param bytes Number of bytes parsed
     */
    void update_stats(bool success, size_t bytes) noexcept;
    
    // Static callbacks for llhttp
    static int on_message_begin(llhttp_t* parser);
    static int on_url(llhttp_t* parser, const char* at, size_t length);
    static int on_status(llhttp_t* parser, const char* at, size_t length);
    static int on_header_field(llhttp_t* parser, const char* at, size_t length);
    static int on_header_value(llhttp_t* parser, const char* at, size_t length);
    static int on_headers_complete(llhttp_t* parser);
    static int on_body(llhttp_t* parser, const char* at, size_t length);
    static int on_message_complete(llhttp_t* parser);
    static int on_chunk_header(llhttp_t* parser);
    static int on_chunk_complete(llhttp_t* parser);
};
