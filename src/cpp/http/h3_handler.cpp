#include "h3_handler.h"
#include "quic/quic_varint.h"
#include <iostream>
#include <cstring>
#include <chrono>

namespace fasterapi {
namespace http {

Http3Handler::Http3Handler(const Settings& settings)
    : settings_(settings),
      running_(false),
      qpack_encoder_(settings.qpack_max_table_capacity, 
                    settings.qpack_blocked_streams),
      total_requests_(0),
      total_bytes_sent_(0),
      total_bytes_received_(0),
      active_streams_(0),
      push_responses_(0),
      quic_connections_(0) {
}

Http3Handler::~Http3Handler() {
    stop();
}

int Http3Handler::initialize() noexcept {
    std::cout << "HTTP/3 handler initialized (pure implementation)" << std::endl;
    std::cout << "  QPACK max table capacity: " << settings_.qpack_max_table_capacity << std::endl;
    std::cout << "  Max header list size: " << settings_.max_header_list_size << std::endl;
    std::cout << "  Connection window: " << settings_.connection_window_size << std::endl;
    return 0;
}

int Http3Handler::add_route(const std::string& method, const std::string& path,
                           RouteHandler handler) noexcept {
    if (running_.load()) {
        return -1;  // Cannot add routes while running
    }
    
    std::string key = method + ":" + path;
    routes_[key] = std::move(handler);
    
    std::cout << "Added HTTP/3 route: " << method << " " << path << std::endl;
    return 0;
}

int Http3Handler::process_datagram(const uint8_t* data, size_t length,
                                  const void* source_addr, uint64_t now) noexcept {
    if (!running_.load()) {
        return -1;
    }
    
    total_bytes_received_.fetch_add(length);
    
    // Extract connection ID from packet
    if (length < 1) return -1;
    
    // Parse packet to get connection ID
    quic::ConnectionID conn_id;
    if ((data[0] & 0x80) != 0) {
        // Long header: skip to DCID
        if (length < 6) return -1;
        size_t pos = 5;  // After version
        uint8_t dcid_len = data[pos++];
        if (length < pos + dcid_len) return -1;
        conn_id = quic::ConnectionID(data + pos, dcid_len);
    } else {
        // Short header: assume 8-byte connection ID (simplified)
        if (length < 9) return -1;
        conn_id = quic::ConnectionID(data + 1, 8);
    }
    
    // Get or create connection
    quic::QUICConnection* conn = get_or_create_connection(conn_id, source_addr);
    if (!conn) return -1;
    
    // Process packet
    int result = conn->process_packet(data, length, now);
    
    // Process HTTP/3 streams
    // For each bidirectional stream, check if we have HTTP/3 data
    for (size_t stream_id = 0; stream_id < 1000; stream_id += 4) {
        quic::QUICStream* stream = conn->get_stream(stream_id);
        if (stream && stream->recv_buffer().available() > 0) {
            process_http3_stream(conn, stream_id, now);
        }
    }
    
    return result;
}

size_t Http3Handler::generate_datagrams(uint8_t* output, size_t capacity,
                                       void** dest_addr, uint64_t now) noexcept {
    if (!running_.load()) {
        return 0;
    }
    
    size_t total_written = 0;
    
    // Generate packets for all connections
    for (auto& [conn_id_str, conn] : connections_) {
        if (conn->is_closed()) continue;
        
        size_t written = conn->generate_packets(output + total_written,
                                                capacity - total_written, now);
        if (written > 0) {
            total_bytes_sent_.fetch_add(written);
            total_written += written;
        }
        
        if (total_written >= capacity) break;
    }
    
    return total_written;
}

int Http3Handler::send_response(uint64_t stream_id, const Response& response) noexcept {
    // Encode response to HTTP/3 frames
    uint8_t encoded[65536];  // 64KB buffer
    size_t encoded_len;
    
    if (encode_response(response, encoded, sizeof(encoded), encoded_len) != 0) {
        return -1;
    }
    
    // Find connection for this stream
    // Simplified: assume first connection
    if (connections_.empty()) return -1;
    
    auto* conn = connections_.begin()->second.get();
    
    // Write to stream
    conn->write_stream(stream_id, encoded, encoded_len);
    
    total_requests_.fetch_add(1);
    
    return 0;
}

int Http3Handler::send_push(uint64_t stream_id, const std::string& path,
                           const Response& response) noexcept {
    // Server push implementation
    // Simplified: create new push stream and send response
    
    if (connections_.empty()) return -1;
    auto* conn = connections_.begin()->second.get();
    
    // Create unidirectional push stream
    uint64_t push_stream_id = conn->create_stream(false);
    if (push_stream_id == 0) return -1;
    
    // Send PUSH_PROMISE frame on request stream
    uint8_t push_promise[1024];
    size_t pos = 0;
    
    // Frame type: PUSH_PROMISE (0x05)
    pos += quic::VarInt::encode(0x05, push_promise + pos);
    
    // Frame length (placeholder)
    pos += quic::VarInt::encode(0, push_promise + pos);
    
    // Push ID
    pos += quic::VarInt::encode(push_stream_id, push_promise + pos);
    
    // Encode headers for promised resource
    std::pair<std::string_view, std::string_view> headers[] = {
        {":method", "GET"},
        {":path", path},
    };
    
    size_t header_len;
    if (qpack_encoder_.encode_field_section(
            headers, 2, push_promise + pos, sizeof(push_promise) - pos, 
            header_len) != 0) {
        return -1;
    }
    pos += header_len;
    
    // Send PUSH_PROMISE frame
    conn->write_stream(stream_id, push_promise, pos);
    
    // Send response on push stream
    send_response(push_stream_id, response);
    
    push_responses_.fetch_add(1);
    
    return 0;
}

std::unordered_map<std::string, uint64_t> Http3Handler::get_stats() const noexcept {
    std::unordered_map<std::string, uint64_t> stats;
    stats["total_requests"] = total_requests_.load();
    stats["total_bytes_sent"] = total_bytes_sent_.load();
    stats["total_bytes_received"] = total_bytes_received_.load();
    stats["active_streams"] = active_streams_.load();
    stats["push_responses"] = push_responses_.load();
    stats["quic_connections"] = quic_connections_.load();
    return stats;
}

quic::QUICConnection* Http3Handler::get_or_create_connection(
    const quic::ConnectionID& conn_id,
    const void* source_addr
) noexcept {
    // Convert connection ID to string key
    std::string key(reinterpret_cast<const char*>(conn_id.data), conn_id.length);
    
    auto it = connections_.find(key);
    if (it != connections_.end()) {
        return it->second.get();
    }
    
    // Create new connection
    quic::ConnectionID local_id;
    local_id.length = 8;
    // Generate random connection ID (simplified)
    for (int i = 0; i < 8; i++) {
        local_id.data[i] = rand() & 0xFF;
    }
    
    auto conn = std::make_unique<quic::QUICConnection>(true, local_id, conn_id);
    auto* conn_ptr = conn.get();
    
    connections_[key] = std::move(conn);
    quic_connections_.fetch_add(1);
    
    std::cout << "Created new QUIC connection (ID: " << key.length() << " bytes)" << std::endl;
    
    return conn_ptr;
}

void Http3Handler::process_http3_stream(quic::QUICConnection* conn,
                                       uint64_t stream_id,
                                       uint64_t now) noexcept {
    quic::QUICStream* stream = conn->get_stream(stream_id);
    if (!stream) return;
    
    // Read data from stream
    uint8_t buffer[65536];
    ssize_t read = stream->read(buffer, sizeof(buffer));
    if (read <= 0) return;
    
    // Parse HTTP/3 frames
    size_t pos = 0;
    while (pos < static_cast<size_t>(read)) {
        HTTP3FrameHeader header;
        size_t consumed;
        
        int result = parser_.parse_frame_header(buffer + pos, read - pos, 
                                               header, consumed);
        if (result != 0) break;
        
        pos += consumed;
        
        // Check if we have full frame
        if (pos + header.length > static_cast<size_t>(read)) break;
        
        // Handle frame based on type
        switch (header.type) {
            case HTTP3FrameType::HEADERS:
                handle_headers_frame(stream_id, buffer + pos, header.length);
                break;
                
            case HTTP3FrameType::DATA:
                handle_data_frame(stream_id, buffer + pos, header.length);
                break;
                
            case HTTP3FrameType::SETTINGS:
                handle_settings_frame(buffer + pos, header.length);
                break;
                
            default:
                // Ignore unknown frames
                break;
        }
        
        pos += header.length;
    }
}

void Http3Handler::handle_headers_frame(uint64_t stream_id, const uint8_t* data,
                                       size_t length) noexcept {
    // Decode QPACK headers
    std::pair<std::string, std::string> headers[256];
    size_t header_count;
    
    if (parser_.parse_headers(data, length, headers, header_count) != 0) {
        return;
    }
    
    // Build request from headers
    Request request;
    request.stream_id = stream_id;
    
    for (size_t i = 0; i < header_count; i++) {
        const auto& [name, value] = headers[i];
        
        if (name == ":method") {
            request.method = value;
        } else if (name == ":path") {
            request.path = value;
        } else if (name == ":scheme") {
            request.scheme = value;
        } else if (name == ":authority") {
            request.authority = value;
        } else {
            request.headers[name] = value;
        }
    }
    
    // Store pending request (wait for DATA frame if present)
    pending_requests_[stream_id] = std::move(request);
    
    active_streams_.fetch_add(1);
}

void Http3Handler::handle_data_frame(uint64_t stream_id, const uint8_t* data,
                                    size_t length) noexcept {
    auto it = pending_requests_.find(stream_id);
    if (it == pending_requests_.end()) {
        return;  // No headers yet
    }
    
    // Append data to request body
    it->second.body.insert(it->second.body.end(), data, data + length);
}

void Http3Handler::handle_settings_frame(const uint8_t* data, size_t length) noexcept {
    HTTP3Settings settings;
    parser_.parse_settings(data, length, settings);
    
    std::cout << "Received HTTP/3 SETTINGS:" << std::endl;
    std::cout << "  QPACK max table capacity: " << settings.qpack_max_table_capacity << std::endl;
    std::cout << "  Max header list size: " << settings.max_header_list_size << std::endl;
}

void Http3Handler::dispatch_request(const Request& request) noexcept {
    // Find matching route
    std::string key = request.method + ":" + request.path;
    
    auto it = routes_.find(key);
    if (it == routes_.end()) {
        // No route found, send 404
        Response response;
        response.status = 404;
        response.body = {'{', '"', 'e', 'r', 'r', 'o', 'r', '"', ':', '"', 
                        'N', 'o', 't', ' ', 'F', 'o', 'u', 'n', 'd', '"', '}'};
        send_response(request.stream_id, response);
        return;
    }
    
    // Call route handler
    Response response;
    it->second(request, response);
    
    // Send response
    send_response(request.stream_id, response);
}

int Http3Handler::encode_response(const Response& response, uint8_t* output,
                                 size_t capacity, size_t& out_length) noexcept {
    size_t pos = 0;
    
    // Encode HEADERS frame
    pos += quic::VarInt::encode(static_cast<uint64_t>(HTTP3FrameType::HEADERS), 
                               output + pos);
    
    // Prepare headers for QPACK encoding
    std::vector<std::pair<std::string_view, std::string_view>> headers;
    
    // Add :status pseudo-header
    std::string status_str = std::to_string(response.status);
    headers.emplace_back(":status", status_str);
    
    // Add other headers
    for (const auto& [name, value] : response.headers) {
        headers.emplace_back(name, value);
    }
    
    // Encode headers with QPACK
    uint8_t qpack_buffer[8192];
    size_t qpack_len;
    
    if (qpack_encoder_.encode_field_section(headers.data(), headers.size(),
                                           qpack_buffer, sizeof(qpack_buffer),
                                           qpack_len) != 0) {
        return -1;
    }
    
    // Write HEADERS frame length
    pos += quic::VarInt::encode(qpack_len, output + pos);
    
    // Write QPACK-encoded headers
    std::memcpy(output + pos, qpack_buffer, qpack_len);
    pos += qpack_len;
    
    // Encode DATA frame if we have body
    if (!response.body.empty()) {
        pos += quic::VarInt::encode(static_cast<uint64_t>(HTTP3FrameType::DATA),
                                   output + pos);
        pos += quic::VarInt::encode(response.body.size(), output + pos);
        std::memcpy(output + pos, response.body.data(), response.body.size());
        pos += response.body.size();
    }
    
    out_length = pos;
    return 0;
}

} // namespace http
} // namespace fasterapi
