/**
 * FasterAPI TCP Socket Tests
 *
 * Comprehensive Google Test suite for TCP socket operations.
 */

#include <gtest/gtest.h>
#include "../test_utils.h"
#include "../../src/cpp/net/tcp_socket.h"
#include "../../src/cpp/net/event_loop.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cerrno>

using namespace fasterapi::net;
using namespace fasterapi::testing;

// =============================================================================
// TcpSocket Test Fixture
// =============================================================================

class TcpSocketTest : public FasterAPITest {
protected:
    RandomGenerator rng_;

    // Helper: find an available port
    uint16_t get_available_port() {
        TcpSocket sock;
        sock.set_reuseaddr();
        sock.bind("127.0.0.1", 0);

        std::string ip;
        uint16_t port;
        sock.get_local_address(ip, port);
        sock.close();

        return port;
    }

    // Helper: create a simple echo server
    struct EchoServer {
        std::thread thread;
        std::atomic<bool> running{true};
        std::atomic<uint16_t> port{0};
        std::atomic<int> connections{0};

        void start() {
            thread = std::thread([this]() {
                TcpSocket listener;
                listener.set_reuseaddr();
                ASSERT_EQ(listener.bind("127.0.0.1", 0), 0);
                ASSERT_EQ(listener.listen(128), 0);

                std::string ip;
                uint16_t p;
                listener.get_local_address(ip, p);
                port.store(p);

                listener.set_nonblocking();

                while (running.load()) {
                    TcpSocket client = listener.accept();
                    if (client.is_valid()) {
                        connections++;
                        client.set_nonblocking();

                        char buf[4096];
                        while (running.load()) {
                            ssize_t n = client.recv(buf, sizeof(buf));
                            if (n > 0) {
                                client.send(buf, n);
                            } else if (n == 0) {
                                break;  // EOF
                            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                break;  // Error
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            });

            // Wait for server to bind
            while (port.load() == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        void stop() {
            running.store(false);
            if (thread.joinable()) {
                thread.join();
            }
        }

        ~EchoServer() {
            stop();
        }
    };
};

// =============================================================================
// Basic Socket Tests
// =============================================================================

TEST_F(TcpSocketTest, DefaultConstruction) {
    TcpSocket sock;
    EXPECT_TRUE(sock.is_valid());
    EXPECT_GE(sock.fd(), 0);
}

TEST_F(TcpSocketTest, ConstructFromFd) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    TcpSocket sock(fd);
    EXPECT_TRUE(sock.is_valid());
    EXPECT_EQ(sock.fd(), fd);

    // Socket closes when sock goes out of scope
}

TEST_F(TcpSocketTest, MoveConstruction) {
    TcpSocket sock1;
    int fd = sock1.fd();

    TcpSocket sock2(std::move(sock1));
    EXPECT_EQ(sock2.fd(), fd);
    EXPECT_FALSE(sock1.is_valid());  // Moved-from socket should be invalid
}

TEST_F(TcpSocketTest, MoveAssignment) {
    TcpSocket sock1;
    TcpSocket sock2;
    int fd1 = sock1.fd();

    sock2 = std::move(sock1);
    EXPECT_EQ(sock2.fd(), fd1);
    EXPECT_FALSE(sock1.is_valid());
}

TEST_F(TcpSocketTest, Close) {
    TcpSocket sock;
    EXPECT_TRUE(sock.is_valid());

    sock.close();
    EXPECT_FALSE(sock.is_valid());
    EXPECT_LT(sock.fd(), 0);
}

TEST_F(TcpSocketTest, Release) {
    TcpSocket sock;
    int fd = sock.fd();

    int released_fd = sock.release();
    EXPECT_EQ(released_fd, fd);
    EXPECT_FALSE(sock.is_valid());

    // Must close manually since socket no longer owns it
    close(released_fd);
}

// =============================================================================
// Socket Options Tests
// =============================================================================

TEST_F(TcpSocketTest, SetNonblocking) {
    TcpSocket sock;
    EXPECT_EQ(sock.set_nonblocking(), 0);

    int flags = fcntl(sock.fd(), F_GETFL, 0);
    EXPECT_NE(flags & O_NONBLOCK, 0);
}

TEST_F(TcpSocketTest, SetNodelay) {
    TcpSocket sock;
    EXPECT_EQ(sock.set_nodelay(), 0);

    int nodelay = 0;
    socklen_t len = sizeof(nodelay);
    getsockopt(sock.fd(), IPPROTO_TCP, TCP_NODELAY, &nodelay, &len);
    EXPECT_NE(nodelay, 0);  // Non-zero means enabled
}

TEST_F(TcpSocketTest, SetReuseaddr) {
    TcpSocket sock;
    EXPECT_EQ(sock.set_reuseaddr(), 0);

    int reuseaddr = 0;
    socklen_t len = sizeof(reuseaddr);
    getsockopt(sock.fd(), SOL_SOCKET, SO_REUSEADDR, &reuseaddr, &len);
    EXPECT_NE(reuseaddr, 0);  // Non-zero means enabled
}

TEST_F(TcpSocketTest, SetKeepalive) {
    TcpSocket sock;
    EXPECT_EQ(sock.set_keepalive(true), 0);

    int keepalive = 0;
    socklen_t len = sizeof(keepalive);
    getsockopt(sock.fd(), SOL_SOCKET, SO_KEEPALIVE, &keepalive, &len);
    EXPECT_NE(keepalive, 0);  // Non-zero means enabled
}

TEST_F(TcpSocketTest, SetBufferSizes) {
    TcpSocket sock;

    EXPECT_EQ(sock.set_recv_buffer_size(65536), 0);
    EXPECT_EQ(sock.set_send_buffer_size(65536), 0);

    // Note: kernel may adjust buffer sizes, so we don't verify exact values
}

// =============================================================================
// Bind and Listen Tests
// =============================================================================

TEST_F(TcpSocketTest, BindToAnyPort) {
    TcpSocket sock;
    sock.set_reuseaddr();

    EXPECT_EQ(sock.bind("127.0.0.1", 0), 0);

    std::string ip;
    uint16_t port;
    EXPECT_TRUE(sock.get_local_address(ip, port));
    EXPECT_EQ(ip, "127.0.0.1");
    EXPECT_GT(port, 0);
}

TEST_F(TcpSocketTest, BindToSpecificPort) {
    TcpSocket sock;
    sock.set_reuseaddr();

    uint16_t test_port = get_available_port();

    EXPECT_EQ(sock.bind("127.0.0.1", test_port), 0);

    std::string ip;
    uint16_t port;
    EXPECT_TRUE(sock.get_local_address(ip, port));
    EXPECT_EQ(port, test_port);
}

TEST_F(TcpSocketTest, Listen) {
    TcpSocket sock;
    sock.set_reuseaddr();
    sock.bind("127.0.0.1", 0);

    EXPECT_EQ(sock.listen(128), 0);
}

// =============================================================================
// Connect and Communication Tests
// =============================================================================

TEST_F(TcpSocketTest, ConnectToServer) {
    EchoServer server;
    server.start();

    TcpSocket client;
    client.set_nonblocking();

    int result = client.connect("127.0.0.1", server.port.load());
    // Non-blocking connect returns -1 with EINPROGRESS
    EXPECT_TRUE(result == 0 || (result == -1 && errno == EINPROGRESS));

    // Wait for connection
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_GE(server.connections.load(), 1);
}

TEST_F(TcpSocketTest, SendReceive) {
    EchoServer server;
    server.start();

    TcpSocket client;
    client.connect("127.0.0.1", server.port.load());

    // Wait for connection
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send data
    auto test_data = rng_.random_string(100);
    ssize_t sent = client.send(test_data.data(), test_data.size());
    EXPECT_EQ(sent, static_cast<ssize_t>(test_data.size()));

    // Wait for echo
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Receive echoed data
    char recv_buf[256];
    client.set_nonblocking();
    ssize_t received = client.recv(recv_buf, sizeof(recv_buf));
    EXPECT_EQ(received, static_cast<ssize_t>(test_data.size()));
    EXPECT_EQ(std::string(recv_buf, received), test_data);
}

TEST_F(TcpSocketTest, LargeDataTransfer) {
    EchoServer server;
    server.start();

    TcpSocket client;
    client.connect("127.0.0.1", server.port.load());

    // Wait for connection
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send larger data in chunks
    constexpr size_t TOTAL_SIZE = 32768;
    auto test_data = rng_.random_bytes(TOTAL_SIZE);

    size_t total_sent = 0;
    while (total_sent < TOTAL_SIZE) {
        ssize_t sent = client.send(test_data.data() + total_sent, TOTAL_SIZE - total_sent);
        if (sent > 0) {
            total_sent += sent;
        } else if (sent < 0 && errno != EAGAIN) {
            break;
        }
    }
    EXPECT_EQ(total_sent, TOTAL_SIZE);

    // Receive echoed data
    client.set_nonblocking();
    std::vector<uint8_t> recv_buf(TOTAL_SIZE * 2);
    size_t total_received = 0;
    int attempts = 0;

    while (total_received < TOTAL_SIZE && attempts < 100) {
        ssize_t received = client.recv(recv_buf.data() + total_received,
                                        recv_buf.size() - total_received);
        if (received > 0) {
            total_received += received;
        } else if (received < 0 && errno != EAGAIN) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        attempts++;
    }

    EXPECT_EQ(total_received, TOTAL_SIZE);
    EXPECT_EQ(memcmp(recv_buf.data(), test_data.data(), TOTAL_SIZE), 0);
}

// =============================================================================
// Accept Tests
// =============================================================================

TEST_F(TcpSocketTest, AcceptConnection) {
    TcpSocket listener;
    listener.set_reuseaddr();
    listener.bind("127.0.0.1", 0);
    listener.listen(128);
    listener.set_nonblocking();

    std::string ip;
    uint16_t port;
    listener.get_local_address(ip, port);

    // Connect from another socket
    TcpSocket client;
    client.connect("127.0.0.1", port);

    // Wait for connection
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Accept should succeed
    struct sockaddr_in client_addr{};
    TcpSocket accepted = listener.accept(&client_addr);

    EXPECT_TRUE(accepted.is_valid());

    // Verify client address
    char addr_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));
    EXPECT_STREQ(addr_str, "127.0.0.1");
}

TEST_F(TcpSocketTest, MultipleAccepts) {
    TcpSocket listener;
    listener.set_reuseaddr();
    listener.bind("127.0.0.1", 0);
    listener.listen(128);
    listener.set_nonblocking();

    std::string ip;
    uint16_t port;
    listener.get_local_address(ip, port);

    constexpr int NUM_CLIENTS = 5;
    std::vector<TcpSocket> clients;
    std::vector<TcpSocket> accepted;

    // Create multiple client connections
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        TcpSocket client;
        client.connect("127.0.0.1", port);
        clients.push_back(std::move(client));
    }

    // Wait for connections
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Accept all connections
    for (int i = 0; i < NUM_CLIENTS + 5; ++i) {  // Try more to ensure we get all
        TcpSocket sock = listener.accept(nullptr);
        if (sock.is_valid()) {
            accepted.push_back(std::move(sock));
        }
    }

    EXPECT_EQ(accepted.size(), NUM_CLIENTS);
}

// =============================================================================
// Address Retrieval Tests
// =============================================================================

TEST_F(TcpSocketTest, GetLocalAddress) {
    TcpSocket sock;
    sock.set_reuseaddr();
    sock.bind("127.0.0.1", 0);

    std::string ip;
    uint16_t port;
    EXPECT_TRUE(sock.get_local_address(ip, port));
    EXPECT_EQ(ip, "127.0.0.1");
    EXPECT_GT(port, 0);
}

TEST_F(TcpSocketTest, GetRemoteAddress) {
    EchoServer server;
    server.start();

    TcpSocket client;
    client.connect("127.0.0.1", server.port.load());

    // Wait for connection
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::string ip;
    uint16_t port;
    EXPECT_TRUE(client.get_remote_address(ip, port));
    EXPECT_EQ(ip, "127.0.0.1");
    EXPECT_EQ(port, server.port.load());
}

// =============================================================================
// Performance Tests
// =============================================================================

TEST_F(TcpSocketTest, SocketCreationPerformance) {
    constexpr int ITERATIONS = 1000;

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < ITERATIONS; ++i) {
        TcpSocket sock;  // Create and destroy
    }
    auto elapsed = std::chrono::steady_clock::now() - start;

    auto us_per_socket = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() / ITERATIONS;
    std::cout << "TcpSocket creation: " << us_per_socket << " us/socket" << std::endl;

    EXPECT_LT(us_per_socket, 100);  // Should be well under 100us
}

TEST_F(TcpSocketTest, ConnectLatency) {
    EchoServer server;
    server.start();

    constexpr int ITERATIONS = 10;
    uint64_t total_us = 0;

    for (int i = 0; i < ITERATIONS; ++i) {
        TcpSocket client;

        auto start = std::chrono::steady_clock::now();
        client.connect("127.0.0.1", server.port.load());
        // Wait for connection to complete
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto elapsed = std::chrono::steady_clock::now() - start;

        total_us += std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    }

    auto avg_us = total_us / ITERATIONS;
    std::cout << "TCP connect latency: " << avg_us << " us avg" << std::endl;

    // Localhost connect should be fast
    EXPECT_LT(avg_us, 50000);  // Under 50ms
}

// =============================================================================
// Error Handling Tests
// =============================================================================

TEST_F(TcpSocketTest, ConnectToRefusedPort) {
    TcpSocket client;
    client.set_nonblocking();

    // Try to connect to localhost on a port that's not listening
    // This should fail immediately with ECONNREFUSED or return EINPROGRESS
    int result = client.connect("127.0.0.1", 59999);  // High port unlikely to be in use

    // Non-blocking connect returns -1 with EINPROGRESS or ECONNREFUSED
    if (result == -1) {
        EXPECT_TRUE(errno == EINPROGRESS || errno == ECONNREFUSED);
    }
}

TEST_F(TcpSocketTest, BindToUsedPort) {
    TcpSocket sock1;
    sock1.set_reuseaddr();
    sock1.bind("127.0.0.1", 0);
    sock1.listen(1);

    std::string ip;
    uint16_t port;
    sock1.get_local_address(ip, port);

    // Try to bind another socket to same port without SO_REUSEADDR
    TcpSocket sock2;
    // Not setting reuseaddr
    int result = sock2.bind("127.0.0.1", port);
    EXPECT_EQ(result, -1);
}

TEST_F(TcpSocketTest, SendOnClosedSocket) {
    TcpSocket sock;
    sock.close();

    ssize_t result = sock.send("test", 4);
    EXPECT_LT(result, 0);
}

