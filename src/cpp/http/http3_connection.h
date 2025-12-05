#pragma once

#include "quic/quic_connection.h"
#include "h3_handler.h"
#include "qpack/qpack_encoder.h"
#include "qpack/qpack_decoder.h"
#include "http3_parser.h"
#include "../core/result.h"
#include <cstdint>
#include <memory>
#include <functional>
#include <unordered_map>
#include <string>
#include <vector>
#include <array>
#include <cstring>

namespace fasterapi {
namespace http {

/**
 * HTTP/3 Connection State.
 *
 * Tracks the lifecycle of an HTTP/3 connection over QUIC.
 */
enum class Http3ConnectionState : uint8_t {
    IDLE = 0,           // Not yet connected
    HANDSHAKE,          // QUIC handshake in progress
    ACTIVE,             // Active and processing HTTP/3
    CLOSING,            // Closing gracefully
    CLOSED              // Connection closed
};

/**
 * HTTP/3 Connection Settings.
 *
 * Configurable parameters for the HTTP/3 connection.
 */
struct Http3ConnectionSettings {
    uint32_t max_header_list_size{16384};       // SETTINGS_MAX_HEADER_LIST_SIZE
    uint32_t qpack_max_table_capacity{4096};    // SETTINGS_QPACK_MAX_TABLE_CAPACITY
    uint32_t qpack_blocked_streams{100};        // SETTINGS_QPACK_BLOCKED_STREAMS
    uint32_t max_concurrent_streams{100};       // Max concurrent bidirectional streams
    uint32_t connection_window_size{16 * 1024 * 1024};  // 16MB connection window
    uint32_t stream_window_size{1024 * 1024};   // 1MB stream window
};

/**
 * Preallocated buffer pool for zero-allocation HTTP/3 processing.
 *
 * Maintains a pool of reusable buffers to avoid heap allocations
 * during frame parsing and response generation.
 */
template<size_t BufferSize = 16384, size_t PoolSize = 16>
class Http3BufferPool {
public:
    Http3BufferPool() {
        // Initialize all buffers as available
        for (size_t i = 0; i < PoolSize; ++i) {
            available_[i] = true;
        }
    }

    /**
     * Acquire buffer from pool.
     *
     * @return Buffer pointer or nullptr if pool exhausted
     */
    uint8_t* acquire() noexcept {
        for (size_t i = 0; i < PoolSize; ++i) {
            if (available_[i]) {
                available_[i] = false;
                return buffers_[i].data();
            }
        }
        return nullptr;  // Pool exhausted
    }

    /**
     * Release buffer back to pool.
     */
    void release(uint8_t* buffer) noexcept {
        // Find which buffer this is
        for (size_t i = 0; i < PoolSize; ++i) {
            if (buffers_[i].data() == buffer) {
                available_[i] = true;
                return;
            }
        }
    }

    /**
     * Get buffer size.
     */
    constexpr size_t buffer_size() const noexcept { return BufferSize; }

private:
    std::array<std::array<uint8_t, BufferSize>, PoolSize> buffers_;
    std::array<bool, PoolSize> available_;
};

/**
 * HTTP/3 stream state tracking.
 *
 * Tracks request assembly for each stream.
 */
struct Http3StreamState {
    uint64_t stream_id{0};
    std::string method;
    std::string path;
    std::string scheme;
    std::string authority;
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;
    bool headers_complete{false};
    bool request_complete{false};
};

/**
 * HTTP/3 Connection.
 *
 * Manages an HTTP/3 connection over QUIC, integrating:
 * - QUICConnection for transport
 * - HTTP/3 frame parsing
 * - QPACK header compression/decompression
 * - Stream management
 * - Request/response handling
 *
 * Designed to mirror Http2Connection API for unified server integration.
 */
class Http3Connection {
public:
    /**
     * Request callback - called when complete request received.
     *
     * Parameters:
     * - method: HTTP method (GET, POST, etc.)
     * - path: Request path
     * - headers: Request headers
     * - body: Request body
     * - send_response: Callback to send response (status, headers, body)
     */
    using RequestCallback = std::function<void(
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body,
        std::function<void(uint16_t, const std::unordered_map<std::string, std::string>&, const std::string&)> send_response
    )>;

    /**
     * Create HTTP/3 connection.
     *
     * @param is_server True if server-side, false if client-side
     * @param local_conn_id Local QUIC connection ID
     * @param peer_conn_id Peer QUIC connection ID
     * @param settings HTTP/3 connection settings
     */
    explicit Http3Connection(
        bool is_server,
        const quic::ConnectionID& local_conn_id,
        const quic::ConnectionID& peer_conn_id,
        const Http3ConnectionSettings& settings = Http3ConnectionSettings()
    );

    /**
     * Destructor.
     */
    ~Http3Connection();

    // No copy
    Http3Connection(const Http3Connection&) = delete;
    Http3Connection& operator=(const Http3Connection&) = delete;

    /**
     * Process incoming UDP datagram (contains QUIC packet).
     *
     * Parses QUIC packets and dispatches HTTP/3 frames to appropriate handlers.
     * Uses buffer pool to avoid allocations.
     *
     * @param data Incoming datagram data
     * @param length Datagram length
     * @param now_us Current time (microseconds since epoch)
     * @return 0 on success, -1 on error
     */
    int process_datagram(const uint8_t* data, size_t length, uint64_t now_us) noexcept;

    /**
     * Generate outgoing UDP datagrams (contains QUIC packets).
     *
     * Drains pending QUIC packets to send.
     *
     * @param output Output buffer
     * @param capacity Output buffer capacity
     * @param now_us Current time (microseconds since epoch)
     * @return Number of bytes written
     */
    size_t generate_datagrams(uint8_t* output, size_t capacity, uint64_t now_us) noexcept;

    /**
     * Set request callback.
     *
     * @param callback Request callback function
     */
    void set_request_callback(RequestCallback callback) {
        request_callback_ = std::move(callback);
    }

    /**
     * Check if connection is closed.
     */
    bool is_closed() const noexcept {
        return state_ == Http3ConnectionState::CLOSED || quic_conn_->is_closed();
    }

    /**
     * Close connection.
     *
     * @param error_code Error code (0 for graceful close)
     * @param reason Reason string (optional)
     */
    void close(uint64_t error_code = 0, const char* reason = nullptr) noexcept;

    /**
     * Get connection state.
     */
    Http3ConnectionState state() const noexcept { return state_; }

    /**
     * Check if connection is active.
     */
    bool is_active() const noexcept {
        return state_ == Http3ConnectionState::ACTIVE && quic_conn_->is_established();
    }

    /**
     * Get local connection ID.
     */
    const quic::ConnectionID& local_conn_id() const noexcept {
        return quic_conn_->local_conn_id();
    }

    /**
     * Get peer connection ID.
     */
    const quic::ConnectionID& peer_conn_id() const noexcept {
        return quic_conn_->peer_conn_id();
    }

    /**
     * Get stream count.
     */
    size_t stream_count() const noexcept {
        return stream_states_.size();
    }

    /**
     * Get connection settings.
     */
    const Http3ConnectionSettings& settings() const noexcept {
        return settings_;
    }

    /**
     * Initialize connection (call after construction).
     *
     * @return 0 on success, -1 on error
     */
    int initialize() noexcept;

private:
    // Connection state
    Http3ConnectionState state_{Http3ConnectionState::IDLE};
    bool is_server_;

    // Settings
    Http3ConnectionSettings settings_;

    // QUIC connection (manages transport)
    std::unique_ptr<quic::QUICConnection> quic_conn_;

    // HTTP/3 parser
    HTTP3Parser http3_parser_;

    // QPACK encoder/decoder
    qpack::QPACKEncoder qpack_encoder_;
    qpack::QPACKDecoder qpack_decoder_;

    // Stream state tracking
    std::unordered_map<uint64_t, Http3StreamState> stream_states_;

    // Buffer pools (zero-allocation processing)
    Http3BufferPool<16384, 16> frame_buffer_pool_;  // For frame data
    Http3BufferPool<8192, 8> header_buffer_pool_;   // For header encoding

    // Request callback
    RequestCallback request_callback_;

    // Control streams
    uint64_t control_stream_id_{0};
    uint64_t qpack_encoder_stream_id_{0};
    uint64_t qpack_decoder_stream_id_{0};

    // Pending responses (keyed by stream ID)
    struct PendingResponse {
        uint16_t status;
        std::unordered_map<std::string, std::string> headers;
        std::string body;
    };
    std::unordered_map<uint64_t, PendingResponse> pending_responses_;

    // HTTP/3 frame processing
    int process_http3_stream(uint64_t stream_id, uint64_t now_us) noexcept;
    int handle_headers_frame(uint64_t stream_id, const uint8_t* data, size_t length) noexcept;
    int handle_data_frame(uint64_t stream_id, const uint8_t* data, size_t length) noexcept;
    int handle_settings_frame(const uint8_t* data, size_t length) noexcept;

    // Response generation
    int send_response_internal(
        uint64_t stream_id,
        uint16_t status,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body
    ) noexcept;

    // QPACK encoding/decoding
    int encode_headers(
        uint16_t status,
        const std::unordered_map<std::string, std::string>& headers,
        uint8_t* output,
        size_t capacity,
        size_t& out_length
    ) noexcept;

    int decode_headers(
        const uint8_t* data,
        size_t length,
        std::string& out_method,
        std::string& out_path,
        std::string& out_scheme,
        std::string& out_authority,
        std::unordered_map<std::string, std::string>& out_headers
    ) noexcept;

    // Control stream setup
    int setup_control_streams() noexcept;
    int send_settings() noexcept;

    // Stream state helpers
    Http3StreamState& get_or_create_stream_state(uint64_t stream_id) noexcept;
    void complete_request(uint64_t stream_id) noexcept;
};

} // namespace http
} // namespace fasterapi
