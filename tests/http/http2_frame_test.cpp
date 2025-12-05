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

