#include "upstream_connection.h"
#include "../transports/stdio_transport.h"
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace fasterapi {
namespace mcp {
namespace proxy {

// ========== StdioUpstreamConnection Implementation ==========

StdioUpstreamConnection::StdioUpstreamConnection(const UpstreamConfig& config)
    : config_(config) {
}

StdioUpstreamConnection::~StdioUpstreamConnection() {
    disconnect();
}

bool StdioUpstreamConnection::connect() {
    if (connected_) {
        return true;
    }

    // Create STDIO transport in client mode
    transport_ = std::make_shared<StdioTransport>(
        config_.command,
        config_.args,
        TransportMode::CLIENT
    );

    // Connect with timeout
    int result = transport_->connect();
    if (result != 0) {
        return false;
    }

    connected_ = true;
    return true;
}

void StdioUpstreamConnection::disconnect() {
    if (transport_ && connected_) {
        transport_->close();
        connected_ = false;
    }
}

std::optional<std::string> StdioUpstreamConnection::send_request(
    const std::string& request,
    uint32_t timeout_ms
) {
    if (!connected_ || !transport_) {
        return std::nullopt;
    }

    // Send request
    int send_result = transport_->send(request);
    if (send_result != 0) {
        return std::nullopt;
    }

    // Receive response with timeout
    auto response = transport_->receive(timeout_ms);
    return response;
}

bool StdioUpstreamConnection::is_healthy() const {
    return connected_ && transport_ && transport_->is_connected();
}

std::string StdioUpstreamConnection::get_name() const {
    return config_.name;
}

// ========== HttpUpstreamConnection Implementation ==========

HttpUpstreamConnection::HttpUpstreamConnection(const UpstreamConfig& config)
    : config_(config) {
}

HttpUpstreamConnection::~HttpUpstreamConnection() {
    disconnect();
}

bool HttpUpstreamConnection::connect() {
    if (connected_) {
        return true;
    }

    // Parse URL
    std::string host = config_.url;
    uint16_t port = 80;

    // Remove protocol
    if (host.find("http://") == 0) {
        host = host.substr(7);
    } else if (host.find("https://") == 0) {
        host = host.substr(8);
        port = 443;
    }

    // Extract port
    auto colon_pos = host.find(':');
    if (colon_pos != std::string::npos) {
        auto slash_pos = host.find('/', colon_pos);
        std::string port_str;
        if (slash_pos != std::string::npos) {
            port_str = host.substr(colon_pos + 1, slash_pos - colon_pos - 1);
        } else {
            port_str = host.substr(colon_pos + 1);
        }
        port = std::stoi(port_str);
        host = host.substr(0, colon_pos);
    }

    // Remove trailing path
    auto slash_pos = host.find('/');
    if (slash_pos != std::string::npos) {
        host = host.substr(0, slash_pos);
    }

    // Create socket
    sock_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd_ < 0) {
        return false;
    }

    // Set timeout
    struct timeval tv;
    tv.tv_sec = config_.connect_timeout_ms / 1000;
    tv.tv_usec = (config_.connect_timeout_ms % 1000) * 1000;
    setsockopt(sock_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock_fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // Connect
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (::connect(sock_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock_fd_);
        sock_fd_ = -1;
        return false;
    }

    connected_ = true;
    return true;
}

void HttpUpstreamConnection::disconnect() {
    if (sock_fd_ >= 0) {
        close(sock_fd_);
        sock_fd_ = -1;
    }
    connected_ = false;
}

std::optional<std::string> HttpUpstreamConnection::send_request(
    const std::string& request,
    uint32_t timeout_ms
) {
    if (!connected_) {
        if (!connect()) {
            return std::nullopt;
        }
    }

    // Parse URL for path
    std::string path = "/mcp";
    auto url_path_pos = config_.url.find('/', 7);  // After http://
    if (url_path_pos != std::string::npos) {
        path = config_.url.substr(url_path_pos);
    }

    // Parse host for HTTP header
    std::string host = config_.url;
    if (host.find("http://") == 0) {
        host = host.substr(7);
    }
    auto slash_pos = host.find('/');
    if (slash_pos != std::string::npos) {
        host = host.substr(0, slash_pos);
    }

    // Build HTTP POST request
    std::ostringstream http_request;
    http_request << "POST " << path << " HTTP/1.1\r\n";
    http_request << "Host: " << host << "\r\n";
    http_request << "Content-Type: application/json\r\n";
    http_request << "Content-Length: " << request.length() << "\r\n";
    if (!config_.auth_token.empty()) {
        http_request << "Authorization: Bearer " << config_.auth_token << "\r\n";
    }
    http_request << "Connection: keep-alive\r\n";
    http_request << "\r\n";
    http_request << request;

    std::string req_str = http_request.str();

    // Send request
    ssize_t bytes_sent = write(sock_fd_, req_str.c_str(), req_str.length());
    if (bytes_sent < 0) {
        disconnect();
        return std::nullopt;
    }

    // Read response
    std::string response;
    char buffer[4096];

    // Read headers first
    bool headers_complete = false;
    size_t content_length = 0;

    while (!headers_complete) {
        ssize_t bytes_read = read(sock_fd_, buffer, sizeof(buffer) - 1);
        if (bytes_read <= 0) {
            disconnect();
            return std::nullopt;
        }
        buffer[bytes_read] = '\0';
        response += buffer;

        // Check for end of headers
        auto header_end = response.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            headers_complete = true;

            // Extract Content-Length
            auto cl_pos = response.find("Content-Length:");
            if (cl_pos != std::string::npos) {
                auto cl_value_start = cl_pos + 15;
                auto cl_value_end = response.find("\r\n", cl_value_start);
                std::string cl_str = response.substr(cl_value_start, cl_value_end - cl_value_start);
                // Trim whitespace
                cl_str.erase(0, cl_str.find_first_not_of(" \t"));
                content_length = std::stoull(cl_str);
            }

            // Calculate body bytes already read
            size_t body_start = header_end + 4;
            size_t body_bytes_read = response.length() - body_start;

            // Read remaining body if needed
            while (body_bytes_read < content_length) {
                bytes_read = read(sock_fd_, buffer, std::min(sizeof(buffer) - 1, content_length - body_bytes_read));
                if (bytes_read <= 0) {
                    disconnect();
                    return std::nullopt;
                }
                buffer[bytes_read] = '\0';
                response += buffer;
                body_bytes_read += bytes_read;
            }

            break;
        }
    }

    // Extract JSON body
    auto body_start = response.find("\r\n\r\n");
    if (body_start == std::string::npos) {
        return std::nullopt;
    }

    return response.substr(body_start + 4);
}

bool HttpUpstreamConnection::is_healthy() const {
    return connected_;
}

std::string HttpUpstreamConnection::get_name() const {
    return config_.name;
}

// ========== WebSocketUpstreamConnection Implementation ==========

WebSocketUpstreamConnection::WebSocketUpstreamConnection(const UpstreamConfig& config)
    : config_(config) {
}

WebSocketUpstreamConnection::~WebSocketUpstreamConnection() {
    disconnect();
}

bool WebSocketUpstreamConnection::connect() {
    if (connected_) {
        return true;
    }

    // Parse URL
    std::string host = config_.url;
    uint16_t port = 80;
    bool use_tls = false;

    // Remove protocol
    if (host.find("ws://") == 0) {
        host = host.substr(5);
    } else if (host.find("wss://") == 0) {
        host = host.substr(6);
        port = 443;
        use_tls = true;
    }

    // Extract port
    auto colon_pos = host.find(':');
    if (colon_pos != std::string::npos) {
        auto slash_pos = host.find('/', colon_pos);
        std::string port_str;
        if (slash_pos != std::string::npos) {
            port_str = host.substr(colon_pos + 1, slash_pos - colon_pos - 1);
        } else {
            port_str = host.substr(colon_pos + 1);
        }
        port = std::stoi(port_str);
        host = host.substr(0, colon_pos);
    }

    // Extract path
    std::string path = "/";
    auto slash_pos = host.find('/');
    if (slash_pos != std::string::npos) {
        path = host.substr(slash_pos);
        host = host.substr(0, slash_pos);
    }

    // Create socket
    sock_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd_ < 0) {
        return false;
    }

    // Set timeout
    struct timeval tv;
    tv.tv_sec = config_.connect_timeout_ms / 1000;
    tv.tv_usec = (config_.connect_timeout_ms % 1000) * 1000;
    setsockopt(sock_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock_fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // Connect
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (::connect(sock_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock_fd_);
        sock_fd_ = -1;
        return false;
    }

    // Send WebSocket handshake
    std::ostringstream handshake;
    handshake << "GET " << path << " HTTP/1.1\r\n";
    handshake << "Host: " << host << ":" << port << "\r\n";
    handshake << "Upgrade: websocket\r\n";
    handshake << "Connection: Upgrade\r\n";
    handshake << "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
    handshake << "Sec-WebSocket-Version: 13\r\n";
    if (!config_.auth_token.empty()) {
        handshake << "Authorization: Bearer " << config_.auth_token << "\r\n";
    }
    handshake << "\r\n";

    std::string handshake_str = handshake.str();
    write(sock_fd_, handshake_str.c_str(), handshake_str.length());

    // Read handshake response
    char buffer[4096];
    ssize_t bytes_read = read(sock_fd_, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        close(sock_fd_);
        sock_fd_ = -1;
        return false;
    }
    buffer[bytes_read] = '\0';

    // Verify handshake
    if (strstr(buffer, "101 Switching Protocols") == nullptr) {
        close(sock_fd_);
        sock_fd_ = -1;
        return false;
    }

    connected_ = true;
    return true;
}

void WebSocketUpstreamConnection::disconnect() {
    if (sock_fd_ >= 0) {
        // Send WebSocket close frame
        uint8_t close_frame[] = {0x88, 0x02, 0x03, 0xE8};  // FIN + Close opcode, length 2, code 1000
        write(sock_fd_, close_frame, sizeof(close_frame));

        close(sock_fd_);
        sock_fd_ = -1;
    }
    connected_ = false;
}

std::optional<std::string> WebSocketUpstreamConnection::send_request(
    const std::string& request,
    uint32_t timeout_ms
) {
    if (!connected_) {
        if (!connect()) {
            return std::nullopt;
        }
    }

    std::lock_guard<std::mutex> lock(send_mutex_);

    // Build WebSocket frame (text message)
    std::vector<uint8_t> frame;

    // Byte 0: FIN + opcode (text = 0x01)
    frame.push_back(0x81);  // FIN=1, RSV=000, Opcode=0001 (text)

    // Byte 1: Mask + payload length
    size_t payload_len = request.length();
    if (payload_len < 126) {
        frame.push_back(0x80 | payload_len);  // Mask=1, length
    } else if (payload_len < 65536) {
        frame.push_back(0x80 | 126);
        frame.push_back((payload_len >> 8) & 0xFF);
        frame.push_back(payload_len & 0xFF);
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; i--) {
            frame.push_back((payload_len >> (i * 8)) & 0xFF);
        }
    }

    // Masking key (random)
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    for (int i = 0; i < 4; i++) {
        frame.push_back(mask[i]);
    }

    // Masked payload
    for (size_t i = 0; i < payload_len; i++) {
        frame.push_back(request[i] ^ mask[i % 4]);
    }

    // Send frame
    ssize_t bytes_sent = write(sock_fd_, frame.data(), frame.size());
    if (bytes_sent < 0) {
        disconnect();
        return std::nullopt;
    }

    // Read response frame
    uint8_t header[2];
    ssize_t bytes_read = read(sock_fd_, header, 2);
    if (bytes_read < 2) {
        disconnect();
        return std::nullopt;
    }

    // Parse header
    bool fin = (header[0] & 0x80) != 0;
    uint8_t opcode = header[0] & 0x0F;
    bool masked = (header[1] & 0x80) != 0;
    uint64_t payload_length = header[1] & 0x7F;

    // Extended payload length
    if (payload_length == 126) {
        uint8_t len_bytes[2];
        read(sock_fd_, len_bytes, 2);
        payload_length = (len_bytes[0] << 8) | len_bytes[1];
    } else if (payload_length == 127) {
        uint8_t len_bytes[8];
        read(sock_fd_, len_bytes, 8);
        payload_length = 0;
        for (int i = 0; i < 8; i++) {
            payload_length = (payload_length << 8) | len_bytes[i];
        }
    }

    // Read masking key (servers don't mask, but check anyway)
    uint8_t unmask[4] = {0};
    if (masked) {
        read(sock_fd_, unmask, 4);
    }

    // Read payload
    std::vector<uint8_t> payload(payload_length);
    size_t total_read = 0;
    while (total_read < payload_length) {
        bytes_read = read(sock_fd_, payload.data() + total_read, payload_length - total_read);
        if (bytes_read <= 0) {
            disconnect();
            return std::nullopt;
        }
        total_read += bytes_read;
    }

    // Unmask if needed
    if (masked) {
        for (size_t i = 0; i < payload_length; i++) {
            payload[i] ^= unmask[i % 4];
        }
    }

    // Handle different opcodes
    if (opcode == 0x01) {
        // Text frame
        return std::string(payload.begin(), payload.end());
    } else if (opcode == 0x08) {
        // Close frame
        disconnect();
        return std::nullopt;
    } else if (opcode == 0x09) {
        // Ping - send pong
        // (Simplified: just try to read next frame)
        return send_request(request, timeout_ms);
    }

    return std::nullopt;
}

bool WebSocketUpstreamConnection::is_healthy() const {
    return connected_;
}

std::string WebSocketUpstreamConnection::get_name() const {
    return config_.name;
}

// ========== UpstreamConnectionFactory Implementation ==========

std::shared_ptr<UpstreamConnection> UpstreamConnectionFactory::create(const UpstreamConfig& config) {
    if (config.transport_type == "stdio") {
        return std::make_shared<StdioUpstreamConnection>(config);
    } else if (config.transport_type == "http") {
        return std::make_shared<HttpUpstreamConnection>(config);
    } else if (config.transport_type == "websocket") {
        return std::make_shared<WebSocketUpstreamConnection>(config);
    }

    return nullptr;
}

} // namespace proxy
} // namespace mcp
} // namespace fasterapi
