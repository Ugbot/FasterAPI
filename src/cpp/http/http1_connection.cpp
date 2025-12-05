/**
 * HTTP/1.1 Connection Implementation
 *
 * Request/response lifecycle management with keep-alive support
 */

#include "http1_connection.h"
#include "core/logger.h"
#include <cstring>
#include <algorithm>
#include <unistd.h>
#include <sys/socket.h>
#include <cerrno>

namespace fasterapi {
namespace http {

using core::result;
using core::ok;
using core::err;
using core::error_code;

Http1Connection::Http1Connection(int socket_fd)
    : socket_fd_(socket_fd)
{
    input_buffer_.reserve(8192);   // 8KB initial buffer
    output_buffer_.reserve(8192);
}

Http1Connection::~Http1Connection() {
    // Socket cleanup handled by owner
}

Http1Connection::Http1Connection(Http1Connection&& other) noexcept
    : socket_fd_(other.socket_fd_)
    , state_(other.state_)
    , parser_(std::move(other.parser_))
    , input_buffer_(std::move(other.input_buffer_))
    , current_request_(other.current_request_)
    , body_bytes_read_(other.body_bytes_read_)
    , output_buffer_(std::move(other.output_buffer_))
    , output_offset_(other.output_offset_)
    , keep_alive_(other.keep_alive_)
    , requests_served_(other.requests_served_)
    , request_callback_(std::move(other.request_callback_))
    , error_message_(std::move(other.error_message_))
{
    other.socket_fd_ = -1;
}

Http1Connection& Http1Connection::operator=(Http1Connection&& other) noexcept {
    if (this != &other) {
        socket_fd_ = other.socket_fd_;
        state_ = other.state_;
        parser_ = std::move(other.parser_);
        input_buffer_ = std::move(other.input_buffer_);
        current_request_ = other.current_request_;
        body_bytes_read_ = other.body_bytes_read_;
        output_buffer_ = std::move(other.output_buffer_);
        output_offset_ = other.output_offset_;
        keep_alive_ = other.keep_alive_;
        requests_served_ = other.requests_served_;
        request_callback_ = std::move(other.request_callback_);
        error_message_ = std::move(other.error_message_);

        other.socket_fd_ = -1;
    }
    return *this;
}

result<size_t> Http1Connection::process_input(const uint8_t* data, size_t len) noexcept {
    if (state_ == Http1State::ERROR || state_ == Http1State::CLOSING) {
        return err<size_t>(error_code::invalid_state);
    }

    // Append to input buffer
    size_t old_size = input_buffer_.size();
    input_buffer_.insert(input_buffer_.end(), data, data + len);

    if (state_ == Http1State::READING_REQUEST || state_ == Http1State::KEEPALIVE) {
        // Try to parse request
        auto parse_result = parse_request();
        if (parse_result.is_err()) {
            state_ = Http1State::ERROR;
            return err<size_t>(parse_result.error());
        }

        // Check if parse completed
        if (parser_.is_complete()) {
            // Request headers parsed
            if (current_request_.has_content_length && current_request_.content_length > 0) {
                // Need to read body
                state_ = Http1State::READING_BODY;
            } else {
                // No body - handle request
                state_ = Http1State::PROCESSING;
                auto handle_result = handle_request();
                if (handle_result.is_err()) {
                    state_ = Http1State::ERROR;
                    return err<size_t>(handle_result.error());
                }
                state_ = Http1State::WRITING_RESPONSE;
            }
        }
    }

    if (state_ == Http1State::READING_BODY) {
        // Check if we have complete body
        size_t body_start = input_buffer_.size() - (len - body_bytes_read_);
        size_t remaining_body = current_request_.content_length - body_bytes_read_;

        if (input_buffer_.size() - body_start >= remaining_body) {
            // Body complete
            body_bytes_read_ = current_request_.content_length;
            state_ = Http1State::PROCESSING;

            auto handle_result = handle_request();
            if (handle_result.is_err()) {
                state_ = Http1State::ERROR;
                return err<size_t>(handle_result.error());
            }
            state_ = Http1State::WRITING_RESPONSE;
        } else {
            body_bytes_read_ += len;
        }
    }

    return ok(len);
}

bool Http1Connection::get_output(const uint8_t** out_data, size_t* out_len) noexcept {
    if (output_offset_ >= output_buffer_.size()) {
        return false;
    }

    *out_data = output_buffer_.data() + output_offset_;
    *out_len = output_buffer_.size() - output_offset_;
    return true;
}

void Http1Connection::commit_output(size_t len) noexcept {
    output_offset_ += len;

    // If all sent, check keep-alive
    if (output_offset_ >= output_buffer_.size()) {
        output_buffer_.clear();
        output_offset_ = 0;

        if (keep_alive_ && should_keep_alive()) {
            // Prepare for next request
            reset_for_next_request();
            state_ = Http1State::KEEPALIVE;
        } else {
            state_ = Http1State::CLOSING;
        }
    }
}

void Http1Connection::reset_for_next_request() noexcept {
    input_buffer_.clear();
    output_buffer_.clear();
    output_offset_ = 0;
    body_bytes_read_ = 0;
    parser_.reset();
    state_ = Http1State::READING_REQUEST;
    requests_served_++;
}

result<void> Http1Connection::parse_request() noexcept {
    if (input_buffer_.empty()) {
        return ok();  // Need more data
    }

    size_t consumed = 0;
    int parse_result = parser_.parse(
        input_buffer_.data(),
        input_buffer_.size(),
        current_request_,
        consumed
    );

    if (parse_result < 0) {
        // Need more data
        return ok();
    }

    if (parse_result > 0) {
        // Parse error
        error_message_ = "HTTP parse error";
        return err<void>(error_code::parse_error);
    }

    // Success - check keep-alive
    keep_alive_ = should_keep_alive_from_request(current_request_);

    // Debug: Log version and keep-alive status
    LOG_DEBUG("HTTP1", "HTTP version: %d keep-alive: %d", static_cast<int>(current_request_.version), keep_alive_);
    auto conn_hdr = current_request_.get_header("Connection");
    if (!conn_hdr.empty()) {
        LOG_DEBUG("HTTP1", "Connection header: %s", std::string(conn_hdr).c_str());
    }

    return ok();
}

result<void> Http1Connection::handle_request() noexcept {
    if (!request_callback_) {
        error_message_ = "No request callback set";
        return err<void>(error_code::invalid_state);
    }

    // Build headers map
    std::unordered_map<std::string, std::string> headers;
    for (size_t i = 0; i < current_request_.header_count; ++i) {
        const auto& header = current_request_.headers[i];
        headers[std::string(header.name)] = std::string(header.value);
    }

    // Extract body
    std::string body;
    if (current_request_.has_content_length && current_request_.content_length > 0) {
        // Find body in input_buffer
        // (simplified - assumes body follows headers immediately)
        if (input_buffer_.size() >= current_request_.content_length) {
            body = std::string(
                input_buffer_.end() - current_request_.content_length,
                input_buffer_.end()
            );
        }
    }

    // Invoke callback
    LOG_DEBUG("HTTP1", "Calling request_callback_...");

    Http1Response response = request_callback_(
        std::string(current_request_.method_str),
        std::string(current_request_.url),  // Pass full URL including query string
        headers,
        body
    );

    LOG_DEBUG("HTTP1", "Callback returned, response status=%d", response.status);

    // Build response
    LOG_DEBUG("HTTP1", "Building response...");

    build_response(response);

    LOG_DEBUG("HTTP1", "Response built successfully, size=%zu bytes", output_buffer_.size());

    // Don't send here - let UnifiedServer handle sending through event loop
    return ok();
}

void Http1Connection::build_response(const Http1Response& response) noexcept {
    // Track WebSocket upgrade for connection mode transition
    pending_websocket_upgrade_ = response.websocket_upgrade;
    pending_websocket_path_ = response.websocket_path;

    // Build entire response as string first (like benchmark does)
    // This avoids complex vector operations and potential iterator invalidation
    std::string response_str;
    response_str.reserve(8192);  // Reserve space to minimize reallocations

    // Status line
    response_str += build_status_line(response.status, response.status_message);

    // Track important headers
    bool has_content_length = false;
    bool has_connection = false;

    // Headers
    for (const auto& [name, value] : response.headers) {
        response_str += name;
        response_str += ": ";
        response_str += value;
        response_str += "\r\n";

        if (name == "Content-Length" || name == "content-length") {
            has_content_length = true;
        }
        if (name == "Connection" || name == "connection") {
            has_connection = true;
        }
    }

    // Add Content-Length if not present
    if (!has_content_length && !response.body.empty()) {
        response_str += "Content-Length: ";
        response_str += std::to_string(response.body.size());
        response_str += "\r\n";
    }

    // Add Connection header if not present
    if (!has_connection) {
        response_str += "Connection: ";
        response_str += keep_alive_ ? "keep-alive" : "close";
        response_str += "\r\n";
    }

    // End of headers
    response_str += "\r\n";

    // Body
    if (!response.body.empty()) {
        response_str += response.body;
    }

    // Copy complete string to output buffer (single operation)
    output_buffer_.clear();
    output_buffer_.insert(output_buffer_.end(), response_str.begin(), response_str.end());

    LOG_DEBUG("HTTP1", "Response built: %zu bytes", output_buffer_.size());
}

std::string Http1Connection::build_status_line(uint16_t status, const std::string& message) const {
    return "HTTP/1.1 " + std::to_string(status) + " " + message + "\r\n";
}

bool Http1Connection::should_keep_alive_from_request(const HTTP1Request& request) const noexcept {
    if (request.version == HTTP1Version::HTTP_1_0) {
        // HTTP/1.0: keep-alive only if explicitly requested
        return request.keep_alive;
    }

    if (request.version == HTTP1Version::HTTP_1_1) {
        // HTTP/1.1: keep-alive by default unless Connection: close
        auto conn_header = request.get_header("Connection");
        if (!conn_header.empty()) {
            // Case-insensitive compare for "close"
            std::string conn_str(conn_header);
            std::transform(conn_str.begin(), conn_str.end(), conn_str.begin(),
                         [](unsigned char c){ return std::tolower(c); });
            return conn_str.find("close") == std::string::npos;
        }
        return true;  // Default to keep-alive for HTTP/1.1
    }

    return false;
}

} // namespace http
} // namespace fasterapi
