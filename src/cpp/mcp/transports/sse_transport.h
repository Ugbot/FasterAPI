#pragma once

#include "transport.h"
#include "../../core/lockfree_queue.h"
#include <thread>
#include <atomic>
#include <memory>

namespace fasterapi {
namespace mcp {

/**
 * SSE (Server-Sent Events) transport for MCP over HTTP.
 *
 * Server mode:
 * - Expose HTTP endpoint at /sse for incoming SSE connections
 * - Send JSON-RPC messages as SSE events to connected clients
 * - Accept POST requests at /message for client->server messages
 *
 * Client mode:
 * - Connect to SSE endpoint for server->client messages
 * - Send POST requests to /message for client->server messages
 *
 * Protocol:
 * - Server->Client: SSE events with JSON-RPC messages
 * - Client->Server: HTTP POST with JSON-RPC messages
 * - Messages are newline-delimited JSON in SSE data fields
 */
class SSETransport : public Transport {
public:
    /**
     * Create SSE transport in server mode.
     *
     * @param host Host to bind to (e.g., "0.0.0.0")
     * @param port Port to listen on
     */
    SSETransport(const std::string& host, uint16_t port);

    /**
     * Create SSE transport in client mode.
     *
     * @param url Server URL (e.g., "http://localhost:8000")
     * @param auth_token Optional authentication token
     */
    SSETransport(const std::string& url, const std::string& auth_token = "");

    ~SSETransport() override;

    // Transport interface
    int connect() override;
    int disconnect() override;
    int send(const std::string& message) override;
    std::optional<std::string> receive(uint32_t timeout_ms = 0) override;
    void set_message_callback(MessageCallback callback) override;
    void set_error_callback(ErrorCallback callback) override;
    void set_state_callback(StateCallback callback) override;
    TransportState get_state() const override;
    bool is_connected() const override;
    TransportType get_type() const override { return TransportType::SSE; }
    std::string get_name() const override { return "sse"; }

private:
    // Mode
    bool is_server_mode_;

    // Server mode
    std::string host_;
    uint16_t port_;
    int server_fd_ = -1;
    std::vector<int> client_fds_;
    std::mutex clients_mutex_;

    // Client mode
    std::string url_;
    std::string auth_token_;
    int sse_connection_fd_ = -1;

    // State
    std::atomic<TransportState> state_{TransportState::DISCONNECTED};

    // Callbacks
    MessageCallback message_callback_;
    ErrorCallback error_callback_;
    StateCallback state_callback_;

    // Reader/server threads
    std::unique_ptr<std::thread> server_thread_;
    std::unique_ptr<std::thread> reader_thread_;
    std::atomic<bool> running_{false};

    // Message queue
    core::AeronMPMCQueue<std::string> message_queue_{16384};

    // Internal methods
    void server_loop();
    void reader_loop();
    void set_state(TransportState new_state);
    void invoke_error(const std::string& error);

    // Server mode methods
    int start_server();
    void handle_client_connection(int client_fd);
    int send_sse_event(int client_fd, const std::string& message);
    int broadcast_sse_event(const std::string& message);

    // Client mode methods
    int connect_sse();
    int send_http_post(const std::string& message);
    std::optional<std::string> read_sse_event(uint32_t timeout_ms);
};

} // namespace mcp
} // namespace fasterapi
