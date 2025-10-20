#pragma once

#include "transport.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>

namespace fasterapi {
namespace mcp {

/**
 * STDIO transport for local subprocess communication.
 *
 * Modes:
 * 1. Server mode: Read from stdin, write to stdout
 * 2. Client mode: Launch subprocess, communicate via its stdin/stdout
 *
 * Protocol:
 * - Messages are newline-delimited JSON
 * - Each message is a complete JSON-RPC message
 * - Binary data is base64-encoded
 */
class StdioTransport : public Transport {
public:
    /**
     * Create STDIO transport in server mode (use current stdin/stdout).
     */
    StdioTransport();

    /**
     * Create STDIO transport in client mode (launch subprocess).
     *
     * @param command Command to execute
     * @param args Command arguments
     */
    StdioTransport(const std::string& command, const std::vector<std::string>& args);

    ~StdioTransport() override;

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
    TransportType get_type() const override { return TransportType::STDIO; }
    std::string get_name() const override { return "stdio"; }

private:
    // Mode
    bool is_server_mode_;

    // Subprocess info (client mode)
    std::string command_;
    std::vector<std::string> args_;
    int child_pid_ = -1;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;

    // State
    std::atomic<TransportState> state_{TransportState::DISCONNECTED};

    // Callbacks
    MessageCallback message_callback_;
    ErrorCallback error_callback_;
    StateCallback state_callback_;

    // Reader thread
    std::unique_ptr<std::thread> reader_thread_;
    std::atomic<bool> reader_running_{false};

    // Message queue
    std::queue<std::string> message_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    // Internal methods
    void reader_loop();
    void set_state(TransportState new_state);
    void invoke_error(const std::string& error);
    int launch_subprocess();
    int close_subprocess();
    std::optional<std::string> read_line(int fd, uint32_t timeout_ms);
    int write_line(int fd, const std::string& line);
};

} // namespace mcp
} // namespace fasterapi
