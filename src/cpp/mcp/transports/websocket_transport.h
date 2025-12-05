#pragma once

#include "transport.h"
#include "../../core/lockfree_queue.h"
#include "../../http/websocket.h"
#include <thread>
#include <atomic>
#include <memory>

namespace fasterapi {
namespace mcp {

/**
 * WebSocket transport for MCP.
 *
 * Server mode:
 * - Expose WebSocket endpoint at /ws
 * - Bidirectional JSON-RPC message exchange
 * - Multiple clients supported
 *
 * Client mode:
 * - Connect to WebSocket endpoint
 * - Bidirectional JSON-RPC message exchange
 *
 * Protocol:
 * - Messages are JSON-RPC over WebSocket text frames
 * - One JSON-RPC message per WebSocket frame
 * - Binary frames are reserved for future use
 */
class WebSocketTransport : public Transport {
public:
    /**
     * Create WebSocket transport in server mode.
     *
     * @param host Host to bind to
     * @param port Port to listen on
     */
    WebSocketTransport(const std::string& host, uint16_t port);

    /**
     * Create WebSocket transport in client mode.
     *
     * @param url WebSocket URL (ws:// or wss://)
     * @param auth_token Optional authentication token
     */
    WebSocketTransport(const std::string& url, const std::string& auth_token = "");

    ~WebSocketTransport() override;

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
    TransportType get_type() const override { return TransportType::WEBSOCKET; }
    std::string get_name() const override { return "websocket"; }

private:
    // Mode
    bool is_server_mode_;

    // Server mode
    std::string host_;
    uint16_t port_;
    int server_fd_ = -1;
    std::vector<std::shared_ptr<http::WebSocketConnection>> connections_;
    std::mutex connections_mutex_;

    // Client mode
    std::string url_;
    std::string auth_token_;
    std::shared_ptr<http::WebSocketConnection> connection_;

    // State
    std::atomic<TransportState> state_{TransportState::DISCONNECTED};

    // Callbacks
    MessageCallback message_callback_;
    ErrorCallback error_callback_;
    StateCallback state_callback_;

    // Server thread
    std::unique_ptr<std::thread> server_thread_;
    std::atomic<bool> running_{false};

    // Message queue
    core::AeronMPMCQueue<std::string> message_queue_{16384};

    // Internal methods
    void server_loop();
    void set_state(TransportState new_state);
    void invoke_error(const std::string& error);

    // Server mode methods
    int start_server();
    void handle_client_connection(std::shared_ptr<http::WebSocketConnection> conn);

    // Client mode methods
    int connect_websocket();

    // WebSocket message handlers
    void on_message(const std::string& message);
    void on_close();
};

} // namespace mcp
} // namespace fasterapi
