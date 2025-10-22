#pragma once
#include <atomic>
#include <mutex>

#include <string>
#include <memory>
#include <functional>
#include <unordered_map>
#include <vector>
#include <cstdint>

// Forward declarations for nghttp2
struct nghttp2_session;
struct nghttp2_session_callbacks;
struct nghttp2_hd_deflater;
struct nghttp2_hd_inflater;
struct nghttp2_frame;
struct nghttp2_data_source;

// Forward declarations for OpenSSL
struct ssl_st;
struct ssl_ctx_st;

/**
 * HTTP/2 handler with ALPN support and HPACK compression.
 * 
 * Features:
 * - nghttp2 session management
 * - ALPN negotiation via OpenSSL
 * - HPACK header compression/decompression
 * - Multiplexing support
 * - Server push capability
 * - Flow control
 */
class Http2Handler {
public:
    // HTTP/2 settings
    struct Settings {
        uint32_t header_table_size;
        uint32_t enable_push;
        uint32_t max_concurrent_streams;
        uint32_t initial_window_size;
        uint32_t max_frame_size;
        uint32_t max_header_list_size;
        
        Settings() : header_table_size(4096), enable_push(1), max_concurrent_streams(100),
                     initial_window_size(65535), max_frame_size(16384), max_header_list_size(8192) {}
    };

    // ALPN configuration
    struct AlpnConfig {
        std::string protocols;
        bool prefer_h2;
        std::string tls_cert_file;
        std::string tls_key_file;
        
        AlpnConfig() : protocols("h2,http/1.1"), prefer_h2(true) {}
    };

    // Stream state
    struct Stream {
        int32_t stream_id;
        std::string method;
        std::string path;
        std::unordered_map<std::string, std::string> headers;
        std::vector<uint8_t> body;
        bool headers_sent = false;
        bool body_sent = false;
        bool closed = false;
    };

    /**
     * Constructor.
     * 
     * @param settings HTTP/2 settings
     * @param alpn_config ALPN configuration
     */
    Http2Handler(const Settings& settings = {}, const AlpnConfig& alpn_config = {});

    /**
     * Destructor.
     */
    ~Http2Handler();

    /**
     * Initialize HTTP/2 handler.
     * 
     * @return Error code (0 = success)
     */
    int initialize() noexcept;

    /**
     * Start HTTP/2 server.
     * 
     * @param port Port to listen on
     * @param host Host to bind to
     * @return Error code (0 = success)
     */
    int start(uint16_t port, const std::string& host) noexcept;

    /**
     * Stop HTTP/2 server.
     * 
     * @return Error code (0 = success)
     */
    int stop() noexcept;

    /**
     * Check if server is running.
     * 
     * @return true if running, false otherwise
     */
    bool is_running() const noexcept;

    /**
     * Add route handler.
     * 
     * @param method HTTP method
     * @param path Route path
     * @param handler Route handler function
     * @return Error code (0 = success)
     */
    int add_route(const std::string& method, const std::string& path, 
                  std::function<void(Stream*)> handler) noexcept;

    /**
     * Process incoming data.
     * 
     * @param data Incoming data
     * @param length Data length
     * @return Error code (0 = success)
     */
    int process_data(const uint8_t* data, size_t length) noexcept;

    /**
     * Send response.
     * 
     * @param stream_id Stream ID
     * @param status HTTP status code
     * @param headers Response headers
     * @param body Response body
     * @return Error code (0 = success)
     */
    int send_response(int32_t stream_id, int status, 
                     const std::unordered_map<std::string, std::string>& headers,
                     const std::vector<uint8_t>& body) noexcept;

    /**
     * Send server push.
     * 
     * @param stream_id Parent stream ID
     * @param path Path to push
     * @param headers Push headers
     * @param body Push body
     * @return Error code (0 = success)
     */
    int send_push(int32_t stream_id, const std::string& path,
                  const std::unordered_map<std::string, std::string>& headers,
                  const std::vector<uint8_t>& body) noexcept;

    /**
     * Get statistics.
     * 
     * @return Statistics map
     */
    std::unordered_map<std::string, uint64_t> get_stats() const noexcept;

private:
    Settings settings_;
    AlpnConfig alpn_config_;
    std::atomic<bool> running_;
    
    // nghttp2 session
    nghttp2_session* session_;
    nghttp2_session_callbacks* callbacks_;
    nghttp2_hd_deflater* deflater_;
    nghttp2_hd_inflater* inflater_;
    
    // OpenSSL context
    ssl_ctx_st* ssl_ctx_;
    
    // Route handlers
    std::unordered_map<std::string, std::function<void(Stream*)>> routes_;
    
    // Active streams
    std::unordered_map<int32_t, std::unique_ptr<Stream>> streams_;
    
    // Statistics
    std::atomic<uint64_t> total_requests_;
    std::atomic<uint64_t> total_bytes_sent_;
    std::atomic<uint64_t> total_bytes_received_;
    std::atomic<uint64_t> active_streams_;
    std::atomic<uint64_t> push_responses_;
    
    // Next stream ID
    std::atomic<int32_t> next_stream_id_;
    
    /**
     * Initialize OpenSSL context.
     * 
     * @return Error code (0 = success)
     */
    int initialize_ssl() noexcept;
    
    /**
     * Initialize nghttp2 session.
     * 
     * @return Error code (0 = success)
     */
    int initialize_session() noexcept;
    
    /**
     * Handle incoming frame.
     * 
     * @param frame Frame data
     * @param length Frame length
     * @return Error code (0 = success)
     */
    int handle_frame(const uint8_t* frame, size_t length) noexcept;
    
    /**
     * Handle headers frame.
     * 
     * @param stream_id Stream ID
     * @param headers Headers data
     * @param length Headers length
     * @return Error code (0 = success)
     */
    int handle_headers(int32_t stream_id, const uint8_t* headers, size_t length) noexcept;
    
    /**
     * Handle data frame.
     * 
     * @param stream_id Stream ID
     * @param data Data
     * @param length Data length
     * @return Error code (0 = success)
     */
    int handle_data(int32_t stream_id, const uint8_t* data, size_t length) noexcept;
    
    /**
     * Handle stream close.
     * 
     * @param stream_id Stream ID
     * @return Error code (0 = success)
     */
    int handle_stream_close(int32_t stream_id) noexcept;
    
    /**
     * Parse headers from HPACK data.
     * 
     * @param data HPACK data
     * @param length Data length
     * @param headers Output headers
     * @return Error code (0 = success)
     */
    int parse_headers(const uint8_t* data, size_t length, 
                     std::unordered_map<std::string, std::string>& headers) noexcept;
    
    /**
     * Compress headers to HPACK.
     * 
     * @param headers Headers to compress
     * @param output Output buffer
     * @return Error code (0 = success)
     */
    int compress_headers(const std::unordered_map<std::string, std::string>& headers,
                        std::vector<uint8_t>& output) noexcept;
    
    /**
     * Send frame.
     * 
     * @param frame Frame data
     * @param length Frame length
     * @return Error code (0 = success)
     */
    int send_frame(const uint8_t* frame, size_t length) noexcept;
    
    /**
     * Create stream.
     * 
     * @param stream_id Stream ID
     * @return Stream pointer
     */
    Stream* create_stream(int32_t stream_id) noexcept;
    
    /**
     * Get stream.
     * 
     * @param stream_id Stream ID
     * @return Stream pointer or nullptr
     */
    Stream* get_stream(int32_t stream_id) noexcept;
    
    /**
     * Remove stream.
     * 
     * @param stream_id Stream ID
     */
    void remove_stream(int32_t stream_id) noexcept;
    
    // Static callbacks for nghttp2
    static int on_begin_headers_callback(nghttp2_session* session,
                                        const nghttp2_frame* frame,
                                        void* user_data);
    
    static int on_header_callback(nghttp2_session* session,
                                 const nghttp2_frame* frame,
                                 const uint8_t* name, size_t namelen,
                                 const uint8_t* value, size_t valuelen,
                                 uint8_t flags, void* user_data);
    
    static int on_data_chunk_recv_callback(nghttp2_session* session,
                                           uint8_t flags, int32_t stream_id,
                                           const uint8_t* data, size_t len,
                                           void* user_data);
    
    static int on_stream_close_callback(nghttp2_session* session,
                                       int32_t stream_id, uint32_t error_code,
                                       void* user_data);
    
    static int on_frame_recv_callback(nghttp2_session* session,
                                     const nghttp2_frame* frame,
                                     void* user_data);
    
    static int on_frame_send_callback(nghttp2_session* session,
                                     const nghttp2_frame* frame,
                                     void* user_data);
    
    static int on_frame_not_send_callback(nghttp2_session* session,
                                         const nghttp2_frame* frame,
                                         int lib_error_code, void* user_data);
    
    static ssize_t on_data_source_read_callback(nghttp2_session* session,
                                               int32_t stream_id,
                                               uint8_t* buf, size_t length,
                                               uint32_t* data_flags,
                                               nghttp2_data_source* source,
                                               void* user_data);
};