/**
 * Native HTTP/1.1 server using TcpListener + EventLoop
 *
 * Demonstrates:
 * - Multi-threaded HTTP/1.1 server
 * - Zero-copy HTTP parsing
 * - High-performance request handling
 */

#include "../src/cpp/net/tcp_listener.h"
#include "../src/cpp/net/tcp_socket.h"
#include "../src/cpp/net/event_loop.h"
#include "../src/cpp/http/http1_parser.h"
#include <iostream>
#include <csignal>
#include <memory>
#include <unordered_map>
#include <cstring>
#include <unistd.h>

using namespace fasterapi::net;
using namespace fasterapi::http;

// Global listener pointer for signal handler
static std::unique_ptr<TcpListener> g_listener;

// Signal handler for Ctrl+C
void signal_handler(int sig) {
    if (sig == SIGINT) {
        std::cout << "\nStopping server..." << std::endl;
        if (g_listener) {
            g_listener->stop();
        }
    }
}

// Connection state
struct HttpConnection {
    int fd;
    char buffer[16384];  // 16KB buffer for HTTP requests
    size_t buffer_pos;   // Current position in buffer
    EventLoop* event_loop;
    HTTP1Parser parser;
};

// Per-worker connection storage (thread-local)
thread_local std::unordered_map<int, std::unique_ptr<HttpConnection>> t_connections;

// Simple HTTP response builder
std::string build_response(const HTTP1Request& request) {
    std::string response;

    // Status line
    response = "HTTP/1.1 200 OK\r\n";

    // Headers
    response += "Content-Type: text/plain\r\n";
    response += "Connection: keep-alive\r\n";

    // Body
    std::string body = "Hello from FasterAPI!\n";
    body += "Method: ";
    body += std::string(request.method_str);
    body += "\n";
    body += "Path: ";
    body += std::string(request.path);
    body += "\n";

    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "\r\n";
    response += body;

    return response;
}

/**
 * Handle HTTP client events
 */
void handle_http_client(int fd, IOEvent events, void* user_data) {
    auto it = t_connections.find(fd);
    if (it == t_connections.end()) {
        return;
    }

    HttpConnection* conn = it->second.get();

    // Handle errors
    if (events & IOEvent::ERROR) {
        conn->event_loop->remove_fd(fd);
        close(fd);
        t_connections.erase(it);
        return;
    }

    // Handle readable event
    if (events & IOEvent::READ) {
        // Read data into buffer
        ssize_t n = recv(fd, conn->buffer + conn->buffer_pos,
                         sizeof(conn->buffer) - conn->buffer_pos, 0);

        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;  // No data available
            }

            // Connection closed
            conn->event_loop->remove_fd(fd);
            close(fd);
            t_connections.erase(it);
            return;
        }

        conn->buffer_pos += n;

        // Try to parse HTTP request
        HTTP1Request request;
        size_t consumed = 0;

        int parse_result = conn->parser.parse(
            reinterpret_cast<const uint8_t*>(conn->buffer),
            conn->buffer_pos,
            request,
            consumed
        );

        if (parse_result == 0) {
            // Successfully parsed request
            std::string response = build_response(request);

            // Send response
            ssize_t sent = 0;
            while (sent < static_cast<ssize_t>(response.size())) {
                ssize_t s = send(fd, response.data() + sent,
                                response.size() - sent, 0);
                if (s < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // Would block - enable write event
                        conn->event_loop->modify_fd(fd, IOEvent::READ | IOEvent::WRITE | IOEvent::EDGE);
                        return;
                    }
                    // Error, close connection
                    conn->event_loop->remove_fd(fd);
                    close(fd);
                    t_connections.erase(it);
                    return;
                }
                sent += s;
            }

            // Reset parser for next request (keep-alive)
            conn->parser.reset();

            // Move remaining data to front of buffer
            if (consumed < conn->buffer_pos) {
                memmove(conn->buffer, conn->buffer + consumed,
                       conn->buffer_pos - consumed);
                conn->buffer_pos -= consumed;
            } else {
                conn->buffer_pos = 0;
            }

            // Check if connection should be closed
            if (!request.keep_alive) {
                conn->event_loop->remove_fd(fd);
                close(fd);
                t_connections.erase(it);
            }
        } else if (parse_result == -1) {
            // Need more data
            return;
        } else {
            // Parse error
            const char* error_response =
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n"
                "\r\n";

            send(fd, error_response, strlen(error_response), 0);
            conn->event_loop->remove_fd(fd);
            close(fd);
            t_connections.erase(it);
        }
    }
}

/**
 * Handle new HTTP connection
 */
void on_http_connection(TcpSocket socket, EventLoop* event_loop) {
    // Set non-blocking
    if (socket.set_nonblocking() < 0) {
        std::cerr << "Failed to set non-blocking: " << strerror(errno) << std::endl;
        return;
    }

    // Disable Nagle's algorithm
    socket.set_nodelay();

    int fd = socket.fd();

    // Create connection state
    auto conn = std::make_unique<HttpConnection>();
    conn->fd = fd;
    conn->buffer_pos = 0;
    conn->event_loop = event_loop;

    // Add to event loop
    if (event_loop->add_fd(fd, IOEvent::READ | IOEvent::EDGE, handle_http_client, nullptr) < 0) {
        std::cerr << "Failed to add client to event loop: " << strerror(errno) << std::endl;
        return;
    }

    // Release ownership and store connection
    socket.release();
    t_connections[fd] = std::move(conn);
}

int main(int argc, char* argv[]) {
    uint16_t port = 8070;
    uint16_t num_workers = 0;  // Auto

    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }
    if (argc > 2) {
        num_workers = static_cast<uint16_t>(std::atoi(argv[2]));
    }

    std::cout << "Native HTTP/1.1 server" << std::endl;
    std::cout << "Port: " << port << std::endl;
    std::cout << "Workers: " << (num_workers == 0 ? "auto" : std::to_string(num_workers)) << std::endl;

    // Configure listener
    TcpListenerConfig config;
    config.port = port;
    config.host = "0.0.0.0";
    config.num_workers = num_workers;
    config.use_reuseport = true;

    // Create listener
    g_listener = std::make_unique<TcpListener>(config, on_http_connection);

    // Setup signal handler
    signal(SIGINT, signal_handler);

    // Start listening (blocks until stop() is called)
    std::cout << "Starting server..." << std::endl;
    int result = g_listener->start();

    if (result < 0) {
        std::cerr << "Failed to start server" << std::endl;
        return 1;
    }

    std::cout << "Server stopped." << std::endl;
    return 0;
}
