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
    , request_start_time_(std::chrono::steady_clock::now())
    , last_activity_time_(std::chrono::steady_clock::now())
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
    , request_start_time_(other.request_start_time_)
    , last_activity_time_(other.last_activity_time_)
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
        request_start_time_ = other.request_start_time_;
        last_activity_time_ = other.last_activity_time_;

        other.socket_fd_ = -1;
    }
    return *this;
}

result<size_t> Http1Connection::process_input(const uint8_t* data, size_t len) noexcept {
    if (state_ == Http1State::ERROR || state_ == Http1State::CLOSING) {
        return err<size_t>(error_code::invalid_state);
    }

    // Update activity timestamp - we received data
    mark_activity();

    // If transitioning from KEEPALIVE, mark new request start
    if (state_ == Http1State::KEEPALIVE) {
        mark_request_start();
        state_ = Http1State::READING_REQUEST;
    }

    // Append to input buffer
    input_buffer_.insert(input_buffer_.end(), data, data + len);

    // Parse-ahead loop: extract all complete requests from buffer
    while (pipeline_count_ < MAX_PIPELINE_DEPTH) {
        // Skip if we're past the buffer
        if (pipeline_parse_pos_ >= input_buffer_.size()) {
            break;
        }

        // Try to parse next request
        parser_.reset();
        HTTP1Request temp_request;
        size_t consumed = 0;
        
        int parse_result = parser_.parse(
            input_buffer_.data() + pipeline_parse_pos_,
            input_buffer_.size() - pipeline_parse_pos_,
            temp_request,
            consumed
        );

        if (parse_result < 0) {
            // Need more data - stop parsing
            break;
        }

        if (parse_result > 0) {
            // Parse error
            error_message_ = "HTTP parse error";
            state_ = Http1State::ERROR;
            return err<size_t>(error_code::parse_error);
        }

        // Successfully parsed a request - store in pipeline slot
        auto& slot = pipeline_requests_[pipeline_write_idx_];
        slot.buffer_start = pipeline_parse_pos_;
        slot.buffer_end = pipeline_parse_pos_ + consumed;
        slot.has_body = temp_request.has_content_length && temp_request.content_length > 0;
        slot.content_length = temp_request.content_length;
        slot.processed = false;

        // Check for body
        if (slot.has_body) {
            // Need body data after headers
            size_t body_available = input_buffer_.size() - slot.buffer_end;
            if (body_available < slot.content_length) {
                // Don't have full body yet - wait for more data
                // Don't advance pipeline yet
                break;
            }
            // Include body in buffer range
            slot.buffer_end += slot.content_length;
        }

        // Update keep-alive based on first request (for connection state)
        if (pipeline_count_ == 0) {
            keep_alive_ = should_keep_alive_from_request(temp_request);
        }

        // Advance pipeline
        pipeline_parse_pos_ = slot.buffer_end;
        pipeline_write_idx_ = (pipeline_write_idx_ + 1) % MAX_PIPELINE_DEPTH;
        pipeline_count_++;

        LOG_DEBUG("HTTP1", "Parsed pipelined request %zu: %.*s %.*s (bytes %zu-%zu)",
                  pipeline_count_,
                  static_cast<int>(temp_request.method_str.size()), temp_request.method_str.data(),
                  static_cast<int>(temp_request.url.size()), temp_request.url.data(),
                  slot.buffer_start, slot.buffer_end);
    }

    // Process all pending requests and generate responses
    if (pipeline_count_ > 0) {
        process_pending_requests();
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
    // Only compact input buffer when pipeline is fully drained
    if (pipeline_count_ == 0) {
        // All requests processed - compact input buffer
        if (pipeline_parse_pos_ > 0 && pipeline_parse_pos_ <= input_buffer_.size()) {
            input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + pipeline_parse_pos_);
        }
        
        // Reset pipeline state
        pipeline_parse_pos_ = 0;
        pipeline_write_idx_ = 0;
        pipeline_read_idx_ = 0;
        
        // Clear any leftover response slots
        for (auto& resp : pipeline_responses_) {
            resp.clear();
        }
        for (auto& req : pipeline_requests_) {
            req = PipelinedRequest{};
        }
    }
    
    // Clear output state
    output_buffer_.clear();
    output_offset_ = 0;
    body_bytes_read_ = 0;
    bytes_consumed_ = 0;
    parser_.reset();
    current_request_ = HTTP1Request{};
    state_ = Http1State::READING_REQUEST;
    
    // Reset timestamps for next request
    auto now = std::chrono::steady_clock::now();
    request_start_time_ = now;
    last_activity_time_ = now;
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

    // Store consumed bytes for pipelining support
    bytes_consumed_ = consumed;

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
    if (!request_callback_ && !fast_request_callback_) {
        error_message_ = "No request callback set";
        return err<void>(error_code::invalid_state);
    }

    Http1Response response;
    
    // Fast path: zero-copy callback (preferred)
    if (fast_request_callback_) {
        Http1RequestView view;
        view.method = current_request_.method_str;
        
        // Split URL into path and query string
        std::string_view url = current_request_.url;
        auto query_pos = url.find('?');
        if (query_pos != std::string_view::npos) {
            view.path = url.substr(0, query_pos);
            view.query_string = url.substr(query_pos + 1);
        } else {
            view.path = url;
            view.query_string = {};
        }
        
        // Copy header views (zero-copy - points into input buffer)
        view.header_count = std::min(current_request_.header_count, Http1RequestView::MAX_HEADERS);
        for (size_t i = 0; i < view.header_count; ++i) {
            view.headers[i] = {current_request_.headers[i].name, current_request_.headers[i].value};
        }
        
        // Body view (zero-copy)
        if (current_request_.has_content_length && current_request_.content_length > 0) {
            if (input_buffer_.size() >= current_request_.content_length) {
                const char* body_start = reinterpret_cast<const char*>(
                    input_buffer_.data() + input_buffer_.size() - current_request_.content_length);
                view.body = std::string_view(body_start, current_request_.content_length);
            }
        }
        
        LOG_DEBUG("HTTP1", "Calling fast_request_callback_...");
        response = fast_request_callback_(view);
    } else {
        // Legacy path: allocating callback
        std::unordered_map<std::string, std::string> headers;
        for (size_t i = 0; i < current_request_.header_count; ++i) {
            const auto& header = current_request_.headers[i];
            headers[std::string(header.name)] = std::string(header.value);
        }

        // Extract body
        std::string body;
        if (current_request_.has_content_length && current_request_.content_length > 0) {
            if (input_buffer_.size() >= current_request_.content_length) {
                body = std::string(
                    input_buffer_.end() - current_request_.content_length,
                    input_buffer_.end()
                );
            }
        }

        LOG_DEBUG("HTTP1", "Calling request_callback_...");
        response = request_callback_(
            std::string(current_request_.method_str),
            std::string(current_request_.url),
            headers,
            body
        );
    }

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

void Http1Connection::send_timeout_response() noexcept {
    // Build 408 Request Timeout response
    Http1Response timeout_response;
    timeout_response.status = 408;
    timeout_response.status_message = "Request Timeout";
    timeout_response.body = "Request Timeout: The server timed out waiting for the request.\r\n";
    timeout_response.add_header("Content-Type", "text/plain");
    timeout_response.add_header("Connection", "close");

    // Don't keep connection alive after timeout
    keep_alive_ = false;

    // Build the response
    build_response(timeout_response);

    // Set state to writing response (will be sent by caller)
    state_ = Http1State::WRITING_RESPONSE;

    LOG_INFO("HTTP1", "Sending 408 Request Timeout (elapsed: %llu ms)", static_cast<unsigned long long>(get_request_elapsed_ms()));
}

void Http1Connection::send_payload_too_large_response() noexcept {
    // Build 413 Payload Too Large response
    Http1Response response;
    response.status = 413;
    response.status_message = "Payload Too Large";
    response.body = "Payload Too Large: The request body exceeds the server's size limit.\r\n";
    response.add_header("Content-Type", "text/plain");
    response.add_header("Connection", "close");

    // Don't keep connection alive after rejection
    keep_alive_ = false;

    // Build the response
    build_response(response);

    // Set state to writing response
    state_ = Http1State::WRITING_RESPONSE;

    LOG_INFO("HTTP1", "Sending 413 Payload Too Large (Content-Length: %zu)", get_content_length());
}

void Http1Connection::send_header_too_large_response() noexcept {
    // Build 431 Request Header Fields Too Large response
    Http1Response response;
    response.status = 431;
    response.status_message = "Request Header Fields Too Large";
    response.body = "Request Header Fields Too Large: The request headers exceed the server's size limit.\r\n";
    response.add_header("Content-Type", "text/plain");
    response.add_header("Connection", "close");

    // Don't keep connection alive after rejection
    keep_alive_ = false;

    // Build the response
    build_response(response);

    // Set state to writing response
    state_ = Http1State::WRITING_RESPONSE;

    LOG_INFO("HTTP1", "Sending 431 Request Header Fields Too Large (header size: %zu)", input_buffer_.size());
}

bool Http1Connection::is_body_too_large(size_t max_body_size) const noexcept {
    // If max_body_size is 0, no limit enforced
    if (max_body_size == 0) return false;

    // Check Content-Length against limit
    size_t content_length = get_content_length();
    return content_length > max_body_size;
}

size_t Http1Connection::get_content_length() const noexcept {
    if (current_request_.has_content_length) {
        return current_request_.content_length;
    }
    return 0;
}

void Http1Connection::process_pending_requests() noexcept {
    // Process each unprocessed request in pipeline order
    size_t idx = pipeline_read_idx_;
    for (size_t i = 0; i < pipeline_count_; i++) {
        auto& req_slot = pipeline_requests_[idx];
        auto& resp_slot = pipeline_responses_[idx];
        
        // Skip already processed requests
        if (req_slot.processed) {
            idx = (idx + 1) % MAX_PIPELINE_DEPTH;
            continue;
        }
        
        // Re-parse the request to get string_views (they point into input_buffer_)
        parser_.reset();
        HTTP1Request req;
        size_t consumed = 0;
        size_t req_len = req_slot.buffer_end - req_slot.buffer_start;
        if (req_slot.has_body) {
            req_len -= req_slot.content_length;  // Exclude body from header parse
        }
        
        int parse_result = parser_.parse(
            input_buffer_.data() + req_slot.buffer_start,
            req_len,
            req,
            consumed
        );
        
        if (parse_result != 0) {
            // Should not happen - we already parsed this successfully
            LOG_ERROR("HTTP1", "Re-parse failed for pipelined request");
            idx = (idx + 1) % MAX_PIPELINE_DEPTH;
            continue;
        }
        
        // Build request view for callback
        Http1RequestView view;
        view.method = req.method_str;
        
        // Split URL into path and query string
        std::string_view url = req.url;
        auto query_pos = url.find('?');
        if (query_pos != std::string_view::npos) {
            view.path = url.substr(0, query_pos);
            view.query_string = url.substr(query_pos + 1);
        } else {
            view.path = url;
            view.query_string = {};
        }
        
        // Copy header views
        view.header_count = std::min(req.header_count, Http1RequestView::MAX_HEADERS);
        for (size_t h = 0; h < view.header_count; h++) {
            view.headers[h] = {req.headers[h].name, req.headers[h].value};
        }
        
        // Body view
        if (req_slot.has_body && req_slot.content_length > 0) {
            size_t body_start = req_slot.buffer_end - req_slot.content_length;
            const char* body_ptr = reinterpret_cast<const char*>(input_buffer_.data() + body_start);
            view.body = std::string_view(body_ptr, req_slot.content_length);
        }
        
        // Call the request handler
        Http1Response response;
        if (fast_request_callback_) {
            response = fast_request_callback_(view);
        } else if (request_callback_) {
            // Legacy callback - need to allocate
            std::unordered_map<std::string, std::string> headers;
            for (size_t h = 0; h < req.header_count; h++) {
                headers[std::string(req.headers[h].name)] = std::string(req.headers[h].value);
            }
            response = request_callback_(
                std::string(req.method_str),
                std::string(req.url),
                headers,
                std::string(view.body)
            );
        } else {
            // No callback - return 500
            response.status = 500;
            response.status_message = "Internal Server Error";
            response.body = "No request handler configured";
        }
        
        // Build response into the pipeline slot
        build_pipelined_response(response, resp_slot);
        req_slot.processed = true;
        
        LOG_DEBUG("HTTP1", "Processed pipelined request %zu, response %d %zu bytes",
                  i, response.status, resp_slot.data.size());
        
        idx = (idx + 1) % MAX_PIPELINE_DEPTH;
    }
    
    // Flush ready responses to output buffer
    flush_ready_responses();
}

void Http1Connection::flush_ready_responses() noexcept {
    // Send responses in FIFO order (head-of-line blocking as per HTTP/1.1 spec)
    while (pipeline_count_ > 0) {
        auto& resp = pipeline_responses_[pipeline_read_idx_];
        
        if (!resp.ready) {
            // Head response not ready yet - must wait (head-of-line blocking)
            break;
        }
        
        // Append response to output buffer
        output_buffer_.insert(output_buffer_.end(), resp.data.begin(), resp.data.end());
        
        // Clear slot for reuse
        resp.clear();
        
        // Advance read pointer
        pipeline_read_idx_ = (pipeline_read_idx_ + 1) % MAX_PIPELINE_DEPTH;
        pipeline_count_--;
        requests_served_++;
        
        LOG_DEBUG("HTTP1", "Flushed pipelined response, %zu remaining in pipeline", pipeline_count_);
    }
    
    // Update connection state
    if (!output_buffer_.empty()) {
        state_ = Http1State::WRITING_RESPONSE;
    } else if (pipeline_count_ == 0) {
        // Pipeline empty and no pending output
        state_ = keep_alive_ ? Http1State::KEEPALIVE : Http1State::CLOSING;
    }
}

void Http1Connection::build_pipelined_response(const Http1Response& response, PipelinedResponse& out) noexcept {
    out.data.clear();
    out.data.reserve(256 + response.body.size());
    
    // Status line: "HTTP/1.1 XXX Message\r\n"
    const char* http11 = "HTTP/1.1 ";
    out.data.insert(out.data.end(), http11, http11 + 9);
    
    std::string status_str = std::to_string(response.status);
    out.data.insert(out.data.end(), status_str.begin(), status_str.end());
    out.data.push_back(' ');
    out.data.insert(out.data.end(), response.status_message.begin(), response.status_message.end());
    out.data.push_back('\r');
    out.data.push_back('\n');
    
    // Track important headers
    bool has_content_length = false;
    bool has_connection = false;
    
    // Headers
    for (const auto& [name, value] : response.headers) {
        out.data.insert(out.data.end(), name.begin(), name.end());
        out.data.push_back(':');
        out.data.push_back(' ');
        out.data.insert(out.data.end(), value.begin(), value.end());
        out.data.push_back('\r');
        out.data.push_back('\n');
        
        if (name == "Content-Length" || name == "content-length") {
            has_content_length = true;
        }
        if (name == "Connection" || name == "connection") {
            has_connection = true;
        }
    }
    
    // Add Content-Length if not present
    if (!has_content_length) {
        const char* cl = "Content-Length: ";
        out.data.insert(out.data.end(), cl, cl + 16);
        std::string len_str = std::to_string(response.body.size());
        out.data.insert(out.data.end(), len_str.begin(), len_str.end());
        out.data.push_back('\r');
        out.data.push_back('\n');
    }
    
    // Add Connection header if not present (always keep-alive for pipelining)
    if (!has_connection) {
        const char* conn = "Connection: keep-alive\r\n";
        out.data.insert(out.data.end(), conn, conn + 24);
    }
    
    // End of headers
    out.data.push_back('\r');
    out.data.push_back('\n');
    
    // Body
    if (!response.body.empty()) {
        out.data.insert(out.data.end(), response.body.begin(), response.body.end());
    }
    
    out.ready = true;
}

} // namespace http
} // namespace fasterapi
