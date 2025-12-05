#include "http2_stream.h"

namespace fasterapi {
namespace http2 {

using core::result;
using core::error_code;
using core::ok;
using core::err;

// Http2Stream implementation

Http2Stream::Http2Stream(uint32_t stream_id, uint32_t initial_window_size)
    : stream_id_(stream_id),
      send_window_(static_cast<int32_t>(initial_window_size)),
      recv_window_(static_cast<int32_t>(initial_window_size)) {}

void Http2Stream::on_headers_sent(bool end_stream) noexcept {
    switch (state_) {
    case StreamState::IDLE:
        state_ = end_stream ? StreamState::HALF_CLOSED_LOCAL : StreamState::OPEN;
        break;
    case StreamState::RESERVED_LOCAL:
        state_ = end_stream ? StreamState::HALF_CLOSED_LOCAL : StreamState::OPEN;
        break;
    case StreamState::OPEN:
        if (end_stream) {
            state_ = StreamState::HALF_CLOSED_LOCAL;
        }
        break;
    case StreamState::HALF_CLOSED_REMOTE:
        if (end_stream) {
            state_ = StreamState::CLOSED;
        }
        break;
    default:
        // Invalid state transition - stream already closed
        break;
    }
}

void Http2Stream::on_headers_received(bool end_stream) noexcept {
    switch (state_) {
    case StreamState::IDLE:
        state_ = end_stream ? StreamState::HALF_CLOSED_REMOTE : StreamState::OPEN;
        break;
    case StreamState::RESERVED_REMOTE:
        state_ = end_stream ? StreamState::HALF_CLOSED_REMOTE : StreamState::OPEN;
        break;
    case StreamState::OPEN:
        if (end_stream) {
            state_ = StreamState::HALF_CLOSED_REMOTE;
        }
        break;
    case StreamState::HALF_CLOSED_LOCAL:
        if (end_stream) {
            state_ = StreamState::CLOSED;
        }
        break;
    default:
        // Invalid state transition
        break;
    }
}

void Http2Stream::on_data_sent(bool end_stream) noexcept {
    if (end_stream) {
        switch (state_) {
        case StreamState::OPEN:
            state_ = StreamState::HALF_CLOSED_LOCAL;
            break;
        case StreamState::HALF_CLOSED_REMOTE:
            state_ = StreamState::CLOSED;
            break;
        default:
            // Invalid state
            break;
        }
    }
}

void Http2Stream::on_data_received(bool end_stream) noexcept {
    if (end_stream) {
        switch (state_) {
        case StreamState::OPEN:
            state_ = StreamState::HALF_CLOSED_REMOTE;
            break;
        case StreamState::HALF_CLOSED_LOCAL:
            state_ = StreamState::CLOSED;
            break;
        default:
            // Invalid state
            break;
        }
    }
}

void Http2Stream::on_rst_stream() noexcept {
    state_ = StreamState::CLOSED;
}

void Http2Stream::on_push_promise_sent() noexcept {
    if (state_ == StreamState::IDLE) {
        state_ = StreamState::RESERVED_LOCAL;
    }
}

void Http2Stream::on_push_promise_received() noexcept {
    if (state_ == StreamState::IDLE) {
        state_ = StreamState::RESERVED_REMOTE;
    }
}

result<void> Http2Stream::update_send_window(int32_t increment) noexcept {
    if (increment <= 0) {
        return err<void>(error_code::internal_error);
    }

    // Check for overflow (RFC 7540 Section 6.9.1)
    if (send_window_ > INT32_MAX - increment) {
        return err<void>(error_code::internal_error);
    }

    send_window_ += increment;
    return ok();
}

result<void> Http2Stream::update_recv_window(int32_t increment) noexcept {
    if (increment <= 0) {
        return err<void>(error_code::internal_error);
    }

    // Check for overflow
    if (recv_window_ > INT32_MAX - increment) {
        return err<void>(error_code::internal_error);
    }

    recv_window_ += increment;
    return ok();
}

result<void> Http2Stream::consume_send_window(uint32_t size) noexcept {
    // Check if we have enough window
    if (static_cast<int32_t>(size) > send_window_) {
        return err<void>(error_code::internal_error);
    }

    send_window_ -= static_cast<int32_t>(size);
    return ok();
}

result<void> Http2Stream::consume_recv_window(uint32_t size) noexcept {
    // Check if we have enough window
    if (static_cast<int32_t>(size) > recv_window_) {
        return err<void>(error_code::internal_error);
    }

    recv_window_ -= static_cast<int32_t>(size);
    return ok();
}

// StreamManager implementation

StreamManager::StreamManager(uint32_t initial_window_size)
    : initial_window_size_(initial_window_size) {}

result<Http2Stream*> StreamManager::create_stream(uint32_t stream_id) noexcept {
    // Check if stream already exists
    if (streams_.find(stream_id) != streams_.end()) {
        return err<Http2Stream*>(error_code::internal_error);
    }

    // Create new stream
    auto result = streams_.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(stream_id),
        std::forward_as_tuple(stream_id, initial_window_size_)
    );

    if (!result.second) {
        return err<Http2Stream*>(error_code::internal_error);
    }

    return ok(&result.first->second);
}

Http2Stream* StreamManager::get_stream(uint32_t stream_id) noexcept {
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
        return nullptr;
    }
    return &it->second;
}

void StreamManager::remove_stream(uint32_t stream_id) noexcept {
    streams_.erase(stream_id);
}

void StreamManager::update_initial_window_size(uint32_t new_size) noexcept {
    // Calculate difference
    int32_t diff = static_cast<int32_t>(new_size) - static_cast<int32_t>(initial_window_size_);

    // Update all existing streams
    for (auto& pair : streams_) {
        Http2Stream& stream = pair.second;

        // Update send window by difference
        if (diff > 0) {
            stream.update_send_window(diff);
        } else if (diff < 0) {
            // Decrease window (can go negative per spec)
            stream.send_window_ += diff;
        }
    }

    initial_window_size_ = new_size;
}

} // namespace http2
} // namespace fasterapi
