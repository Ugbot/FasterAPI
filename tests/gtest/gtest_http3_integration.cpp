/**
 * HTTP/3 Integration Tests
 *
 * Tests the full HTTP/3 stack integration:
 * - Http3Connection + QUICConnection + QPACK
 * - Request/Response handling
 * - Stream management
 * - Frame encoding/decoding
 * - End-to-end data flow
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "src/cpp/http/http3_connection.h"
#include "src/cpp/http/quic/quic_connection.h"
#include "src/cpp/http/quic/quic_frames.h"
#include "src/cpp/http/quic/quic_varint.h"
#include "src/cpp/http/qpack/qpack_encoder.h"
#include "src/cpp/http/qpack/qpack_decoder.h"
#include <random>
#include <chrono>

using namespace fasterapi::http;
using namespace fasterapi::quic;
using namespace fasterapi::qpack;

// =============================================================================
// Helper Functions
// =============================================================================

static uint64_t get_time_us() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()
    ).count();
}

static std::string random_string(size_t length) {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, sizeof(charset) - 2);

    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        result += charset[dis(gen)];
    }
    return result;
}

static ConnectionID make_conn_id(uint64_t value) {
    ConnectionID id;
    id.length = 8;
    std::memcpy(id.data, &value, 8);
    return id;
}

// =============================================================================
// QUIC Connection Tests
// =============================================================================

class QUICConnectionTest : public ::testing::Test {
protected:
    ConnectionID server_cid;
    ConnectionID client_cid;
    std::unique_ptr<QUICConnection> server_conn;
    std::unique_ptr<QUICConnection> client_conn;

    void SetUp() override {
        server_cid = make_conn_id(0x1234567890ABCDEF);
        client_cid = make_conn_id(0xFEDCBA0987654321);

        server_conn = std::make_unique<QUICConnection>(true, server_cid, client_cid);
        client_conn = std::make_unique<QUICConnection>(false, client_cid, server_cid);

        server_conn->initialize();
        client_conn->initialize();
    }
};

TEST_F(QUICConnectionTest, ConnectionInitialization) {
    EXPECT_EQ(server_conn->state(), ConnectionState::HANDSHAKE);
    EXPECT_EQ(client_conn->state(), ConnectionState::HANDSHAKE);
    EXPECT_FALSE(server_conn->is_closed());
    EXPECT_FALSE(client_conn->is_closed());
}

TEST_F(QUICConnectionTest, StreamCreationAfterEstablished) {
    // Simulate handshake completion
    // After processing first packet, connection transitions to ESTABLISHED
    uint8_t dummy_packet[100];
    std::memset(dummy_packet, 0, sizeof(dummy_packet));
    dummy_packet[0] = 0x80;  // Long header
    // Set version
    dummy_packet[1] = 0x00;
    dummy_packet[2] = 0x00;
    dummy_packet[3] = 0x00;
    dummy_packet[4] = 0x01;  // Version 1
    // Set DCID
    dummy_packet[5] = 8;  // DCID len
    std::memcpy(dummy_packet + 6, server_cid.data, 8);
    // Set SCID
    dummy_packet[14] = 8;  // SCID len
    std::memcpy(dummy_packet + 15, client_cid.data, 8);

    // Process packet to trigger handshake completion
    server_conn->process_packet(dummy_packet, 50, get_time_us());

    EXPECT_EQ(server_conn->state(), ConnectionState::ESTABLISHED);

    // Now can create streams
    uint64_t stream_id = server_conn->create_stream(true);
    EXPECT_NE(stream_id, 0u);
}

TEST_F(QUICConnectionTest, FlowControlIntegration) {
    auto& fc = server_conn->flow_control();

    // Initial window should allow sending
    EXPECT_TRUE(fc.can_send(1000));
    EXPECT_TRUE(fc.can_send(1024 * 1024));  // 1MB

    // Cannot exceed window
    EXPECT_FALSE(fc.can_send(100 * 1024 * 1024));  // 100MB > 16MB window
}

TEST_F(QUICConnectionTest, CongestionControlIntegration) {
    auto& cc = server_conn->congestion_control();

    // Initial window should allow sending
    EXPECT_TRUE(cc.can_send(1200));  // One packet

    // Get congestion window
    size_t cwnd = cc.congestion_window();
    EXPECT_GT(cwnd, 0u);
}

TEST_F(QUICConnectionTest, ConnectionClose) {
    server_conn->close(0x01, "test_close");

    EXPECT_EQ(server_conn->state(), ConnectionState::CLOSING);

    // Generate close packet
    uint8_t output[1500];
    size_t written = server_conn->generate_packets(output, sizeof(output), get_time_us());

    EXPECT_GT(written, 0u);

    // After sending close, transitions to DRAINING
    EXPECT_EQ(server_conn->state(), ConnectionState::DRAINING);
}

TEST_F(QUICConnectionTest, IdleTimeout) {
    // Simulate connection being idle
    uint64_t old_time = get_time_us();
    uint64_t timeout_time = old_time + 31000000;  // 31 seconds (past 30s timeout)

    bool timed_out = server_conn->check_idle_timeout(timeout_time);
    EXPECT_TRUE(timed_out);
}

// =============================================================================
// HTTP/3 Connection Tests
// =============================================================================

class Http3ConnectionTest : public ::testing::Test {
protected:
    ConnectionID local_cid;
    ConnectionID peer_cid;
    std::unique_ptr<Http3Connection> server_http3;

    void SetUp() override {
        local_cid = make_conn_id(0xAABBCCDDEEFF0011);
        peer_cid = make_conn_id(0x1100FFEEDDCCBBAA);

        Http3ConnectionSettings settings;
        settings.max_concurrent_streams = 100;
        settings.qpack_max_table_capacity = 4096;

        server_http3 = std::make_unique<Http3Connection>(
            true, local_cid, peer_cid, settings
        );
    }
};

TEST_F(Http3ConnectionTest, Initialization) {
    EXPECT_EQ(server_http3->initialize(), 0);
    EXPECT_EQ(server_http3->state(), Http3ConnectionState::HANDSHAKE);
    EXPECT_FALSE(server_http3->is_closed());
}

TEST_F(Http3ConnectionTest, Settings) {
    server_http3->initialize();

    auto& settings = server_http3->settings();
    EXPECT_EQ(settings.max_concurrent_streams, 100u);
    EXPECT_EQ(settings.qpack_max_table_capacity, 4096u);
}

TEST_F(Http3ConnectionTest, RequestCallback) {
    server_http3->initialize();

    bool callback_called = false;
    std::string received_method;
    std::string received_path;

    server_http3->set_request_callback([&](
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body,
        auto send_response
    ) {
        callback_called = true;
        received_method = method;
        received_path = path;

        // Send response
        send_response(200, {{"content-type", "text/plain"}}, "Hello");
    });

    // Verify callback is set (actual invocation requires full packet flow)
    EXPECT_FALSE(callback_called);  // Not called yet
}

TEST_F(Http3ConnectionTest, ConnectionClose) {
    server_http3->initialize();
    server_http3->close(0, "graceful");

    EXPECT_TRUE(server_http3->is_closed());
    EXPECT_EQ(server_http3->state(), Http3ConnectionState::CLOSED);
}

TEST_F(Http3ConnectionTest, StreamCount) {
    server_http3->initialize();

    EXPECT_EQ(server_http3->stream_count(), 0u);
}

// =============================================================================
// QPACK Integration Tests
// =============================================================================

class QPACKIntegrationTest : public ::testing::Test {
protected:
    QPACKEncoder encoder{4096, 100};
    QPACKDecoder decoder{4096};
};

TEST_F(QPACKIntegrationTest, EncodeDecodeHeaders) {
    // Encode headers
    std::vector<std::pair<std::string_view, std::string_view>> headers = {
        {":status", "200"},
        {"content-type", "application/json"},
        {"content-length", "123"},
    };

    uint8_t encoded[1024];
    size_t encoded_len;

    int result = encoder.encode_field_section(
        headers.data(), headers.size(),
        encoded, sizeof(encoded), encoded_len
    );

    EXPECT_EQ(result, 0);
    EXPECT_GT(encoded_len, 0u);

    // Decode headers
    std::pair<std::string, std::string> decoded_headers[256];
    size_t decoded_count;

    result = decoder.decode_field_section(
        encoded, encoded_len,
        decoded_headers, decoded_count
    );

    EXPECT_EQ(result, 0);
    EXPECT_EQ(decoded_count, headers.size());

    // Verify decoded values
    EXPECT_EQ(decoded_headers[0].first, ":status");
    EXPECT_EQ(decoded_headers[0].second, "200");
    EXPECT_EQ(decoded_headers[1].first, "content-type");
    EXPECT_EQ(decoded_headers[1].second, "application/json");
}

TEST_F(QPACKIntegrationTest, EncodeRequestHeaders) {
    std::vector<std::pair<std::string_view, std::string_view>> headers = {
        {":method", "GET"},
        {":scheme", "https"},
        {":authority", "example.com"},
        {":path", "/api/users"},
        {"user-agent", "FasterAPI/1.0"},
    };

    uint8_t encoded[1024];
    size_t encoded_len;

    int result = encoder.encode_field_section(
        headers.data(), headers.size(),
        encoded, sizeof(encoded), encoded_len
    );

    EXPECT_EQ(result, 0);
    EXPECT_GT(encoded_len, 0u);

    // Decode and verify
    std::pair<std::string, std::string> decoded[256];
    size_t decoded_count;

    result = decoder.decode_field_section(encoded, encoded_len, decoded, decoded_count);

    EXPECT_EQ(result, 0);
    EXPECT_EQ(decoded_count, 5u);
    EXPECT_EQ(decoded[0].second, "GET");
    EXPECT_EQ(decoded[3].second, "/api/users");
}

TEST_F(QPACKIntegrationTest, LargeHeaderValue) {
    std::string large_value = random_string(4096);

    std::vector<std::pair<std::string_view, std::string_view>> headers = {
        {":status", "200"},
        {"x-large-header", large_value},
    };

    uint8_t encoded[8192];
    size_t encoded_len;

    int result = encoder.encode_field_section(
        headers.data(), headers.size(),
        encoded, sizeof(encoded), encoded_len
    );

    EXPECT_EQ(result, 0);

    // Decode and verify
    std::pair<std::string, std::string> decoded[256];
    size_t decoded_count;

    result = decoder.decode_field_section(encoded, encoded_len, decoded, decoded_count);

    EXPECT_EQ(result, 0);
    EXPECT_EQ(decoded[1].second, large_value);
}

// =============================================================================
// VarInt Encoding Tests (Foundation for HTTP/3)
// =============================================================================

TEST(VarIntTest, EncodeDecode) {
    std::vector<uint64_t> test_values = {
        0, 1, 63,           // 1-byte values
        64, 16383,          // 2-byte values
        16384, 1073741823,  // 4-byte values
        1073741824, 4611686018427387903ULL  // 8-byte values
    };

    for (uint64_t val : test_values) {
        uint8_t buffer[8];
        size_t encoded_len = VarInt::encode(val, buffer);

        uint64_t decoded;
        int decoded_len = VarInt::decode(buffer, encoded_len, decoded);

        EXPECT_EQ(decoded_len, static_cast<int>(encoded_len))
            << "For value " << val;
        EXPECT_EQ(decoded, val) << "For value " << val;
    }
}

// =============================================================================
// HTTP/3 Frame Tests
// =============================================================================

TEST(Http3FrameTest, StreamFrameRoundtrip) {
    StreamFrame original;
    original.stream_id = 4;
    original.offset = 1000;
    original.length = 100;
    original.fin = true;

    uint8_t data[100];
    std::memset(data, 'A', 100);
    original.data = data;

    // Serialize
    uint8_t buffer[256];
    size_t serialized_len = original.serialize(buffer);

    EXPECT_GT(serialized_len, 0u);

    // Parse - serialize() writes type byte at position 0, parse() expects it consumed
    StreamFrame parsed;
    size_t consumed;
    // First byte of serialized buffer contains the frame type with flags
    uint8_t frame_type = buffer[0];
    // Pass buffer+1 since parse() expects data after type byte
    int result = parsed.parse(frame_type, buffer + 1, serialized_len - 1, consumed);

    EXPECT_EQ(result, 0);
    EXPECT_EQ(parsed.stream_id, original.stream_id);
    EXPECT_EQ(parsed.offset, original.offset);
    EXPECT_EQ(parsed.length, original.length);
    EXPECT_EQ(parsed.fin, original.fin);
}

TEST(Http3FrameTest, AckFrameRoundtrip) {
    AckFrame original;
    original.largest_acked = 100;
    original.ack_delay = 1000;
    original.first_ack_range = 5;
    original.range_count = 0;

    // Serialize
    uint8_t buffer[256];
    size_t serialized_len = original.serialize(buffer);

    EXPECT_GT(serialized_len, 0u);

    // Parse - serialize() writes type byte at position 0, parse() expects it consumed
    AckFrame parsed;
    size_t consumed;
    // Skip the frame type byte (0x02) since parse expects it already consumed
    int result = parsed.parse(buffer + 1, serialized_len - 1, consumed);

    EXPECT_EQ(result, 0);
    EXPECT_EQ(parsed.largest_acked, original.largest_acked);
    EXPECT_EQ(parsed.ack_delay, original.ack_delay);
    EXPECT_EQ(parsed.first_ack_range, original.first_ack_range);
}

// =============================================================================
// Buffer Pool Tests (Zero-allocation)
// =============================================================================

TEST(Http3BufferPoolTest, AcquireRelease) {
    Http3BufferPool<1024, 4> pool;

    // Acquire all buffers
    std::vector<uint8_t*> buffers;
    for (int i = 0; i < 4; ++i) {
        uint8_t* buf = pool.acquire();
        ASSERT_NE(buf, nullptr) << "Buffer " << i << " should be available";
        buffers.push_back(buf);
    }

    // Pool should be exhausted
    EXPECT_EQ(pool.acquire(), nullptr);

    // Release one
    pool.release(buffers[0]);

    // Now can acquire again
    uint8_t* new_buf = pool.acquire();
    EXPECT_NE(new_buf, nullptr);
    EXPECT_EQ(new_buf, buffers[0]);  // Should be same buffer
}

TEST(Http3BufferPoolTest, BufferSize) {
    Http3BufferPool<8192, 2> pool;

    EXPECT_EQ(pool.buffer_size(), 8192u);
}

// =============================================================================
// Performance Tests
// =============================================================================

TEST(Http3PerformanceTest, VarIntEncodingSpeed) {
    const int iterations = 1000000;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> dis(0, 1000000);

    uint8_t buffer[8];
    uint64_t checksum = 0;

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < iterations; ++i) {
        uint64_t val = dis(gen);
        VarInt::encode(val, buffer);
        uint64_t decoded;
        VarInt::decode(buffer, 8, decoded);
        checksum += decoded;
    }

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double ops_per_sec = (iterations * 2.0 * 1e6) / duration.count();

    std::cout << "VarInt encode+decode: " << ops_per_sec << " ops/sec" << std::endl;

    EXPECT_GT(ops_per_sec, 10000000.0);  // > 10M ops/sec
    (void)checksum;  // Prevent optimization
}

TEST(Http3PerformanceTest, QPACKEncodingSpeed) {
    QPACKEncoder encoder{4096, 100};

    std::vector<std::pair<std::string_view, std::string_view>> headers = {
        {":status", "200"},
        {"content-type", "application/json"},
        {"content-length", "1234"},
        {"server", "FasterAPI/1.0"},
        {"x-custom-header", "custom-value"},
    };

    uint8_t buffer[1024];
    size_t encoded_len;

    const int iterations = 100000;

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < iterations; ++i) {
        encoder.encode_field_section(
            headers.data(), headers.size(),
            buffer, sizeof(buffer), encoded_len
        );
    }

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double encodes_per_sec = (iterations * 1e6) / duration.count();

    std::cout << "QPACK encoding: " << encodes_per_sec << " encodes/sec" << std::endl;

    EXPECT_GT(encodes_per_sec, 100000.0);  // > 100K encodes/sec
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
