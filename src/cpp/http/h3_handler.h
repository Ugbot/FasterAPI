#pragma once

#include "http3_parser.h"
#include "quic/quic_connection.h"
#include "qpack/qpack_encoder.h"
#include <string>
#include <memory>
#include <functional>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <atomic>

namespace fasterapi {
namespace http {

/**
 * HTTP/3 handler with pure QUIC implementation.
 * 
 * Features:
 * - Native QUIC protocol (no MsQuic dependency)
 * - HTTP/3 over QUIC
 * - QPACK header compression/decompression
 * - Stream multiplexing
 * - Server push capability
 * - Flow control
 * - Congestion control (NewReno)
 * - Loss detection
 */
class Http3Handler {
public:
    /**
     * HTTP/3 request.
     */
    struct Request {
        uint64_t stream_id;
        std::string method;
        std::string path;
        std::string scheme;
        std::string authority;
        std::unordered_map<std::string, std::string> headers;
        std::vector<uint8_t> body;
    };
    
    /**
     * HTTP/3 response.
     */
    struct Response {
        int status{200};
        std::unordered_map<std::string, std::string> headers;
        std::vector<uint8_t> body;
    };
    
    /**
     * Route handler callback.
     */
    using RouteHandler = std::function<void(const Request&, Response&)>;
    
    /**
     * HTTP/3 settings.
     */
    struct Settings {
        uint32_t max_header_list_size = 16384;
        uint32_t qpack_max_table_capacity = 4096;
        uint32_t qpack_blocked_streams = 100;
        uint32_t connection_window_size = 16 * 1024 * 1024;  // 16MB
        uint32_t stream_window_size = 1024 * 1024;           // 1MB
    };
    
    /**
     * Constructor.
     *
     * @param settings HTTP/3 settings
     */
    explicit Http3Handler(const Settings& settings);
    
    /**
     * Destructor.
     */
    ~Http3Handler();
    
    /**
     * Initialize HTTP/3 handler.
     * 
     * @return 0 on success, -1 on error
     */
    int initialize() noexcept;
    
    /**
     * Add route handler.
     * 
     * @param method HTTP method (GET, POST, etc.)
     * @param path Route path
     * @param handler Route handler function
     * @return 0 on success, -1 on error
     */
    int add_route(const std::string& method, const std::string& path, 
                  RouteHandler handler) noexcept;
    
    /**
     * Process incoming UDP datagram (contains QUIC packets).
     * 
     * @param data Datagram data
     * @param length Datagram length
     * @param source_addr Source address (for routing responses)
     * @param now Current time (microseconds since epoch)
     * @return 0 on success, -1 on error
     */
    int process_datagram(const uint8_t* data, size_t length,
                        const void* source_addr, uint64_t now) noexcept;
    
    /**
     * Generate outgoing UDP datagrams (contains QUIC packets).
     * 
     * @param output Output buffer
     * @param capacity Output capacity
     * @param dest_addr Destination address (output)
     * @param now Current time (microseconds)
     * @return Number of bytes written
     */
    size_t generate_datagrams(uint8_t* output, size_t capacity,
                             void** dest_addr, uint64_t now) noexcept;
    
    /**
     * Send response on stream.
     * 
     * @param stream_id Stream ID
     * @param response Response to send
     * @return 0 on success, -1 on error
     */
    int send_response(uint64_t stream_id, const Response& response) noexcept;
    
    /**
     * Send server push.
     * 
     * @param stream_id Parent stream ID
     * @param path Path to push
     * @param response Response to push
     * @return 0 on success, -1 on error
     */
    int send_push(uint64_t stream_id, const std::string& path,
                  const Response& response) noexcept;
    
    /**
     * Get statistics.
     * 
     * @return Statistics map
     */
    std::unordered_map<std::string, uint64_t> get_stats() const noexcept;
    
    /**
     * Check if handler is running.
     */
    bool is_running() const noexcept { return running_.load(); }
    
    /**
     * Start handler.
     */
    void start() noexcept { running_.store(true); }
    
    /**
     * Stop handler.
     */
    void stop() noexcept { running_.store(false); }

private:
    Settings settings_;
    std::atomic<bool> running_;
    
    // QUIC connections (keyed by connection ID)
    std::unordered_map<std::string, std::unique_ptr<quic::QUICConnection>> connections_;
    
    // HTTP/3 parser
    HTTP3Parser parser_;
    
    // QPACK encoder for responses
    qpack::QPACKEncoder qpack_encoder_;
    
    // Route handlers
    std::unordered_map<std::string, RouteHandler> routes_;
    
    // Pending requests (keyed by stream ID)
    std::unordered_map<uint64_t, Request> pending_requests_;
    
    // Statistics
    std::atomic<uint64_t> total_requests_;
    std::atomic<uint64_t> total_bytes_sent_;
    std::atomic<uint64_t> total_bytes_received_;
    std::atomic<uint64_t> active_streams_;
    std::atomic<uint64_t> push_responses_;
    std::atomic<uint64_t> quic_connections_;
    
    /**
     * Get or create connection.
     * 
     * @param conn_id Connection ID
     * @param source_addr Source address
     * @return Connection pointer
     */
    quic::QUICConnection* get_or_create_connection(
        const quic::ConnectionID& conn_id,
        const void* source_addr
    ) noexcept;
    
    /**
     * Process HTTP/3 stream.
     * 
     * @param conn Connection
     * @param stream_id Stream ID
     * @param now Current time
     */
    void process_http3_stream(quic::QUICConnection* conn, 
                             uint64_t stream_id,
                             uint64_t now) noexcept;
    
    /**
     * Handle HEADERS frame.
     * 
     * @param stream_id Stream ID
     * @param data QPACK-encoded headers
     * @param length Data length
     */
    void handle_headers_frame(uint64_t stream_id, const uint8_t* data, 
                             size_t length) noexcept;
    
    /**
     * Handle DATA frame.
     * 
     * @param stream_id Stream ID
     * @param data Frame data
     * @param length Data length
     */
    void handle_data_frame(uint64_t stream_id, const uint8_t* data,
                          size_t length) noexcept;
    
    /**
     * Handle SETTINGS frame.
     * 
     * @param data Settings data
     * @param length Data length
     */
    void handle_settings_frame(const uint8_t* data, size_t length) noexcept;
    
    /**
     * Dispatch request to route handler.
     * 
     * @param request Request
     */
    void dispatch_request(const Request& request) noexcept;
    
    /**
     * Encode response to QPACK + HTTP/3 frames.
     * 
     * @param response Response to encode
     * @param output Output buffer
     * @param capacity Output capacity
     * @param out_length Encoded length
     * @return 0 on success, -1 on error
     */
    int encode_response(const Response& response, uint8_t* output,
                       size_t capacity, size_t& out_length) noexcept;
};

} // namespace http
} // namespace fasterapi
