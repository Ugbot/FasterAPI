#include "sse.h"
#include <sstream>
#include <iostream>

namespace fasterapi {
namespace http {

// ============================================================================
// SSEConnection Implementation
// ============================================================================

struct SSEConnection::Impl {
    // Would hold actual connection state (socket, buffer, etc.)
    // For now, this is a placeholder
    std::vector<std::string> queued_messages;
};

SSEConnection::SSEConnection(uint64_t connection_id)
    : impl_(std::make_unique<Impl>()),
      connection_id_(connection_id) {
}

SSEConnection::~SSEConnection() {
    close();
}

int SSEConnection::send(
    const std::string& data,
    const char* event,
    const char* id,
    int retry
) noexcept {
    if (!open_.load(std::memory_order_acquire)) {
        return 1;  // Connection closed
    }
    
    // Format message according to SSE spec
    std::string message = format_message(data, event, id, retry);
    
    // Send to client
    int result = send_raw(message);
    
    if (result == 0) {
        events_sent_.fetch_add(1, std::memory_order_relaxed);
        bytes_sent_.fetch_add(message.length(), std::memory_order_relaxed);
        
        // Update last event ID if provided
        if (id) {
            last_event_id_ = id;
        }
    }
    
    return result;
}

int SSEConnection::send_comment(const std::string& comment) noexcept {
    if (!open_.load(std::memory_order_acquire)) {
        return 1;
    }
    
    // Comments start with ':'
    std::string message = ": " + comment + "\n\n";
    return send_raw(message);
}

int SSEConnection::ping() noexcept {
    return send_comment("ping");
}

int SSEConnection::close() noexcept {
    bool was_open = open_.exchange(false, std::memory_order_acq_rel);
    if (!was_open) {
        return 1;  // Already closed
    }
    
    // Clean up resources
    impl_->queued_messages.clear();
    
    return 0;
}

bool SSEConnection::is_open() const noexcept {
    return open_.load(std::memory_order_acquire);
}

uint64_t SSEConnection::get_id() const noexcept {
    return connection_id_;
}

uint64_t SSEConnection::events_sent() const noexcept {
    return events_sent_.load(std::memory_order_relaxed);
}

uint64_t SSEConnection::bytes_sent() const noexcept {
    return bytes_sent_.load(std::memory_order_relaxed);
}

void SSEConnection::set_last_event_id(const std::string& id) noexcept {
    last_event_id_ = id;
}

const std::string& SSEConnection::get_last_event_id() const noexcept {
    return last_event_id_;
}

std::string SSEConnection::format_message(
    const std::string& data,
    const char* event,
    const char* id,
    int retry
) const noexcept {
    std::ostringstream oss;
    
    // Event type (optional)
    if (event && event[0] != '\0') {
        oss << "event: " << event << "\n";
    }
    
    // Event ID (optional, for reconnection)
    if (id && id[0] != '\0') {
        oss << "id: " << id << "\n";
    }
    
    // Retry time (optional)
    if (retry >= 0) {
        oss << "retry: " << retry << "\n";
    }
    
    // Data (required) - split by newlines
    std::string::size_type start = 0;
    std::string::size_type end = data.find('\n');
    
    while (end != std::string::npos) {
        oss << "data: " << data.substr(start, end - start) << "\n";
        start = end + 1;
        end = data.find('\n', start);
    }
    
    // Last line (or only line if no newlines)
    if (start < data.length()) {
        oss << "data: " << data.substr(start) << "\n";
    }
    
    // Empty line to signal end of event
    oss << "\n";
    
    return oss.str();
}

int SSEConnection::send_raw(const std::string& data) noexcept {
    if (!open_.load(std::memory_order_acquire)) {
        return 1;
    }
    
    // In real implementation, would write to socket
    // For now, queue the message
    impl_->queued_messages.push_back(data);
    
    return 0;
}

// ============================================================================
// SSEEndpoint Implementation
// ============================================================================

SSEEndpoint::SSEEndpoint(const Config& config)
    : config_(config) {
}

SSEEndpoint::~SSEEndpoint() {
    close_all();
}

SSEConnection* SSEEndpoint::accept(
    SSEHandler handler,
    const std::string& last_event_id
) noexcept {
    if (connection_count_.load() >= config_.max_connections) {
        return nullptr;  // Too many connections
    }
    
    // Create new connection
    uint64_t conn_id = next_connection_id_.fetch_add(1, std::memory_order_relaxed);
    auto conn = std::make_unique<SSEConnection>(conn_id);
    
    // Set last event ID for reconnection
    if (!last_event_id.empty()) {
        conn->set_last_event_id(last_event_id);
    }
    
    connection_count_.fetch_add(1, std::memory_order_relaxed);
    
    // Call handler (in real implementation, this would run async)
    auto* conn_ptr = conn.get();
    connections_.push_back(std::move(conn));
    
    if (handler) {
        handler(conn_ptr);
    }
    
    return conn_ptr;
}

uint32_t SSEEndpoint::active_connections() const noexcept {
    return connection_count_.load(std::memory_order_relaxed);
}

uint64_t SSEEndpoint::total_events_sent() const noexcept {
    uint64_t total = 0;
    for (const auto& conn : connections_) {
        total += conn->events_sent();
    }
    return total;
}

void SSEEndpoint::close_all() noexcept {
    for (auto& conn : connections_) {
        if (conn) {
            conn->close();
        }
    }
    connections_.clear();
    connection_count_.store(0, std::memory_order_relaxed);
}

} // namespace http
} // namespace fasterapi

