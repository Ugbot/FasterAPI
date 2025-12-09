/**
 * Request Timeout Unit Tests
 *
 * Tests the request timeout implementation:
 * - Http1Connection timeout tracking (request_start_time_, last_activity_time_)
 * - Timeout detection methods (is_request_timed_out, is_idle_timed_out)
 * - 408 Request Timeout response generation
 * - Config defaults for timeout values
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/http1_connection.h"
#include "../../src/cpp/http/unified_server.h"
#include <thread>
#include <chrono>

namespace fasterapi {
namespace http {
namespace test {

class RequestTimeoutTest : public ::testing::Test {
protected:
    std::unique_ptr<Http1Connection> conn_;

    void SetUp() override {
        // Create a dummy connection (fd=-1 since we're just testing the class)
        conn_ = std::make_unique<Http1Connection>(-1);
    }

    void TearDown() override {
        conn_.reset();
    }
};

// Test initial timestamp state
TEST_F(RequestTimeoutTest, InitialTimestampState) {
    // Immediately after construction, elapsed time should be very small
    uint64_t elapsed = conn_->get_request_elapsed_ms();
    EXPECT_LE(elapsed, 10u);  // Should be < 10ms

    uint64_t idle = conn_->get_idle_time_ms();
    EXPECT_LE(idle, 10u);
}

// Test mark_request_start() updates timestamp
TEST_F(RequestTimeoutTest, MarkRequestStart) {
    // Wait a bit, then mark start
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    conn_->mark_request_start();

    // After marking, elapsed should be very small again
    uint64_t elapsed = conn_->get_request_elapsed_ms();
    EXPECT_LE(elapsed, 10u);
}

// Test mark_activity() updates timestamp
TEST_F(RequestTimeoutTest, MarkActivity) {
    // Wait a bit, then mark activity
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    conn_->mark_activity();

    // After marking, idle time should be very small again
    uint64_t idle = conn_->get_idle_time_ms();
    EXPECT_LE(idle, 10u);
}

// Test is_request_timed_out() with short timeout
TEST_F(RequestTimeoutTest, RequestTimeoutDetection) {
    // Initially should not be timed out with 1000ms timeout
    EXPECT_FALSE(conn_->is_request_timed_out(1000));

    // Wait 30ms, should not be timed out with 1000ms timeout
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_FALSE(conn_->is_request_timed_out(1000));

    // But should be timed out with 10ms timeout
    EXPECT_TRUE(conn_->is_request_timed_out(10));
}

// Test is_idle_timed_out() with short timeout
TEST_F(RequestTimeoutTest, IdleTimeoutDetection) {
    // Initially should not be timed out with 1000ms timeout
    EXPECT_FALSE(conn_->is_idle_timed_out(1000));

    // Wait 30ms, should not be timed out with 1000ms timeout
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_FALSE(conn_->is_idle_timed_out(1000));

    // But should be timed out with 10ms timeout
    EXPECT_TRUE(conn_->is_idle_timed_out(10));
}

// Test get_request_elapsed_ms() accuracy
TEST_F(RequestTimeoutTest, ElapsedTimeAccuracy) {
    conn_->mark_request_start();

    // Wait 50ms
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    uint64_t elapsed = conn_->get_request_elapsed_ms();
    // Should be approximately 50ms (allow some slack for scheduling)
    EXPECT_GE(elapsed, 40u);
    EXPECT_LE(elapsed, 100u);
}

// Test get_idle_time_ms() accuracy
TEST_F(RequestTimeoutTest, IdleTimeAccuracy) {
    conn_->mark_activity();

    // Wait 50ms
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    uint64_t idle = conn_->get_idle_time_ms();
    // Should be approximately 50ms (allow some slack for scheduling)
    EXPECT_GE(idle, 40u);
    EXPECT_LE(idle, 100u);
}

// Test send_timeout_response() generates valid HTTP response
TEST_F(RequestTimeoutTest, TimeoutResponseGeneration) {
    // Set up a dummy callback (required for some internal state)
    conn_->set_request_callback([](
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body
    ) -> Http1Response {
        return Http1Response{};
    });

    // Call send_timeout_response
    conn_->send_timeout_response();

    // Verify state transitioned to WRITING_RESPONSE
    EXPECT_EQ(conn_->get_state(), Http1State::WRITING_RESPONSE);

    // Verify there is output to send
    EXPECT_TRUE(conn_->has_pending_output());

    // Get the output and verify it contains 408
    const uint8_t* data;
    size_t len;
    EXPECT_TRUE(conn_->get_output(&data, &len));

    std::string response(reinterpret_cast<const char*>(data), len);
    EXPECT_TRUE(response.find("HTTP/1.1 408") != std::string::npos);
    EXPECT_TRUE(response.find("Request Timeout") != std::string::npos);
    EXPECT_TRUE(response.find("Connection: close") != std::string::npos);
}

// Test timeout response closes connection (keep_alive = false)
TEST_F(RequestTimeoutTest, TimeoutResponseClosesConnection) {
    conn_->set_request_callback([](
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body
    ) -> Http1Response {
        return Http1Response{};
    });

    conn_->send_timeout_response();

    // After sending timeout, should not keep alive
    EXPECT_FALSE(conn_->should_keep_alive());
}

// Test config defaults
TEST_F(RequestTimeoutTest, ConfigDefaults) {
    UnifiedServerConfig config;

    EXPECT_EQ(config.request_timeout_ms, 30000u);  // 30 second default
    EXPECT_EQ(config.idle_timeout_ms, 60000u);      // 60 second default
    EXPECT_EQ(config.max_body_size, 10 * 1024 * 1024u);  // 10MB default
    EXPECT_EQ(config.max_header_size, 8192u);       // 8KB default
}

// Test config customization
TEST_F(RequestTimeoutTest, ConfigCustomization) {
    UnifiedServerConfig config;
    config.request_timeout_ms = 5000;
    config.idle_timeout_ms = 10000;
    config.max_body_size = 1024 * 1024;  // 1MB
    config.max_header_size = 4096;       // 4KB

    EXPECT_EQ(config.request_timeout_ms, 5000u);
    EXPECT_EQ(config.idle_timeout_ms, 10000u);
    EXPECT_EQ(config.max_body_size, 1024 * 1024u);
    EXPECT_EQ(config.max_header_size, 4096u);
}

// Test server timeout getters
TEST_F(RequestTimeoutTest, ServerTimeoutGetters) {
    UnifiedServerConfig config;
    config.request_timeout_ms = 15000;
    config.idle_timeout_ms = 30000;
    config.max_body_size = 5 * 1024 * 1024;
    config.max_header_size = 16384;
    config.enable_tls = false;
    config.enable_http1_cleartext = false;
    config.enable_http3 = false;
    config.enable_signal_handlers = false;
    config.pure_cpp_mode = true;

    UnifiedServer server(config);

    EXPECT_EQ(server.get_request_timeout_ms(), 15000u);
    EXPECT_EQ(server.get_idle_timeout_ms(), 30000u);
    EXPECT_EQ(server.get_max_body_size(), 5 * 1024 * 1024u);
    EXPECT_EQ(server.get_max_header_size(), 16384u);
}

// Test reset_for_next_request() resets timestamps
TEST_F(RequestTimeoutTest, ResetForNextRequestResetsTimestamps) {
    // Wait to build up some elapsed time
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    uint64_t before_elapsed = conn_->get_request_elapsed_ms();
    EXPECT_GE(before_elapsed, 25u);

    // Reset for next request
    conn_->reset_for_next_request();

    // After reset, timestamps should be fresh
    uint64_t after_elapsed = conn_->get_request_elapsed_ms();
    EXPECT_LE(after_elapsed, 10u);

    uint64_t after_idle = conn_->get_idle_time_ms();
    EXPECT_LE(after_idle, 10u);
}

// Test timeout edge cases - zero timeout
TEST_F(RequestTimeoutTest, ZeroTimeoutNeverExpires) {
    // Zero timeout means "disabled" - never expires (0 > 0 is false)
    // This is safe default behavior for production
    EXPECT_FALSE(conn_->is_request_timed_out(0));
    EXPECT_FALSE(conn_->is_idle_timed_out(0));
}

// Test timeout edge cases - very large timeout
TEST_F(RequestTimeoutTest, LargeTimeoutNeverExpires) {
    // Very large timeout should never expire (within test duration)
    EXPECT_FALSE(conn_->is_request_timed_out(UINT32_MAX));
    EXPECT_FALSE(conn_->is_idle_timed_out(UINT32_MAX));
}

// Test multiple mark_activity() calls
TEST_F(RequestTimeoutTest, MultipleActivityMarks) {
    for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        conn_->mark_activity();
        uint64_t idle = conn_->get_idle_time_ms();
        EXPECT_LE(idle, 15u);  // Should be fresh after each mark
    }
}

}  // namespace test
}  // namespace http
}  // namespace fasterapi

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
