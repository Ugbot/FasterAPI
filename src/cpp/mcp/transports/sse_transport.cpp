#include "sse_transport.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <sstream>
#include <iostream>

namespace fasterapi {
namespace mcp {

SSETransport::SSETransport(const std::string& host, uint16_t port)
    : is_server_mode_(true)
    , host_(host)
    , port_(port)
{
}

SSETransport::SSETransport(const std::string& url, const std::string& auth_token)
    : is_server_mode_(false)
    , url_(url)
    , auth_token_(auth_token)
{
}

SSETransport::~SSETransport() {
    disconnect();
}

int SSETransport::connect() {
    if (state_ != TransportState::DISCONNECTED) {
        return -1;
    }

    set_state(TransportState::CONNECTING);

    int result = 0;
    if (is_server_mode_) {
        result = start_server();
    } else {
        result = connect_sse();
    }

    if (result != 0) {
        set_state(TransportState::ERROR);
        return result;
    }

    // Start reader thread
    running_ = true;
    reader_thread_ = std::make_unique<std::thread>(&SSETransport::reader_loop, this);

    set_state(TransportState::CONNECTED);
    return 0;
}

int SSETransport::disconnect() {
    if (state_ == TransportState::DISCONNECTED) {
        return 0;
    }

    set_state(TransportState::DISCONNECTING);

    running_ = false;

    if (server_thread_ && server_thread_->joinable()) {
        server_thread_->join();
    }

    if (reader_thread_ && reader_thread_->joinable()) {
        reader_thread_->join();
    }

    if (is_server_mode_) {
        // Close all client connections
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (int fd : client_fds_) {
                close(fd);
            }
            client_fds_.clear();
        }

        // Close server socket
        if (server_fd_ >= 0) {
            close(server_fd_);
            server_fd_ = -1;
        }
    } else {
        // Close SSE connection
        if (sse_connection_fd_ >= 0) {
            close(sse_connection_fd_);
            sse_connection_fd_ = -1;
        }
    }

    set_state(TransportState::DISCONNECTED);
    return 0;
}

int SSETransport::send(const std::string& message) {
    if (!is_connected()) {
        return -1;
    }

    if (is_server_mode_) {
        // Broadcast to all connected clients
        return broadcast_sse_event(message);
    } else {
        // Send HTTP POST to server
        return send_http_post(message);
    }
}

std::optional<std::string> SSETransport::receive(uint32_t timeout_ms) {
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

void SSETransport::set_message_callback(MessageCallback callback) {
    message_callback_ = callback;
}

void SSETransport::set_error_callback(ErrorCallback callback) {
    error_callback_ = callback;
}

void SSETransport::set_state_callback(StateCallback callback) {
    state_callback_ = callback;
}

TransportState SSETransport::get_state() const {
    return state_.load();
}

bool SSETransport::is_connected() const {
    return state_ == TransportState::CONNECTED;
}

void SSETransport::set_state(TransportState new_state) {
    TransportState old_state = state_.exchange(new_state);
    if (old_state != new_state && state_callback_) {
        state_callback_(new_state);
    }
}

void SSETransport::invoke_error(const std::string& error) {
    if (error_callback_) {
        error_callback_(error);
    }
    set_state(TransportState::ERROR);
}

int SSETransport::start_server() {
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

    // Start server thread to accept connections
    server_thread_ = std::make_unique<std::thread>(&SSETransport::server_loop, this);

    return 0;
}

void SSETransport::server_loop() {
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

        // Set non-blocking
        fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL) | O_NONBLOCK);

        // Send SSE headers
        const char* headers = 
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";

        write(client_fd, headers, strlen(headers));

        // Add to client list
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            client_fds_.push_back(client_fd);
        }

        // Handle client in separate thread (for POST requests)
        std::thread([this, client_fd]() {
            handle_client_connection(client_fd);
        }).detach();
    }
}

void SSETransport::handle_client_connection(int client_fd) {
    char buffer[4096];

    while (running_) {
        struct pollfd pfd;
        pfd.fd = client_fd;
        pfd.events = POLLIN;

        int poll_result = poll(&pfd, 1, 100);
        if (poll_result <= 0) {
            continue;
        }

        ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
        if (bytes_read <= 0) {
            break;
        }

        buffer[bytes_read] = '\0';

        // Parse HTTP request for POST /message
        if (strstr(buffer, "POST /message") != nullptr) {
            // Extract JSON body
            char* body_start = strstr(buffer, "\r\n\r\n");
            if (body_start) {
                body_start += 4;
                std::string message(body_start);

                // Queue message
                if (message_callback_) {
                    message_callback_(message);
                } else {
                    while (!message_queue_.try_push(message)) {
                        std::this_thread::sleep_for(std::chrono::microseconds(10));
                    }
                }

                // Send response
                const char* response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
                write(client_fd, response, strlen(response));
            }
        }
    }
}

int SSETransport::send_sse_event(int client_fd, const std::string& message) {
    // SSE format: "data: {json}\n\n"
    std::ostringstream oss;
    oss << "data: " << message << "\n\n";
    std::string event = oss.str();

    ssize_t bytes_written = write(client_fd, event.c_str(), event.length());
    if (bytes_written < 0) {
        return -1;
    }

    return 0;
}

int SSETransport::broadcast_sse_event(const std::string& message) {
    std::lock_guard<std::mutex> lock(clients_mutex_);

    // Remove disconnected clients
    auto it = client_fds_.begin();
    while (it != client_fds_.end()) {
        if (send_sse_event(*it, message) != 0) {
            close(*it);
            it = client_fds_.erase(it);
        } else {
            ++it;
        }
    }

    return 0;
}

int SSETransport::connect_sse() {
    // Parse URL
    std::string host = url_;
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
        port = std::stoi(host.substr(colon_pos + 1));
        host = host.substr(0, colon_pos);
    }

    // Remove trailing path
    auto slash_pos = host.find('/');
    if (slash_pos != std::string::npos) {
        host = host.substr(0, slash_pos);
    }

    // Create socket
    sse_connection_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sse_connection_fd_ < 0) {
        invoke_error("Failed to create socket");
        return -1;
    }

    // Connect
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (::connect(sse_connection_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        invoke_error("Failed to connect");
        close(sse_connection_fd_);
        sse_connection_fd_ = -1;
        return -1;
    }

    // Send SSE request
    std::ostringstream request;
    request << "GET /sse HTTP/1.1\r\n";
    request << "Host: " << host << "\r\n";
    request << "Accept: text/event-stream\r\n";
    if (!auth_token_.empty()) {
        request << "Authorization: Bearer " << auth_token_ << "\r\n";
    }
    request << "Cache-Control: no-cache\r\n";
    request << "\r\n";

    std::string req_str = request.str();
    write(sse_connection_fd_, req_str.c_str(), req_str.length());

    // Set non-blocking
    fcntl(sse_connection_fd_, F_SETFL, fcntl(sse_connection_fd_, F_GETFL) | O_NONBLOCK);

    return 0;
}

int SSETransport::send_http_post(const std::string& message) {
    // Parse URL for POST request
    std::string host = url_;
    uint16_t port = 80;

    if (host.find("http://") == 0) {
        host = host.substr(7);
    }

    auto colon_pos = host.find(':');
    if (colon_pos != std::string::npos) {
        port = std::stoi(host.substr(colon_pos + 1));
        host = host.substr(0, colon_pos);
    }

    auto slash_pos = host.find('/');
    if (slash_pos != std::string::npos) {
        host = host.substr(0, slash_pos);
    }

    // Create temporary socket for POST
    int post_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (post_fd < 0) {
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (::connect(post_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(post_fd);
        return -1;
    }

    // Send POST request
    std::ostringstream request;
    request << "POST /message HTTP/1.1\r\n";
    request << "Host: " << host << "\r\n";
    request << "Content-Type: application/json\r\n";
    request << "Content-Length: " << message.length() << "\r\n";
    if (!auth_token_.empty()) {
        request << "Authorization: Bearer " << auth_token_ << "\r\n";
    }
    request << "\r\n";
    request << message;

    std::string req_str = request.str();
    write(post_fd, req_str.c_str(), req_str.length());

    close(post_fd);
    return 0;
}

void SSETransport::reader_loop() {
    if (!is_server_mode_) {
        // Client mode: read SSE events
        static std::string buffer;

        while (running_) {
            auto event = read_sse_event(100);
            if (event.has_value()) {
                if (message_callback_) {
                    message_callback_(event.value());
                } else {
                    while (!message_queue_.try_push(event.value())) {
                        std::this_thread::sleep_for(std::chrono::microseconds(10));
                    }
                }
            }
        }
    }
    // Server mode doesn't need a reader loop (handled per-client)
}

std::optional<std::string> SSETransport::read_sse_event(uint32_t timeout_ms) {
    static std::string buffer;

    struct pollfd pfd;
    pfd.fd = sse_connection_fd_;
    pfd.events = POLLIN;

    int poll_result = poll(&pfd, 1, timeout_ms);
    if (poll_result <= 0) {
        return std::nullopt;
    }

    char temp_buf[4096];
    ssize_t bytes_read = read(sse_connection_fd_, temp_buf, sizeof(temp_buf) - 1);

    if (bytes_read <= 0) {
        return std::nullopt;
    }

    temp_buf[bytes_read] = '\0';
    buffer += temp_buf;

    // Parse SSE event: "data: {json}\n\n"
    auto data_pos = buffer.find("data: ");
    if (data_pos != std::string::npos) {
        auto newline_pos = buffer.find("\n\n", data_pos);
        if (newline_pos != std::string::npos) {
            std::string event = buffer.substr(data_pos + 6, newline_pos - data_pos - 6);
            buffer = buffer.substr(newline_pos + 2);

            // Trim
            while (!event.empty() && std::isspace(event.back())) {
                event.pop_back();
            }

            return event;
        }
    }

    return std::nullopt;
}

// Factory method
std::unique_ptr<Transport> TransportFactory::create_sse(
    const std::string& url,
    const std::string& auth_token
) {
    return std::make_unique<SSETransport>(url, auth_token);
}

} // namespace mcp
} // namespace fasterapi
