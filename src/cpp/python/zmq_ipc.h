#pragma once

#ifdef FASTERAPI_USE_ZMQ

#include "ipc_protocol.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <memory>
#include <vector>
#include <zmq.h>

namespace fasterapi {
namespace python {

/**
 * ZeroMQ-based IPC transport for multi-language worker support.
 * Uses PUSH/PULL pattern for reliable message delivery.
 *
 * Architecture:
 *   C++ → Python (requests):  PUSH → PULL
 *   Python → C++ (responses): PUSH → PULL
 *
 * IPC paths:
 *   ipc:///tmp/fasterapi_<pid>_req
 *   ipc:///tmp/fasterapi_<pid>_resp
 */
class ZmqIPC {
public:
    /**
     * Create ZeroMQ IPC (master/server side).
     * @param ipc_prefix Unique prefix for IPC paths (e.g., "fasterapi_12345")
     */
    explicit ZmqIPC(const std::string& ipc_prefix);

    /**
     * Attach to existing ZeroMQ IPC (worker side).
     * @param ipc_prefix Prefix for IPC paths to connect to
     */
    static std::unique_ptr<ZmqIPC> attach(const std::string& ipc_prefix);

    ~ZmqIPC();

    // Disable copy
    ZmqIPC(const ZmqIPC&) = delete;
    ZmqIPC& operator=(const ZmqIPC&) = delete;

    /**
     * Write a request to the queue (master side).
     * Non-blocking on ZMQ_PUSH socket.
     * @param kwargs_data The serialized kwargs (JSON, binary TLV, or MessagePack)
     * @param format The serialization format used (default: JSON for backward compat)
     * @return true if successful, false on error
     */
    bool write_request(uint32_t request_id,
                      const std::string& module_name,
                      const std::string& function_name,
                      const std::string& kwargs_data,
                      PayloadFormat format = PayloadFormat::FORMAT_JSON);

    /**
     * Write a request with binary kwargs (master side).
     * Zero-copy version for binary data from buffer pool.
     */
    bool write_request_binary(uint32_t request_id,
                              const std::string& module_name,
                              const std::string& function_name,
                              const uint8_t* kwargs_data,
                              size_t kwargs_len,
                              PayloadFormat format = PayloadFormat::FORMAT_BINARY_TLV);

    /**
     * Read a request from the queue (worker side).
     * Blocks on ZMQ_PULL socket.
     * @return true if successful, false on error or shutdown signal
     */
    bool read_request(uint32_t& request_id,
                     std::string& module_name,
                     std::string& function_name,
                     std::string& kwargs_json);

    /**
     * Write a response to the queue (worker side).
     * Non-blocking on ZMQ_PUSH socket.
     */
    bool write_response(uint32_t request_id,
                       uint16_t status_code,
                       bool success,
                       const std::string& body_json,
                       const std::string& error_message = "");

    /**
     * Read a response from the queue (master side).
     * Blocks on ZMQ_PULL socket.
     */
    bool read_response(uint32_t& request_id,
                      uint16_t& status_code,
                      bool& success,
                      std::string& body_json,
                      std::string& error_message);

    /**
     * Signal shutdown to all workers.
     * Sends shutdown message to request queue.
     */
    void signal_shutdown();

    /**
     * Wake the response reader thread (used during shutdown).
     * Sends a dummy response to unblock read_response().
     */
    void wake_response_reader();

    // ========================================================================
    // WebSocket IPC methods
    // ========================================================================

    /**
     * Write a WebSocket event to the queue (master side).
     * @param type Event type (WS_CONNECT, WS_MESSAGE, WS_DISCONNECT)
     * @param connection_id Unique WebSocket connection ID
     * @param path WebSocket path
     * @param payload Message payload (for WS_MESSAGE)
     * @param is_binary True if payload is binary, false for text
     * @return true if successful
     */
    bool write_ws_event(MessageType type,
                        uint64_t connection_id,
                        const std::string& path,
                        const std::string& payload = "",
                        bool is_binary = false);

    /**
     * Read a WebSocket response from Python (master side).
     * @param type Response type (WS_SEND or WS_CLOSE)
     * @param connection_id Target connection ID
     * @param payload Message payload to send
     * @param is_binary True if binary message
     * @param close_code Close code (for WS_CLOSE)
     * @return true if successful
     */
    bool read_ws_response(MessageType& type,
                          uint64_t& connection_id,
                          std::string& payload,
                          bool& is_binary,
                          uint16_t& close_code);

    /**
     * Read any response from the queue (master side).
     * Returns the message type so caller can route appropriately.
     * @param type Output: message type (RESPONSE, WS_SEND, WS_CLOSE)
     * @param data Output: raw message data (including header)
     * @return true if successful, false on error or shutdown
     */
    bool read_any_response(MessageType& type, std::vector<uint8_t>& data);

    /**
     * Parse a WebSocket response from raw data.
     * Use after read_any_response() returns WS_SEND or WS_CLOSE type.
     */
    static bool parse_ws_response(const std::vector<uint8_t>& data,
                                   uint64_t& connection_id,
                                   std::string& payload,
                                   bool& is_binary,
                                   uint16_t& close_code);

    /**
     * Get the IPC prefix (for passing to workers).
     */
    const std::string& get_ipc_prefix() const { return ipc_prefix_; }

    /**
     * Check if this is the master (bind) or worker (connect).
     */
    bool is_master() const { return is_master_; }

    /**
     * Deserialize HTTP response from raw data.
     * Use after read_any_response() returns RESPONSE type.
     */
    static bool deserialize_response(
        const std::vector<uint8_t>& data,
        uint32_t& request_id,
        uint16_t& status_code,
        bool& success,
        std::string& body_json,
        std::string& error_message);

private:
    std::string ipc_prefix_;
    bool is_master_;

    // ZeroMQ context (shared by all sockets)
    void* zmq_context_;

    // Sockets
    void* request_socket_;   // PUSH (master) or PULL (worker)
    void* response_socket_;  // PULL (master) or PUSH (worker)

    // IPC paths
    std::string request_ipc_path_;
    std::string response_ipc_path_;

    std::atomic<bool> shutdown_;

    // Private constructor for attach()
    ZmqIPC(const std::string& ipc_prefix, bool is_master);

    // Initialize ZMQ context and sockets
    bool initialize();

    // Close sockets and context
    void cleanup();

    // Helper to serialize message
    std::vector<uint8_t> serialize_request(
        uint32_t request_id,
        const std::string& module_name,
        const std::string& function_name,
        const std::string& kwargs_data,
        PayloadFormat format);

    // Helper to serialize message with raw binary data
    std::vector<uint8_t> serialize_request_binary(
        uint32_t request_id,
        const std::string& module_name,
        const std::string& function_name,
        const uint8_t* kwargs_data,
        size_t kwargs_len,
        PayloadFormat format);

    // Helper to serialize response
    std::vector<uint8_t> serialize_response(
        uint32_t request_id,
        uint16_t status_code,
        bool success,
        const std::string& body_json,
        const std::string& error_message);

    // Helper to deserialize message
    bool deserialize_request(
        const std::vector<uint8_t>& data,
        uint32_t& request_id,
        std::string& module_name,
        std::string& function_name,
        std::string& kwargs_json);
};

}  // namespace python
}  // namespace fasterapi

#endif  // FASTERAPI_USE_ZMQ
