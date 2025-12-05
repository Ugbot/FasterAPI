#include "zmq_ipc.h"

#ifdef FASTERAPI_USE_ZMQ

#include "../core/logger.h"
#include <cstring>
#include <vector>
#include <unistd.h>

namespace fasterapi {
namespace python {

using namespace fasterapi::core;

ZmqIPC::ZmqIPC(const std::string& ipc_prefix)
    : ipc_prefix_(ipc_prefix)
    , is_master_(true)
    , zmq_context_(nullptr)
    , request_socket_(nullptr)
    , response_socket_(nullptr)
    , shutdown_(false) {

    // Generate IPC paths
    request_ipc_path_ = "ipc:///tmp/" + ipc_prefix + "_req";
    response_ipc_path_ = "ipc:///tmp/" + ipc_prefix + "_resp";

    LOG_INFO("ZmqIPC", "Initializing ZeroMQ IPC (master)");
    LOG_INFO("ZmqIPC", "Request path:  %s", request_ipc_path_.c_str());
    LOG_INFO("ZmqIPC", "Response path: %s", response_ipc_path_.c_str());

    if (!initialize()) {
        LOG_ERROR("ZmqIPC", "Failed to initialize ZeroMQ IPC");
    }
}

ZmqIPC::ZmqIPC(const std::string& ipc_prefix, bool is_master)
    : ipc_prefix_(ipc_prefix)
    , is_master_(is_master)
    , zmq_context_(nullptr)
    , request_socket_(nullptr)
    , response_socket_(nullptr)
    , shutdown_(false) {

    // Generate IPC paths
    request_ipc_path_ = "ipc:///tmp/" + ipc_prefix + "_req";
    response_ipc_path_ = "ipc:///tmp/" + ipc_prefix + "_resp";

    LOG_INFO("ZmqIPC", "Attaching to ZeroMQ IPC (worker)");

    if (!initialize()) {
        LOG_ERROR("ZmqIPC", "Failed to attach to ZeroMQ IPC");
    }
}

std::unique_ptr<ZmqIPC> ZmqIPC::attach(const std::string& ipc_prefix) {
    return std::unique_ptr<ZmqIPC>(new ZmqIPC(ipc_prefix, false));
}

ZmqIPC::~ZmqIPC() {
    cleanup();
}

bool ZmqIPC::initialize() {
    // Create ZeroMQ context
    zmq_context_ = zmq_ctx_new();
    if (!zmq_context_) {
        LOG_ERROR("ZmqIPC", "Failed to create ZMQ context: %s", zmq_strerror(errno));
        return false;
    }

    // Set context options for better performance
    zmq_ctx_set(zmq_context_, ZMQ_IO_THREADS, 1);

    if (is_master_) {
        // Master: PUSH requests, PULL responses
        request_socket_ = zmq_socket(zmq_context_, ZMQ_PUSH);
        if (!request_socket_) {
            LOG_ERROR("ZmqIPC", "Failed to create request socket: %s", zmq_strerror(errno));
            return false;
        }

        // Bind request socket
        if (zmq_bind(request_socket_, request_ipc_path_.c_str()) != 0) {
            LOG_ERROR("ZmqIPC", "Failed to bind request socket to %s: %s",
                     request_ipc_path_.c_str(), zmq_strerror(errno));
            return false;
        }

        response_socket_ = zmq_socket(zmq_context_, ZMQ_PULL);
        if (!response_socket_) {
            LOG_ERROR("ZmqIPC", "Failed to create response socket: %s", zmq_strerror(errno));
            return false;
        }

        // Bind response socket
        if (zmq_bind(response_socket_, response_ipc_path_.c_str()) != 0) {
            LOG_ERROR("ZmqIPC", "Failed to bind response socket to %s: %s",
                     response_ipc_path_.c_str(), zmq_strerror(errno));
            return false;
        }

        LOG_INFO("ZmqIPC", "Master sockets bound successfully");

    } else {
        // Worker: PULL requests, PUSH responses
        request_socket_ = zmq_socket(zmq_context_, ZMQ_PULL);
        if (!request_socket_) {
            LOG_ERROR("ZmqIPC", "Failed to create request socket: %s", zmq_strerror(errno));
            return false;
        }

        // Connect request socket (with retry)
        int retries = 10;
        while (retries > 0) {
            if (zmq_connect(request_socket_, request_ipc_path_.c_str()) == 0) {
                break;
            }
            LOG_WARN("ZmqIPC", "Failed to connect to %s, retrying... (%d left)",
                    request_ipc_path_.c_str(), retries);
            usleep(100000);  // 100ms
            retries--;
        }

        if (retries == 0) {
            LOG_ERROR("ZmqIPC", "Failed to connect request socket to %s: %s",
                     request_ipc_path_.c_str(), zmq_strerror(errno));
            return false;
        }

        response_socket_ = zmq_socket(zmq_context_, ZMQ_PUSH);
        if (!response_socket_) {
            LOG_ERROR("ZmqIPC", "Failed to create response socket: %s", zmq_strerror(errno));
            return false;
        }

        // Connect response socket
        if (zmq_connect(response_socket_, response_ipc_path_.c_str()) != 0) {
            LOG_ERROR("ZmqIPC", "Failed to connect response socket to %s: %s",
                     response_ipc_path_.c_str(), zmq_strerror(errno));
            return false;
        }

        LOG_INFO("ZmqIPC", "Worker sockets connected successfully");
    }

    return true;
}

void ZmqIPC::cleanup() {
    if (request_socket_) {
        zmq_close(request_socket_);
        request_socket_ = nullptr;
    }

    if (response_socket_) {
        zmq_close(response_socket_);
        response_socket_ = nullptr;
    }

    if (zmq_context_) {
        zmq_ctx_destroy(zmq_context_);
        zmq_context_ = nullptr;
    }

    // Clean up IPC files (master only)
    if (is_master_) {
        std::string req_file = "/tmp/" + ipc_prefix_ + "_req";
        std::string resp_file = "/tmp/" + ipc_prefix_ + "_resp";
        unlink(req_file.c_str());
        unlink(resp_file.c_str());
    }
}

std::vector<uint8_t> ZmqIPC::serialize_request(
    uint32_t request_id,
    const std::string& module_name,
    const std::string& function_name,
    const std::string& kwargs_data,
    PayloadFormat format) {

    MessageHeader header;
    header.type = MessageType::REQUEST;
    header.request_id = request_id;
    header.module_name_len = static_cast<uint32_t>(module_name.size());
    header.function_name_len = static_cast<uint32_t>(function_name.size());
    header.kwargs_len = static_cast<uint32_t>(kwargs_data.size());
    header.kwargs_format = format;
    header.total_length = sizeof(MessageHeader) +
                         module_name.size() +
                         function_name.size() +
                         kwargs_data.size();

    std::vector<uint8_t> data(header.total_length);
    size_t offset = 0;

    // Copy header
    std::memcpy(data.data() + offset, &header, sizeof(MessageHeader));
    offset += sizeof(MessageHeader);

    // Copy strings
    std::memcpy(data.data() + offset, module_name.data(), module_name.size());
    offset += module_name.size();

    std::memcpy(data.data() + offset, function_name.data(), function_name.size());
    offset += function_name.size();

    std::memcpy(data.data() + offset, kwargs_data.data(), kwargs_data.size());

    return data;
}

std::vector<uint8_t> ZmqIPC::serialize_request_binary(
    uint32_t request_id,
    const std::string& module_name,
    const std::string& function_name,
    const uint8_t* kwargs_data,
    size_t kwargs_len,
    PayloadFormat format) {

    MessageHeader header;
    header.type = MessageType::REQUEST;
    header.request_id = request_id;
    header.module_name_len = static_cast<uint32_t>(module_name.size());
    header.function_name_len = static_cast<uint32_t>(function_name.size());
    header.kwargs_len = static_cast<uint32_t>(kwargs_len);
    header.kwargs_format = format;
    header.total_length = sizeof(MessageHeader) +
                         module_name.size() +
                         function_name.size() +
                         kwargs_len;

    std::vector<uint8_t> data(header.total_length);
    size_t offset = 0;

    // Copy header
    std::memcpy(data.data() + offset, &header, sizeof(MessageHeader));
    offset += sizeof(MessageHeader);

    // Copy strings
    std::memcpy(data.data() + offset, module_name.data(), module_name.size());
    offset += module_name.size();

    std::memcpy(data.data() + offset, function_name.data(), function_name.size());
    offset += function_name.size();

    // Copy binary kwargs data
    if (kwargs_len > 0 && kwargs_data) {
        std::memcpy(data.data() + offset, kwargs_data, kwargs_len);
    }

    return data;
}

std::vector<uint8_t> ZmqIPC::serialize_response(
    uint32_t request_id,
    uint16_t status_code,
    bool success,
    const std::string& body_json,
    const std::string& error_message) {

    ResponseHeader header;
    header.type = MessageType::RESPONSE;
    header.request_id = request_id;
    header.status_code = status_code;
    header.success = success ? 1 : 0;
    header.body_len = static_cast<uint32_t>(body_json.size());
    header.error_message_len = static_cast<uint32_t>(error_message.size());
    header.body_format = PayloadFormat::FORMAT_JSON;  // Default to JSON for now
    header.total_length = sizeof(ResponseHeader) +
                         body_json.size() +
                         error_message.size();

    std::vector<uint8_t> data(header.total_length);
    size_t offset = 0;

    // Copy header
    std::memcpy(data.data() + offset, &header, sizeof(ResponseHeader));
    offset += sizeof(ResponseHeader);

    // Copy strings
    std::memcpy(data.data() + offset, body_json.data(), body_json.size());
    offset += body_json.size();

    std::memcpy(data.data() + offset, error_message.data(), error_message.size());

    return data;
}

bool ZmqIPC::deserialize_request(
    const std::vector<uint8_t>& data,
    uint32_t& request_id,
    std::string& module_name,
    std::string& function_name,
    std::string& kwargs_json) {

    if (data.size() < sizeof(MessageHeader)) {
        return false;
    }

    MessageHeader header;
    std::memcpy(&header, data.data(), sizeof(MessageHeader));

    if (header.type != MessageType::REQUEST) {
        return false;
    }

    request_id = header.request_id;

    size_t offset = sizeof(MessageHeader);

    // Extract strings
    module_name.assign(
        reinterpret_cast<const char*>(data.data() + offset),
        header.module_name_len);
    offset += header.module_name_len;

    function_name.assign(
        reinterpret_cast<const char*>(data.data() + offset),
        header.function_name_len);
    offset += header.function_name_len;

    // Note: kwargs_json may contain binary data if format is not JSON
    // The Python side will handle format detection based on magic byte
    kwargs_json.assign(
        reinterpret_cast<const char*>(data.data() + offset),
        header.kwargs_len);

    return true;
}

bool ZmqIPC::deserialize_response(
    const std::vector<uint8_t>& data,
    uint32_t& request_id,
    uint16_t& status_code,
    bool& success,
    std::string& body_json,
    std::string& error_message) {

    if (data.size() < sizeof(ResponseHeader)) {
        return false;
    }

    ResponseHeader header;
    std::memcpy(&header, data.data(), sizeof(ResponseHeader));

    if (header.type != MessageType::RESPONSE) {
        return false;
    }

    request_id = header.request_id;
    status_code = header.status_code;
    success = (header.success != 0);

    size_t offset = sizeof(ResponseHeader);

    // Extract strings (body may contain binary data if format is not JSON)
    body_json.assign(
        reinterpret_cast<const char*>(data.data() + offset),
        header.body_len);
    offset += header.body_len;

    error_message.assign(
        reinterpret_cast<const char*>(data.data() + offset),
        header.error_message_len);

    return true;
}

bool ZmqIPC::write_request(
    uint32_t request_id,
    const std::string& module_name,
    const std::string& function_name,
    const std::string& kwargs_data,
    PayloadFormat format) {

    if (!request_socket_) {
        return false;
    }

    auto data = serialize_request(request_id, module_name, function_name, kwargs_data, format);

    int rc = zmq_send(request_socket_, data.data(), data.size(), 0);
    if (rc == -1) {
        LOG_ERROR("ZmqIPC", "Failed to send request: %s", zmq_strerror(errno));
        return false;
    }

    return true;
}

bool ZmqIPC::write_request_binary(
    uint32_t request_id,
    const std::string& module_name,
    const std::string& function_name,
    const uint8_t* kwargs_data,
    size_t kwargs_len,
    PayloadFormat format) {

    if (!request_socket_) {
        return false;
    }

    auto data = serialize_request_binary(request_id, module_name, function_name,
                                          kwargs_data, kwargs_len, format);

    int rc = zmq_send(request_socket_, data.data(), data.size(), 0);
    if (rc == -1) {
        LOG_ERROR("ZmqIPC", "Failed to send binary request: %s", zmq_strerror(errno));
        return false;
    }

    return true;
}

bool ZmqIPC::read_request(
    uint32_t& request_id,
    std::string& module_name,
    std::string& function_name,
    std::string& kwargs_json) {

    if (!request_socket_) {
        return false;
    }

    // Check for shutdown
    if (shutdown_.load(std::memory_order_acquire)) {
        return false;
    }

    // Receive message (blocking)
    zmq_msg_t msg;
    zmq_msg_init(&msg);

    int rc = zmq_msg_recv(&msg, request_socket_, 0);
    if (rc == -1) {
        zmq_msg_close(&msg);
        if (errno == EAGAIN || errno == EINTR) {
            return false;  // Non-fatal
        }
        LOG_ERROR("ZmqIPC", "Failed to receive request: %s", zmq_strerror(errno));
        return false;
    }

    // Extract data
    size_t size = zmq_msg_size(&msg);
    std::vector<uint8_t> data(size);
    std::memcpy(data.data(), zmq_msg_data(&msg), size);
    zmq_msg_close(&msg);

    // Check for shutdown message
    if (size >= sizeof(MessageHeader)) {
        MessageHeader header;
        std::memcpy(&header, data.data(), sizeof(MessageHeader));
        if (header.type == MessageType::SHUTDOWN) {
            LOG_INFO("ZmqIPC", "Received shutdown signal");
            shutdown_.store(true, std::memory_order_release);
            return false;
        }
    }

    // Deserialize
    return deserialize_request(data, request_id, module_name, function_name, kwargs_json);
}

bool ZmqIPC::write_response(
    uint32_t request_id,
    uint16_t status_code,
    bool success,
    const std::string& body_json,
    const std::string& error_message) {

    if (!response_socket_) {
        return false;
    }

    auto data = serialize_response(request_id, status_code, success, body_json, error_message);

    int rc = zmq_send(response_socket_, data.data(), data.size(), 0);
    if (rc == -1) {
        LOG_ERROR("ZmqIPC", "Failed to send response: %s", zmq_strerror(errno));
        return false;
    }

    return true;
}

bool ZmqIPC::read_response(
    uint32_t& request_id,
    uint16_t& status_code,
    bool& success,
    std::string& body_json,
    std::string& error_message) {

    if (!response_socket_) {
        return false;
    }

    // Check for shutdown
    if (shutdown_.load(std::memory_order_acquire)) {
        return false;
    }

    // Receive message (blocking)
    zmq_msg_t msg;
    zmq_msg_init(&msg);

    int rc = zmq_msg_recv(&msg, response_socket_, 0);
    if (rc == -1) {
        zmq_msg_close(&msg);
        if (errno == EAGAIN || errno == EINTR) {
            return false;  // Non-fatal
        }
        LOG_ERROR("ZmqIPC", "Failed to receive response: %s", zmq_strerror(errno));
        return false;
    }

    // Extract data
    size_t size = zmq_msg_size(&msg);
    std::vector<uint8_t> data(size);
    std::memcpy(data.data(), zmq_msg_data(&msg), size);
    zmq_msg_close(&msg);

    // Deserialize
    return deserialize_response(data, request_id, status_code, success, body_json, error_message);
}

void ZmqIPC::signal_shutdown() {
    if (!request_socket_) {
        return;
    }

    LOG_INFO("ZmqIPC", "Signaling shutdown to workers");

    // Send shutdown message
    MessageHeader header;
    header.type = MessageType::SHUTDOWN;
    header.request_id = 0;
    header.total_length = sizeof(MessageHeader);
    header.module_name_len = 0;
    header.function_name_len = 0;
    header.kwargs_len = 0;
    header.kwargs_format = PayloadFormat::FORMAT_JSON;

    zmq_send(request_socket_, &header, sizeof(MessageHeader), 0);

    shutdown_.store(true, std::memory_order_release);
}

void ZmqIPC::wake_response_reader() {
    if (!response_socket_) {
        return;
    }

    LOG_INFO("ZmqIPC", "Waking response reader thread");

    // Send dummy response to unblock reader
    ResponseHeader header;
    header.type = MessageType::RESPONSE;
    header.request_id = 0xFFFFFFFF;  // Sentinel value
    header.total_length = sizeof(ResponseHeader);
    header.status_code = 0;
    header.success = 0;
    header.body_len = 0;
    header.error_message_len = 0;
    header.body_format = PayloadFormat::FORMAT_JSON;

    // Send from a temporary socket to avoid blocking
    void* temp_socket = zmq_socket(zmq_context_, ZMQ_PUSH);
    if (temp_socket) {
        zmq_connect(temp_socket, response_ipc_path_.c_str());
        zmq_send(temp_socket, &header, sizeof(ResponseHeader), ZMQ_DONTWAIT);
        zmq_close(temp_socket);
    }
}

// =============================================================================
// WebSocket IPC methods
// =============================================================================

bool ZmqIPC::write_ws_event(MessageType type,
                             uint64_t connection_id,
                             const std::string& path,
                             const std::string& payload,
                             bool is_binary) {
    if (!is_master_ || !request_socket_) {
        LOG_ERROR("ZmqIPC", "write_ws_event called on non-master or socket not ready");
        return false;
    }

    // Build WebSocket message
    WebSocketMessageHeader header;
    header.type = type;
    header.connection_id = connection_id;
    header.path_len = static_cast<uint32_t>(path.size());
    header.payload_len = static_cast<uint32_t>(payload.size());
    header.is_binary = is_binary ? 1 : 0;
    header.total_length = sizeof(WebSocketMessageHeader) + header.path_len + header.payload_len;

    // Serialize: header + path + payload
    std::vector<uint8_t> message(header.total_length);
    memcpy(message.data(), &header, sizeof(WebSocketMessageHeader));
    if (!path.empty()) {
        memcpy(message.data() + sizeof(WebSocketMessageHeader), path.data(), path.size());
    }
    if (!payload.empty()) {
        memcpy(message.data() + sizeof(WebSocketMessageHeader) + path.size(), payload.data(), payload.size());
    }

    // Send via ZMQ
    int rc = zmq_send(request_socket_, message.data(), message.size(), ZMQ_DONTWAIT);
    if (rc < 0) {
        LOG_ERROR("ZmqIPC", "write_ws_event send failed: %s", zmq_strerror(zmq_errno()));
        return false;
    }

    LOG_DEBUG("ZmqIPC", "Sent WS event type=%d conn=%lu path=%s payload_len=%zu",
              static_cast<int>(type), connection_id, path.c_str(), payload.size());

    return true;
}

bool ZmqIPC::read_ws_response(MessageType& type,
                               uint64_t& connection_id,
                               std::string& payload,
                               bool& is_binary,
                               uint16_t& close_code) {
    // Read raw data first
    std::vector<uint8_t> data;
    MessageType msg_type;
    if (!read_any_response(msg_type, data)) {
        return false;
    }

    type = msg_type;

    // Only parse WS responses
    if (msg_type != MessageType::WS_SEND && msg_type != MessageType::WS_CLOSE) {
        return false;
    }

    return parse_ws_response(data, connection_id, payload, is_binary, close_code);
}

bool ZmqIPC::read_any_response(MessageType& type, std::vector<uint8_t>& data) {
    if (!response_socket_) {
        return false;
    }

    // Check for shutdown
    if (shutdown_.load(std::memory_order_acquire)) {
        return false;
    }

    // Receive message (blocking)
    zmq_msg_t msg;
    zmq_msg_init(&msg);

    int rc = zmq_msg_recv(&msg, response_socket_, 0);
    if (rc == -1) {
        zmq_msg_close(&msg);
        if (errno == EAGAIN || errno == EINTR) {
            return false;  // Non-fatal
        }
        LOG_ERROR("ZmqIPC", "Failed to receive response: %s", zmq_strerror(errno));
        return false;
    }

    // Extract data
    size_t size = zmq_msg_size(&msg);
    if (size < 1) {
        zmq_msg_close(&msg);
        return false;
    }

    data.resize(size);
    std::memcpy(data.data(), zmq_msg_data(&msg), size);
    zmq_msg_close(&msg);

    // Extract message type (first byte)
    type = static_cast<MessageType>(data[0]);

    return true;
}

bool ZmqIPC::parse_ws_response(const std::vector<uint8_t>& data,
                                uint64_t& connection_id,
                                std::string& payload,
                                bool& is_binary,
                                uint16_t& close_code) {
    if (data.size() < sizeof(WebSocketResponseHeader)) {
        LOG_ERROR("ZmqIPC", "WS response too small: %zu bytes", data.size());
        return false;
    }

    WebSocketResponseHeader header;
    std::memcpy(&header, data.data(), sizeof(WebSocketResponseHeader));

    connection_id = header.connection_id;
    is_binary = header.is_binary != 0;
    close_code = header.close_code;

    // Extract payload
    if (header.payload_len > 0) {
        size_t offset = sizeof(WebSocketResponseHeader);
        if (offset + header.payload_len <= data.size()) {
            payload.assign(
                reinterpret_cast<const char*>(data.data() + offset),
                header.payload_len);
        } else {
            LOG_WARN("ZmqIPC", "WS response payload truncated");
            payload.assign(
                reinterpret_cast<const char*>(data.data() + offset),
                data.size() - offset);
        }
    } else {
        payload.clear();
    }

    LOG_DEBUG("ZmqIPC", "Parsed WS response: type=%d conn=%lu payload_len=%zu",
              static_cast<int>(header.type), connection_id, payload.size());

    return true;
}

}  // namespace python
}  // namespace fasterapi

#endif  // FASTERAPI_USE_ZMQ
