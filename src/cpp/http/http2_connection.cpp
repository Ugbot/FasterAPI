#include "http2_connection.h"
#include <cstring>
#include <algorithm>
#include <iostream>

namespace fasterapi {
namespace http2 {

using core::result;
using core::error_code;
using core::ok;
using core::err;

// Http2Connection implementation

Http2Connection::Http2Connection(bool is_server)
    : is_server_(is_server),
      stream_manager_(local_settings_.initial_window_size),
      hpack_encoder_(local_settings_.header_table_size),
      hpack_decoder_(local_settings_.header_table_size) {

    if (is_server) {
        state_ = ConnectionState::PREFACE_PENDING;
        // Queue initial SETTINGS frame (server connection preface per RFC 7540 Section 3.5)
        // This must be sent before processing any client data
        send_settings();
    } else {
        state_ = ConnectionState::ACTIVE;
    }
}

core::result<size_t> Http2Connection::process_input(const uint8_t* data, size_t len) noexcept {
    if (state_ == ConnectionState::CLOSED) {
        return err<size_t>(error_code::invalid_state);
    }

    size_t consumed = 0;

    // Check for client preface if server and not yet received
    // Validate preface incrementally to handle partial reads correctly
    if (is_server_ && state_ == ConnectionState::PREFACE_PENDING) {
        size_t bytes_needed = CONNECTION_PREFACE_LEN - preface_bytes_validated_;
        size_t bytes_available = std::min(bytes_needed, len);

        // Debug: Log received bytes
        std::cerr << "[HTTP/2] Validating preface: offset=" << preface_bytes_validated_
                  << " bytes_available=" << bytes_available << " len=" << len << std::endl;
        std::cerr << "[HTTP/2] Expected: ";
        for (size_t i = 0; i < bytes_available; ++i) {
            std::cerr << std::hex << (int)(unsigned char)CONNECTION_PREFACE[preface_bytes_validated_ + i] << " ";
        }
        std::cerr << std::endl << "[HTTP/2] Received: ";
        for (size_t i = 0; i < bytes_available; ++i) {
            std::cerr << std::hex << (int)(unsigned char)data[i] << " ";
        }
        std::cerr << std::dec << std::endl;

        // Validate the bytes we can against the correct position in preface
        if (std::memcmp(
            CONNECTION_PREFACE + preface_bytes_validated_,
            data,
            bytes_available
        ) != 0) {
            // Preface mismatch - protocol error
            std::cerr << "[HTTP/2] Preface mismatch!" << std::endl;
            return err<size_t>(error_code::internal_error);
        }

        std::cerr << "[HTTP/2] Preface bytes validated OK" << std::endl;

        // Update validated byte count and consumed count
        preface_bytes_validated_ += bytes_available;
        consumed = bytes_available;

        // Check if we've received the complete preface
        if (preface_bytes_validated_ == CONNECTION_PREFACE_LEN) {
            // Complete preface received and validated
            std::cerr << "[HTTP/2] Complete preface validated, transitioning to ACTIVE state" << std::endl;
            state_ = ConnectionState::ACTIVE;
            // Note: Initial SETTINGS already queued in constructor
        } else {
            // Need more data to complete preface validation
            return ok(consumed);
        }
    }

    std::cerr << "[HTTP/2] Processing frames, consumed=" << consumed << " len=" << len << std::endl;

    // Process frames
    while (consumed < len) {
        size_t remaining = len - consumed;

        // Need at least 9 bytes for frame header
        if (remaining < 9) {
            // Buffer partial frame header
            std::memcpy(input_buffer_.data() + input_buffer_len_,
                       data + consumed, remaining);
            input_buffer_len_ += remaining;
            consumed += remaining;
            break;
        }

        // Parse frame header
        auto header_result = parse_frame_header(data + consumed);
        if (header_result.is_err()) {
            std::cerr << "[HTTP/2] Frame header parse error!" << std::endl;
            return err<size_t>(header_result.error());
        }

        FrameHeader header = header_result.value();
        std::cerr << "[HTTP/2] Parsed frame: type=" << (int)header.type
                  << " flags=" << (int)header.flags
                  << " stream_id=" << header.stream_id
                  << " length=" << header.length << std::endl;

        // Check if we have complete frame
        size_t frame_size = 9 + header.length;
        if (remaining < frame_size) {
            // Buffer partial frame
            std::memcpy(input_buffer_.data() + input_buffer_len_,
                       data + consumed, remaining);
            input_buffer_len_ += remaining;
            consumed += remaining;
            break;
        }

        // Process complete frame
        const uint8_t* payload = data + consumed + 9;

        // Dispatch to appropriate handler
        result<void> handle_result = ok();

        switch (header.type) {
        case FrameType::SETTINGS:
            handle_result = handle_settings_frame(header, payload);
            break;
        case FrameType::HEADERS:
            handle_result = handle_headers_frame(header, payload, header.length);
            break;
        case FrameType::DATA:
            handle_result = handle_data_frame(header, payload, header.length);
            break;
        case FrameType::WINDOW_UPDATE:
            handle_result = handle_window_update_frame(header, payload);
            break;
        case FrameType::PING:
            handle_result = handle_ping_frame(header, payload);
            break;
        case FrameType::RST_STREAM:
            handle_result = handle_rst_stream_frame(header, payload);
            break;
        case FrameType::GOAWAY:
            handle_result = handle_goaway_frame(payload, header.length);
            break;
        default:
            // Unknown frame type - ignore per spec
            break;
        }

        if (handle_result.is_err()) {
            return err<size_t>(handle_result.error());
        }

        consumed += frame_size;
    }

    return ok(consumed);
}

bool Http2Connection::get_output(const uint8_t** out_data, size_t* out_len) noexcept {
    if (output_offset_ >= output_buffer_.size()) {
        return false;  // No data available
    }

    *out_data = output_buffer_.data() + output_offset_;
    *out_len = output_buffer_.size() - output_offset_;
    return true;
}

void Http2Connection::commit_output(size_t len) noexcept {
    output_offset_ += len;

    // Clear buffer if fully consumed
    if (output_offset_ >= output_buffer_.size()) {
        output_buffer_.clear();
        output_offset_ = 0;
    }
}

core::result<void> Http2Connection::send_response(
    uint32_t stream_id,
    uint16_t status,
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& body
) noexcept {
    Http2Stream* stream = stream_manager_.get_stream(stream_id);
    if (!stream) {
        return err<void>(error_code::invalid_state);
    }

    // Build headers vector for HPACK encoding
    std::vector<http::HPACKHeader> hpack_headers;
    hpack_headers.reserve(headers.size() + 1);

    // Add :status pseudo-header
    char status_buf[4];
    snprintf(status_buf, sizeof(status_buf), "%u", status);
    hpack_headers.push_back({":status", status_buf, false});

    // Add response headers
    for (const auto& [name, value] : headers) {
        hpack_headers.push_back({name, value, false});
    }

    // Encode headers with HPACK
    uint8_t encoded_buffer[4096];  // Stack buffer for encoded headers
    size_t encoded_len = 0;

    int encode_result = hpack_encoder_.encode(
        hpack_headers.data(),
        hpack_headers.size(),
        encoded_buffer,
        sizeof(encoded_buffer),
        encoded_len
    );
    if (encode_result != 0) {
        return err<void>(error_code::internal_error);
    }

    std::vector<uint8_t> encoded_headers(encoded_buffer, encoded_buffer + encoded_len);

    // Create HEADERS frame
    bool end_stream = body.empty();
    auto headers_frame = write_headers_frame(stream_id, encoded_headers, end_stream, true);

    auto queue_result = queue_frame(headers_frame);
    if (queue_result.is_err()) {
        return queue_result;
    }

    // Send DATA frame if body present
    if (!body.empty()) {
        auto data_frame = write_data_frame(stream_id, body, true);
        queue_result = queue_frame(data_frame);
        if (queue_result.is_err()) {
            return queue_result;
        }
    }

    // Update stream state
    stream->on_headers_sent(end_stream);
    if (!body.empty()) {
        stream->on_data_sent(true);
    }

    return ok();
}

core::result<void> Http2Connection::send_rst_stream(uint32_t stream_id, ErrorCode error) noexcept {
    auto frame = write_rst_stream_frame(stream_id, error);
    return queue_frame(frame);
}

core::result<void> Http2Connection::send_goaway(ErrorCode error, const std::string& debug_data) noexcept {
    auto frame = write_goaway_frame(last_stream_id_, error, debug_data);
    state_ = ConnectionState::GOAWAY_SENT;
    return queue_frame(frame);
}

Http2Stream* Http2Connection::get_stream(uint32_t stream_id) noexcept {
    return stream_manager_.get_stream(stream_id);
}

// Frame handlers

core::result<void> Http2Connection::handle_settings_frame(
    const FrameHeader& header,
    const uint8_t* payload
) noexcept {
    // SETTINGS ACK
    if (header.flags & FrameFlags::SETTINGS_ACK) {
        settings_ack_pending_ = false;
        return ok();
    }

    // Parse SETTINGS parameters
    auto params_result = parse_settings_frame(header, payload, header.length);
    if (params_result.is_err()) {
        return err<void>(params_result.error());
    }

    // Apply settings
    auto apply_result = apply_settings(params_result.value());
    if (apply_result.is_err()) {
        return apply_result;
    }

    // Send SETTINGS ACK
    return send_settings_ack();
}

core::result<void> Http2Connection::handle_headers_frame(
    const FrameHeader& header,
    const uint8_t* payload,
    size_t payload_len
) noexcept {
    std::cerr << "[HTTP/2] handle_headers_frame: stream_id=" << header.stream_id
              << " payload_len=" << payload_len << std::endl;

    // Get or create stream
    Http2Stream* stream = stream_manager_.get_stream(header.stream_id);
    if (!stream) {
        std::cerr << "[HTTP/2] Creating new stream " << header.stream_id << std::endl;
        auto create_result = stream_manager_.create_stream(header.stream_id);
        if (create_result.is_err()) {
            std::cerr << "[HTTP/2] Failed to create stream!" << std::endl;
            return err<void>(create_result.error());
        }
        stream = create_result.value();
        last_stream_id_ = header.stream_id;
    }

    // Parse HEADERS frame to extract header block
    PrioritySpec priority;
    std::vector<uint8_t> header_block;

    std::cerr << "[HTTP/2] Parsing HEADERS frame..." << std::endl;
    auto parse_result = parse_headers_frame(header, payload, payload_len,
                                           &priority, header_block);
    if (parse_result.is_err()) {
        std::cerr << "[HTTP/2] Failed to parse HEADERS frame!" << std::endl;
        return err<void>(parse_result.error());
    }
    std::cerr << "[HTTP/2] Parsed HEADERS frame, header_block size=" << header_block.size() << std::endl;

    // Decode HPACK headers
    std::vector<http::HPACKHeader> decoded_headers;
    std::cerr << "[HTTP/2] Decoding HPACK headers..." << std::endl;
    int decode_result = hpack_decoder_.decode(header_block.data(),
                                             header_block.size(),
                                             decoded_headers);
    if (decode_result != 0) {
        std::cerr << "[HTTP/2] Failed to decode HPACK headers! decode_result=" << decode_result << std::endl;
        return err<void>(error_code::internal_error);
    }
    std::cerr << "[HTTP/2] Decoded " << decoded_headers.size() << " headers" << std::endl;

    // Store headers in stream
    for (const auto& h : decoded_headers) {
        std::cerr << "[HTTP/2] Header: " << h.name << ": " << h.value << std::endl;
        stream->add_request_header(std::string(h.name), std::string(h.value));
    }

    // Update stream state
    bool end_stream = (header.flags & FrameFlags::HEADERS_END_STREAM) != 0;
    std::cerr << "[HTTP/2] end_stream=" << end_stream << " request_callback_=" << (request_callback_ ? "set" : "null") << std::endl;
    stream->on_headers_received(end_stream);

    // If request complete, invoke callback
    if (end_stream && request_callback_) {
        std::cerr << "[HTTP/2] Invoking request callback for stream " << stream->id() << std::endl;
        request_callback_(stream);
        std::cerr << "[HTTP/2] Request callback completed" << std::endl;
    }

    return ok();
}

core::result<void> Http2Connection::handle_data_frame(
    const FrameHeader& header,
    const uint8_t* payload,
    size_t payload_len
) noexcept {
    Http2Stream* stream = stream_manager_.get_stream(header.stream_id);
    if (!stream) {
        return err<void>(error_code::invalid_state);
    }

    // Parse DATA frame
    auto data_result = parse_data_frame(header, payload, payload_len);
    if (data_result.is_err()) {
        return err<void>(data_result.error());
    }

    // Check flow control
    auto consume_conn = consume_recv_window(static_cast<uint32_t>(data_result.value().size()));
    if (consume_conn.is_err()) {
        return consume_conn;
    }

    auto consume_stream = stream->consume_recv_window(static_cast<uint32_t>(data_result.value().size()));
    if (consume_stream.is_err()) {
        return consume_stream;
    }

    // Append to stream body
    stream->append_request_body(data_result.value());

    // Update stream state
    bool end_stream = (header.flags & FrameFlags::DATA_END_STREAM) != 0;
    stream->on_data_received(end_stream);

    // If request complete, invoke callback
    if (end_stream && request_callback_) {
        request_callback_(stream);
    }

    return ok();
}

core::result<void> Http2Connection::handle_window_update_frame(
    const FrameHeader& header,
    const uint8_t* payload
) noexcept {
    auto increment_result = parse_window_update_frame(payload);
    if (increment_result.is_err()) {
        return err<void>(increment_result.error());
    }

    uint32_t increment = increment_result.value();

    if (header.stream_id == 0) {
        // Connection-level window update
        if (connection_send_window_ > INT32_MAX - static_cast<int32_t>(increment)) {
            return err<void>(error_code::internal_error);
        }
        connection_send_window_ += static_cast<int32_t>(increment);
    } else {
        // Stream-level window update
        Http2Stream* stream = stream_manager_.get_stream(header.stream_id);
        if (!stream) {
            return err<void>(error_code::invalid_state);
        }
        return stream->update_send_window(static_cast<int32_t>(increment));
    }

    return ok();
}

core::result<void> Http2Connection::handle_ping_frame(
    const FrameHeader& header,
    const uint8_t* payload
) noexcept {
    auto opaque_result = parse_ping_frame(payload);
    if (opaque_result.is_err()) {
        return err<void>(opaque_result.error());
    }

    // If not ACK, send PING ACK with same data
    if (!(header.flags & FrameFlags::PING_ACK)) {
        auto frame = write_ping_frame(opaque_result.value(), true);
        return queue_frame(frame);
    }

    return ok();
}

core::result<void> Http2Connection::handle_rst_stream_frame(
    const FrameHeader& header,
    const uint8_t* payload
) noexcept {
    auto error_result = parse_rst_stream_frame(payload);
    if (error_result.is_err()) {
        return err<void>(error_result.error());
    }

    Http2Stream* stream = stream_manager_.get_stream(header.stream_id);
    if (stream) {
        stream->on_rst_stream();
        stream->set_error_code(error_result.value());
    }

    return ok();
}

core::result<void> Http2Connection::handle_goaway_frame(
    const uint8_t* payload,
    size_t payload_len
) noexcept {
    uint32_t last_stream_id;
    ErrorCode error_code_val;
    std::string debug_data;

    auto parse_result = parse_goaway_frame(payload, payload_len,
                                          last_stream_id, error_code_val, debug_data);
    if (parse_result.is_err()) {
        return err<void>(parse_result.error());
    }

    state_ = ConnectionState::GOAWAY_RECEIVED;
    return ok();
}

// Settings management

core::result<void> Http2Connection::apply_settings(
    const std::vector<SettingsParameter>& params
) noexcept {
    for (const auto& param : params) {
        switch (param.id) {
        case SettingsId::HEADER_TABLE_SIZE:
            remote_settings_.header_table_size = param.value;
            hpack_encoder_.set_max_table_size(param.value);
            break;
        case SettingsId::ENABLE_PUSH:
            remote_settings_.enable_push = (param.value != 0);
            break;
        case SettingsId::MAX_CONCURRENT_STREAMS:
            remote_settings_.max_concurrent_streams = param.value;
            break;
        case SettingsId::INITIAL_WINDOW_SIZE:
            remote_settings_.initial_window_size = param.value;
            stream_manager_.update_initial_window_size(param.value);
            break;
        case SettingsId::MAX_FRAME_SIZE:
            if (param.value < 16384 || param.value > 16777215) {
                return err<void>(error_code::internal_error);
            }
            remote_settings_.max_frame_size = param.value;
            break;
        case SettingsId::MAX_HEADER_LIST_SIZE:
            remote_settings_.max_header_list_size = param.value;
            break;
        }
    }

    settings_received_ = true;
    return ok();
}

core::result<void> Http2Connection::send_settings() noexcept {
    std::vector<SettingsParameter> params;
    params.push_back({SettingsId::HEADER_TABLE_SIZE, local_settings_.header_table_size});
    params.push_back({SettingsId::ENABLE_PUSH, local_settings_.enable_push ? 1u : 0u});
    params.push_back({SettingsId::MAX_CONCURRENT_STREAMS, local_settings_.max_concurrent_streams});
    params.push_back({SettingsId::INITIAL_WINDOW_SIZE, local_settings_.initial_window_size});
    params.push_back({SettingsId::MAX_FRAME_SIZE, local_settings_.max_frame_size});

    auto frame = write_settings_frame(params);
    settings_ack_pending_ = true;
    return queue_frame(frame);
}

core::result<void> Http2Connection::send_settings_ack() noexcept {
    auto frame = write_settings_ack();
    return queue_frame(frame);
}

// Output helpers

core::result<void> Http2Connection::queue_frame(const std::vector<uint8_t>& frame) noexcept {
    return queue_data(frame.data(), frame.size());
}

core::result<void> Http2Connection::queue_data(const uint8_t* data, size_t len) noexcept {
    output_buffer_.insert(output_buffer_.end(), data, data + len);
    return ok();
}

// Preface handling (now handled inline in process_input for incremental validation)

// Flow control helpers (connection-level)

core::result<void> Http2Connection::consume_recv_window(uint32_t size) noexcept {
    if (static_cast<int32_t>(size) > connection_recv_window_) {
        return err<void>(error_code::internal_error);
    }
    connection_recv_window_ -= static_cast<int32_t>(size);
    return ok();
}

} // namespace http2
} // namespace fasterapi
