#pragma once

#include <string>
#include <memory>
#include <functional>
#include <unordered_map>
#include <vector>
#include <cstdint>

// Forward declarations for MsQuic
struct QUIC_API_TABLE;
struct QUIC_HANDLE;
struct QUIC_CONNECTION;
struct QUIC_STREAM;

/**
 * HTTP/3 handler with QUIC support and QPACK compression.
 * 
 * Features:
 * - MsQuic integration for QUIC protocol
 * - HTTP/3 over QUIC
 * - QPACK header compression/decompression
 * - Multiplexing support
 * - Server push capability
 * - Flow control
 * - TLS 1.3 support
 */
class Http3Handler {
public:
    // HTTP/3 settings
    struct Settings {
        uint32_t max_header_list_size = 8192;
        uint32_t max_field_section_size = 4096;
        uint32_t qpack_max_table_capacity = 4096;
        uint32_t qpack_blocked_streams = 100;
        uint32_t connection_window_size = 16777216;  // 16MB
        uint32_t stream_window_size = 16777216;     // 16MB
    };

    // QUIC configuration
    struct QuicConfig {
        std::string server_name = "localhost";
        uint16_t port = 443;
        std::string cert_file;
        std::string key_file;
        bool enable_0rtt = true;
        bool enable_migration = false;
    };

    // Stream state
    struct Stream {
        QUIC_STREAM* quic_stream;
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
     * @param settings HTTP/3 settings
     * @param quic_config QUIC configuration
     */
    Http3Handler(const Settings& settings = {}, const QuicConfig& quic_config = {});

    /**
     * Destructor.
     */
    ~Http3Handler();

    /**
     * Initialize HTTP/3 handler.
     * 
     * @return Error code (0 = success)
     */
    int initialize() noexcept;

    /**
     * Start HTTP/3 server.
     * 
     * @param port Port to listen on
     * @param host Host to bind to
     * @return Error code (0 = success)
     */
    int start(uint16_t port, const std::string& host) noexcept;

    /**
     * Stop HTTP/3 server.
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
    QuicConfig quic_config_;
    std::atomic<bool> running_;
    
    // MsQuic API table
    QUIC_API_TABLE* api_table_;
    
    // QUIC handles
    QUIC_HANDLE* registration_;
    QUIC_HANDLE* listener_;
    QUIC_CONNECTION* connection_;
    
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
    std::atomic<uint64_t> quic_connections_;
    
    // Next stream ID
    std::atomic<int32_t> next_stream_id_;
    
    /**
     * Initialize MsQuic.
     * 
     * @return Error code (0 = success)
     */
    int initialize_quic() noexcept;
    
    /**
     * Initialize QUIC registration.
     * 
     * @return Error code (0 = success)
     */
    int initialize_registration() noexcept;
    
    /**
     * Initialize QUIC listener.
     * 
     * @return Error code (0 = success)
     */
    int initialize_listener() noexcept;
    
    /**
     * Handle QUIC connection event.
     * 
     * @param connection QUIC connection
     * @param event Event data
     * @return Error code (0 = success)
     */
    int handle_connection_event(QUIC_CONNECTION* connection, void* event) noexcept;
    
    /**
     * Handle QUIC stream event.
     * 
     * @param stream QUIC stream
     * @param event Event data
     * @return Error code (0 = success)
     */
    int handle_stream_event(QUIC_STREAM* stream, void* event) noexcept;
    
    /**
     * Handle HTTP/3 frame.
     * 
     * @param stream_id Stream ID
     * @param frame Frame data
     * @param length Frame length
     * @return Error code (0 = success)
     */
    int handle_frame(int32_t stream_id, const uint8_t* frame, size_t length) noexcept;
    
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
     * Parse headers from QPACK data.
     * 
     * @param data QPACK data
     * @param length Data length
     * @param headers Output headers
     * @return Error code (0 = success)
     */
    int parse_headers(const uint8_t* data, size_t length, 
                     std::unordered_map<std::string, std::string>& headers) noexcept;
    
    /**
     * Compress headers to QPACK.
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
     * @param stream_id Stream ID
     * @param frame Frame data
     * @param length Frame length
     * @return Error code (0 = success)
     */
    int send_frame(int32_t stream_id, const uint8_t* frame, size_t length) noexcept;
    
    /**
     * Create stream.
     * 
     * @param stream_id Stream ID
     * @param quic_stream QUIC stream handle
     * @return Stream pointer
     */
    Stream* create_stream(int32_t stream_id, QUIC_STREAM* quic_stream) noexcept;
    
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
    
    // Static callbacks for MsQuic
    static void connection_callback(QUIC_HANDLE* listener, void* context, QUIC_CONNECTION_EVENT* event);
    static void stream_callback(QUIC_STREAM* stream, void* context, QUIC_STREAM_EVENT* event);
};