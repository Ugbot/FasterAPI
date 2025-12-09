/**
 * QUIC Frames Unit Tests
 *
 * Tests the QUIC frame parsing and serialization (RFC 9000 Section 19):
 * - StreamFrame (parse, serialize, flags)
 * - AckFrame (parse, serialize, ranges)
 * - CryptoFrame (parse, serialize)
 * - DatagramFrame (parse, serialize)
 * - Round-trip tests
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/quic/quic_frames.h"
#include <random>
#include <chrono>
#include <cstring>
#include <vector>

namespace fasterapi {
namespace quic {
namespace test {

// ===========================================================================
// StreamFrame Tests
// ===========================================================================

class StreamFrameTest : public ::testing::Test {
protected:
    uint8_t buffer_[4096];
    std::mt19937_64 rng_;

    void SetUp() override {
        memset(buffer_, 0, sizeof(buffer_));
        rng_.seed(std::random_device{}());
    }

    std::string random_data(size_t length) {
        static const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);
        std::string result;
        result.reserve(length);
        for (size_t i = 0; i < length; ++i) {
            result += charset[dist(rng_)];
        }
        return result;
    }
};

TEST_F(StreamFrameTest, SerializeBasic) {
    StreamFrame frame;
    frame.stream_id = 4;
    frame.offset = 0;
    frame.length = 5;
    frame.fin = false;

    std::string data = "hello";
    frame.data = reinterpret_cast<const uint8_t*>(data.c_str());

    size_t len = frame.serialize(buffer_);
    EXPECT_GT(len, 0u);

    // Type byte should be 0x08 | FLAG_LEN (0x02) = 0x0A
    EXPECT_EQ(buffer_[0] & 0xF8, 0x08);  // Base STREAM type
}

TEST_F(StreamFrameTest, SerializeWithOffset) {
    StreamFrame frame;
    frame.stream_id = 100;
    frame.offset = 1000;
    frame.length = 10;
    frame.fin = false;

    std::string data = "0123456789";
    frame.data = reinterpret_cast<const uint8_t*>(data.c_str());

    size_t len = frame.serialize(buffer_);
    EXPECT_GT(len, 0u);

    // Should have OFF flag set
    EXPECT_NE(buffer_[0] & StreamFrame::FLAG_OFF, 0);
}

TEST_F(StreamFrameTest, SerializeWithFin) {
    StreamFrame frame;
    frame.stream_id = 0;
    frame.offset = 0;
    frame.length = 3;
    frame.fin = true;

    std::string data = "end";
    frame.data = reinterpret_cast<const uint8_t*>(data.c_str());

    size_t len = frame.serialize(buffer_);
    EXPECT_GT(len, 0u);

    // Should have FIN flag set
    EXPECT_NE(buffer_[0] & StreamFrame::FLAG_FIN, 0);
}

TEST_F(StreamFrameTest, ParseBasic) {
    // Manually construct a STREAM frame
    // Type: 0x0A (STREAM + LEN)
    // Stream ID: 4 (1 byte)
    // Length: 5 (1 byte)
    // Data: "hello"
    buffer_[0] = 0x0A;  // STREAM + LEN
    buffer_[1] = 4;     // Stream ID
    buffer_[2] = 5;     // Length
    memcpy(buffer_ + 3, "hello", 5);

    StreamFrame frame;
    size_t consumed;
    int result = frame.parse(buffer_, 8, consumed);

    EXPECT_EQ(result, 0);
    EXPECT_EQ(frame.stream_id, 4u);
    EXPECT_EQ(frame.offset, 0u);
    EXPECT_EQ(frame.length, 5u);
    EXPECT_FALSE(frame.fin);
    EXPECT_EQ(consumed, 8u);
}

TEST_F(StreamFrameTest, RoundTrip) {
    StreamFrame original;
    original.stream_id = 12345;
    original.offset = 67890;
    original.length = 20;
    original.fin = true;

    std::string data = random_data(20);
    original.data = reinterpret_cast<const uint8_t*>(data.c_str());

    // Serialize
    size_t serialized_len = original.serialize(buffer_);
    EXPECT_GT(serialized_len, 0u);

    // Parse
    StreamFrame parsed;
    size_t consumed;
    int result = parsed.parse(buffer_, serialized_len, consumed);

    EXPECT_EQ(result, 0);
    EXPECT_EQ(parsed.stream_id, original.stream_id);
    EXPECT_EQ(parsed.offset, original.offset);
    EXPECT_EQ(parsed.length, original.length);
    EXPECT_EQ(parsed.fin, original.fin);
    EXPECT_EQ(consumed, serialized_len);
    EXPECT_EQ(memcmp(parsed.data, data.c_str(), data.length()), 0);
}

TEST_F(StreamFrameTest, ParseInsufficientData) {
    buffer_[0] = 0x0A;
    buffer_[1] = 4;
    // Missing length and data

    StreamFrame frame;
    size_t consumed;
    int result = frame.parse(buffer_, 2, consumed);

    EXPECT_EQ(result, -1);
}

TEST_F(StreamFrameTest, LargeStreamId) {
    StreamFrame frame;
    frame.stream_id = 0x3FFFFFFF;  // Max 4-byte varint
    frame.offset = 0;
    frame.length = 1;
    frame.fin = false;

    uint8_t data = 'X';
    frame.data = &data;

    size_t len = frame.serialize(buffer_);
    EXPECT_GT(len, 0u);

    StreamFrame parsed;
    size_t consumed;
    int result = parsed.parse(buffer_, len, consumed);

    EXPECT_EQ(result, 0);
    EXPECT_EQ(parsed.stream_id, frame.stream_id);
}

// ===========================================================================
// AckFrame Tests
// ===========================================================================

class AckFrameTest : public ::testing::Test {
protected:
    uint8_t buffer_[4096];
    std::mt19937_64 rng_;

    void SetUp() override {
        memset(buffer_, 0, sizeof(buffer_));
        rng_.seed(std::random_device{}());
    }
};

TEST_F(AckFrameTest, SerializeSimple) {
    AckFrame frame;
    frame.largest_acked = 10;
    frame.ack_delay = 100;
    frame.first_ack_range = 5;  // Acks 5-10
    frame.range_count = 0;

    size_t len = frame.serialize(buffer_);
    EXPECT_GT(len, 0u);
    EXPECT_EQ(buffer_[0], 0x02);  // ACK type
}

TEST_F(AckFrameTest, SerializeWithRanges) {
    AckFrame frame;
    frame.largest_acked = 100;
    frame.ack_delay = 50;
    frame.first_ack_range = 10;  // Acks 90-100
    frame.range_count = 2;
    frame.ranges[0].gap = 5;     // Gap of 5 unacked packets
    frame.ranges[0].length = 3;  // Then 3 acked
    frame.ranges[1].gap = 2;
    frame.ranges[1].length = 5;

    size_t len = frame.serialize(buffer_);
    EXPECT_GT(len, 0u);
}

TEST_F(AckFrameTest, ParseSimple) {
    // Manually construct ACK frame
    buffer_[0] = 0x02;  // ACK type
    size_t pos = 1;
    pos += VarInt::encode(10, buffer_ + pos);   // Largest acked
    pos += VarInt::encode(100, buffer_ + pos);  // ACK delay
    pos += VarInt::encode(0, buffer_ + pos);    // Range count
    pos += VarInt::encode(5, buffer_ + pos);    // First ACK range

    AckFrame frame;
    size_t consumed;
    int result = frame.parse(buffer_, pos, consumed);

    EXPECT_EQ(result, 0);
    EXPECT_EQ(frame.largest_acked, 10u);
    EXPECT_EQ(frame.ack_delay, 100u);
    EXPECT_EQ(frame.first_ack_range, 5u);
    EXPECT_EQ(frame.range_count, 0u);
}

TEST_F(AckFrameTest, RoundTrip) {
    AckFrame original;
    original.largest_acked = 1000;
    original.ack_delay = 5000;
    original.first_ack_range = 50;
    original.range_count = 3;
    original.ranges[0].gap = 10;
    original.ranges[0].length = 20;
    original.ranges[1].gap = 5;
    original.ranges[1].length = 15;
    original.ranges[2].gap = 2;
    original.ranges[2].length = 30;

    size_t serialized_len = original.serialize(buffer_);
    EXPECT_GT(serialized_len, 0u);

    AckFrame parsed;
    size_t consumed;
    int result = parsed.parse(buffer_, serialized_len, consumed);

    EXPECT_EQ(result, 0);
    EXPECT_EQ(parsed.largest_acked, original.largest_acked);
    EXPECT_EQ(parsed.ack_delay, original.ack_delay);
    EXPECT_EQ(parsed.first_ack_range, original.first_ack_range);
    EXPECT_EQ(parsed.range_count, original.range_count);

    for (size_t i = 0; i < original.range_count; i++) {
        EXPECT_EQ(parsed.ranges[i].gap, original.ranges[i].gap);
        EXPECT_EQ(parsed.ranges[i].length, original.ranges[i].length);
    }
}

TEST_F(AckFrameTest, MaxRanges) {
    AckFrame frame;
    frame.largest_acked = 10000;
    frame.ack_delay = 100;
    frame.first_ack_range = 10;
    frame.range_count = 64;  // Max ranges

    for (size_t i = 0; i < 64; i++) {
        frame.ranges[i].gap = 1;
        frame.ranges[i].length = 1;
    }

    size_t len = frame.serialize(buffer_);
    EXPECT_GT(len, 0u);

    AckFrame parsed;
    size_t consumed;
    int result = parsed.parse(buffer_, len, consumed);

    EXPECT_EQ(result, 0);
    EXPECT_EQ(parsed.range_count, 64u);
}

TEST_F(AckFrameTest, TooManyRanges) {
    // Manually construct ACK with > 64 ranges
    buffer_[0] = 0x02;
    size_t pos = 1;
    pos += VarInt::encode(100, buffer_ + pos);  // Largest acked
    pos += VarInt::encode(0, buffer_ + pos);    // ACK delay
    pos += VarInt::encode(100, buffer_ + pos);  // Range count > 64
    pos += VarInt::encode(10, buffer_ + pos);   // First ACK range

    AckFrame frame;
    size_t consumed;
    int result = frame.parse(buffer_, pos, consumed);

    EXPECT_EQ(result, 1);  // Error: too many ranges
}

// ===========================================================================
// CryptoFrame Tests
// ===========================================================================

class CryptoFrameTest : public ::testing::Test {
protected:
    uint8_t buffer_[4096];
    std::mt19937_64 rng_;

    void SetUp() override {
        memset(buffer_, 0, sizeof(buffer_));
        rng_.seed(std::random_device{}());
    }
};

TEST_F(CryptoFrameTest, SerializeBasic) {
    CryptoFrame frame;
    frame.offset = 0;
    frame.length = 32;

    uint8_t data[32] = {0};
    frame.data = data;

    size_t len = frame.serialize(buffer_);
    EXPECT_GT(len, 0u);
    EXPECT_EQ(buffer_[0], 0x06);  // CRYPTO type
}

TEST_F(CryptoFrameTest, RoundTrip) {
    CryptoFrame original;
    original.offset = 1000;
    original.length = 50;

    std::vector<uint8_t> data(50);
    for (size_t i = 0; i < 50; i++) {
        data[i] = static_cast<uint8_t>(i);
    }
    original.data = data.data();

    size_t serialized_len = original.serialize(buffer_);

    CryptoFrame parsed;
    size_t consumed;
    int result = parsed.parse(buffer_, serialized_len, consumed);

    EXPECT_EQ(result, 0);
    EXPECT_EQ(parsed.offset, original.offset);
    EXPECT_EQ(parsed.length, original.length);
    EXPECT_EQ(memcmp(parsed.data, data.data(), 50), 0);
}

TEST_F(CryptoFrameTest, LargeOffset) {
    CryptoFrame frame;
    frame.offset = 0x3FFFFFFF;  // Large offset
    frame.length = 1;

    uint8_t data = 0xFF;
    frame.data = &data;

    size_t len = frame.serialize(buffer_);

    CryptoFrame parsed;
    size_t consumed;
    int result = parsed.parse(buffer_, len, consumed);

    EXPECT_EQ(result, 0);
    EXPECT_EQ(parsed.offset, frame.offset);
}

// ===========================================================================
// DatagramFrame Tests
// ===========================================================================

class DatagramFrameTest : public ::testing::Test {
protected:
    uint8_t buffer_[4096];
    std::mt19937_64 rng_;

    void SetUp() override {
        memset(buffer_, 0, sizeof(buffer_));
        rng_.seed(std::random_device{}());
    }
};

TEST_F(DatagramFrameTest, SerializeWithLength) {
    DatagramFrame frame;
    frame.length = 10;

    std::string data = "0123456789";
    frame.data = reinterpret_cast<const uint8_t*>(data.c_str());

    size_t len = frame.serialize(buffer_, true);
    EXPECT_GT(len, 0u);
    EXPECT_EQ(buffer_[0], 0x31);  // DATAGRAM_WITH_LEN
}

TEST_F(DatagramFrameTest, SerializeWithoutLength) {
    DatagramFrame frame;
    frame.length = 10;

    std::string data = "0123456789";
    frame.data = reinterpret_cast<const uint8_t*>(data.c_str());

    size_t len = frame.serialize(buffer_, false);
    EXPECT_GT(len, 0u);
    EXPECT_EQ(buffer_[0], 0x30);  // DATAGRAM (no length)
}

TEST_F(DatagramFrameTest, ParseWithLength) {
    // Construct DATAGRAM frame with length
    buffer_[0] = 0x31;  // DATAGRAM_WITH_LEN
    size_t pos = 1;
    pos += VarInt::encode(5, buffer_ + pos);  // Length = 5
    memcpy(buffer_ + pos, "hello", 5);
    pos += 5;

    DatagramFrame frame;
    size_t consumed;
    int result = frame.parse(buffer_, pos, consumed);

    EXPECT_EQ(result, 0);
    EXPECT_EQ(frame.length, 5u);
    EXPECT_EQ(memcmp(frame.data, "hello", 5), 0);
}

TEST_F(DatagramFrameTest, ParseWithoutLength) {
    // Construct DATAGRAM frame without length
    buffer_[0] = 0x30;  // DATAGRAM (no length)
    memcpy(buffer_ + 1, "world", 5);

    DatagramFrame frame;
    size_t consumed;
    int result = frame.parse(buffer_, 6, consumed);

    EXPECT_EQ(result, 0);
    EXPECT_EQ(frame.length, 5u);
    EXPECT_EQ(memcmp(frame.data, "world", 5), 0);
}

TEST_F(DatagramFrameTest, RoundTripWithLength) {
    DatagramFrame original;
    original.length = 100;

    std::vector<uint8_t> data(100);
    for (size_t i = 0; i < 100; i++) {
        data[i] = static_cast<uint8_t>(i);
    }
    original.data = data.data();

    size_t serialized_len = original.serialize(buffer_, true);

    DatagramFrame parsed;
    size_t consumed;
    int result = parsed.parse(buffer_, serialized_len, consumed);

    EXPECT_EQ(result, 0);
    EXPECT_EQ(parsed.length, original.length);
    EXPECT_EQ(memcmp(parsed.data, data.data(), 100), 0);
}

// ===========================================================================
// Frame Type Tests
// ===========================================================================

TEST(FrameTypeTest, EnumValues) {
    EXPECT_EQ(static_cast<uint64_t>(FrameType::PADDING), 0x00);
    EXPECT_EQ(static_cast<uint64_t>(FrameType::PING), 0x01);
    EXPECT_EQ(static_cast<uint64_t>(FrameType::ACK), 0x02);
    EXPECT_EQ(static_cast<uint64_t>(FrameType::CRYPTO), 0x06);
    EXPECT_EQ(static_cast<uint64_t>(FrameType::STREAM), 0x08);
    EXPECT_EQ(static_cast<uint64_t>(FrameType::MAX_DATA), 0x10);
    EXPECT_EQ(static_cast<uint64_t>(FrameType::DATAGRAM), 0x30);
    EXPECT_EQ(static_cast<uint64_t>(FrameType::DATAGRAM_WITH_LEN), 0x31);
}

// ===========================================================================
// Performance Tests
// ===========================================================================

class FramePerformanceTest : public ::testing::Test {
protected:
    uint8_t buffer_[4096];
    std::mt19937_64 rng_;

    void SetUp() override {
        rng_.seed(std::random_device{}());
    }
};

TEST_F(FramePerformanceTest, StreamFrameSerializePerformance) {
    StreamFrame frame;
    frame.stream_id = 1000;
    frame.offset = 50000;
    frame.length = 1200;
    frame.fin = false;

    std::vector<uint8_t> data(1200, 'X');
    frame.data = data.data();

    const int iterations = 100000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        volatile size_t len = frame.serialize(buffer_);
        (void)len;
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "StreamFrame serialize (1200 bytes): " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 500.0);
}

TEST_F(FramePerformanceTest, StreamFrameParsePerformance) {
    // Pre-serialize a frame
    StreamFrame original;
    original.stream_id = 1000;
    original.offset = 50000;
    original.length = 1200;
    original.fin = false;

    std::vector<uint8_t> data(1200, 'X');
    original.data = data.data();
    size_t serialized_len = original.serialize(buffer_);

    const int iterations = 100000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        StreamFrame frame;
        size_t consumed;
        volatile int result = frame.parse(buffer_, serialized_len, consumed);
        (void)result;
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "StreamFrame parse (1200 bytes): " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 200.0);
}

TEST_F(FramePerformanceTest, AckFrameSerializePerformance) {
    AckFrame frame;
    frame.largest_acked = 10000;
    frame.ack_delay = 1000;
    frame.first_ack_range = 100;
    frame.range_count = 10;
    for (size_t i = 0; i < 10; i++) {
        frame.ranges[i].gap = 5;
        frame.ranges[i].length = 10;
    }

    const int iterations = 100000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        volatile size_t len = frame.serialize(buffer_);
        (void)len;
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "AckFrame serialize (10 ranges): " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 200.0);
}

TEST_F(FramePerformanceTest, AckFrameParsePerformance) {
    // Pre-serialize
    AckFrame original;
    original.largest_acked = 10000;
    original.ack_delay = 1000;
    original.first_ack_range = 100;
    original.range_count = 10;
    for (size_t i = 0; i < 10; i++) {
        original.ranges[i].gap = 5;
        original.ranges[i].length = 10;
    }
    size_t serialized_len = original.serialize(buffer_);

    const int iterations = 100000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        AckFrame frame;
        size_t consumed;
        volatile int result = frame.parse(buffer_, serialized_len, consumed);
        (void)result;
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "AckFrame parse (10 ranges): " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 200.0);
}

}  // namespace test
}  // namespace quic
}  // namespace fasterapi

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
