#include "websocket_transport.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <sstream>
#include <iostream>

namespace fasterapi {
namespace mcp {

WebSocketTransport::WebSocketTransport(const std::string& host, uint16_t port)
    : is_server_mode_(true)
    , host_(host)
    , port_(port)
{
}

WebSocketTransport::WebSocketTransport(const std::string& url, const std::string& auth_token)
    : is_server_mode_(false)
    , url_(url)
    , auth_token_(auth_token)
{
}

WebSocketTransport::~WebSocketTransport() {
    disconnect();
}

int WebSocketTransport::connect() {
    if (state_ != TransportState::DISCONNECTED) {
        return -1;
    }

    set_state(TransportState::CONNECTING);

    int result = 0;
    if (is_server_mode_) {
        result = start_server();
    } else {
        result = connect_websocket();
    }

    if (result != 0) {
        set_state(TransportState::ERROR);
        return result;
    }

    running_ = true;
    set_state(TransportState::CONNECTED);
    return 0;
}

int WebSocketTransport::disconnect() {
    if (state_ == TransportState::DISCONNECTED) {
        return 0;
    }

    set_state(TransportState::DISCONNECTING);

    running_ = false;

    if (server_thread_ && server_thread_->joinable()) {
        server_thread_->join();
    }

    if (is_server_mode_) {
        // Close all client connections
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            for (auto& conn : connections_) {
                conn->close(http::websocket::CloseCode::NORMAL, "Server shutdown");
            }
            connections_.clear();
        }

        // Close server socket
        if (server_fd_ >= 0) {
            close(server_fd_);
            server_fd_ = -1;
        }
    } else {
        // Close client connection
        if (connection_) {
            connection_->close(http::websocket::CloseCode::NORMAL, "Client disconnect");
            connection_.reset();
        }
    }

    set_state(TransportState::DISCONNECTED);
    return 0;
}

int WebSocketTransport::send(const std::string& message) {
    if (!is_connected()) {
        return -1;
    }

    if (is_server_mode_) {
        // Broadcast to all connected clients
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (auto& conn : connections_) {
            conn->send_text(message);
        }
        return 0;
    } else {
        // Send to server
        if (connection_) {
            return connection_->send_text(message);
        }
        return -1;
    }
}

std::optional<std::string> WebSocketTransport::receive(uint32_t timeout_ms) {
    std::string message;

    if (timeout_ms == 0) {
        while (running_) {
            if (message_queue_.try_pop(message)) {
                return message;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    } else {
        auto start = std::chrono::steady_clock::now();
        auto deadline = start + std::chrono::milliseconds(timeout_ms);

        while (std::chrono::steady_clock::now() < deadline) {
            if (message_queue_.try_pop(message)) {
                return message;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    return std::nullopt;
}

void WebSocketTransport::set_message_callback(MessageCallback callback) {
    message_callback_ = callback;
}

void WebSocketTransport::set_error_callback(ErrorCallback callback) {
    error_callback_ = callback;
}

void WebSocketTransport::set_state_callback(StateCallback callback) {
    state_callback_ = callback;
}

TransportState WebSocketTransport::get_state() const {
    return state_.load();
}

bool WebSocketTransport::is_connected() const {
    return state_ == TransportState::CONNECTED;
}

void WebSocketTransport::set_state(TransportState new_state) {
    TransportState old_state = state_.exchange(new_state);
    if (old_state != new_state && state_callback_) {
        state_callback_(new_state);
    }
}

void WebSocketTransport::invoke_error(const std::string& error) {
    if (error_callback_) {
        error_callback_(error);
    }
    set_state(TransportState::ERROR);
}

int WebSocketTransport::start_server() {
    // Create socket
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        invoke_error("Failed to create socket");
        return -1;
    }

    // Set socket options
    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    inet_pton(AF_INET, host_.c_str(), &addr.sin_addr);

    if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        invoke_error("Failed to bind socket");
        close(server_fd_);
        server_fd_ = -1;
        return -1;
    }

    // Listen
    if (listen(server_fd_, 10) < 0) {
        invoke_error("Failed to listen");
        close(server_fd_);
        server_fd_ = -1;
        return -1;
    }

    // Start server thread
    server_thread_ = std::make_unique<std::thread>(&WebSocketTransport::server_loop, this);

    return 0;
}

void WebSocketTransport::server_loop() {
    uint64_t conn_id = 0;

    while (running_) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR || !running_) {
                break;
            }
            continue;
        }

        // Create WebSocket connection
        auto conn = std::make_shared<http::WebSocketConnection>(++conn_id);

        // Set message handler
        conn->set_message_callback([this](const std::string& message) {
            on_message(message);
        });

        // Set close handler
        conn->set_close_callback([this, conn]() {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_.erase(
                std::remove(connections_.begin(), connections_.end(), conn),
                connections_.end()
            );
            on_close();
        });

        // Add to connections
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_.push_back(conn);
        }

        // Handle connection in separate thread
        std::thread([this, conn, client_fd]() {
            handle_client_connection(conn);
            close(client_fd);
        }).detach();
    }
}

void WebSocketTransport::handle_client_connection(std::shared_ptr<http::WebSocketConnection> conn) {
    // Connection is managed by WebSocketConnection class
    // Messages are delivered via callback
    while (running_ && conn->is_open()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

int WebSocketTransport::connect_websocket() {
    // Parse URL
    std::string host = url_;
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
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        invoke_error("Failed to create socket");
        return -1;
    }

    // Connect
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (::connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        invoke_error("Failed to connect");
        close(sock_fd);
        return -1;
    }

    // Send WebSocket handshake
    std::ostringstream handshake;
    handshake << "GET " << path << " HTTP/1.1\r\n";
    handshake << "Host: " << host << ":" << port << "\r\n";
    handshake << "Upgrade: websocket\r\n";
    handshake << "Connection: Upgrade\r\n";
    handshake << "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
    handshake << "Sec-WebSocket-Version: 13\r\n";
    if (!auth_token_.empty()) {
        handshake << "Authorization: Bearer " << auth_token_ << "\r\n";
    }
    handshake << "\r\n";

    std::string handshake_str = handshake.str();
    write(sock_fd, handshake_str.c_str(), handshake_str.length());

    // Read handshake response
    char buffer[4096];
    ssize_t bytes_read = read(sock_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        invoke_error("Failed to read handshake response");
        close(sock_fd);
        return -1;
    }
    buffer[bytes_read] = '\0';

    // Verify handshake
    if (strstr(buffer, "101 Switching Protocols") == nullptr) {
        invoke_error("WebSocket handshake failed");
        close(sock_fd);
        return -1;
    }

    // Create WebSocket connection
    connection_ = std::make_shared<http::WebSocketConnection>(1);

    // Set message handler
    connection_->set_message_callback([this](const std::string& message) {
        on_message(message);
    });

    // Set close handler
    connection_->set_close_callback([this]() {
        on_close();
    });

    return 0;
}

void WebSocketTransport::on_message(const std::string& message) {
    if (message_callback_) {
        message_callback_(message);
    } else {
        while (!message_queue_.try_push(message)) {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }
}

void WebSocketTransport::on_close() {
    // Connection closed
    if (error_callback_) {
        error_callback_("WebSocket connection closed");
    }
}

// Factory method
std::unique_ptr<Transport> TransportFactory::create_websocket(
    const std::string& url,
    const std::string& auth_token
) {
    return std::make_unique<WebSocketTransport>(url, auth_token);
}

} // namespace mcp
} // namespace fasterapi
