/**
 * Multi-threaded echo server using TcpListener
 *
 * Demonstrates:
 * - Multi-threaded event loop
 * - SO_REUSEPORT for kernel load balancing
 * - High-performance TCP connections
 */

#include "../src/cpp/net/tcp_listener.h"
#include "../src/cpp/net/tcp_socket.h"
#include "../src/cpp/net/event_loop.h"
#include <iostream>
#include <csignal>
#include <memory>
#include <unordered_map>
#include <cstring>
#include <unistd.h>

using namespace fasterapi::net;

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
struct Connection {
    int fd;
    char buffer[4096];
    EventLoop* event_loop;
};

// Per-worker connection storage (thread-local)
thread_local std::unordered_map<int, std::unique_ptr<Connection>> t_connections;

/**
 * Handle client connection events
 */
void handle_client(int fd, IOEvent events, void* user_data) {
    auto it = t_connections.find(fd);
    if (it == t_connections.end()) {
        return;
    }

    Connection* conn = it->second.get();

    // Handle errors (but not HUP yet - we might have data to read first)
    if (events & IOEvent::ERROR) {
        conn->event_loop->remove_fd(fd);
        close(fd);
        t_connections.erase(it);
        return;
    }

    // Handle readable event (read data even if HUP is also set)
    if (events & IOEvent::READ) {
        ssize_t n = recv(fd, conn->buffer, sizeof(conn->buffer), 0);

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

        // Echo back
        ssize_t sent = 0;
        while (sent < n) {
            ssize_t s = send(fd, conn->buffer + sent, n - sent, 0);
            if (s < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Would block
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
    }
}

/**
 * Handle new connection
 */
void on_connection(TcpSocket socket, EventLoop* event_loop) {
    // Set non-blocking
    if (socket.set_nonblocking() < 0) {
        std::cerr << "Failed to set non-blocking: " << strerror(errno) << std::endl;
        return;
    }

    // Disable Nagle's algorithm
    socket.set_nodelay();

    int fd = socket.fd();

    // Create connection state
    auto conn = std::make_unique<Connection>();
    conn->fd = fd;
    conn->event_loop = event_loop;

    // Add to event loop
    if (event_loop->add_fd(fd, IOEvent::READ | IOEvent::EDGE, handle_client, nullptr) < 0) {
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

    std::cout << "Multi-threaded echo server" << std::endl;
    std::cout << "Port: " << port << std::endl;
    std::cout << "Workers: " << (num_workers == 0 ? "auto" : std::to_string(num_workers)) << std::endl;

    // Configure listener
    TcpListenerConfig config;
    config.port = port;
    config.host = "0.0.0.0";
    config.num_workers = num_workers;
    config.use_reuseport = true;

    // Create listener
    g_listener = std::make_unique<TcpListener>(config, on_connection);

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
