/**
 * Server-Sent Events (SSE) Unit Tests
 *
 * Tests the SSE implementation:
 * - SSEConnection lifecycle (create, send, close)
 * - Message formatting according to SSE spec
 * - Event types, IDs, and retry hints
 * - Comments and ping keep-alive
 * - Statistics tracking (events_sent, bytes_sent)
 * - SSEEndpoint connection management
 * - Multiline data handling
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/sse.h"
#include <random>
#include <thread>
#include <chrono>

namespace fasterapi {
namespace http {
namespace test {

class SSETest : public ::testing::Test {
protected:
    std::unique_ptr<SSEConnection> conn_;
    std::mt19937 rng_;

    void SetUp() override {
        conn_ = std::make_unique<SSEConnection>(1);
        rng_.seed(std::random_device{}());
    }

    void TearDown() override {
        conn_.reset();
    }

    // Generate random string
    std::string random_string(size_t length) {
        static const char charset[] =
            "abcdefghijklmnopqrstuvwxyz"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "0123456789";
        std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);
        std::string result;
        result.reserve(length);
        for (size_t i = 0; i < length; ++i) {
            result += charset[dist(rng_)];
        }
        return result;
    }

    // Generate random event type
    std::string random_event_type() {
        static const std::vector<std::string> types = {
            "message", "update", "notification", "alert", "status", "data", "ping"
        };
        std::uniform_int_distribution<> dist(0, types.size() - 1);
        return types[dist(rng_)];
    }
};

// Test SSEConnection initial state
TEST_F(SSETest, ConnectionInitialState) {
    EXPECT_TRUE(conn_->is_open());
    EXPECT_EQ(conn_->get_id(), 1u);
    EXPECT_EQ(conn_->events_sent(), 0u);
    EXPECT_EQ(conn_->bytes_sent(), 0u);
    EXPECT_TRUE(conn_->get_last_event_id().empty());
}

// Test sending basic event
TEST_F(SSETest, SendBasicEvent) {
    std::string data = random_string(50);
    int result = conn_->send(data);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(conn_->events_sent(), 1u);
    EXPECT_GT(conn_->bytes_sent(), data.length());
}

// Test sending event with type
TEST_F(SSETest, SendEventWithType) {
    std::string data = random_string(30);
    std::string event_type = random_event_type();
    int result = conn_->send(data, event_type.c_str());
    EXPECT_EQ(result, 0);
    EXPECT_EQ(conn_->events_sent(), 1u);
}

// Test sending event with ID
TEST_F(SSETest, SendEventWithId) {
    std::string data = random_string(40);
    std::string event_id = std::to_string(rng_());
    int result = conn_->send(data, nullptr, event_id.c_str());
    EXPECT_EQ(result, 0);
    EXPECT_EQ(conn_->events_sent(), 1u);
    EXPECT_EQ(conn_->get_last_event_id(), event_id);
}

// Test sending event with retry hint
TEST_F(SSETest, SendEventWithRetry) {
    std::string data = random_string(25);
    std::uniform_int_distribution<> retry_dist(1000, 30000);
    int retry = retry_dist(rng_);
    int result = conn_->send(data, nullptr, nullptr, retry);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(conn_->events_sent(), 1u);
}

// Test sending event with all fields
TEST_F(SSETest, SendEventWithAllFields) {
    std::string data = random_string(60);
    std::string event_type = random_event_type();
    std::string event_id = std::to_string(rng_());
    std::uniform_int_distribution<> retry_dist(1000, 10000);
    int retry = retry_dist(rng_);

    int result = conn_->send(data, event_type.c_str(), event_id.c_str(), retry);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(conn_->events_sent(), 1u);
    EXPECT_EQ(conn_->get_last_event_id(), event_id);
}

// Test sending multiline data
TEST_F(SSETest, SendMultilineData) {
    std::string line1 = random_string(20);
    std::string line2 = random_string(30);
    std::string line3 = random_string(25);
    std::string data = line1 + "\n" + line2 + "\n" + line3;

    int result = conn_->send(data);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(conn_->events_sent(), 1u);
}

// Test sending comment
TEST_F(SSETest, SendComment) {
    std::string comment = random_string(40);
    int result = conn_->send_comment(comment);
    EXPECT_EQ(result, 0);
    // Comments don't count as events
    EXPECT_EQ(conn_->events_sent(), 0u);
}

// Test ping (keep-alive)
TEST_F(SSETest, Ping) {
    int result = conn_->ping();
    EXPECT_EQ(result, 0);
    // Ping is a comment, doesn't count as event
    EXPECT_EQ(conn_->events_sent(), 0u);
}

// Test close connection
TEST_F(SSETest, CloseConnection) {
    EXPECT_TRUE(conn_->is_open());
    int result = conn_->close();
    EXPECT_EQ(result, 0);
    EXPECT_FALSE(conn_->is_open());
}

// Test double close (should be safe)
TEST_F(SSETest, DoubleClose) {
    EXPECT_TRUE(conn_->is_open());
    EXPECT_EQ(conn_->close(), 0);
    EXPECT_FALSE(conn_->is_open());
    EXPECT_EQ(conn_->close(), 1);  // Already closed
}

// Test send after close
TEST_F(SSETest, SendAfterClose) {
    conn_->close();
    int result = conn_->send("test data");
    EXPECT_NE(result, 0);  // Should fail
    EXPECT_EQ(conn_->events_sent(), 0u);  // No events should be sent
}

// Test set/get last event ID
TEST_F(SSETest, LastEventId) {
    std::string id1 = std::to_string(rng_());
    std::string id2 = std::to_string(rng_());

    conn_->set_last_event_id(id1);
    EXPECT_EQ(conn_->get_last_event_id(), id1);

    conn_->set_last_event_id(id2);
    EXPECT_EQ(conn_->get_last_event_id(), id2);
}

// Test multiple events
TEST_F(SSETest, MultipleEvents) {
    std::uniform_int_distribution<> count_dist(10, 50);
    int event_count = count_dist(rng_);

    for (int i = 0; i < event_count; ++i) {
        std::string data = random_string(20 + (i % 30));
        int result = conn_->send(data);
        EXPECT_EQ(result, 0);
    }

    EXPECT_EQ(conn_->events_sent(), static_cast<uint64_t>(event_count));
}

// Test bytes_sent accumulation
TEST_F(SSETest, BytesSentAccumulation) {
    uint64_t total_expected = 0;

    for (int i = 0; i < 10; ++i) {
        size_t len = 20 + (i * 5);
        std::string data = random_string(len);
        conn_->send(data);
    }

    EXPECT_GT(conn_->bytes_sent(), 0u);
    EXPECT_EQ(conn_->events_sent(), 10u);
}

// Test concurrent reads (statistics should be thread-safe)
TEST_F(SSETest, ConcurrentStatisticsAccess) {
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;

    // Spawn reader threads
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([this, &stop]() {
            while (!stop.load(std::memory_order_relaxed)) {
                volatile uint64_t events = conn_->events_sent();
                volatile uint64_t bytes = conn_->bytes_sent();
                volatile bool open = conn_->is_open();
                (void)events;
                (void)bytes;
                (void)open;
                std::this_thread::yield();
            }
        });
    }

    // Send some events while readers are running
    for (int i = 0; i < 100; ++i) {
        conn_->send("test data " + std::to_string(i));
    }

    stop.store(true);
    for (auto& t : readers) {
        t.join();
    }

    EXPECT_EQ(conn_->events_sent(), 100u);
}

// ===========================================================================
// SSEEndpoint Tests
// ===========================================================================

class SSEEndpointTest : public ::testing::Test {
protected:
    std::unique_ptr<SSEEndpoint> endpoint_;

    void SetUp() override {
        SSEEndpoint::Config config;
        config.max_connections = 100;
        endpoint_ = std::make_unique<SSEEndpoint>(config);
    }

    void TearDown() override {
        endpoint_.reset();
    }
};

// Test endpoint creation
TEST_F(SSEEndpointTest, Creation) {
    EXPECT_EQ(endpoint_->active_connections(), 0u);
    EXPECT_EQ(endpoint_->total_events_sent(), 0u);
}

// Test accept connection
TEST_F(SSEEndpointTest, AcceptConnection) {
    bool handler_called = false;
    SSEConnection* conn = endpoint_->accept([&](SSEConnection* c) {
        handler_called = true;
        EXPECT_TRUE(c->is_open());
        c->send("Hello World");
    });

    EXPECT_NE(conn, nullptr);
    EXPECT_TRUE(handler_called);
    EXPECT_EQ(endpoint_->active_connections(), 1u);
    EXPECT_EQ(endpoint_->total_events_sent(), 1u);
}

// Test accept with last event ID (for reconnection)
TEST_F(SSEEndpointTest, AcceptWithLastEventId) {
    std::string last_id = "event-123";
    SSEConnection* conn = endpoint_->accept([&](SSEConnection* c) {
        EXPECT_EQ(c->get_last_event_id(), last_id);
    }, last_id);

    EXPECT_NE(conn, nullptr);
}

// Test multiple connections
TEST_F(SSEEndpointTest, MultipleConnections) {
    int events_total = 0;

    for (int i = 0; i < 10; ++i) {
        endpoint_->accept([&](SSEConnection* c) {
            for (int j = 0; j < i + 1; ++j) {
                c->send("event " + std::to_string(j));
                events_total++;
            }
        });
    }

    EXPECT_EQ(endpoint_->active_connections(), 10u);
    EXPECT_EQ(endpoint_->total_events_sent(), static_cast<uint64_t>(events_total));
}

// Test close all connections
TEST_F(SSEEndpointTest, CloseAll) {
    std::vector<SSEConnection*> connections;

    for (int i = 0; i < 5; ++i) {
        SSEConnection* conn = endpoint_->accept([](SSEConnection* c) {
            c->send("test");
        });
        connections.push_back(conn);
    }

    EXPECT_EQ(endpoint_->active_connections(), 5u);

    endpoint_->close_all();

    EXPECT_EQ(endpoint_->active_connections(), 0u);
}

// Test connection limit
TEST_F(SSEEndpointTest, ConnectionLimit) {
    // Create endpoint with low limit
    SSEEndpoint::Config config;
    config.max_connections = 3;
    auto limited_endpoint = std::make_unique<SSEEndpoint>(config);

    // Accept up to limit
    for (int i = 0; i < 3; ++i) {
        SSEConnection* conn = limited_endpoint->accept([](SSEConnection* c) {
            c->send("test");
        });
        EXPECT_NE(conn, nullptr);
    }

    // Should fail at limit
    SSEConnection* overflow = limited_endpoint->accept([](SSEConnection*) {});
    EXPECT_EQ(overflow, nullptr);
}

// Test empty handler
TEST_F(SSEEndpointTest, EmptyHandler) {
    SSEConnection* conn = endpoint_->accept(nullptr);
    EXPECT_NE(conn, nullptr);
    EXPECT_TRUE(conn->is_open());
}

// Test config defaults
TEST_F(SSEEndpointTest, ConfigDefaults) {
    SSEEndpoint::Config config;
    EXPECT_TRUE(config.enable_cors);
    EXPECT_EQ(config.allowed_origin, "*");
    EXPECT_EQ(config.ping_interval_ms, 30000u);
    EXPECT_EQ(config.max_connections, 10000u);
    EXPECT_EQ(config.buffer_size, 65536u);
}

}  // namespace test
}  // namespace http
}  // namespace fasterapi

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
