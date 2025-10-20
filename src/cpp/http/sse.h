#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>
#include <cstdint>

namespace fasterapi {
namespace http {

/**
 * Server-Sent Events (SSE) connection.
 * 
 * Implements the SSE protocol (text/event-stream) for real-time
 * server-to-client push notifications.
 * 
 * Features:
 * - Event streaming with automatic keep-alive
 * - Named events with custom types
 * - Event ID tracking for reconnection
 * - Automatic retry hints for clients
 * - Zero-copy where possible
 * - Backpressure handling
 * 
 * Spec: https://html.spec.whatwg.org/multipage/server-sent-events.html
 */
class SSEConnection {
public:
    /**
     * Create an SSE connection.
     * 
     * @param connection_id Unique connection identifier
     */
    explicit SSEConnection(uint64_t connection_id);
    
    ~SSEConnection();
    
    // Non-copyable, non-movable (due to atomics)
    SSEConnection(const SSEConnection&) = delete;
    SSEConnection& operator=(const SSEConnection&) = delete;
    SSEConnection(SSEConnection&&) = delete;
    SSEConnection& operator=(SSEConnection&&) = delete;
    
    /**
     * Send an event to the client.
     * 
     * @param data Event data (will be sent as "data: ..." lines)
     * @param event Event type (optional, defaults to "message")
     * @param id Event ID (optional, for client reconnection)
     * @param retry Retry time in milliseconds (optional)
     * @return 0 on success, error code otherwise
     * 
     * Example:
     *   sse.send("Hello World");
     *   sse.send("{\"msg\":\"hi\"}", "chat", "123");
     */
    int send(
        const std::string& data,
        const char* event = nullptr,
        const char* id = nullptr,
        int retry = -1
    ) noexcept;
    
    /**
     * Send a comment (ignored by client, useful for keep-alive).
     * 
     * @param comment Comment text
     * @return 0 on success
     */
    int send_comment(const std::string& comment) noexcept;
    
    /**
     * Send a keep-alive ping.
     * 
     * Sends a comment to keep connection alive.
     * @return 0 on success
     */
    int ping() noexcept;
    
    /**
     * Close the connection.
     * 
     * @return 0 on success
     */
    int close() noexcept;
    
    /**
     * Check if connection is open.
     */
    bool is_open() const noexcept;
    
    /**
     * Get connection ID.
     */
    uint64_t get_id() const noexcept;
    
    /**
     * Get number of events sent.
     */
    uint64_t events_sent() const noexcept;
    
    /**
     * Get total bytes sent.
     */
    uint64_t bytes_sent() const noexcept;
    
    /**
     * Set last event ID (for reconnection).
     */
    void set_last_event_id(const std::string& id) noexcept;
    
    /**
     * Get last event ID.
     */
    const std::string& get_last_event_id() const noexcept;
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    
    uint64_t connection_id_;
    std::atomic<bool> open_{true};
    std::atomic<uint64_t> events_sent_{0};
    std::atomic<uint64_t> bytes_sent_{0};
    std::string last_event_id_;
    
    /**
     * Format SSE message according to spec.
     */
    std::string format_message(
        const std::string& data,
        const char* event,
        const char* id,
        int retry
    ) const noexcept;
    
    /**
     * Send raw data to client.
     */
    int send_raw(const std::string& data) noexcept;
};

/**
 * SSE handler function type.
 * 
 * The handler receives an SSE connection and can send events
 * for as long as the connection is open.
 */
using SSEHandler = std::function<void(SSEConnection*)>;

/**
 * SSE endpoint manager.
 * 
 * Manages SSE connections and handles client reconnection.
 */
class SSEEndpoint {
public:
    /**
     * Configuration for SSE endpoint.
     */
    struct Config {
        bool enable_cors;
        std::string allowed_origin;
        uint32_t ping_interval_ms;
        uint32_t max_connections;
        uint32_t buffer_size;
        
        Config()
            : enable_cors(true),
              allowed_origin("*"),
              ping_interval_ms(30000),
              max_connections(10000),
              buffer_size(65536) {}
    };
    
    explicit SSEEndpoint(const Config& config = Config{});
    ~SSEEndpoint();
    
    /**
     * Handle a new SSE connection.
     * 
     * @param handler Function to handle the connection
     * @param last_event_id Last event ID from client (for reconnection)
     * @return Connection object
     */
    SSEConnection* accept(
        SSEHandler handler,
        const std::string& last_event_id = ""
    ) noexcept;
    
    /**
     * Get number of active connections.
     */
    uint32_t active_connections() const noexcept;
    
    /**
     * Get total events sent across all connections.
     */
    uint64_t total_events_sent() const noexcept;
    
    /**
     * Close all connections.
     */
    void close_all() noexcept;
    
private:
    Config config_;
    std::atomic<uint32_t> connection_count_{0};
    std::atomic<uint64_t> total_events_{0};
    std::atomic<uint64_t> next_connection_id_{1};
    
    std::vector<std::unique_ptr<SSEConnection>> connections_;
};

} // namespace http
} // namespace fasterapi

