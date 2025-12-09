/**
 * Body Size Limits Unit Tests
 *
 * Tests the body size limit implementation:
 * - is_body_too_large() detection
 * - get_content_length() accessor
 * - 413 Payload Too Large response generation
 * - 431 Request Header Fields Too Large response generation
 * - Config defaults for size limits
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/http1_connection.h"
#include "../../src/cpp/http/unified_server.h"

namespace fasterapi {
namespace http {
namespace test {

class BodySizeLimitsTest : public ::testing::Test {
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

// Test get_content_length() returns 0 for new connection
TEST_F(BodySizeLimitsTest, InitialContentLengthZero) {
    EXPECT_EQ(conn_->get_content_length(), 0u);
}

// Test is_body_too_large() with zero max (disabled)
TEST_F(BodySizeLimitsTest, ZeroMaxDisablesLimit) {
    // Zero max_body_size means "no limit" - never too large
    EXPECT_FALSE(conn_->is_body_too_large(0));
}

// Test is_body_too_large() with no Content-Length set
TEST_F(BodySizeLimitsTest, NoContentLengthNotTooLarge) {
    // Without Content-Length, body size is 0
    EXPECT_FALSE(conn_->is_body_too_large(100));
    EXPECT_FALSE(conn_->is_body_too_large(1));
}

// Test send_payload_too_large_response() generates valid response
TEST_F(BodySizeLimitsTest, PayloadTooLargeResponse) {
    conn_->set_request_callback([](
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body
    ) -> Http1Response {
        return Http1Response{};
    });

    conn_->send_payload_too_large_response();

    // Verify state
    EXPECT_EQ(conn_->get_state(), Http1State::WRITING_RESPONSE);
    EXPECT_TRUE(conn_->has_pending_output());
    EXPECT_FALSE(conn_->should_keep_alive());

    // Verify response content
    const uint8_t* data;
    size_t len;
    EXPECT_TRUE(conn_->get_output(&data, &len));

    std::string response(reinterpret_cast<const char*>(data), len);
    EXPECT_TRUE(response.find("HTTP/1.1 413") != std::string::npos);
    EXPECT_TRUE(response.find("Payload Too Large") != std::string::npos);
    EXPECT_TRUE(response.find("Connection: close") != std::string::npos);
}

// Test send_header_too_large_response() generates valid response
TEST_F(BodySizeLimitsTest, HeaderTooLargeResponse) {
    conn_->set_request_callback([](
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body
    ) -> Http1Response {
        return Http1Response{};
    });

    conn_->send_header_too_large_response();

    // Verify state
    EXPECT_EQ(conn_->get_state(), Http1State::WRITING_RESPONSE);
    EXPECT_TRUE(conn_->has_pending_output());
    EXPECT_FALSE(conn_->should_keep_alive());

    // Verify response content
    const uint8_t* data;
    size_t len;
    EXPECT_TRUE(conn_->get_output(&data, &len));

    std::string response(reinterpret_cast<const char*>(data), len);
    EXPECT_TRUE(response.find("HTTP/1.1 431") != std::string::npos);
    EXPECT_TRUE(response.find("Request Header Fields Too Large") != std::string::npos);
    EXPECT_TRUE(response.find("Connection: close") != std::string::npos);
}

// Test config defaults
TEST_F(BodySizeLimitsTest, ConfigDefaults) {
    UnifiedServerConfig config;

    EXPECT_EQ(config.max_body_size, 10u * 1024 * 1024);  // 10MB default
    EXPECT_EQ(config.max_header_size, 8192u);            // 8KB default
}

// Test config customization
TEST_F(BodySizeLimitsTest, ConfigCustomization) {
    UnifiedServerConfig config;
    config.max_body_size = 1024 * 1024;  // 1MB
    config.max_header_size = 4096;       // 4KB

    EXPECT_EQ(config.max_body_size, 1024u * 1024);
    EXPECT_EQ(config.max_header_size, 4096u);
}

// Test various size limits
TEST_F(BodySizeLimitsTest, VariousSizeLimits) {
    for (size_t max_size : {1024u, 4096u, 8192u, 1024u*1024, 10u*1024*1024}) {
        UnifiedServerConfig config;
        config.max_body_size = max_size;
        config.max_header_size = max_size;
        config.enable_tls = false;
        config.enable_http1_cleartext = false;
        config.enable_http3 = false;
        config.enable_signal_handlers = false;
        config.pure_cpp_mode = true;

        UnifiedServer server(config);
        EXPECT_EQ(server.get_max_body_size(), max_size);
        EXPECT_EQ(server.get_max_header_size(), max_size);
    }
}

// Test server getters
TEST_F(BodySizeLimitsTest, ServerGetters) {
    UnifiedServerConfig config;
    config.max_body_size = 5 * 1024 * 1024;
    config.max_header_size = 16384;
    config.enable_tls = false;
    config.enable_http1_cleartext = false;
    config.enable_http3 = false;
    config.enable_signal_handlers = false;
    config.pure_cpp_mode = true;

    UnifiedServer server(config);
    EXPECT_EQ(server.get_max_body_size(), 5u * 1024 * 1024);
    EXPECT_EQ(server.get_max_header_size(), 16384u);
}

// Test response closes connection
TEST_F(BodySizeLimitsTest, ResponseClosesConnection) {
    conn_->set_request_callback([](
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body
    ) -> Http1Response {
        return Http1Response{};
    });

    // Send 413 response
    conn_->send_payload_too_large_response();
    EXPECT_FALSE(conn_->should_keep_alive());

    // Reset for another test
    conn_->reset_for_next_request();

    // Send 431 response
    conn_->send_header_too_large_response();
    EXPECT_FALSE(conn_->should_keep_alive());
}

// Test large body size value (boundary test)
TEST_F(BodySizeLimitsTest, LargeSizeValue) {
    UnifiedServerConfig config;
    config.max_body_size = SIZE_MAX;
    config.max_header_size = SIZE_MAX;

    EXPECT_EQ(config.max_body_size, SIZE_MAX);
    EXPECT_EQ(config.max_header_size, SIZE_MAX);
}

// Test minimum size value (1 byte)
TEST_F(BodySizeLimitsTest, MinimumSizeValue) {
    UnifiedServerConfig config;
    config.max_body_size = 1;
    config.max_header_size = 1;

    EXPECT_EQ(config.max_body_size, 1u);
    EXPECT_EQ(config.max_header_size, 1u);
}

}  // namespace test
}  // namespace http
}  // namespace fasterapi

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
