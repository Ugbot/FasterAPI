#include "http3_connection.h"
#include "quic/quic_varint.h"
#include <cstring>
#include <algorithm>
#include <iostream>

namespace fasterapi {
namespace http {

using core::result;
using core::error_code;

// HTTP/3 frame types (RFC 9114)
static constexpr uint64_t kFrameTypeData = 0x00;
static constexpr uint64_t kFrameTypeHeaders = 0x01;
static constexpr uint64_t kFrameTypeCancelPush = 0x03;
static constexpr uint64_t kFrameTypeSettings = 0x04;
static constexpr uint64_t kFrameTypePushPromise = 0x05;
static constexpr uint64_t kFrameTypeGoaway = 0x07;
static constexpr uint64_t kFrameTypeMaxPushId = 0x0D;

// HTTP/3 settings IDs (RFC 9114)
static constexpr uint64_t kSettingsMaxHeaderListSize = 0x06;
static constexpr uint64_t kSettingsQpackMaxTableCapacity = 0x01;
static constexpr uint64_t kSettingsQpackBlockedStreams = 0x07;

// Stream type IDs (RFC 9114)
static constexpr uint64_t kStreamTypeControl = 0x00;
static constexpr uint64_t kStreamTypePush = 0x01;
static constexpr uint64_t kStreamTypeQpackEncoder = 0x02;
static constexpr uint64_t kStreamTypeQpackDecoder = 0x03;

Http3Connection::Http3Connection(
    bool is_server,
    const quic::ConnectionID& local_conn_id,
    const quic::ConnectionID& peer_conn_id,
    const Http3ConnectionSettings& settings
)
    : state_(Http3ConnectionState::IDLE),
      is_server_(is_server),
      settings_(settings),
      qpack_encoder_(settings.qpack_max_table_capacity, settings.qpack_blocked_streams),
      qpack_decoder_(settings.qpack_max_table_capacity) {

    // Create QUIC connection
    quic_conn_ = std::make_unique<quic::QUICConnection>(is_server, local_conn_id, peer_conn_id);
}

Http3Connection::~Http3Connection() {
    if (!is_closed()) {
        close();
    }
}

int Http3Connection::initialize() noexcept {
    // Initialize QUIC connection
    quic_conn_->initialize();

    if (is_server_) {
        // Server waits for client to initiate
        state_ = Http3ConnectionState::HANDSHAKE;
    } else {
        // Client initiates connection
        state_ = Http3ConnectionState::HANDSHAKE;
    }

    std::cout << "[HTTP/3] Connection initialized (is_server=" << is_server_ << ")" << std::endl;
    return 0;
}

int Http3Connection::process_datagram(const uint8_t* data, size_t length, uint64_t now_us) noexcept {
    if (state_ == Http3ConnectionState::CLOSED) {
        return -1;
    }

    // Process QUIC packet
    int result = quic_conn_->process_packet(data, length, now_us);
    if (result != 0) {
        std::cerr << "[HTTP/3] QUIC packet processing failed" << std::endl;
        return result;
    }

    // Transition to ACTIVE once QUIC is established
    if (state_ == Http3ConnectionState::HANDSHAKE && quic_conn_->is_established()) {
        state_ = Http3ConnectionState::ACTIVE;
        std::cout << "[HTTP/3] Connection established, transitioning to ACTIVE" << std::endl;

        // Setup HTTP/3 control streams
        if (setup_control_streams() != 0) {
            std::cerr << "[HTTP/3] Failed to setup control streams" << std::endl;
            return -1;
        }

        // Send SETTINGS frame
        if (send_settings() != 0) {
            std::cerr << "[HTTP/3] Failed to send SETTINGS" << std::endl;
            return -1;
        }
    }

    // Process HTTP/3 streams if active
    if (state_ == Http3ConnectionState::ACTIVE) {
        // Check all streams for HTTP/3 data
        // Iterate through active stream states and process any with pending data
        std::vector<uint64_t> streams_to_process;

        // Collect streams that need processing
        for (auto& [stream_id, stream_state] : stream_states_) {
            quic::QUICStream* stream = quic_conn_->get_stream(stream_id);
            if (stream && stream->recv_buffer().available() > 0) {
                streams_to_process.push_back(stream_id);
            }
        }

        // Also check for new streams (client-initiated requests)
        // Check stream IDs 0, 4, 8, 12, ... (client-initiated bidirectional in server mode)
        if (is_server_) {
            for (uint64_t stream_id = 0; stream_id < 1000; stream_id += 4) {
                quic::QUICStream* stream = quic_conn_->get_stream(stream_id);
                if (stream && stream->recv_buffer().available() > 0) {
                    if (stream_states_.find(stream_id) == stream_states_.end()) {
                        // New stream
                        streams_to_process.push_back(stream_id);
                    }
                }
            }
        }

        // Process streams
        for (uint64_t stream_id : streams_to_process) {
            if (process_http3_stream(stream_id, now_us) != 0) {
                std::cerr << "[HTTP/3] Stream " << stream_id << " processing failed" << std::endl;
            }
        }
    }

    return 0;
}

size_t Http3Connection::generate_datagrams(uint8_t* output, size_t capacity, uint64_t now_us) noexcept {
    if (state_ == Http3ConnectionState::CLOSED) {
        return 0;
    }

    // Generate pending responses before QUIC packet generation
    for (auto it = pending_responses_.begin(); it != pending_responses_.end(); ) {
        uint64_t stream_id = it->first;
        auto& pending = it->second;

        if (send_response_internal(stream_id, pending.status, pending.headers, pending.body) == 0) {
            it = pending_responses_.erase(it);
        } else {
            ++it;
        }
    }

    // Generate QUIC packets
    return quic_conn_->generate_packets(output, capacity, now_us);
}

void Http3Connection::close(uint64_t error_code, const char* reason) noexcept {
    if (state_ == Http3ConnectionState::CLOSED) {
        return;
    }

    std::cout << "[HTTP/3] Closing connection (error=" << error_code << ")" << std::endl;

    state_ = Http3ConnectionState::CLOSING;
    quic_conn_->close(error_code, reason);
    state_ = Http3ConnectionState::CLOSED;
}

int Http3Connection::setup_control_streams() noexcept {
    if (!is_server_) {
        // Client creates control stream (unidirectional)
        control_stream_id_ = quic_conn_->create_stream(false);
        if (control_stream_id_ == 0) {
            return -1;
        }

        // Write stream type
        uint8_t stream_type[8];
        size_t type_len = quic::VarInt::encode(kStreamTypeControl, stream_type);
        quic_conn_->write_stream(control_stream_id_, stream_type, type_len);

        std::cout << "[HTTP/3] Created control stream: " << control_stream_id_ << std::endl;
    }

    return 0;
}

int Http3Connection::send_settings() noexcept {
    // Encode SETTINGS frame
    uint8_t settings_buf[256];
    size_t pos = 0;

    // Frame type: SETTINGS
    pos += quic::VarInt::encode(kFrameTypeSettings, settings_buf + pos);

    // Frame length (placeholder, will update)
    size_t length_pos = pos;
    pos += quic::VarInt::encode(0, settings_buf + pos);

    size_t payload_start = pos;

    // SETTINGS_MAX_HEADER_LIST_SIZE
    pos += quic::VarInt::encode(kSettingsMaxHeaderListSize, settings_buf + pos);
    pos += quic::VarInt::encode(settings_.max_header_list_size, settings_buf + pos);

    // SETTINGS_QPACK_MAX_TABLE_CAPACITY
    pos += quic::VarInt::encode(kSettingsQpackMaxTableCapacity, settings_buf + pos);
    pos += quic::VarInt::encode(settings_.qpack_max_table_capacity, settings_buf + pos);

    // SETTINGS_QPACK_BLOCKED_STREAMS
    pos += quic::VarInt::encode(kSettingsQpackBlockedStreams, settings_buf + pos);
    pos += quic::VarInt::encode(settings_.qpack_blocked_streams, settings_buf + pos);

    // Update frame length
    size_t payload_len = pos - payload_start;
    uint8_t length_encoded[8];
    size_t length_encoded_len = quic::VarInt::encode(payload_len, length_encoded);

    // Shift payload if length encoding changed size
    if (length_encoded_len != (payload_start - length_pos)) {
        std::memmove(settings_buf + length_pos + length_encoded_len,
                    settings_buf + payload_start,
                    payload_len);
        std::memcpy(settings_buf + length_pos, length_encoded, length_encoded_len);
        pos = length_pos + length_encoded_len + payload_len;
    }

    // Send on control stream
    if (control_stream_id_ != 0) {
        ssize_t written = quic_conn_->write_stream(control_stream_id_, settings_buf, pos);
        if (written < 0) {
            return -1;
        }
    }

    std::cout << "[HTTP/3] Sent SETTINGS frame (" << pos << " bytes)" << std::endl;
    return 0;
}

int Http3Connection::process_http3_stream(uint64_t stream_id, uint64_t now_us) noexcept {
    quic::QUICStream* stream = quic_conn_->get_stream(stream_id);
    if (!stream) {
        return -1;
    }

    // Get or create stream state
    Http3StreamState& state = get_or_create_stream_state(stream_id);

    // Read available data
    uint8_t buffer[16384];
    ssize_t read_len = quic_conn_->read_stream(stream_id, buffer, sizeof(buffer));
    if (read_len <= 0) {
        return 0;  // No data or error
    }

    // Parse HTTP/3 frames
    size_t pos = 0;
    while (pos < static_cast<size_t>(read_len)) {
        // Read frame type
        uint64_t frame_type;
        int consumed = quic::VarInt::decode(buffer + pos, read_len - pos, frame_type);
        if (consumed < 0) {
            std::cerr << "[HTTP/3] Failed to decode frame type" << std::endl;
            return -1;
        }
        pos += consumed;

        // Read frame length
        uint64_t frame_length;
        consumed = quic::VarInt::decode(buffer + pos, read_len - pos, frame_length);
        if (consumed < 0) {
            std::cerr << "[HTTP/3] Failed to decode frame length" << std::endl;
            return -1;
        }
        pos += consumed;

        // Check if we have complete frame
        if (pos + frame_length > static_cast<size_t>(read_len)) {
            std::cerr << "[HTTP/3] Incomplete frame (need " << frame_length
                     << " bytes, have " << (read_len - pos) << ")" << std::endl;
            // Would need to buffer partial frame - simplified for now
            return 0;
        }

        // Dispatch frame
        const uint8_t* frame_data = buffer + pos;

        switch (frame_type) {
        case kFrameTypeHeaders:
            if (handle_headers_frame(stream_id, frame_data, frame_length) != 0) {
                return -1;
            }
            state.headers_complete = true;
            break;

        case kFrameTypeData:
            if (handle_data_frame(stream_id, frame_data, frame_length) != 0) {
                return -1;
            }
            break;

        case kFrameTypeSettings:
            if (handle_settings_frame(frame_data, frame_length) != 0) {
                return -1;
            }
            break;

        default:
            std::cout << "[HTTP/3] Ignoring unknown frame type: " << frame_type << std::endl;
            break;
        }

        pos += frame_length;
    }

    // Check if request is complete
    auto stream_state = stream->state();
    if (state.headers_complete &&
        (stream_state == quic::StreamState::RECV_CLOSED || stream_state == quic::StreamState::CLOSED)) {
        complete_request(stream_id);
    }

    return 0;
}

int Http3Connection::handle_headers_frame(uint64_t stream_id, const uint8_t* data, size_t length) noexcept {
    Http3StreamState& state = get_or_create_stream_state(stream_id);

    // Decode QPACK headers
    std::pair<std::string, std::string> headers[256];
    size_t header_count;

    if (qpack_decoder_.decode_field_section(data, length, headers, header_count) != 0) {
        std::cerr << "[HTTP/3] QPACK decode failed" << std::endl;
        return -1;
    }

    std::cout << "[HTTP/3] Decoded " << header_count << " headers on stream " << stream_id << std::endl;

    // Extract pseudo-headers and regular headers
    for (size_t i = 0; i < header_count; ++i) {
        const auto& [name, value] = headers[i];

        if (name == ":method") {
            state.method = value;
        } else if (name == ":path") {
            state.path = value;
        } else if (name == ":scheme") {
            state.scheme = value;
        } else if (name == ":authority") {
            state.authority = value;
        } else if (name[0] != ':') {
            // Regular header
            state.headers[name] = value;
        }
    }

    std::cout << "[HTTP/3] Request: " << state.method << " " << state.path << std::endl;

    return 0;
}

int Http3Connection::handle_data_frame(uint64_t stream_id, const uint8_t* data, size_t length) noexcept {
    Http3StreamState& state = get_or_create_stream_state(stream_id);

    // Append to body
    state.body.insert(state.body.end(), data, data + length);

    std::cout << "[HTTP/3] Received " << length << " bytes of data on stream " << stream_id
             << " (total: " << state.body.size() << ")" << std::endl;

    return 0;
}

int Http3Connection::handle_settings_frame(const uint8_t* data, size_t length) noexcept {
    std::cout << "[HTTP/3] Received SETTINGS frame (" << length << " bytes)" << std::endl;

    // Parse settings
    size_t pos = 0;
    while (pos < length) {
        uint64_t setting_id;
        int consumed = quic::VarInt::decode(data + pos, length - pos, setting_id);
        if (consumed < 0) {
            return -1;
        }
        pos += consumed;

        uint64_t value;
        consumed = quic::VarInt::decode(data + pos, length - pos, value);
        if (consumed < 0) {
            return -1;
        }
        pos += consumed;

        std::cout << "[HTTP/3] Setting: " << setting_id << " = " << value << std::endl;

        // Apply settings (simplified - just log for now)
        switch (setting_id) {
        case kSettingsMaxHeaderListSize:
            std::cout << "[HTTP/3] Peer max header list size: " << value << std::endl;
            break;
        case kSettingsQpackMaxTableCapacity:
            std::cout << "[HTTP/3] Peer QPACK max table capacity: " << value << std::endl;
            break;
        case kSettingsQpackBlockedStreams:
            std::cout << "[HTTP/3] Peer QPACK blocked streams: " << value << std::endl;
            break;
        default:
            break;
        }
    }

    return 0;
}

void Http3Connection::complete_request(uint64_t stream_id) noexcept {
    auto it = stream_states_.find(stream_id);
    if (it == stream_states_.end()) {
        return;
    }

    Http3StreamState& state = it->second;
    if (state.request_complete) {
        return;  // Already processed
    }

    state.request_complete = true;

    std::cout << "[HTTP/3] Request complete on stream " << stream_id << std::endl;

    // Invoke request callback if set
    if (request_callback_) {
        // Convert body to string
        std::string body_str(state.body.begin(), state.body.end());

        // Create send_response callback
        auto send_response = [this, stream_id](
            uint16_t status,
            const std::unordered_map<std::string, std::string>& headers,
            const std::string& body
        ) {
            // Queue response to be sent in next generate_datagrams call
            pending_responses_[stream_id] = {status, headers, body};
        };

        // Invoke callback
        request_callback_(state.method, state.path, state.headers, body_str, send_response);
    }
}

int Http3Connection::send_response_internal(
    uint64_t stream_id,
    uint16_t status,
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& body
) noexcept {
    std::cout << "[HTTP/3] Sending response on stream " << stream_id
             << " (status=" << status << ", body_size=" << body.size() << ")" << std::endl;

    // Encode headers with QPACK
    uint8_t* header_buffer = header_buffer_pool_.acquire();
    if (!header_buffer) {
        std::cerr << "[HTTP/3] Header buffer pool exhausted" << std::endl;
        return -1;
    }

    size_t encoded_header_len;
    if (encode_headers(status, headers, header_buffer,
                      header_buffer_pool_.buffer_size(), encoded_header_len) != 0) {
        header_buffer_pool_.release(header_buffer);
        std::cerr << "[HTTP/3] Header encoding failed" << std::endl;
        return -1;
    }

    // Build HEADERS frame
    uint8_t* frame_buffer = frame_buffer_pool_.acquire();
    if (!frame_buffer) {
        header_buffer_pool_.release(header_buffer);
        std::cerr << "[HTTP/3] Frame buffer pool exhausted" << std::endl;
        return -1;
    }

    size_t frame_pos = 0;

    // Frame type: HEADERS
    frame_pos += quic::VarInt::encode(kFrameTypeHeaders, frame_buffer + frame_pos);

    // Frame length
    frame_pos += quic::VarInt::encode(encoded_header_len, frame_buffer + frame_pos);

    // Headers payload
    if (frame_pos + encoded_header_len > frame_buffer_pool_.buffer_size()) {
        header_buffer_pool_.release(header_buffer);
        frame_buffer_pool_.release(frame_buffer);
        std::cerr << "[HTTP/3] Headers too large for buffer" << std::endl;
        return -1;
    }
    std::memcpy(frame_buffer + frame_pos, header_buffer, encoded_header_len);
    frame_pos += encoded_header_len;

    // Send HEADERS frame
    ssize_t written = quic_conn_->write_stream(stream_id, frame_buffer, frame_pos);
    if (written < 0) {
        header_buffer_pool_.release(header_buffer);
        frame_buffer_pool_.release(frame_buffer);
        std::cerr << "[HTTP/3] Failed to write HEADERS frame" << std::endl;
        return -1;
    }

    header_buffer_pool_.release(header_buffer);
    frame_buffer_pool_.release(frame_buffer);

    // Send DATA frame if body present
    if (!body.empty()) {
        uint8_t data_frame_header[16];
        size_t data_header_pos = 0;

        // Frame type: DATA
        data_header_pos += quic::VarInt::encode(kFrameTypeData, data_frame_header + data_header_pos);

        // Frame length
        data_header_pos += quic::VarInt::encode(body.size(), data_frame_header + data_header_pos);

        // Send DATA frame header
        written = quic_conn_->write_stream(stream_id, data_frame_header, data_header_pos);
        if (written < 0) {
            std::cerr << "[HTTP/3] Failed to write DATA frame header" << std::endl;
            return -1;
        }

        // Send body
        written = quic_conn_->write_stream(stream_id,
                                          reinterpret_cast<const uint8_t*>(body.data()),
                                          body.size());
        if (written < 0) {
            std::cerr << "[HTTP/3] Failed to write DATA frame body" << std::endl;
            return -1;
        }
    }

    // Close stream (FIN)
    quic_conn_->close_stream(stream_id);

    std::cout << "[HTTP/3] Response sent successfully on stream " << stream_id << std::endl;

    return 0;
}

int Http3Connection::encode_headers(
    uint16_t status,
    const std::unordered_map<std::string, std::string>& headers,
    uint8_t* output,
    size_t capacity,
    size_t& out_length
) noexcept {
    // Build header list
    std::vector<std::pair<std::string_view, std::string_view>> header_list;
    header_list.reserve(headers.size() + 1);

    // Add :status pseudo-header
    char status_buf[4];
    snprintf(status_buf, sizeof(status_buf), "%u", status);
    header_list.emplace_back(":status", status_buf);

    // Add regular headers
    for (const auto& [name, value] : headers) {
        header_list.emplace_back(name, value);
    }

    // Encode with QPACK
    return qpack_encoder_.encode_field_section(
        header_list.data(),
        header_list.size(),
        output,
        capacity,
        out_length
    );
}

Http3StreamState& Http3Connection::get_or_create_stream_state(uint64_t stream_id) noexcept {
    auto it = stream_states_.find(stream_id);
    if (it != stream_states_.end()) {
        return it->second;
    }

    // Create new state
    Http3StreamState state;
    state.stream_id = stream_id;
    stream_states_[stream_id] = state;

    return stream_states_[stream_id];
}

} // namespace http
} // namespace fasterapi
