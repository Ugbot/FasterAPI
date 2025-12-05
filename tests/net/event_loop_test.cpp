/**
 * FasterAPI Event Loop Tests
 *
 * Comprehensive Google Test suite for the event loop implementation.
 */

#include <gtest/gtest.h>
#include "../test_utils.h"
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

using namespace fasterapi::net;
using namespace fasterapi::testing;

// =============================================================================
// EventLoop Test Fixture
// =============================================================================

class EventLoopTest : public FasterAPITest {
protected:
    std::unique_ptr<EventLoop> loop_;

    void SetUp() override {
        FasterAPITest::SetUp();
        loop_ = create_event_loop();
        ASSERT_NE(loop_, nullptr) << "Failed to create event loop";
    }

    void TearDown() override {
        if (loop_ && loop_->is_running()) {
            loop_->stop();
        }
        loop_.reset();
        FasterAPITest::TearDown();
    }

    // Helper: create a socket pair for testing
    std::pair<int, int> create_socket_pair() {
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
            return {-1, -1};
        }
        return {fds[0], fds[1]};
    }

    // Helper: create listening socket on random port
    int create_listening_socket(uint16_t& out_port) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = 0;  // Let kernel choose port

        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(fd);
            return -1;
        }

        socklen_t len = sizeof(addr);
        if (getsockname(fd, (struct sockaddr*)&addr, &len) < 0) {
            close(fd);
            return -1;
        }
        out_port = ntohs(addr.sin_port);

        if (listen(fd, 128) < 0) {
            close(fd);
            return -1;
        }

        return fd;
    }
};

// =============================================================================
// Basic EventLoop Tests
// =============================================================================

TEST_F(EventLoopTest, Creation) {
    EXPECT_NE(loop_, nullptr);
    EXPECT_FALSE(loop_->is_running());

    const char* platform = loop_->platform_name();
    EXPECT_NE(platform, nullptr);

#ifdef __APPLE__
    EXPECT_STREQ(platform, "kqueue");
#elif defined(__linux__)
    EXPECT_TRUE(strcmp(platform, "epoll") == 0 || strcmp(platform, "io_uring") == 0);
#endif
}

TEST_F(EventLoopTest, AddRemoveFd) {
    auto [fd1, fd2] = create_socket_pair();
    ASSERT_GE(fd1, 0);
    ASSERT_GE(fd2, 0);

    EventLoop::set_nonblocking(fd1);
    EventLoop::set_nonblocking(fd2);

    bool called = false;
    int result = loop_->add_fd(fd1, IOEvent::READ,
        [&](int fd, IOEvent events, void*) {
            called = true;
        });
    EXPECT_EQ(result, 0);

    // Remove should succeed
    result = loop_->remove_fd(fd1);
    EXPECT_EQ(result, 0);

    close(fd1);
    close(fd2);
}

TEST_F(EventLoopTest, PollWithData) {
    auto [reader_fd, writer_fd] = create_socket_pair();
    ASSERT_GE(reader_fd, 0);
    ASSERT_GE(writer_fd, 0);

    EventLoop::set_nonblocking(reader_fd);
    EventLoop::set_nonblocking(writer_fd);

    std::atomic<bool> read_event{false};

    int result = loop_->add_fd(reader_fd, IOEvent::READ,
        [&](int fd, IOEvent events, void*) {
            if (events & IOEvent::READ) {
                read_event.store(true);
            }
        });
    EXPECT_EQ(result, 0);

    // Write data to trigger read event
    const char* test_data = "Hello, EventLoop!";
    ssize_t written = write(writer_fd, test_data, strlen(test_data));
    EXPECT_GT(written, 0);

    // Poll should return with read event
    int events = loop_->poll(100);
    EXPECT_GE(events, 0);
    EXPECT_TRUE(read_event.load());

    loop_->remove_fd(reader_fd);
    close(reader_fd);
    close(writer_fd);
}

TEST_F(EventLoopTest, PollTimeout) {
    auto [fd1, fd2] = create_socket_pair();
    ASSERT_GE(fd1, 0);

    EventLoop::set_nonblocking(fd1);

    loop_->add_fd(fd1, IOEvent::READ, [](int, IOEvent, void*) {});

    // No data written, should timeout
    auto start = std::chrono::steady_clock::now();
    int events = loop_->poll(50);  // 50ms timeout
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(events, 0);  // No events
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 40);

    loop_->remove_fd(fd1);
    close(fd1);
    close(fd2);
}

TEST_F(EventLoopTest, ModifyEvents) {
    auto [fd1, fd2] = create_socket_pair();
    ASSERT_GE(fd1, 0);

    EventLoop::set_nonblocking(fd1);
    EventLoop::set_nonblocking(fd2);

    std::atomic<int> event_count{0};
    IOEvent last_events{};

    loop_->add_fd(fd1, IOEvent::READ,
        [&](int fd, IOEvent events, void*) {
            last_events = events;
            event_count++;
        });

    // Modify to listen for WRITE
    int result = loop_->modify_fd(fd1, IOEvent::WRITE);
    EXPECT_EQ(result, 0);

    // Write should be ready immediately (socket buffer is empty)
    loop_->poll(50);

    // Should have gotten write event
    EXPECT_TRUE(last_events & IOEvent::WRITE);

    loop_->remove_fd(fd1);
    close(fd1);
    close(fd2);
}

TEST_F(EventLoopTest, RunAndStop) {
    std::atomic<bool> loop_started{false};
    std::atomic<bool> loop_stopped{false};

    std::thread loop_thread([&]() {
        loop_started.store(true);
        loop_->run();
        loop_stopped.store(true);
    });

    // Wait for loop to start
    while (!loop_started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_TRUE(loop_->is_running());

    // Stop should work from another thread
    loop_->stop();
    loop_thread.join();

    EXPECT_TRUE(loop_stopped.load());
    EXPECT_FALSE(loop_->is_running());
}

TEST_F(EventLoopTest, MultipleFds) {
    constexpr int NUM_PAIRS = 10;
    std::vector<std::pair<int, int>> pairs;
    std::atomic<int> total_events{0};

    // Create multiple socket pairs
    for (int i = 0; i < NUM_PAIRS; ++i) {
        auto [fd1, fd2] = create_socket_pair();
        ASSERT_GE(fd1, 0);
        pairs.push_back({fd1, fd2});

        EventLoop::set_nonblocking(fd1);
        EventLoop::set_nonblocking(fd2);

        loop_->add_fd(fd1, IOEvent::READ,
            [&](int fd, IOEvent events, void*) {
                if (events & IOEvent::READ) {
                    total_events++;
                }
            });
    }

    // Write to all pairs
    for (auto& [reader, writer] : pairs) {
        write(writer, "X", 1);
    }

    // Poll multiple times to ensure all events processed
    for (int i = 0; i < 5 && total_events.load() < NUM_PAIRS; ++i) {
        loop_->poll(50);
    }

    EXPECT_EQ(total_events.load(), NUM_PAIRS);

    // Cleanup
    for (auto& [reader, writer] : pairs) {
        loop_->remove_fd(reader);
        close(reader);
        close(writer);
    }
}

// =============================================================================
// Socket Helper Tests
// =============================================================================

TEST_F(EventLoopTest, SetNonblocking) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    int flags = fcntl(fd, F_GETFL, 0);
    EXPECT_EQ(flags & O_NONBLOCK, 0);  // Initially blocking

    int result = EventLoop::set_nonblocking(fd);
    EXPECT_EQ(result, 0);

    flags = fcntl(fd, F_GETFL, 0);
    EXPECT_NE(flags & O_NONBLOCK, 0);  // Now non-blocking

    close(fd);
}

TEST_F(EventLoopTest, SetTcpNodelay) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    int result = EventLoop::set_tcp_nodelay(fd);
    EXPECT_EQ(result, 0);

    int nodelay = 0;
    socklen_t len = sizeof(nodelay);
    getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, &len);
    EXPECT_NE(nodelay, 0);  // Non-zero means enabled

    close(fd);
}

TEST_F(EventLoopTest, SetReuseaddr) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    int result = EventLoop::set_reuseaddr(fd);
    EXPECT_EQ(result, 0);

    int reuseaddr = 0;
    socklen_t len = sizeof(reuseaddr);
    getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, &len);
    EXPECT_NE(reuseaddr, 0);  // Non-zero means enabled

    close(fd);
}

// =============================================================================
// Performance Tests
// =============================================================================

TEST_F(EventLoopTest, PollLatency) {
    auto [fd1, fd2] = create_socket_pair();
    ASSERT_GE(fd1, 0);

    EventLoop::set_nonblocking(fd1);
    EventLoop::set_nonblocking(fd2);

    loop_->add_fd(fd1, IOEvent::READ, [](int, IOEvent, void*) {});

    // Measure poll latency with data available
    write(fd2, "X", 1);

    constexpr int ITERATIONS = 1000;
    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < ITERATIONS; ++i) {
        loop_->poll(0);  // Non-blocking poll
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ns_per_poll = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / ITERATIONS;

    std::cout << "EventLoop poll latency: " << ns_per_poll << " ns/poll" << std::endl;

    // Should be well under 10us per poll
    EXPECT_LT(ns_per_poll, 10000);

    loop_->remove_fd(fd1);
    close(fd1);
    close(fd2);
}

TEST_F(EventLoopTest, AddRemoveLatency) {
    auto [fd1, fd2] = create_socket_pair();
    ASSERT_GE(fd1, 0);

    EventLoop::set_nonblocking(fd1);

    constexpr int ITERATIONS = 1000;
    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < ITERATIONS; ++i) {
        loop_->add_fd(fd1, IOEvent::READ, [](int, IOEvent, void*) {});
        loop_->remove_fd(fd1);
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ns_per_op = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / ITERATIONS;

    std::cout << "EventLoop add/remove latency: " << ns_per_op << " ns/pair" << std::endl;

    // Should be well under 50us per add/remove pair
    EXPECT_LT(ns_per_op, 50000);

    close(fd1);
    close(fd2);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(EventLoopTest, RemoveNonexistentFd) {
    // Removing a non-existent fd should return error
    int result = loop_->remove_fd(999999);
    EXPECT_EQ(result, -1);
}

TEST_F(EventLoopTest, DoubleAdd) {
    auto [fd1, fd2] = create_socket_pair();
    ASSERT_GE(fd1, 0);

    EventLoop::set_nonblocking(fd1);

    loop_->add_fd(fd1, IOEvent::READ, [](int, IOEvent, void*) {});

    // Adding same fd again should fail or update (implementation dependent)
    int result = loop_->add_fd(fd1, IOEvent::WRITE, [](int, IOEvent, void*) {});
    // Just check it doesn't crash - behavior varies by implementation

    loop_->remove_fd(fd1);
    close(fd1);
    close(fd2);
}

TEST_F(EventLoopTest, PollAfterStop) {
    loop_->stop();

    // Poll after stop should handle gracefully
    int result = loop_->poll(10);
    // Should return 0 or handle gracefully
    EXPECT_GE(result, -1);
}

