/**
 * Simple echo server to test the native event loop
 *
 * This is a standalone test that doesn't depend on any HTTP code.
 * It accepts TCP connections and echoes back whatever is sent.
 */

#include "../src/cpp/net/event_loop.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <iostream>
#include <csignal>
#include <memory>
#include <unordered_map>

using namespace fasterapi::net;

// Global event loop pointer for signal handler
static std::unique_ptr<EventLoop> g_event_loop;

// Signal handler for Ctrl+C
void signal_handler(int sig) {
    if (sig == SIGINT) {
        std::cout << "\nStopping server..." << std::endl;
        if (g_event_loop) {
            g_event_loop->stop();
        }
    }
}

// Connection state
struct Connection {
    int fd;
    char buffer[4096];
    ssize_t bytes_read;
};

// Connection storage
static std::unordered_map<int, std::unique_ptr<Connection>> connections;

/**
 * Handle client connection events
 */
void handle_client(int fd, IOEvent events, void* user_data) {
    auto it = connections.find(fd);
    if (it == connections.end()) {
        std::cerr << "Unknown connection fd: " << fd << std::endl;
        return;
    }

    Connection* conn = it->second.get();

    // Handle errors or connection closed
    if (events & IOEvent::ERROR || events & IOEvent::HUP) {
        std::cout << "Connection " << fd << " closed" << std::endl;
        g_event_loop->remove_fd(fd);
        close(fd);
        connections.erase(it);
        return;
    }

    // Handle readable event
    if (events & IOEvent::READ) {
        ssize_t n = recv(fd, conn->buffer, sizeof(conn->buffer), 0);

        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // No data available, try again later
                return;
            }

            // Connection closed or error
            std::cout << "Connection " << fd << " closed" << std::endl;
            g_event_loop->remove_fd(fd);
            close(fd);
            connections.erase(it);
            return;
        }

        conn->bytes_read = n;

        // Echo back
        ssize_t sent = 0;
        while (sent < n) {
            ssize_t s = send(fd, conn->buffer + sent, n - sent, 0);
            if (s < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Would block, switch to write-ready event
                    g_event_loop->modify_fd(fd, IOEvent::READ | IOEvent::WRITE | IOEvent::EDGE);
                    return;
                }
                // Error, close connection
                std::cerr << "Send error: " << strerror(errno) << std::endl;
                g_event_loop->remove_fd(fd);
                close(fd);
                connections.erase(it);
                return;
            }
            sent += s;
        }
    }
}

/**
 * Handle new connections on the listen socket
 */
void handle_accept(int listen_fd, IOEvent events, void* user_data) {
    if (!(events & IOEvent::READ)) {
        return;
    }

    // Accept all pending connections (edge-triggered)
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &addr_len);

        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No more connections
                break;
            }
            std::cerr << "Accept error: " << strerror(errno) << std::endl;
            continue;
        }

        // Set non-blocking
        if (EventLoop::set_nonblocking(client_fd) < 0) {
            std::cerr << "Failed to set non-blocking: " << strerror(errno) << std::endl;
            close(client_fd);
            continue;
        }

        // Disable Nagle's algorithm
        EventLoop::set_tcp_nodelay(client_fd);

        // Create connection state
        auto conn = std::make_unique<Connection>();
        conn->fd = client_fd;
        conn->bytes_read = 0;

        // Add to event loop
        if (g_event_loop->add_fd(client_fd, IOEvent::READ | IOEvent::EDGE, handle_client, nullptr) < 0) {
            std::cerr << "Failed to add client to event loop: " << strerror(errno) << std::endl;
            close(client_fd);
            continue;
        }

        connections[client_fd] = std::move(conn);
        std::cout << "New connection: " << client_fd << " from "
                  << inet_ntoa(client_addr.sin_addr) << ":" << ntohs(client_addr.sin_port) << std::endl;
    }
}

int main(int argc, char* argv[]) {
    uint16_t port = 8070;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }

    std::cout << "Starting echo server on port " << port << std::endl;

    // Create event loop
    g_event_loop = create_event_loop();
    if (!g_event_loop) {
        std::cerr << "Failed to create event loop" << std::endl;
        return 1;
    }

    std::cout << "Using event loop: " << g_event_loop->platform_name() << std::endl;

    // Create listen socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "Socket creation failed: " << strerror(errno) << std::endl;
        return 1;
    }

    // Set socket options
    EventLoop::set_reuseaddr(listen_fd);
    EventLoop::set_nonblocking(listen_fd);

    // Bind to port
    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Bind failed: " << strerror(errno) << std::endl;
        close(listen_fd);
        return 1;
    }

    // Listen
    if (listen(listen_fd, 1024) < 0) {
        std::cerr << "Listen failed: " << strerror(errno) << std::endl;
        close(listen_fd);
        return 1;
    }

    std::cout << "Listening on 0.0.0.0:" << port << std::endl;

    // Add listen socket to event loop
    if (g_event_loop->add_fd(listen_fd, IOEvent::READ | IOEvent::EDGE, handle_accept, nullptr) < 0) {
        std::cerr << "Failed to add listen socket to event loop: " << strerror(errno) << std::endl;
        close(listen_fd);
        return 1;
    }

    // Setup signal handler
    signal(SIGINT, signal_handler);

    // Run event loop
    std::cout << "Server running. Press Ctrl+C to stop." << std::endl;
    g_event_loop->run();

    // Cleanup
    std::cout << "Cleaning up..." << std::endl;
    g_event_loop->remove_fd(listen_fd);
    close(listen_fd);

    // Close all client connections
    for (auto& [fd, conn] : connections) {
        close(fd);
    }
    connections.clear();

    std::cout << "Server stopped." << std::endl;
    return 0;
}
