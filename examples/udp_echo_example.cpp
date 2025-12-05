/**
 * UDP Echo Server Example
 *
 * Demonstrates the UdpListener API for building a simple echo server.
 * This shows how to use the UDP infrastructure for HTTP/3/QUIC applications.
 *
 * Usage:
 *   ./udp_echo_example
 *   # Test with: echo "hello" | nc -u localhost 8888
 */

#include "src/cpp/net/udp_listener.h"
#include <iostream>
#include <csignal>
#include <atomic>

using namespace fasterapi::net;

// Global flag for clean shutdown
std::atomic<bool> keep_running{true};

void signal_handler(int signum) {
    std::cout << "\nReceived signal " << signum << ", shutting down..." << std::endl;
    keep_running.store(false);
}

int main(int argc, char* argv[]) {
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Configure UDP listener
    UdpListenerConfig config;
    config.host = "0.0.0.0";
    config.port = 8888;
    config.num_workers = 4;  // 4 worker threads
    config.use_reuseport = true;
    config.recv_buffer_size = 2 * 1024 * 1024;  // 2MB socket buffer
    config.max_datagram_size = 65535;  // 64KB max datagram
    config.address_family = AF_INET;  // IPv4
    config.enable_pktinfo = true;
    config.enable_tos = true;

    std::cout << "UDP Echo Server" << std::endl;
    std::cout << "===============" << std::endl;
    std::cout << "Listening on " << config.host << ":" << config.port << std::endl;
    std::cout << "Workers: " << config.num_workers << std::endl;
    std::cout << std::endl;

    // Create UDP listener with echo callback
    UdpListener listener(config, [](const uint8_t* data, size_t length,
                                     const struct sockaddr* addr, socklen_t addrlen,
                                     EventLoop* event_loop) {

        // Print received datagram
        std::cout << "Received " << length << " bytes: ";
        std::cout.write(reinterpret_cast<const char*>(data), length);
        std::cout << std::endl;

        // Echo back to sender
        UdpSocket socket(false);  // Create temporary socket for sending
        if (socket.is_valid()) {
            socket.bind("0.0.0.0", 0);  // Bind to any port
            ssize_t sent = socket.sendto(data, length, addr, addrlen);
            if (sent != static_cast<ssize_t>(length)) {
                std::cerr << "Warning: Failed to echo " << length << " bytes" << std::endl;
            }
        }
    });

    // Start listener (blocks until stop() is called)
    std::cout << "Starting UDP listener..." << std::endl;
    listener.start();

    std::cout << "Server stopped" << std::endl;
    return 0;
}

/**
 * Example output:
 *
 * UDP Echo Server
 * ===============
 * Listening on 0.0.0.0:8888
 * Workers: 4
 *
 * Starting UDP listener on 0.0.0.0:8888
 * Workers: 4
 * SO_REUSEPORT: enabled
 * Address family: IPv4
 * Max datagram size: 65535 bytes
 * Socket buffer size: 2097152 bytes
 * Worker 0 starting
 * Worker 1 starting
 * Worker 2 starting
 * Worker 3 starting
 * Worker 0: Using kqueue event loop
 * Worker 0: Listening on fd 12
 * Worker 0: Running event loop
 * Worker 1: Using kqueue event loop
 * Worker 1: Listening on fd 13
 * Worker 1: Running event loop
 * Worker 2: Using kqueue event loop
 * Worker 2: Listening on fd 14
 * Worker 2: Running event loop
 * Worker 3: Using kqueue event loop
 * Worker 3: Listening on fd 15
 * Worker 3: Running event loop
 * Received 6 bytes: hello
 *
 * ^C
 * Received signal 2, shutting down...
 * Worker 0 stopped
 * Worker 1 stopped
 * Worker 2 stopped
 * Worker 3 stopped
 * Server stopped
 */
