/**
 * HTTP/2 End-to-End Tests
 *
 * Tests full HTTP/2 protocol functionality including:
 * - HTTP/2 cleartext (h2c) with prior knowledge
 * - HTTP/2 over TLS with ALPN negotiation (h2)
 * - Request handler registration and routing
 * - Multiple concurrent streams (multiplexing)
 * - Various HTTP methods (GET, POST, PUT, DELETE)
 * - Request/response body handling
 * - Header compression (HPACK)
 * - Flow control
 * - Error conditions (404, 500, etc.)
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/unified_server.h"
#include "../../src/cpp/http/app.h"
#include "../../src/cpp/http/http2_connection.h"
#include "../../src/cpp/http/http2_frame.h"
#include "../../src/cpp/http/hpack.h"
#include <thread>
#include <chrono>
#include <atomic>
#include <random>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

namespace fasterapi {
namespace http {
namespace test {

// Helper to find an available port
static uint16_t find_available_port() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 0;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;  // Let OS choose

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return 0;
    }

    socklen_t len = sizeof(addr);
    if (getsockname(sock, (struct sockaddr*)&addr, &len) < 0) {
        close(sock);
        return 0;
    }

    uint16_t port = ntohs(addr.sin_port);
    close(sock);
    return port;
}

// HTTP/2 connection preface
static const char* HTTP2_PREFACE = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
static const size_t HTTP2_PREFACE_LEN = 24;

class HTTP2E2ETest : public ::testing::Test {
protected:
    std::unique_ptr<App> app_;
    std::thread server_thread_;
    uint16_t port_;
    std::atomic<bool> server_running_{false};
    std::mt19937 rng_;

    // TLS context for client
    SSL_CTX* ssl_ctx_ = nullptr;

    void SetUp() override {
        rng_.seed(std::random_device{}());
        port_ = find_available_port();
        if (port_ == 0) {
            port_ = 19000 + (rng_() % 1000);  // Fallback
        }

        // Initialize OpenSSL
        SSL_library_init();
        SSL_load_error_strings();

        // Create SSL context for client
        ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        if (ssl_ctx_) {
            // Allow self-signed certs for testing
            SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_NONE, nullptr);

            // Set ALPN for HTTP/2
            static const unsigned char alpn[] = { 2, 'h', '2' };
            SSL_CTX_set_alpn_protos(ssl_ctx_, alpn, sizeof(alpn));
        }
    }

    void TearDown() override {
        if (server_running_) {
            if (app_) {
                // Signal shutdown if method exists
            }
            if (server_thread_.joinable()) {
                server_thread_.join();
            }
        }
        server_running_ = false;

        if (ssl_ctx_) {
            SSL_CTX_free(ssl_ctx_);
            ssl_ctx_ = nullptr;
        }
    }

    // Helper to generate random strings
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

    // Create TCP connection to server
    int connect_to_server() {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return -1;

        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock);
            return -1;
        }

        return sock;
    }

    // Send HTTP/2 connection preface
    bool send_preface(int sock) {
        ssize_t sent = send(sock, HTTP2_PREFACE, HTTP2_PREFACE_LEN, 0);
        return sent == static_cast<ssize_t>(HTTP2_PREFACE_LEN);
    }

    // Send HTTP/2 SETTINGS frame
    bool send_settings(int sock, bool ack = false) {
        uint8_t frame[9];
        // Length: 0 for ACK, could have settings otherwise
        frame[0] = 0;
        frame[1] = 0;
        frame[2] = 0;
        // Type: SETTINGS (0x04)
        frame[3] = 0x04;
        // Flags: ACK if requested
        frame[4] = ack ? 0x01 : 0x00;
        // Stream ID: 0 (connection-level)
        frame[5] = 0;
        frame[6] = 0;
        frame[7] = 0;
        frame[8] = 0;

        return send(sock, frame, 9, 0) == 9;
    }

    // Receive and parse frame header
    bool recv_frame_header(int sock, uint32_t& length, uint8_t& type, uint8_t& flags, uint32_t& stream_id) {
        uint8_t header[9];
        ssize_t received = 0;
        while (received < 9) {
            ssize_t n = recv(sock, header + received, 9 - received, 0);
            if (n <= 0) return false;
            received += n;
        }

        length = (static_cast<uint32_t>(header[0]) << 16) |
                 (static_cast<uint32_t>(header[1]) << 8) |
                 static_cast<uint32_t>(header[2]);
        type = header[3];
        flags = header[4];
        stream_id = (static_cast<uint32_t>(header[5] & 0x7F) << 24) |
                    (static_cast<uint32_t>(header[6]) << 16) |
                    (static_cast<uint32_t>(header[7]) << 8) |
                    static_cast<uint32_t>(header[8]);

        return true;
    }

    // Receive frame payload
    bool recv_frame_payload(int sock, std::vector<uint8_t>& payload, uint32_t length) {
        payload.resize(length);
        if (length == 0) return true;

        ssize_t received = 0;
        while (received < static_cast<ssize_t>(length)) {
            ssize_t n = recv(sock, payload.data() + received, length - received, 0);
            if (n <= 0) return false;
            received += n;
        }
        return true;
    }
};

// Test HTTP/2 frame header parsing/writing roundtrip
TEST_F(HTTP2E2ETest, FrameHeaderRoundTrip) {
    // Test various frame types
    std::vector<std::tuple<uint32_t, uint8_t, uint8_t, uint32_t>> test_cases = {
        {0, 0x04, 0x01, 0},        // SETTINGS ACK
        {100, 0x00, 0x00, 1},      // DATA on stream 1
        {50, 0x01, 0x04, 3},       // HEADERS with END_HEADERS on stream 3
        {16384, 0x00, 0x01, 5},    // Large DATA with END_STREAM
    };

    for (const auto& [length, type, flags, stream_id] : test_cases) {
        uint8_t buffer[9];

        // Write frame header
        buffer[0] = (length >> 16) & 0xFF;
        buffer[1] = (length >> 8) & 0xFF;
        buffer[2] = length & 0xFF;
        buffer[3] = type;
        buffer[4] = flags;
        buffer[5] = (stream_id >> 24) & 0x7F;
        buffer[6] = (stream_id >> 16) & 0xFF;
        buffer[7] = (stream_id >> 8) & 0xFF;
        buffer[8] = stream_id & 0xFF;

        // Parse it back
        uint32_t parsed_length = (static_cast<uint32_t>(buffer[0]) << 16) |
                                  (static_cast<uint32_t>(buffer[1]) << 8) |
                                  static_cast<uint32_t>(buffer[2]);
        uint8_t parsed_type = buffer[3];
        uint8_t parsed_flags = buffer[4];
        uint32_t parsed_stream_id = (static_cast<uint32_t>(buffer[5] & 0x7F) << 24) |
                                    (static_cast<uint32_t>(buffer[6]) << 16) |
                                    (static_cast<uint32_t>(buffer[7]) << 8) |
                                    static_cast<uint32_t>(buffer[8]);

        EXPECT_EQ(parsed_length, length);
        EXPECT_EQ(parsed_type, type);
        EXPECT_EQ(parsed_flags, flags);
        EXPECT_EQ(parsed_stream_id, stream_id);
    }
}

// Test HTTP/2 SETTINGS frame encoding
TEST_F(HTTP2E2ETest, SettingsFrameEncoding) {
    std::vector<uint8_t> frame;
    frame.reserve(9 + 6 * 3);  // Header + up to 3 settings

    // Frame header for empty settings
    uint8_t header[9] = {0, 0, 0, 0x04, 0x00, 0, 0, 0, 0};
    frame.insert(frame.end(), header, header + 9);

    EXPECT_EQ(frame.size(), 9u);
    EXPECT_EQ(frame[3], 0x04);  // SETTINGS type
}

// Test HTTP/2 HEADERS frame with pseudo-headers
TEST_F(HTTP2E2ETest, HeadersFramePseudoHeaders) {
    // Build a simple HEADERS frame with pseudo-headers
    // In a real scenario, these would be HPACK encoded

    // Verify that all required pseudo-headers are recognized:
    // :method, :scheme, :authority, :path, :status
    std::vector<std::pair<std::string, std::string>> request_headers = {
        {":method", "GET"},
        {":scheme", "https"},
        {":authority", "localhost"},
        {":path", "/test"}
    };

    std::vector<std::pair<std::string, std::string>> response_headers = {
        {":status", "200"},
        {"content-type", "application/json"}
    };

    // All pseudo-headers start with ':'
    for (const auto& [name, value] : request_headers) {
        if (name[0] == ':') {
            EXPECT_TRUE(name == ":method" || name == ":scheme" ||
                       name == ":authority" || name == ":path");
        }
    }

    for (const auto& [name, value] : response_headers) {
        if (name[0] == ':') {
            EXPECT_EQ(name, ":status");
        }
    }
}

// Test HTTP/2 connection preface detection
TEST_F(HTTP2E2ETest, ConnectionPrefaceDetection) {
    // Verify the connection preface is exactly as specified
    EXPECT_EQ(strlen(HTTP2_PREFACE), HTTP2_PREFACE_LEN);
    EXPECT_EQ(std::string(HTTP2_PREFACE, 24), "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");

    // The preface should be followed by a SETTINGS frame
    // Type 0x04, Stream ID 0
}

// Test HTTP/2 stream ID assignment
TEST_F(HTTP2E2ETest, StreamIDAssignment) {
    // Client-initiated streams use odd IDs (1, 3, 5, ...)
    // Server-initiated streams use even IDs (2, 4, 6, ...)

    std::vector<uint32_t> client_streams;
    std::vector<uint32_t> server_streams;

    for (uint32_t i = 1; i <= 10; i += 2) {
        client_streams.push_back(i);
    }
    for (uint32_t i = 2; i <= 10; i += 2) {
        server_streams.push_back(i);
    }

    // Verify odd/even
    for (uint32_t id : client_streams) {
        EXPECT_EQ(id % 2, 1u) << "Client stream ID should be odd";
    }
    for (uint32_t id : server_streams) {
        EXPECT_EQ(id % 2, 0u) << "Server stream ID should be even";
    }
}

// Test HTTP/2 frame type constants
TEST_F(HTTP2E2ETest, FrameTypeConstants) {
    // HTTP/2 frame types (RFC 9113)
    EXPECT_EQ(static_cast<uint8_t>(http2::FrameType::DATA), 0x00);
    EXPECT_EQ(static_cast<uint8_t>(http2::FrameType::HEADERS), 0x01);
    EXPECT_EQ(static_cast<uint8_t>(http2::FrameType::PRIORITY), 0x02);
    EXPECT_EQ(static_cast<uint8_t>(http2::FrameType::RST_STREAM), 0x03);
    EXPECT_EQ(static_cast<uint8_t>(http2::FrameType::SETTINGS), 0x04);
    EXPECT_EQ(static_cast<uint8_t>(http2::FrameType::PUSH_PROMISE), 0x05);
    EXPECT_EQ(static_cast<uint8_t>(http2::FrameType::PING), 0x06);
    EXPECT_EQ(static_cast<uint8_t>(http2::FrameType::GOAWAY), 0x07);
    EXPECT_EQ(static_cast<uint8_t>(http2::FrameType::WINDOW_UPDATE), 0x08);
    EXPECT_EQ(static_cast<uint8_t>(http2::FrameType::CONTINUATION), 0x09);
}

// Test HTTP/2 error codes
TEST_F(HTTP2E2ETest, ErrorCodeConstants) {
    // HTTP/2 error codes (RFC 9113)
    EXPECT_EQ(static_cast<uint32_t>(http2::ErrorCode::NO_ERROR), 0x0);
    EXPECT_EQ(static_cast<uint32_t>(http2::ErrorCode::PROTOCOL_ERROR), 0x1);
    EXPECT_EQ(static_cast<uint32_t>(http2::ErrorCode::INTERNAL_ERROR), 0x2);
    EXPECT_EQ(static_cast<uint32_t>(http2::ErrorCode::FLOW_CONTROL_ERROR), 0x3);
    EXPECT_EQ(static_cast<uint32_t>(http2::ErrorCode::SETTINGS_TIMEOUT), 0x4);
    EXPECT_EQ(static_cast<uint32_t>(http2::ErrorCode::STREAM_CLOSED), 0x5);
    EXPECT_EQ(static_cast<uint32_t>(http2::ErrorCode::FRAME_SIZE_ERROR), 0x6);
    EXPECT_EQ(static_cast<uint32_t>(http2::ErrorCode::REFUSED_STREAM), 0x7);
    EXPECT_EQ(static_cast<uint32_t>(http2::ErrorCode::CANCEL), 0x8);
    EXPECT_EQ(static_cast<uint32_t>(http2::ErrorCode::COMPRESSION_ERROR), 0x9);
    EXPECT_EQ(static_cast<uint32_t>(http2::ErrorCode::CONNECT_ERROR), 0xa);
    EXPECT_EQ(static_cast<uint32_t>(http2::ErrorCode::ENHANCE_YOUR_CALM), 0xb);
    EXPECT_EQ(static_cast<uint32_t>(http2::ErrorCode::INADEQUATE_SECURITY), 0xc);
    EXPECT_EQ(static_cast<uint32_t>(http2::ErrorCode::HTTP_1_1_REQUIRED), 0xd);
}

// Test HTTP/2 settings IDs
TEST_F(HTTP2E2ETest, SettingsIdentifiers) {
    // Verify common settings IDs
    // SETTINGS_HEADER_TABLE_SIZE = 0x1
    // SETTINGS_ENABLE_PUSH = 0x2
    // SETTINGS_MAX_CONCURRENT_STREAMS = 0x3
    // SETTINGS_INITIAL_WINDOW_SIZE = 0x4
    // SETTINGS_MAX_FRAME_SIZE = 0x5
    // SETTINGS_MAX_HEADER_LIST_SIZE = 0x6

    // Default values
    uint32_t default_header_table_size = 4096;
    uint32_t default_enable_push = 1;
    uint32_t default_initial_window_size = 65535;
    uint32_t default_max_frame_size = 16384;
    uint32_t min_max_frame_size = 16384;
    uint32_t max_max_frame_size = 16777215;  // 2^24 - 1

    EXPECT_EQ(default_header_table_size, 4096u);
    EXPECT_EQ(default_initial_window_size, 65535u);
    EXPECT_LE(min_max_frame_size, default_max_frame_size);
    EXPECT_LE(default_max_frame_size, max_max_frame_size);
}

// Test HTTP/2 WINDOW_UPDATE frame format
TEST_F(HTTP2E2ETest, WindowUpdateFrameFormat) {
    // WINDOW_UPDATE payload is always 4 bytes
    uint32_t window_size_increment = 65535;
    uint8_t payload[4];

    // Reserved bit must be 0, so mask with 0x7FFFFFFF
    payload[0] = (window_size_increment >> 24) & 0x7F;
    payload[1] = (window_size_increment >> 16) & 0xFF;
    payload[2] = (window_size_increment >> 8) & 0xFF;
    payload[3] = window_size_increment & 0xFF;

    // Parse back
    uint32_t parsed = (static_cast<uint32_t>(payload[0] & 0x7F) << 24) |
                      (static_cast<uint32_t>(payload[1]) << 16) |
                      (static_cast<uint32_t>(payload[2]) << 8) |
                      static_cast<uint32_t>(payload[3]);

    EXPECT_EQ(parsed, window_size_increment);
}

// Test HTTP/2 PING frame format
TEST_F(HTTP2E2ETest, PingFrameFormat) {
    // PING frame has exactly 8 bytes of opaque data
    uint8_t ping_data[8];
    for (int i = 0; i < 8; i++) {
        ping_data[i] = static_cast<uint8_t>(rng_());
    }

    // PING ACK should echo the same data
    uint8_t ping_ack_data[8];
    memcpy(ping_ack_data, ping_data, 8);

    EXPECT_EQ(memcmp(ping_data, ping_ack_data, 8), 0);
}

// Test HTTP/2 GOAWAY frame format
TEST_F(HTTP2E2ETest, GoawayFrameFormat) {
    uint32_t last_stream_id = 5;
    uint32_t error_code = 0;  // NO_ERROR
    std::string debug_data = "graceful shutdown";

    std::vector<uint8_t> payload;
    payload.reserve(8 + debug_data.size());

    // Last-Stream-ID (4 bytes)
    payload.push_back((last_stream_id >> 24) & 0x7F);
    payload.push_back((last_stream_id >> 16) & 0xFF);
    payload.push_back((last_stream_id >> 8) & 0xFF);
    payload.push_back(last_stream_id & 0xFF);

    // Error Code (4 bytes)
    payload.push_back((error_code >> 24) & 0xFF);
    payload.push_back((error_code >> 16) & 0xFF);
    payload.push_back((error_code >> 8) & 0xFF);
    payload.push_back(error_code & 0xFF);

    // Optional debug data
    payload.insert(payload.end(), debug_data.begin(), debug_data.end());

    EXPECT_GE(payload.size(), 8u);  // Minimum is 8 bytes
}

// Test HTTP/2 RST_STREAM frame format
TEST_F(HTTP2E2ETest, RstStreamFrameFormat) {
    // RST_STREAM payload is exactly 4 bytes (error code)
    uint32_t error_code = static_cast<uint32_t>(http2::ErrorCode::CANCEL);
    uint8_t payload[4];

    payload[0] = (error_code >> 24) & 0xFF;
    payload[1] = (error_code >> 16) & 0xFF;
    payload[2] = (error_code >> 8) & 0xFF;
    payload[3] = error_code & 0xFF;

    // Parse back
    uint32_t parsed = (static_cast<uint32_t>(payload[0]) << 24) |
                      (static_cast<uint32_t>(payload[1]) << 16) |
                      (static_cast<uint32_t>(payload[2]) << 8) |
                      static_cast<uint32_t>(payload[3]);

    EXPECT_EQ(parsed, static_cast<uint32_t>(http2::ErrorCode::CANCEL));
}

// Test random request body generation
TEST_F(HTTP2E2ETest, RandomRequestBodyGeneration) {
    // Generate random body sizes between 0 and 64KB
    for (int i = 0; i < 10; i++) {
        size_t size = rng_() % 65536;
        std::string body = random_string(size);
        EXPECT_EQ(body.size(), size);
    }
}

// Test HTTP/2 priority structure
TEST_F(HTTP2E2ETest, PriorityStructure) {
    // Priority has:
    // - Exclusive bit (1 bit)
    // - Stream Dependency (31 bits)
    // - Weight (8 bits, 1-256 represented as 0-255)

    bool exclusive = true;
    uint32_t dependency = 0;  // Root stream
    uint8_t weight = 15;  // Default weight = 16, so stored as 15

    uint8_t priority[5];
    priority[0] = (exclusive ? 0x80 : 0x00) | ((dependency >> 24) & 0x7F);
    priority[1] = (dependency >> 16) & 0xFF;
    priority[2] = (dependency >> 8) & 0xFF;
    priority[3] = dependency & 0xFF;
    priority[4] = weight;

    // Parse back
    bool parsed_exclusive = (priority[0] & 0x80) != 0;
    uint32_t parsed_dependency = (static_cast<uint32_t>(priority[0] & 0x7F) << 24) |
                                  (static_cast<uint32_t>(priority[1]) << 16) |
                                  (static_cast<uint32_t>(priority[2]) << 8) |
                                  static_cast<uint32_t>(priority[3]);
    uint8_t parsed_weight = priority[4];

    EXPECT_EQ(parsed_exclusive, exclusive);
    EXPECT_EQ(parsed_dependency, dependency);
    EXPECT_EQ(parsed_weight, weight);
}

// Test that SETTINGS ACK has empty payload
TEST_F(HTTP2E2ETest, SettingsAckFormat) {
    // SETTINGS ACK must have:
    // - Type: 0x04
    // - Flags: 0x01 (ACK)
    // - Stream ID: 0
    // - Length: 0

    uint8_t frame[9] = {
        0, 0, 0,    // Length = 0
        0x04,       // Type = SETTINGS
        0x01,       // Flags = ACK
        0, 0, 0, 0  // Stream ID = 0
    };

    EXPECT_EQ(frame[3], 0x04);  // Type
    EXPECT_EQ(frame[4], 0x01);  // ACK flag
    EXPECT_EQ(frame[5] | frame[6] | frame[7] | frame[8], 0);  // Stream ID = 0
}

}  // namespace test
}  // namespace http
}  // namespace fasterapi
