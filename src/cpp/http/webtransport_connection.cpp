#include "webtransport_connection.h"
#include "qpack/qpack_encoder.h"
#include "qpack/qpack_decoder.h"
#include "quic/quic_varint.h"
#include "http3_parser.h"
#include <cstring>
#include <algorithm>
#include <vector>
#include <cstdio>

namespace fasterapi {
namespace http {

namespace {

/**
 * Get current time in microseconds.
 */
uint64_t get_current_time_us() noexcept {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()
    ).count();
}

/**
 * WebTransport error codes (RFC 9297).
 */
constexpr uint64_t WT_ERROR_NO_ERROR = 0x00;
constexpr uint64_t WT_ERROR_INTERNAL_ERROR = 0x01;
constexpr uint64_t WT_ERROR_SESSION_CLOSED = 0x02;

/**
 * HTTP/3 frame types.
 */
constexpr uint8_t H3_FRAME_DATA = 0x00;
constexpr uint8_t H3_FRAME_HEADERS = 0x01;
constexpr uint8_t H3_FRAME_SETTINGS = 0x04;
constexpr uint8_t H3_FRAME_WEBTRANSPORT_STREAM = 0x41;

} // anonymous namespace

WebTransportConnection::WebTransportConnection(
    std::unique_ptr<quic::QUICConnection> quic_conn
) noexcept
    : state_(State::CONNECTING),
      is_server_(true),
      owns_quic_conn_(true),
      quic_conn_(std::move(quic_conn)),
      quic_conn_ptr_(quic_conn_.get()),
      session_stream_id_(0)
{
    pending_datagrams_.reserve(max_pending_datagrams_);
}

WebTransportConnection::WebTransportConnection(
    quic::QUICConnection* quic_conn,
    bool is_server
) noexcept
    : state_(State::CONNECTING),
      is_server_(is_server),
      owns_quic_conn_(false),
      quic_conn_(nullptr),
      quic_conn_ptr_(quic_conn),
      session_stream_id_(0)
{
    pending_datagrams_.reserve(max_pending_datagrams_);
}

WebTransportConnection::~WebTransportConnection() noexcept {
    if (!is_closed()) {
        close(WT_ERROR_NO_ERROR, "Connection destroyed");
    }
    // Note: quic_conn_ will be automatically cleaned up only if we own it
}

int WebTransportConnection::initialize() noexcept {
    if (!quic_conn_ptr_) {
        return -1;
    }

    // Initialize QUIC connection if needed
    if (!quic_conn_ptr_->is_established()) {
        quic_conn_ptr_->initialize();
    }

    return 0;
}

int WebTransportConnection::connect(const char* url) noexcept {
    if (is_server_) {
        return -1;  // Server cannot initiate connect
    }

    if (state_ != State::CONNECTING) {
        return -1;  // Already connected or closed
    }

    // Send HTTP/3 CONNECT request
    if (send_connect_request(url) != 0) {
        return -1;
    }

    return 0;
}

int WebTransportConnection::accept() noexcept {
    if (!is_server_) {
        return -1;  // Client cannot accept
    }

    if (state_ != State::CONNECTING) {
        return -1;  // Already connected or closed
    }

    // Wait for CONNECT request (handled in process_datagram)
    // For now, just mark as connected
    state_ = State::CONNECTED;

    return 0;
}

uint64_t WebTransportConnection::open_stream() noexcept {
    if (state_ != State::CONNECTED) {
        return 0;
    }

    // Create bidirectional QUIC stream
    uint64_t stream_id = quic_conn_ptr_->create_stream(true);
    if (stream_id == 0) {
        return 0;
    }

    // Track stream
    active_streams_[stream_id] = true;  // true = bidirectional
    total_streams_opened_.fetch_add(1, std::memory_order_relaxed);

    return stream_id;
}

ssize_t WebTransportConnection::send_stream(
    uint64_t stream_id,
    const uint8_t* data,
    size_t length
) noexcept {
    if (state_ != State::CONNECTED) {
        return -1;
    }

    // Verify stream exists and is bidirectional
    auto it = active_streams_.find(stream_id);
    if (it == active_streams_.end() || !it->second) {
        return -1;  // Stream not found or not bidirectional
    }

    // Write to QUIC stream
    ssize_t written = quic_conn_ptr_->write_stream(stream_id, data, length);
    if (written > 0) {
        total_bytes_sent_.fetch_add(written, std::memory_order_relaxed);
    }

    return written;
}

int WebTransportConnection::close_stream(uint64_t stream_id) noexcept {
    if (state_ != State::CONNECTED) {
        return -1;
    }

    // Close QUIC stream
    quic_conn_ptr_->close_stream(stream_id);

    // Remove from active streams
    active_streams_.erase(stream_id);

    // Notify callback
    if (stream_closed_callback_) {
        stream_closed_callback_(stream_id);
    }

    return 0;
}

uint64_t WebTransportConnection::open_unidirectional_stream() noexcept {
    if (state_ != State::CONNECTED) {
        return 0;
    }

    // Create unidirectional QUIC stream
    uint64_t stream_id = quic_conn_ptr_->create_stream(false);
    if (stream_id == 0) {
        return 0;
    }

    // Track stream
    active_streams_[stream_id] = false;  // false = unidirectional
    total_streams_opened_.fetch_add(1, std::memory_order_relaxed);

    return stream_id;
}

ssize_t WebTransportConnection::send_unidirectional(
    uint64_t stream_id,
    const uint8_t* data,
    size_t length
) noexcept {
    if (state_ != State::CONNECTED) {
        return -1;
    }

    // Verify stream exists and is unidirectional
    auto it = active_streams_.find(stream_id);
    if (it == active_streams_.end() || it->second) {
        return -1;  // Stream not found or not unidirectional
    }

    // Write to QUIC stream
    ssize_t written = quic_conn_ptr_->write_stream(stream_id, data, length);
    if (written > 0) {
        total_bytes_sent_.fetch_add(written, std::memory_order_relaxed);
    }

    return written;
}

int WebTransportConnection::close_unidirectional_stream(uint64_t stream_id) noexcept {
    return close_stream(stream_id);  // Same implementation
}

int WebTransportConnection::send_datagram(const uint8_t* data, size_t length) noexcept {
    if (state_ != State::CONNECTED) {
        return -1;
    }

    // Check datagram size (must fit in single packet)
    constexpr size_t MAX_DATAGRAM_SIZE = 1200;  // Conservative MTU
    if (length > MAX_DATAGRAM_SIZE) {
        return -1;
    }

    // Enqueue datagram
    if (enqueue_datagram(data, length) != 0) {
        return -1;
    }

    total_datagrams_sent_.fetch_add(1, std::memory_order_relaxed);
    total_bytes_sent_.fetch_add(length, std::memory_order_relaxed);

    return 0;
}

int WebTransportConnection::enqueue_datagram(const uint8_t* data, size_t length) noexcept {
    // Check queue capacity
    if (pending_datagrams_.size() >= max_pending_datagrams_) {
        return -1;  // Queue full
    }

    // Add to pending datagrams
    DatagramItem item;
    std::memcpy(item.data, data, length);
    item.length = length;
    pending_datagrams_.push_back(item);

    return 0;
}

int WebTransportConnection::process_datagram(
    const uint8_t* data,
    size_t length,
    uint64_t now_us
) noexcept {
    if (!quic_conn_) {
        return -1;
    }

    // Process through QUIC connection
    int result = quic_conn_ptr_->process_packet(data, length, now_us);
    if (result != 0) {
        return result;
    }

    // Process all streams for received data
    // This is a simplification - in reality we'd iterate through streams
    // that have pending data and dispatch callbacks

    // Check if connection is now established
    if (state_ == State::CONNECTING && quic_conn_ptr_->is_established()) {
        state_ = State::CONNECTED;
    }

    // Check if connection is closed
    if (quic_conn_ptr_->is_closed() && state_ != State::CLOSED) {
        state_ = State::CLOSED;
        if (connection_closed_callback_) {
            connection_closed_callback_(WT_ERROR_SESSION_CLOSED, "QUIC connection closed");
        }
    }

    return 0;
}

size_t WebTransportConnection::generate_datagrams(
    uint8_t* output,
    size_t capacity,
    uint64_t now_us
) noexcept {
    if (!quic_conn_) {
        return 0;
    }

    size_t total_written = 0;

    // Send pending datagrams first
    while (!pending_datagrams_.empty() && total_written < capacity) {
        const auto& item = pending_datagrams_.front();

        // Create DATAGRAM frame
        quic::DatagramFrame frame;
        frame.data = item.data;
        frame.length = item.length;

        // Calculate frame size
        uint8_t frame_buffer[2048];
        size_t frame_size = frame.serialize(frame_buffer, true);

        // Check if it fits
        if (total_written + frame_size > capacity) {
            break;  // Doesn't fit
        }

        // Copy to output
        std::memcpy(output + total_written, frame_buffer, frame_size);
        total_written += frame_size;

        // Remove from queue
        pending_datagrams_.erase(pending_datagrams_.begin());
    }

    // Generate QUIC packets for streams
    if (total_written < capacity) {
        size_t quic_written = quic_conn_ptr_->generate_packets(
            output + total_written,
            capacity - total_written,
            now_us
        );
        total_written += quic_written;
    }

    return total_written;
}

void WebTransportConnection::close(uint64_t error_code, const char* reason) noexcept {
    if (state_ == State::CLOSED) {
        return;
    }

    state_ = State::CLOSING;

    // Close all active streams
    for (const auto& [stream_id, _] : active_streams_) {
        quic_conn_ptr_->close_stream(stream_id);
    }
    active_streams_.clear();

    // Close QUIC connection
    if (quic_conn_) {
        quic_conn_ptr_->close(error_code, reason);
    }

    state_ = State::CLOSED;

    // Notify callback
    if (connection_closed_callback_) {
        connection_closed_callback_(error_code, reason ? reason : "");
    }
}

std::unordered_map<std::string, uint64_t> WebTransportConnection::get_stats() const noexcept {
    std::unordered_map<std::string, uint64_t> stats;

    stats["streams_opened"] = total_streams_opened_.load(std::memory_order_relaxed);
    stats["datagrams_sent"] = total_datagrams_sent_.load(std::memory_order_relaxed);
    stats["datagrams_received"] = total_datagrams_received_.load(std::memory_order_relaxed);
    stats["bytes_sent"] = total_bytes_sent_.load(std::memory_order_relaxed);
    stats["bytes_received"] = total_bytes_received_.load(std::memory_order_relaxed);
    stats["active_streams"] = active_streams_.size();
    stats["pending_datagrams"] = pending_datagrams_.size();

    return stats;
}

void WebTransportConnection::process_stream_data(
    uint64_t stream_id,
    const uint8_t* data,
    size_t length
) noexcept {
    total_bytes_received_.fetch_add(length, std::memory_order_relaxed);

    // Check if this is a new stream from peer
    if (active_streams_.find(stream_id) == active_streams_.end()) {
        if (is_peer_initiated_stream(stream_id)) {
            handle_peer_stream_opened(stream_id);
        }
    }

    // Dispatch to appropriate callback
    if (is_bidirectional_stream(stream_id)) {
        if (stream_data_callback_) {
            stream_data_callback_(stream_id, data, length);
        }
    } else {
        if (unidirectional_data_callback_) {
            unidirectional_data_callback_(stream_id, data, length);
        }
    }
}

void WebTransportConnection::process_datagram_frame(
    const quic::DatagramFrame& frame
) noexcept {
    total_datagrams_received_.fetch_add(1, std::memory_order_relaxed);
    total_bytes_received_.fetch_add(frame.length, std::memory_order_relaxed);

    // Dispatch to callback
    if (datagram_callback_) {
        datagram_callback_(frame.data, frame.length);
    }
}

void WebTransportConnection::handle_peer_stream_opened(uint64_t stream_id) noexcept {
    bool is_bidi = is_bidirectional_stream(stream_id);

    // Track stream
    active_streams_[stream_id] = is_bidi;
    total_streams_opened_.fetch_add(1, std::memory_order_relaxed);

    // Notify callback
    if (stream_opened_callback_) {
        stream_opened_callback_(stream_id, is_bidi);
    }
}

void WebTransportConnection::handle_stream_closed(uint64_t stream_id) noexcept {
    // Remove from active streams
    active_streams_.erase(stream_id);

    // Notify callback
    if (stream_closed_callback_) {
        stream_closed_callback_(stream_id);
    }
}

int WebTransportConnection::send_connect_request(const char* url) noexcept {
    // Create control stream (stream 0 or 2 for client)
    session_stream_id_ = quic_conn_ptr_->create_stream(true);
    if (session_stream_id_ == 0) {
        return -1;
    }

    // Parse URL to extract path and authority
    std::string url_str(url);
    std::string scheme = "https";
    std::string authority;
    std::string path = "/";

    // Simple URL parsing
    size_t scheme_end = url_str.find("://");
    size_t path_start = std::string::npos;
    if (scheme_end != std::string::npos) {
        scheme = url_str.substr(0, scheme_end);
        size_t auth_start = scheme_end + 3;
        path_start = url_str.find('/', auth_start);
        if (path_start != std::string::npos) {
            authority = url_str.substr(auth_start, path_start - auth_start);
            path = url_str.substr(path_start);
        } else {
            authority = url_str.substr(auth_start);
        }
    }

    // Build HTTP/3 CONNECT request headers using QPACK
    std::vector<std::pair<std::string_view, std::string_view>> headers = {
        {":method", "CONNECT"},
        {":protocol", "webtransport"},
        {":scheme", scheme},
        {":path", path},
        {":authority", authority}
    };

    // QPACK encode the headers
    qpack::QPACKEncoder encoder{4096, 100};
    uint8_t qpack_buffer[4096];
    size_t qpack_len = 0;

    if (encoder.encode_field_section(headers.data(), headers.size(),
                                     qpack_buffer, sizeof(qpack_buffer), qpack_len) != 0) {
        return -1;
    }

    // Build HTTP/3 HEADERS frame
    uint8_t frame_buffer[8192];
    size_t pos = 0;

    // Frame type (HEADERS = 0x01)
    pos += quic::VarInt::encode(H3_FRAME_HEADERS, frame_buffer + pos);

    // Frame length
    pos += quic::VarInt::encode(qpack_len, frame_buffer + pos);

    // Frame payload (QPACK encoded headers)
    std::memcpy(frame_buffer + pos, qpack_buffer, qpack_len);
    pos += qpack_len;

    // Send on session stream
    ssize_t written = quic_conn_ptr_->write_stream(session_stream_id_, frame_buffer, pos);
    if (written <= 0) {
        return -1;
    }

    return 0;
}

int WebTransportConnection::send_connect_response(
    uint64_t stream_id,
    int status_code
) noexcept {
    // Build HTTP/3 response with :status pseudo-header
    char status_str[8];
    snprintf(status_str, sizeof(status_str), "%d", status_code);

    std::vector<std::pair<std::string_view, std::string_view>> headers = {
        {":status", status_str}
    };

    // Add sec-webtransport-http3-draft header for WebTransport capability
    if (status_code == 200) {
        headers.push_back({"sec-webtransport-http3-draft", "draft02"});
    }

    // QPACK encode the headers
    qpack::QPACKEncoder encoder{4096, 100};
    uint8_t qpack_buffer[1024];
    size_t qpack_len = 0;

    if (encoder.encode_field_section(headers.data(), headers.size(),
                                     qpack_buffer, sizeof(qpack_buffer), qpack_len) != 0) {
        return -1;
    }

    // Build HTTP/3 HEADERS frame
    uint8_t frame_buffer[2048];
    size_t pos = 0;

    // Frame type (HEADERS = 0x01)
    pos += quic::VarInt::encode(H3_FRAME_HEADERS, frame_buffer + pos);

    // Frame length
    pos += quic::VarInt::encode(qpack_len, frame_buffer + pos);

    // Frame payload (QPACK encoded headers)
    std::memcpy(frame_buffer + pos, qpack_buffer, qpack_len);
    pos += qpack_len;

    // Send on stream
    ssize_t written = quic_conn_ptr_->write_stream(stream_id, frame_buffer, pos);
    if (written <= 0) {
        return -1;
    }

    // Transition to connected state on successful 200 response
    if (status_code == 200) {
        state_ = State::CONNECTED;
        session_stream_id_ = stream_id;
    }

    return 0;
}

} // namespace http
} // namespace fasterapi
