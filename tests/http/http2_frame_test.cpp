/**
 * FasterAPI HTTP/2 Frame Tests
 *
 * Comprehensive Google Test suite for HTTP/2 frame parsing and serialization.
 * Tests based on RFC 7540 (HTTP/2 Specification).
 */

#include <gtest/gtest.h>
#include "../test_utils.h"
#include "../../src/cpp/http/http2_frame.h"

#include <cstring>
#include <random>
#include <chrono>

using namespace fasterapi::http2;
using namespace fasterapi::testing;
using namespace fasterapi::core;

// =============================================================================
// HTTP/2 Frame Test Fixture
// =============================================================================

class HTTP2FrameTest : public FasterAPITest {
protected:
    RandomGenerator rng_;

    // Build a frame header manually
    void build_header(uint8_t* out, uint32_t length, FrameType type,
                      uint8_t flags, uint32_t stream_id) {
        // Length (24-bit big-endian)
        out[0] = (length >> 16) & 0xFF;
        out[1] = (length >> 8) & 0xFF;
        out[2] = length & 0xFF;
        // Type
        out[3] = static_cast<uint8_t>(type);
        // Flags
        out[4] = flags;
        // Stream ID (31-bit big-endian, R bit = 0)
        out[5] = (stream_id >> 24) & 0x7F;  // Mask R bit
        out[6] = (stream_id >> 16) & 0xFF;
        out[7] = (stream_id >> 8) & 0xFF;
        out[8] = stream_id & 0xFF;
    }

    // Generate random stream ID (31-bit)
    uint32_t random_stream_id() {
        return static_cast<uint32_t>(rng_.random_size(0, 0x7FFFFFFF));
    }

    // Generate random uint64_t
    uint64_t random_u64() {
        uint64_t high = static_cast<uint64_t>(rng_.random_int(0, INT32_MAX));
        uint64_t low = static_cast<uint64_t>(rng_.random_int(0, INT32_MAX));
        return (high << 32) | low;
    }
};

// =============================================================================
// Frame Header Parsing Tests
// =============================================================================

TEST_F(HTTP2FrameTest, ParseFrameHeader) {
    uint8_t data[9];
    build_header(data, 100, FrameType::DATA, FrameFlags::DATA_END_STREAM, 1);

    auto result = parse_frame_header(data);
    ASSERT_TRUE(result.is_ok());

    auto header = result.value();
    EXPECT_EQ(header.length, 100);
    EXPECT_EQ(header.type, FrameType::DATA);
    EXPECT_EQ(header.flags, FrameFlags::DATA_END_STREAM);
    EXPECT_EQ(header.stream_id, 1);
}

TEST_F(HTTP2FrameTest, ParseFrameHeaderAllTypes) {
    std::vector<FrameType> types = {
        FrameType::DATA,
        FrameType::HEADERS,
        FrameType::PRIORITY,
        FrameType::RST_STREAM,
        FrameType::SETTINGS,
        FrameType::PUSH_PROMISE,
        FrameType::PING,
        FrameType::GOAWAY,
        FrameType::WINDOW_UPDATE,
        FrameType::CONTINUATION
    };

    for (auto type : types) {
        uint8_t data[9];
        build_header(data, 50, type, 0, 1);

        auto result = parse_frame_header(data);
        ASSERT_TRUE(result.is_ok()) << "Failed for type: " << static_cast<int>(type);
        EXPECT_EQ(result.value().type, type);
    }
}

TEST_F(HTTP2FrameTest, ParseFrameHeaderMaxLength) {
    uint8_t data[9];
    // Maximum length is 2^24 - 1 = 16777215
    build_header(data, 16777215, FrameType::DATA, 0, 1);

    auto result = parse_frame_header(data);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().length, 16777215);
}

TEST_F(HTTP2FrameTest, ParseFrameHeaderMaxStreamId) {
    uint8_t data[9];
    // Maximum stream ID is 2^31 - 1
    build_header(data, 0, FrameType::HEADERS, 0, 0x7FFFFFFF);

    auto result = parse_frame_header(data);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().stream_id, 0x7FFFFFFF);
}

TEST_F(HTTP2FrameTest, ParseRandomFrameHeaders) {
    constexpr int NUM_TESTS = 100;

    for (int i = 0; i < NUM_TESTS; ++i) {
        uint32_t length = static_cast<uint32_t>(rng_.random_size(0, (1 << 24) - 1));  // 24-bit
        auto type = static_cast<FrameType>(rng_.random_int(0, 9));  // 0-9
        uint8_t flags = static_cast<uint8_t>(rng_.random_int(0, 255));
        uint32_t stream_id = random_stream_id();

        uint8_t data[9];
        build_header(data, length, type, flags, stream_id);

        auto result = parse_frame_header(data);
        ASSERT_TRUE(result.is_ok());

        auto header = result.value();
        EXPECT_EQ(header.length, length);
        EXPECT_EQ(header.type, type);
        EXPECT_EQ(header.flags, flags);
        EXPECT_EQ(header.stream_id, stream_id);
    }
}

// =============================================================================
// Frame Header Serialization Tests
// =============================================================================

TEST_F(HTTP2FrameTest, WriteFrameHeader) {
    FrameHeader header{100, FrameType::DATA, FrameFlags::DATA_END_STREAM, 1};

    uint8_t out[9];
    write_frame_header(header, out);

    // Verify by parsing back
    auto parsed = parse_frame_header(out);
    ASSERT_TRUE(parsed.is_ok());
    EXPECT_EQ(parsed.value().length, 100);
    EXPECT_EQ(parsed.value().type, FrameType::DATA);
    EXPECT_EQ(parsed.value().flags, FrameFlags::DATA_END_STREAM);
    EXPECT_EQ(parsed.value().stream_id, 1);
}

TEST_F(HTTP2FrameTest, WriteFrameHeaderRoundTrip) {
    constexpr int NUM_TESTS = 100;

    for (int i = 0; i < NUM_TESTS; ++i) {
        FrameHeader original{
            static_cast<uint32_t>(rng_.random_size(0, (1 << 24) - 1)),
            static_cast<FrameType>(rng_.random_int(0, 9)),
            static_cast<uint8_t>(rng_.random_int(0, 255)),
            random_stream_id()
        };

        uint8_t buffer[9];
        write_frame_header(original, buffer);

        auto parsed = parse_frame_header(buffer);
        ASSERT_TRUE(parsed.is_ok());

        auto result = parsed.value();
        EXPECT_EQ(result.length, original.length);
        EXPECT_EQ(result.type, original.type);
        EXPECT_EQ(result.flags, original.flags);
        EXPECT_EQ(result.stream_id, original.stream_id);
    }
}

// =============================================================================
// DATA Frame Tests
// =============================================================================

TEST_F(HTTP2FrameTest, WriteDataFrame) {
    std::string data = "Hello, HTTP/2!";
    auto frame = write_data_frame(1, data, false);

    // Parse header
    auto header = parse_frame_header(frame.data());
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header.value().type, FrameType::DATA);
    EXPECT_EQ(header.value().length, data.size());
    EXPECT_EQ(header.value().stream_id, 1);
    EXPECT_EQ(header.value().flags & FrameFlags::DATA_END_STREAM, 0);
}

TEST_F(HTTP2FrameTest, WriteDataFrameEndStream) {
    std::string data = "Final data";
    auto frame = write_data_frame(5, data, true);

    auto header = parse_frame_header(frame.data());
    ASSERT_TRUE(header.is_ok());
    EXPECT_NE(header.value().flags & FrameFlags::DATA_END_STREAM, 0);
}

TEST_F(HTTP2FrameTest, ParseDataFrame) {
    std::string original_data = "Test payload";
    auto frame = write_data_frame(1, original_data, false);

    auto header_result = parse_frame_header(frame.data());
    ASSERT_TRUE(header_result.is_ok());

    auto data_result = parse_data_frame(
        header_result.value(),
        frame.data() + 9,
        frame.size() - 9
    );

    ASSERT_TRUE(data_result.is_ok());
    EXPECT_EQ(data_result.value(), original_data);
}

// =============================================================================
// SETTINGS Frame Tests
// =============================================================================

TEST_F(HTTP2FrameTest, WriteSettingsFrame) {
    std::vector<SettingsParameter> settings = {
        {SettingsId::MAX_CONCURRENT_STREAMS, 100},
        {SettingsId::INITIAL_WINDOW_SIZE, 65535},
        {SettingsId::MAX_FRAME_SIZE, 16384}
    };

    auto frame = write_settings_frame(settings, false);

    auto header = parse_frame_header(frame.data());
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header.value().type, FrameType::SETTINGS);
    EXPECT_EQ(header.value().length, settings.size() * 6);  // 6 bytes per setting
    EXPECT_EQ(header.value().stream_id, 0);  // SETTINGS must be stream 0
    EXPECT_EQ(header.value().flags & FrameFlags::SETTINGS_ACK, 0);
}

TEST_F(HTTP2FrameTest, WriteSettingsAck) {
    auto frame = write_settings_ack();

    auto header = parse_frame_header(frame.data());
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header.value().type, FrameType::SETTINGS);
    EXPECT_EQ(header.value().length, 0);  // ACK has no payload
    EXPECT_NE(header.value().flags & FrameFlags::SETTINGS_ACK, 0);
}

TEST_F(HTTP2FrameTest, ParseSettingsFrame) {
    std::vector<SettingsParameter> original = {
        {SettingsId::HEADER_TABLE_SIZE, 4096},
        {SettingsId::ENABLE_PUSH, 1},
        {SettingsId::MAX_CONCURRENT_STREAMS, 200}
    };

    auto frame = write_settings_frame(original, false);

    auto header_result = parse_frame_header(frame.data());
    ASSERT_TRUE(header_result.is_ok());

    auto settings_result = parse_settings_frame(
        header_result.value(),
        frame.data() + 9,
        frame.size() - 9
    );

    ASSERT_TRUE(settings_result.is_ok());
    auto& settings = settings_result.value();

    EXPECT_EQ(settings.size(), original.size());
    for (size_t i = 0; i < settings.size(); ++i) {
        EXPECT_EQ(settings[i].id, original[i].id);
        EXPECT_EQ(settings[i].value, original[i].value);
    }
}

// =============================================================================
// HEADERS Frame Tests
// =============================================================================

TEST_F(HTTP2FrameTest, WriteHeadersFrame) {
    std::vector<uint8_t> header_block = {0x82, 0x86, 0x84};  // Sample HPACK
    auto frame = write_headers_frame(1, header_block, false, true, nullptr);

    auto header = parse_frame_header(frame.data());
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header.value().type, FrameType::HEADERS);
    EXPECT_EQ(header.value().stream_id, 1);
    EXPECT_NE(header.value().flags & FrameFlags::HEADERS_END_HEADERS, 0);
    EXPECT_EQ(header.value().flags & FrameFlags::HEADERS_END_STREAM, 0);
}

TEST_F(HTTP2FrameTest, WriteHeadersFrameEndStream) {
    std::vector<uint8_t> header_block = {0x82};
    auto frame = write_headers_frame(3, header_block, true, true, nullptr);

    auto header = parse_frame_header(frame.data());
    ASSERT_TRUE(header.is_ok());
    EXPECT_NE(header.value().flags & FrameFlags::HEADERS_END_STREAM, 0);
    EXPECT_NE(header.value().flags & FrameFlags::HEADERS_END_HEADERS, 0);
}

TEST_F(HTTP2FrameTest, WriteHeadersFrameWithPriority) {
    std::vector<uint8_t> header_block = {0x82, 0x86};
    PrioritySpec priority;
    priority.exclusive = false;
    priority.stream_dependency = 0;
    priority.weight = 32;
    auto frame = write_headers_frame(5, header_block, false, true, &priority);

    auto header = parse_frame_header(frame.data());
    ASSERT_TRUE(header.is_ok());
    EXPECT_NE(header.value().flags & FrameFlags::HEADERS_PRIORITY, 0);
}

TEST_F(HTTP2FrameTest, ParseHeadersFrame) {
    std::vector<uint8_t> original_block = {0x82, 0x86, 0x84, 0x41, 0x8a};
    auto frame = write_headers_frame(1, original_block, false, true, nullptr);

    auto header_result = parse_frame_header(frame.data());
    ASSERT_TRUE(header_result.is_ok());

    PrioritySpec priority;
    std::vector<uint8_t> parsed_block;

    auto result = parse_headers_frame(
        header_result.value(),
        frame.data() + 9,
        frame.size() - 9,
        &priority,
        parsed_block
    );

    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(parsed_block, original_block);
}

// =============================================================================
// WINDOW_UPDATE Frame Tests
// =============================================================================

TEST_F(HTTP2FrameTest, WriteWindowUpdateFrame) {
    auto frame = write_window_update_frame(0, 65535);

    auto header = parse_frame_header(frame.data());
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header.value().type, FrameType::WINDOW_UPDATE);
    EXPECT_EQ(header.value().length, 4);
    EXPECT_EQ(header.value().stream_id, 0);
}

TEST_F(HTTP2FrameTest, ParseWindowUpdateFrame) {
    uint32_t increment = 32768;
    auto frame = write_window_update_frame(1, increment);

    // Extract payload (skip 9-byte header)
    auto result = parse_window_update_frame(frame.data() + 9);

    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), increment);
}

TEST_F(HTTP2FrameTest, WindowUpdateMaxValue) {
    uint32_t max_increment = 0x7FFFFFFF;  // Maximum window increment
    auto frame = write_window_update_frame(0, max_increment);

    auto result = parse_window_update_frame(frame.data() + 9);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), max_increment);
}

// =============================================================================
// PING Frame Tests
// =============================================================================

TEST_F(HTTP2FrameTest, WritePingFrame) {
    uint64_t opaque_data = 0x1234567890ABCDEF;
    auto frame = write_ping_frame(opaque_data, false);

    auto header = parse_frame_header(frame.data());
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header.value().type, FrameType::PING);
    EXPECT_EQ(header.value().length, 8);
    EXPECT_EQ(header.value().stream_id, 0);  // PING must be stream 0
    EXPECT_EQ(header.value().flags & FrameFlags::PING_ACK, 0);
}

TEST_F(HTTP2FrameTest, WritePingAck) {
    uint64_t opaque_data = 0xDEADBEEFCAFEBABE;
    auto frame = write_ping_frame(opaque_data, true);

    auto header = parse_frame_header(frame.data());
    ASSERT_TRUE(header.is_ok());
    EXPECT_NE(header.value().flags & FrameFlags::PING_ACK, 0);
}

TEST_F(HTTP2FrameTest, ParsePingFrame) {
    uint64_t original = 0x0102030405060708;
    auto frame = write_ping_frame(original, false);

    auto result = parse_ping_frame(frame.data() + 9);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), original);
}

TEST_F(HTTP2FrameTest, PingRoundTrip) {
    constexpr int NUM_TESTS = 50;

    for (int i = 0; i < NUM_TESTS; ++i) {
        uint64_t original = random_u64();
        auto frame = write_ping_frame(original, false);
        auto parsed = parse_ping_frame(frame.data() + 9);

        ASSERT_TRUE(parsed.is_ok());
        EXPECT_EQ(parsed.value(), original);
    }
}

// =============================================================================
// GOAWAY Frame Tests
// =============================================================================

TEST_F(HTTP2FrameTest, WriteGoawayFrame) {
    auto frame = write_goaway_frame(100, ErrorCode::NO_ERROR, "");

    auto header = parse_frame_header(frame.data());
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header.value().type, FrameType::GOAWAY);
    EXPECT_EQ(header.value().stream_id, 0);  // GOAWAY must be stream 0
    EXPECT_GE(header.value().length, 8);  // At least 8 bytes
}

TEST_F(HTTP2FrameTest, WriteGoawayFrameWithDebug) {
    std::string debug = "Connection timeout";
    auto frame = write_goaway_frame(50, ErrorCode::INTERNAL_ERROR, debug);

    auto header = parse_frame_header(frame.data());
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header.value().length, 8 + debug.size());
}

TEST_F(HTTP2FrameTest, ParseGoawayFrame) {
    uint32_t last_stream = 42;
    ErrorCode error = ErrorCode::PROTOCOL_ERROR;
    std::string debug = "Protocol violation";

    auto frame = write_goaway_frame(last_stream, error, debug);

    uint32_t parsed_stream;
    ErrorCode parsed_error;
    std::string parsed_debug;

    auto result = parse_goaway_frame(
        frame.data() + 9,
        frame.size() - 9,
        parsed_stream,
        parsed_error,
        parsed_debug
    );

    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(parsed_stream, last_stream);
    EXPECT_EQ(parsed_error, error);
    EXPECT_EQ(parsed_debug, debug);
}

// =============================================================================
// RST_STREAM Frame Tests
// =============================================================================

TEST_F(HTTP2FrameTest, WriteRstStreamFrame) {
    auto frame = write_rst_stream_frame(5, ErrorCode::CANCEL);

    auto header = parse_frame_header(frame.data());
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header.value().type, FrameType::RST_STREAM);
    EXPECT_EQ(header.value().length, 4);
    EXPECT_EQ(header.value().stream_id, 5);
}

TEST_F(HTTP2FrameTest, ParseRstStreamFrame) {
    auto frame = write_rst_stream_frame(7, ErrorCode::FLOW_CONTROL_ERROR);

    auto result = parse_rst_stream_frame(frame.data() + 9);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), ErrorCode::FLOW_CONTROL_ERROR);
}

TEST_F(HTTP2FrameTest, AllErrorCodes) {
    std::vector<ErrorCode> codes = {
        ErrorCode::NO_ERROR,
        ErrorCode::PROTOCOL_ERROR,
        ErrorCode::INTERNAL_ERROR,
        ErrorCode::FLOW_CONTROL_ERROR,
        ErrorCode::SETTINGS_TIMEOUT,
        ErrorCode::STREAM_CLOSED,
        ErrorCode::FRAME_SIZE_ERROR,
        ErrorCode::REFUSED_STREAM,
        ErrorCode::CANCEL,
        ErrorCode::COMPRESSION_ERROR,
        ErrorCode::CONNECT_ERROR,
        ErrorCode::ENHANCE_YOUR_CALM,
        ErrorCode::INADEQUATE_SECURITY,
        ErrorCode::HTTP_1_1_REQUIRED
    };

    for (auto code : codes) {
        auto frame = write_rst_stream_frame(1, code);
        auto result = parse_rst_stream_frame(frame.data() + 9);

        ASSERT_TRUE(result.is_ok()) << "Failed for error code: " << static_cast<int>(code);
        EXPECT_EQ(result.value(), code);
    }
}

// =============================================================================
// PRIORITY Frame Tests
// =============================================================================

TEST_F(HTTP2FrameTest, ParsePriorityFrame) {
    // Build a priority frame payload manually
    // Format: E (1 bit) | Stream Dependency (31 bits) | Weight (8 bits)
    uint8_t payload[5];
    uint32_t dependency = 3;  // Non-exclusive
    payload[0] = (dependency >> 24) & 0x7F;
    payload[1] = (dependency >> 16) & 0xFF;
    payload[2] = (dependency >> 8) & 0xFF;
    payload[3] = dependency & 0xFF;
    payload[4] = 15;  // Weight (0-255, represents 1-256)

    auto result = parse_priority_frame(payload);
    ASSERT_TRUE(result.is_ok());
    EXPECT_FALSE(result.value().exclusive);
    EXPECT_EQ(result.value().stream_dependency, 3);
    EXPECT_EQ(result.value().weight, 15);
}

TEST_F(HTTP2FrameTest, ParsePriorityFrameExclusive) {
    uint8_t payload[5];
    uint32_t dependency = 5;
    payload[0] = 0x80 | ((dependency >> 24) & 0x7F);  // E bit set
    payload[1] = (dependency >> 16) & 0xFF;
    payload[2] = (dependency >> 8) & 0xFF;
    payload[3] = dependency & 0xFF;
    payload[4] = 255;

    auto result = parse_priority_frame(payload);
    ASSERT_TRUE(result.is_ok());
    EXPECT_TRUE(result.value().exclusive);
    EXPECT_EQ(result.value().stream_dependency, 5);
    EXPECT_EQ(result.value().weight, 255);
}

// =============================================================================
// Connection Preface Tests
// =============================================================================

TEST_F(HTTP2FrameTest, ConnectionPreface) {
    EXPECT_EQ(std::string(CONNECTION_PREFACE), "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");
    EXPECT_EQ(CONNECTION_PREFACE_LEN, 24);
}

// =============================================================================
// Frame Flags Tests
// =============================================================================

TEST_F(HTTP2FrameTest, DataFrameFlags) {
    EXPECT_EQ(FrameFlags::DATA_END_STREAM, 0x1);
    EXPECT_EQ(FrameFlags::DATA_PADDED, 0x8);
}

TEST_F(HTTP2FrameTest, HeadersFrameFlags) {
    EXPECT_EQ(FrameFlags::HEADERS_END_STREAM, 0x1);
    EXPECT_EQ(FrameFlags::HEADERS_END_HEADERS, 0x4);
    EXPECT_EQ(FrameFlags::HEADERS_PADDED, 0x8);
    EXPECT_EQ(FrameFlags::HEADERS_PRIORITY, 0x20);
}

TEST_F(HTTP2FrameTest, SettingsFrameFlags) {
    EXPECT_EQ(FrameFlags::SETTINGS_ACK, 0x1);
}

TEST_F(HTTP2FrameTest, PingFrameFlags) {
    EXPECT_EQ(FrameFlags::PING_ACK, 0x1);
}

// =============================================================================
// PrioritySpec Tests
// =============================================================================

TEST_F(HTTP2FrameTest, PrioritySpecDefaults) {
    PrioritySpec spec;
    EXPECT_FALSE(spec.exclusive);
    EXPECT_EQ(spec.stream_dependency, 0);
    EXPECT_EQ(spec.weight, 16);  // Default weight
}

// =============================================================================
// FrameHeader Tests
// =============================================================================

TEST_F(HTTP2FrameTest, FrameHeaderDefaults) {
    FrameHeader header;
    EXPECT_EQ(header.length, 0);
    EXPECT_EQ(header.type, FrameType::DATA);
    EXPECT_EQ(header.flags, 0);
    EXPECT_EQ(header.stream_id, 0);
}

TEST_F(HTTP2FrameTest, FrameHeaderConstructor) {
    FrameHeader header{100, FrameType::HEADERS, 0x05, 7};
    EXPECT_EQ(header.length, 100);
    EXPECT_EQ(header.type, FrameType::HEADERS);
    EXPECT_EQ(header.flags, 0x05);
    EXPECT_EQ(header.stream_id, 7);
}

// =============================================================================
// Performance Tests
// =============================================================================

TEST_F(HTTP2FrameTest, FrameHeaderParsePerformance) {
    uint8_t data[9];
    build_header(data, 1000, FrameType::DATA, 0, 1);

    constexpr int ITERATIONS = 100000;

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < ITERATIONS; ++i) {
        auto result = parse_frame_header(data);
        (void)result;
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ns_per_parse = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / ITERATIONS;

    std::cout << "Frame header parse: " << ns_per_parse << " ns/parse" << std::endl;

    // Should be well under 1us per parse
    EXPECT_LT(ns_per_parse, 1000);
}

TEST_F(HTTP2FrameTest, FrameHeaderWritePerformance) {
    FrameHeader header{1000, FrameType::DATA, 0, 1};
    uint8_t buffer[9];

    constexpr int ITERATIONS = 100000;

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < ITERATIONS; ++i) {
        write_frame_header(header, buffer);
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ns_per_write = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / ITERATIONS;

    std::cout << "Frame header write: " << ns_per_write << " ns/write" << std::endl;

    // Should be well under 1us per write
    EXPECT_LT(ns_per_write, 1000);
}

TEST_F(HTTP2FrameTest, DataFrameRoundTripPerformance) {
    std::string data(1000, 'x');

    constexpr int ITERATIONS = 10000;

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < ITERATIONS; ++i) {
        auto frame = write_data_frame(1, data, false);
        auto header = parse_frame_header(frame.data());
        auto parsed = parse_data_frame(header.value(), frame.data() + 9, frame.size() - 9);
        (void)parsed;
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ns_per_roundtrip = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / ITERATIONS;

    std::cout << "DATA frame roundtrip: " << ns_per_roundtrip << " ns/roundtrip" << std::endl;

    // Should be under 50us per roundtrip (generous for CI environments)
    EXPECT_LT(ns_per_roundtrip, 50000);
}

// =============================================================================
// Zero-Allocation Frame Serialization Tests (_to variants)
// =============================================================================

TEST_F(HTTP2FrameTest, WriteDataFrameTo) {
    uint8_t buffer[1024];
    const char* data = "Hello, HTTP/2!";
    size_t data_len = strlen(data);

    size_t written = write_data_frame_to(buffer, sizeof(buffer), 1,
        reinterpret_cast<const uint8_t*>(data), data_len, false);

    EXPECT_EQ(written, 9 + data_len);

    // Verify by parsing
    auto header = parse_frame_header(buffer);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header.value().type, FrameType::DATA);
    EXPECT_EQ(header.value().length, data_len);
    EXPECT_EQ(header.value().stream_id, 1);
    EXPECT_EQ(header.value().flags & FrameFlags::DATA_END_STREAM, 0);

    // Verify data content
    EXPECT_EQ(memcmp(buffer + 9, data, data_len), 0);
}

TEST_F(HTTP2FrameTest, WriteDataFrameToEndStream) {
    uint8_t buffer[128];
    const char* data = "Final";

    size_t written = write_data_frame_to(buffer, sizeof(buffer), 5,
        reinterpret_cast<const uint8_t*>(data), strlen(data), true);

    EXPECT_GT(written, 0);

    auto header = parse_frame_header(buffer);
    ASSERT_TRUE(header.is_ok());
    EXPECT_NE(header.value().flags & FrameFlags::DATA_END_STREAM, 0);
}

TEST_F(HTTP2FrameTest, WriteDataFrameToInsufficientCapacity) {
    uint8_t buffer[5];  // Too small for even the header
    const char* data = "Test";

    size_t written = write_data_frame_to(buffer, sizeof(buffer), 1,
        reinterpret_cast<const uint8_t*>(data), strlen(data), false);

    EXPECT_EQ(written, 0);  // Should fail
}

TEST_F(HTTP2FrameTest, WriteHeadersFrameTo) {
    uint8_t buffer[1024];
    std::vector<uint8_t> header_block = {0x82, 0x86, 0x84};

    size_t written = write_headers_frame_to(buffer, sizeof(buffer), 1,
        header_block.data(), header_block.size(), false, true, nullptr);

    EXPECT_EQ(written, 9 + header_block.size());

    auto header = parse_frame_header(buffer);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header.value().type, FrameType::HEADERS);
    EXPECT_EQ(header.value().stream_id, 1);
    EXPECT_NE(header.value().flags & FrameFlags::HEADERS_END_HEADERS, 0);
    EXPECT_EQ(header.value().flags & FrameFlags::HEADERS_END_STREAM, 0);
}

TEST_F(HTTP2FrameTest, WriteHeadersFrameToWithPriority) {
    uint8_t buffer[1024];
    std::vector<uint8_t> header_block = {0x82, 0x86};
    PrioritySpec priority;
    priority.exclusive = true;
    priority.stream_dependency = 3;
    priority.weight = 64;

    size_t written = write_headers_frame_to(buffer, sizeof(buffer), 5,
        header_block.data(), header_block.size(), true, true, &priority);

    EXPECT_EQ(written, 9 + 5 + header_block.size());  // header + priority + block

    auto header = parse_frame_header(buffer);
    ASSERT_TRUE(header.is_ok());
    EXPECT_NE(header.value().flags & FrameFlags::HEADERS_PRIORITY, 0);
    EXPECT_NE(header.value().flags & FrameFlags::HEADERS_END_STREAM, 0);
    EXPECT_NE(header.value().flags & FrameFlags::HEADERS_END_HEADERS, 0);
}

TEST_F(HTTP2FrameTest, WriteSettingsFrameTo) {
    uint8_t buffer[128];
    SettingsParameter params[] = {
        {SettingsId::MAX_CONCURRENT_STREAMS, 100},
        {SettingsId::INITIAL_WINDOW_SIZE, 65535}
    };

    size_t written = write_settings_frame_to(buffer, sizeof(buffer), params, 2, false);

    EXPECT_EQ(written, 9 + 12);  // header + 2 params * 6 bytes

    auto header = parse_frame_header(buffer);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header.value().type, FrameType::SETTINGS);
    EXPECT_EQ(header.value().length, 12);
    EXPECT_EQ(header.value().stream_id, 0);
}

TEST_F(HTTP2FrameTest, WriteSettingsAckTo) {
    uint8_t buffer[16];

    size_t written = write_settings_ack_to(buffer, sizeof(buffer));

    EXPECT_EQ(written, 9);  // Header only

    auto header = parse_frame_header(buffer);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header.value().type, FrameType::SETTINGS);
    EXPECT_EQ(header.value().length, 0);
    EXPECT_NE(header.value().flags & FrameFlags::SETTINGS_ACK, 0);
}

TEST_F(HTTP2FrameTest, WriteWindowUpdateFrameTo) {
    uint8_t buffer[16];

    size_t written = write_window_update_frame_to(buffer, sizeof(buffer), 1, 65535);

    EXPECT_EQ(written, 13);  // 9 header + 4 payload

    auto header = parse_frame_header(buffer);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header.value().type, FrameType::WINDOW_UPDATE);
    EXPECT_EQ(header.value().stream_id, 1);

    auto increment = parse_window_update_frame(buffer + 9);
    ASSERT_TRUE(increment.is_ok());
    EXPECT_EQ(increment.value(), 65535);
}

TEST_F(HTTP2FrameTest, WritePingFrameTo) {
    uint8_t buffer[32];
    uint64_t opaque_data = 0xDEADBEEFCAFEBABE;

    size_t written = write_ping_frame_to(buffer, sizeof(buffer), opaque_data, false);

    EXPECT_EQ(written, 17);  // 9 header + 8 payload

    auto header = parse_frame_header(buffer);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header.value().type, FrameType::PING);
    EXPECT_EQ(header.value().flags & FrameFlags::PING_ACK, 0);

    auto parsed = parse_ping_frame(buffer + 9);
    ASSERT_TRUE(parsed.is_ok());
    EXPECT_EQ(parsed.value(), opaque_data);
}

TEST_F(HTTP2FrameTest, WritePingFrameToAck) {
    uint8_t buffer[32];

    size_t written = write_ping_frame_to(buffer, sizeof(buffer), 0x1234, true);

    EXPECT_EQ(written, 17);

    auto header = parse_frame_header(buffer);
    ASSERT_TRUE(header.is_ok());
    EXPECT_NE(header.value().flags & FrameFlags::PING_ACK, 0);
}

TEST_F(HTTP2FrameTest, WriteGoawayFrameTo) {
    uint8_t buffer[128];
    const char* debug = "Goodbye";

    size_t written = write_goaway_frame_to(buffer, sizeof(buffer), 100,
        ErrorCode::NO_ERROR, reinterpret_cast<const uint8_t*>(debug), strlen(debug));

    EXPECT_EQ(written, 9 + 8 + strlen(debug));

    auto header = parse_frame_header(buffer);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header.value().type, FrameType::GOAWAY);
    EXPECT_EQ(header.value().stream_id, 0);

    uint32_t last_stream;
    ErrorCode error;
    std::string debug_data;
    auto result = parse_goaway_frame(buffer + 9, written - 9, last_stream, error, debug_data);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(last_stream, 100);
    EXPECT_EQ(error, ErrorCode::NO_ERROR);
    EXPECT_EQ(debug_data, "Goodbye");
}

TEST_F(HTTP2FrameTest, WriteRstStreamFrameTo) {
    uint8_t buffer[16];

    size_t written = write_rst_stream_frame_to(buffer, sizeof(buffer), 7, ErrorCode::CANCEL);

    EXPECT_EQ(written, 13);  // 9 header + 4 payload

    auto header = parse_frame_header(buffer);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header.value().type, FrameType::RST_STREAM);
    EXPECT_EQ(header.value().stream_id, 7);

    auto error = parse_rst_stream_frame(buffer + 9);
    ASSERT_TRUE(error.is_ok());
    EXPECT_EQ(error.value(), ErrorCode::CANCEL);
}

TEST_F(HTTP2FrameTest, WritePushPromiseFrameTo) {
    uint8_t buffer[1024];
    std::vector<uint8_t> header_block = {0x82, 0x86, 0x84, 0x41};

    size_t written = write_push_promise_frame_to(buffer, sizeof(buffer), 1, 2,
        header_block.data(), header_block.size(), true);

    EXPECT_EQ(written, 9 + 4 + header_block.size());

    auto header = parse_frame_header(buffer);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header.value().type, FrameType::PUSH_PROMISE);
    EXPECT_EQ(header.value().stream_id, 1);
    EXPECT_NE(header.value().flags & FrameFlags::PUSH_PROMISE_END_HEADERS, 0);
}

TEST_F(HTTP2FrameTest, WriteContinuationFrameTo) {
    uint8_t buffer[1024];
    std::vector<uint8_t> header_block = {0x82, 0x86};

    size_t written = write_continuation_frame_to(buffer, sizeof(buffer), 1,
        header_block.data(), header_block.size(), true);

    EXPECT_EQ(written, 9 + header_block.size());

    auto header = parse_frame_header(buffer);
    ASSERT_TRUE(header.is_ok());
    EXPECT_EQ(header.value().type, FrameType::CONTINUATION);
    EXPECT_EQ(header.value().stream_id, 1);
    EXPECT_NE(header.value().flags & FrameFlags::CONTINUATION_END_HEADERS, 0);
}

// =============================================================================
// Zero-Allocation vs Allocating Comparison Tests
// =============================================================================

TEST_F(HTTP2FrameTest, ZeroAllocDataFrameEquivalence) {
    constexpr int NUM_TESTS = 100;

    for (int i = 0; i < NUM_TESTS; ++i) {
        std::string data = rng_.random_string(rng_.random_size(0, 1000));
        uint32_t stream_id = random_stream_id();
        bool end_stream = rng_.random_int(0, 1) == 1;

        // Allocating version
        auto alloc_frame = write_data_frame(stream_id, data, end_stream);

        // Zero-alloc version
        uint8_t buffer[2048];
        size_t written = write_data_frame_to(buffer, sizeof(buffer), stream_id,
            reinterpret_cast<const uint8_t*>(data.data()), data.size(), end_stream);

        ASSERT_EQ(written, alloc_frame.size());
        EXPECT_EQ(memcmp(buffer, alloc_frame.data(), written), 0)
            << "Mismatch at iteration " << i;
    }
}

TEST_F(HTTP2FrameTest, ZeroAllocHeadersFrameEquivalence) {
    constexpr int NUM_TESTS = 100;

    for (int i = 0; i < NUM_TESTS; ++i) {
        std::vector<uint8_t> header_block(rng_.random_size(1, 200));
        for (auto& b : header_block) {
            b = static_cast<uint8_t>(rng_.random_int(0, 255));
        }
        uint32_t stream_id = random_stream_id();
        bool end_stream = rng_.random_int(0, 1) == 1;
        bool end_headers = rng_.random_int(0, 1) == 1;

        // Allocating version
        auto alloc_frame = write_headers_frame(stream_id, header_block, end_stream, end_headers, nullptr);

        // Zero-alloc version
        uint8_t buffer[2048];
        size_t written = write_headers_frame_to(buffer, sizeof(buffer), stream_id,
            header_block.data(), header_block.size(), end_stream, end_headers, nullptr);

        ASSERT_EQ(written, alloc_frame.size());
        EXPECT_EQ(memcmp(buffer, alloc_frame.data(), written), 0)
            << "Mismatch at iteration " << i;
    }
}

TEST_F(HTTP2FrameTest, ZeroAllocSettingsFrameEquivalence) {
    constexpr int NUM_TESTS = 50;

    for (int i = 0; i < NUM_TESTS; ++i) {
        std::vector<SettingsParameter> params;
        size_t num_params = rng_.random_size(0, 6);
        for (size_t j = 0; j < num_params; ++j) {
            params.push_back({
                static_cast<SettingsId>(rng_.random_int(1, 6)),
                static_cast<uint32_t>(rng_.random_int(0, INT32_MAX))
            });
        }

        // Allocating version
        auto alloc_frame = write_settings_frame(params, false);

        // Zero-alloc version
        uint8_t buffer[256];
        size_t written = write_settings_frame_to(buffer, sizeof(buffer),
            params.data(), params.size(), false);

        ASSERT_EQ(written, alloc_frame.size());
        EXPECT_EQ(memcmp(buffer, alloc_frame.data(), written), 0)
            << "Mismatch at iteration " << i;
    }
}

TEST_F(HTTP2FrameTest, ZeroAllocPingFrameEquivalence) {
    constexpr int NUM_TESTS = 50;

    for (int i = 0; i < NUM_TESTS; ++i) {
        uint64_t opaque = random_u64();
        bool ack = rng_.random_int(0, 1) == 1;

        auto alloc_frame = write_ping_frame(opaque, ack);

        uint8_t buffer[32];
        size_t written = write_ping_frame_to(buffer, sizeof(buffer), opaque, ack);

        ASSERT_EQ(written, alloc_frame.size());
        EXPECT_EQ(memcmp(buffer, alloc_frame.data(), written), 0);
    }
}

// =============================================================================
// Zero-Allocation Performance Tests
// =============================================================================

TEST_F(HTTP2FrameTest, ZeroAllocDataFramePerformance) {
    uint8_t buffer[2048];
    const char* data = "Hello, HTTP/2! This is test data for performance.";
    size_t data_len = strlen(data);

    constexpr int ITERATIONS = 100000;

    // Zero-alloc version
    auto start_za = std::chrono::steady_clock::now();
    for (int i = 0; i < ITERATIONS; ++i) {
        size_t written = write_data_frame_to(buffer, sizeof(buffer), 1,
            reinterpret_cast<const uint8_t*>(data), data_len, false);
        (void)written;
    }
    auto elapsed_za = std::chrono::steady_clock::now() - start_za;
    auto ns_za = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed_za).count() / ITERATIONS;

    // Allocating version
    std::string data_str(data);
    auto start_alloc = std::chrono::steady_clock::now();
    for (int i = 0; i < ITERATIONS; ++i) {
        auto frame = write_data_frame(1, data_str, false);
        (void)frame;
    }
    auto elapsed_alloc = std::chrono::steady_clock::now() - start_alloc;
    auto ns_alloc = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed_alloc).count() / ITERATIONS;

    std::cout << "DATA frame write:" << std::endl;
    std::cout << "  Zero-alloc: " << ns_za << " ns/write" << std::endl;
    std::cout << "  Allocating: " << ns_alloc << " ns/write" << std::endl;
    std::cout << "  Speedup: " << static_cast<double>(ns_alloc) / ns_za << "x" << std::endl;

    // Zero-alloc should be faster (at least not slower)
    EXPECT_LE(ns_za, ns_alloc * 2);  // Allow 2x margin for noisy CI
}

TEST_F(HTTP2FrameTest, ZeroAllocSettingsFramePerformance) {
    uint8_t buffer[128];
    SettingsParameter params[] = {
        {SettingsId::MAX_CONCURRENT_STREAMS, 100},
        {SettingsId::INITIAL_WINDOW_SIZE, 65535},
        {SettingsId::MAX_FRAME_SIZE, 16384}
    };

    constexpr int ITERATIONS = 100000;

    // Zero-alloc version
    auto start_za = std::chrono::steady_clock::now();
    for (int i = 0; i < ITERATIONS; ++i) {
        size_t written = write_settings_frame_to(buffer, sizeof(buffer), params, 3, false);
        (void)written;
    }
    auto elapsed_za = std::chrono::steady_clock::now() - start_za;
    auto ns_za = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed_za).count() / ITERATIONS;

    // Allocating version
    std::vector<SettingsParameter> params_vec(params, params + 3);
    auto start_alloc = std::chrono::steady_clock::now();
    for (int i = 0; i < ITERATIONS; ++i) {
        auto frame = write_settings_frame(params_vec, false);
        (void)frame;
    }
    auto elapsed_alloc = std::chrono::steady_clock::now() - start_alloc;
    auto ns_alloc = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed_alloc).count() / ITERATIONS;

    std::cout << "SETTINGS frame write:" << std::endl;
    std::cout << "  Zero-alloc: " << ns_za << " ns/write" << std::endl;
    std::cout << "  Allocating: " << ns_alloc << " ns/write" << std::endl;
    std::cout << "  Speedup: " << static_cast<double>(ns_alloc) / ns_za << "x" << std::endl;
}

// =============================================================================
// Cached HPACK Headers Tests
// =============================================================================

// Include http2_connection.h for CachedHpackHeaders
#include "../../src/cpp/http/http2_connection.h"

TEST_F(HTTP2FrameTest, CachedHpackStatusIndexed) {
    uint8_t buffer[16];
    
    // Test indexed status codes (single byte)
    EXPECT_EQ(CachedHpackHeaders::get_status(200, buffer, sizeof(buffer)), 1);
    EXPECT_EQ(buffer[0], 0x88);  // Index 8
    
    EXPECT_EQ(CachedHpackHeaders::get_status(204, buffer, sizeof(buffer)), 1);
    EXPECT_EQ(buffer[0], 0x89);  // Index 9
    
    EXPECT_EQ(CachedHpackHeaders::get_status(404, buffer, sizeof(buffer)), 1);
    EXPECT_EQ(buffer[0], 0x8d);  // Index 13
    
    EXPECT_EQ(CachedHpackHeaders::get_status(500, buffer, sizeof(buffer)), 1);
    EXPECT_EQ(buffer[0], 0x8e);  // Index 14
}

TEST_F(HTTP2FrameTest, CachedHpackStatusNonIndexed) {
    uint8_t buffer[16];
    
    // Test non-indexed status codes (literal)
    size_t len = CachedHpackHeaders::get_status(201, buffer, sizeof(buffer));
    EXPECT_GE(len, 4);  // At least: prefix + length + "201"
    EXPECT_EQ(buffer[0], 0x08);  // Literal without indexing, index 8 (:status)
    EXPECT_EQ(buffer[1], 3);     // Length 3 for "201"
    EXPECT_EQ(buffer[2], '2');
    EXPECT_EQ(buffer[3], '0');
    EXPECT_EQ(buffer[4], '1');
}

TEST_F(HTTP2FrameTest, CachedHpackContentLength) {
    uint8_t buffer[32];
    
    // Test content-length: 0 (cached)
    size_t len = CachedHpackHeaders::encode_content_length(0, buffer, sizeof(buffer));
    EXPECT_EQ(len, 4);  // prefix(2) + len(1) + "0"(1)
    
    // Test dynamic content-length
    len = CachedHpackHeaders::encode_content_length(12345, buffer, sizeof(buffer));
    EXPECT_GT(len, 0);
    EXPECT_EQ(buffer[0], 0x0f);  // Literal without indexing
    EXPECT_EQ(buffer[1], 0x0d);  // Index 28 - 15 = 13
    EXPECT_EQ(buffer[2], 5);     // Length 5 for "12345"
}

TEST_F(HTTP2FrameTest, CachedHpackPrecomputedResponses) {
    // Verify pre-computed response header blocks
    EXPECT_GT(CachedHpackHeaders::RESP_200_JSON_LEN, 0u);
    EXPECT_EQ(CachedHpackHeaders::RESP_200_JSON[0], 0x88);  // :status 200
    
    EXPECT_GT(CachedHpackHeaders::RESP_200_TEXT_LEN, 0u);
    EXPECT_EQ(CachedHpackHeaders::RESP_200_TEXT[0], 0x88);  // :status 200
    
    EXPECT_GT(CachedHpackHeaders::RESP_404_TEXT_LEN, 0u);
    EXPECT_EQ(CachedHpackHeaders::RESP_404_TEXT[0], 0x8d);  // :status 404
    
    EXPECT_GT(CachedHpackHeaders::RESP_500_TEXT_LEN, 0u);
    EXPECT_EQ(CachedHpackHeaders::RESP_500_TEXT[0], 0x8e);  // :status 500
}

TEST_F(HTTP2FrameTest, CachedHpackContentTypeHeaders) {
    // Verify pre-computed content-type headers
    EXPECT_GT(CachedHpackHeaders::CT_JSON.len, 0);
    EXPECT_GT(CachedHpackHeaders::CT_TEXT_PLAIN.len, 0);
    EXPECT_GT(CachedHpackHeaders::CT_TEXT_HTML.len, 0);
    EXPECT_GT(CachedHpackHeaders::CT_OCTET_STREAM.len, 0);
    
    // Check format: literal without indexing, indexed name (content-type = 31)
    EXPECT_EQ(CachedHpackHeaders::CT_JSON.data[0], 0x0f);
    EXPECT_EQ(CachedHpackHeaders::CT_JSON.data[1], 0x10);  // 31 - 15 = 16
}

TEST_F(HTTP2FrameTest, CachedHpackStatusInsufficientCapacity) {
    uint8_t buffer[1];
    
    // Indexed status should succeed with 1 byte
    EXPECT_EQ(CachedHpackHeaders::get_status(200, buffer, 1), 1);
    
    // Non-indexed needs more space
    EXPECT_EQ(CachedHpackHeaders::get_status(201, buffer, 1), 0);
}

TEST_F(HTTP2FrameTest, CachedHpackAllIndexedStatuses) {
    uint8_t buffer[16];
    
    // All RFC 7541 indexed status codes
    struct StatusTest {
        uint16_t code;
        uint8_t expected_index;
    } tests[] = {
        {200, 0x88}, {204, 0x89}, {206, 0x8a},
        {304, 0x8b}, {400, 0x8c}, {404, 0x8d}, {500, 0x8e}
    };
    
    for (const auto& test : tests) {
        size_t len = CachedHpackHeaders::get_status(test.code, buffer, sizeof(buffer));
        EXPECT_EQ(len, 1) << "Status " << test.code << " should be 1 byte";
        EXPECT_EQ(buffer[0], test.expected_index) << "Status " << test.code << " index mismatch";
    }
}

TEST_F(HTTP2FrameTest, CachedHpackPerformance) {
    uint8_t buffer[64];
    constexpr int ITERATIONS = 100000;
    
    // Cached status (single byte lookup)
    auto start_cached = std::chrono::steady_clock::now();
    for (int i = 0; i < ITERATIONS; ++i) {
        size_t len = CachedHpackHeaders::get_status(200, buffer, sizeof(buffer));
        (void)len;
    }
    auto elapsed_cached = std::chrono::steady_clock::now() - start_cached;
    auto ns_cached = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed_cached).count() / ITERATIONS;
    
    // Non-cached status (requires encoding)
    auto start_encode = std::chrono::steady_clock::now();
    for (int i = 0; i < ITERATIONS; ++i) {
        size_t len = CachedHpackHeaders::get_status(201, buffer, sizeof(buffer));
        (void)len;
    }
    auto elapsed_encode = std::chrono::steady_clock::now() - start_encode;
    auto ns_encode = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed_encode).count() / ITERATIONS;
    
    std::cout << "HPACK status encoding:" << std::endl;
    std::cout << "  Cached (200): " << ns_cached << " ns" << std::endl;
    std::cout << "  Encoded (201): " << ns_encode << " ns" << std::endl;
    
    // Cached should be significantly faster
    EXPECT_LE(ns_cached, ns_encode * 2);
}

// =============================================================================
// FastHeaders Tests
// =============================================================================

using std::string_view_literals::operator""sv;

TEST_F(HTTP2FrameTest, FastHeadersPseudoHeaders) {
    FastHeaders headers;
    
    // Add pseudo-headers
    headers.add(":method"sv, "GET"sv);
    headers.add(":path"sv, "/api/users"sv);
    headers.add(":scheme"sv, "https"sv);
    headers.add(":authority"sv, "example.com"sv);
    
    // Quick access methods
    EXPECT_EQ(headers.method(), "GET");
    EXPECT_EQ(headers.path(), "/api/users");
    EXPECT_EQ(headers.scheme(), "https");
    EXPECT_EQ(headers.authority(), "example.com");
    
    // Also accessible via get()
    EXPECT_EQ(headers.get(":method"), "GET");
    EXPECT_EQ(headers.get(":path"), "/api/users");
}

TEST_F(HTTP2FrameTest, FastHeadersMethodInterning) {
    FastHeaders headers;
    
    // Common methods should be interned (point to constexpr)
    headers.add(":method"sv, "GET"sv);
    EXPECT_EQ(headers.method().data(), common_headers::METHOD_GET.data());
    
    headers.add(":method"sv, "POST"sv);
    EXPECT_EQ(headers.method().data(), common_headers::METHOD_POST.data());
    
    headers.add(":method"sv, "DELETE"sv);
    EXPECT_EQ(headers.method().data(), common_headers::METHOD_DELETE.data());
}

TEST_F(HTTP2FrameTest, FastHeadersContentTypeInterning) {
    FastHeaders headers;
    
    // Add content-type with common value
    headers.add("content-type"sv, "application/json"sv);
    
    // Should be interned
    std::string_view ct = headers.get("content-type");
    EXPECT_EQ(ct, "application/json");
    EXPECT_EQ(ct.data(), common_headers::CT_JSON.data());
}

TEST_F(HTTP2FrameTest, FastHeadersRegularHeaders) {
    FastHeaders headers;
    
    headers.add("content-length"sv, "1234"sv);
    headers.add("accept"sv, "text/html"sv);
    headers.add("user-agent"sv, "TestClient/1.0"sv);
    headers.add("x-custom-header"sv, "custom-value"sv);
    
    EXPECT_EQ(headers.get("content-length"), "1234");
    EXPECT_EQ(headers.get("accept"), "text/html");
    EXPECT_EQ(headers.get("user-agent"), "TestClient/1.0");
    EXPECT_EQ(headers.get("x-custom-header"), "custom-value");
}

TEST_F(HTTP2FrameTest, FastHeadersHas) {
    FastHeaders headers;
    
    headers.add(":method"sv, "GET"sv);
    headers.add("content-type"sv, "application/json"sv);
    
    EXPECT_TRUE(headers.has(":method"));
    EXPECT_TRUE(headers.has("content-type"));
    EXPECT_FALSE(headers.has("authorization"));
    EXPECT_FALSE(headers.has("x-missing"));
}

TEST_F(HTTP2FrameTest, FastHeadersSize) {
    FastHeaders headers;
    
    EXPECT_EQ(headers.size(), 0);
    
    headers.add(":method"sv, "GET"sv);
    EXPECT_EQ(headers.size(), 1);
    
    headers.add(":path"sv, "/"sv);
    EXPECT_EQ(headers.size(), 2);
    
    headers.add("content-type"sv, "text/plain"sv);
    EXPECT_EQ(headers.size(), 3);
}

TEST_F(HTTP2FrameTest, FastHeadersClear) {
    FastHeaders headers;
    
    headers.add(":method"sv, "GET"sv);
    headers.add(":path"sv, "/api"sv);
    headers.add("content-type"sv, "application/json"sv);
    
    EXPECT_EQ(headers.size(), 3);
    
    headers.clear();
    
    EXPECT_EQ(headers.size(), 0);
    EXPECT_TRUE(headers.method().empty());
    EXPECT_TRUE(headers.path().empty());
    EXPECT_TRUE(headers.get("content-type").empty());
}

TEST_F(HTTP2FrameTest, FastHeadersForEach) {
    FastHeaders headers;
    
    headers.add(":method"sv, "POST"sv);
    headers.add(":path"sv, "/submit"sv);
    headers.add("content-type"sv, "application/json"sv);
    headers.add("accept"sv, "application/json"sv);
    
    std::vector<std::pair<std::string, std::string>> collected;
    headers.for_each([&](std::string_view name, std::string_view value) {
        collected.emplace_back(std::string(name), std::string(value));
    });
    
    EXPECT_EQ(collected.size(), 4);
    
    // Check all headers are present
    bool found_method = false, found_path = false, found_ct = false, found_accept = false;
    for (const auto& [name, value] : collected) {
        if (name == ":method" && value == "POST") found_method = true;
        if (name == ":path" && value == "/submit") found_path = true;
        if (name == "content-type" && value == "application/json") found_ct = true;
        if (name == "accept" && value == "application/json") found_accept = true;
    }
    EXPECT_TRUE(found_method);
    EXPECT_TRUE(found_path);
    EXPECT_TRUE(found_ct);
    EXPECT_TRUE(found_accept);
}

TEST_F(HTTP2FrameTest, FastHeadersToMap) {
    FastHeaders headers;
    
    headers.add(":method"sv, "GET"sv);
    headers.add(":path"sv, "/users"sv);
    headers.add("authorization"sv, "Bearer token123"sv);
    
    auto map = headers.to_map();
    
    EXPECT_EQ(map.size(), 3);
    EXPECT_EQ(map[":method"], "GET");
    EXPECT_EQ(map[":path"], "/users");
    EXPECT_EQ(map["authorization"], "Bearer token123");
}

TEST_F(HTTP2FrameTest, FastHeadersOverflow) {
    FastHeaders headers;
    
    // Add more than COMMON_HEADER_SLOTS headers using std::string overload
    for (int i = 0; i < 20; ++i) {
        headers.add(std::string("x-header-") + std::to_string(i),
                   std::string("value-") + std::to_string(i));
    }
    
    EXPECT_EQ(headers.size(), 20);
    
    // All should be retrievable
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(headers.get(std::string("x-header-") + std::to_string(i)),
                 std::string("value-") + std::to_string(i));
    }
}

TEST_F(HTTP2FrameTest, FastHeadersPerformance) {
    constexpr int ITERATIONS = 100000;
    
    // FastHeaders add/get
    auto start_fast = std::chrono::steady_clock::now();
    for (int i = 0; i < ITERATIONS; ++i) {
        FastHeaders headers;
        headers.add(":method"sv, "GET"sv);
        headers.add(":path"sv, "/api/users"sv);
        headers.add("content-type"sv, "application/json"sv);
        headers.add("accept"sv, "application/json"sv);
        auto m = headers.method();
        auto ct = headers.get("content-type");
        (void)m; (void)ct;
    }
    auto elapsed_fast = std::chrono::steady_clock::now() - start_fast;
    auto ns_fast = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed_fast).count() / ITERATIONS;
    
    // unordered_map add/get
    auto start_map = std::chrono::steady_clock::now();
    for (int i = 0; i < ITERATIONS; ++i) {
        std::unordered_map<std::string, std::string> headers;
        headers[":method"] = "GET";
        headers[":path"] = "/api/users";
        headers["content-type"] = "application/json";
        headers["accept"] = "application/json";
        auto m = headers[":method"];
        auto ct = headers["content-type"];
        (void)m; (void)ct;
    }
    auto elapsed_map = std::chrono::steady_clock::now() - start_map;
    auto ns_map = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed_map).count() / ITERATIONS;
    
    std::cout << "Header storage performance:" << std::endl;
    std::cout << "  FastHeaders: " << ns_fast << " ns/iter" << std::endl;
    std::cout << "  unordered_map: " << ns_map << " ns/iter" << std::endl;
    if (ns_fast > 0) {
        std::cout << "  Speedup: " << static_cast<double>(ns_map) / ns_fast << "x" << std::endl;
    }
}

