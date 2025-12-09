/**
 * QUIC Variable-Length Integer Unit Tests
 *
 * Tests the QUIC VarInt implementation (RFC 9000 Section 16):
 * - 1-byte encoding (0-63)
 * - 2-byte encoding (64-16383)
 * - 4-byte encoding (16384-1073741823)
 * - 8-byte encoding (1073741824-4611686018427387903)
 * - Boundary conditions
 * - Round-trip encode/decode
 * - Performance benchmarks
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/quic/quic_varint.h"
#include <random>
#include <chrono>
#include <vector>

namespace fasterapi {
namespace quic {
namespace test {

// ===========================================================================
// Encoding Size Tests
// ===========================================================================

class VarIntSizeTest : public ::testing::Test {
protected:
    std::mt19937_64 rng_;

    void SetUp() override {
        rng_.seed(std::random_device{}());
    }
};

TEST_F(VarIntSizeTest, OneByte) {
    EXPECT_EQ(VarInt::encoded_size(0), 1u);
    EXPECT_EQ(VarInt::encoded_size(1), 1u);
    EXPECT_EQ(VarInt::encoded_size(32), 1u);
    EXPECT_EQ(VarInt::encoded_size(63), 1u);
}

TEST_F(VarIntSizeTest, TwoBytes) {
    EXPECT_EQ(VarInt::encoded_size(64), 2u);
    EXPECT_EQ(VarInt::encoded_size(100), 2u);
    EXPECT_EQ(VarInt::encoded_size(1000), 2u);
    EXPECT_EQ(VarInt::encoded_size(16383), 2u);
}

TEST_F(VarIntSizeTest, FourBytes) {
    EXPECT_EQ(VarInt::encoded_size(16384), 4u);
    EXPECT_EQ(VarInt::encoded_size(100000), 4u);
    EXPECT_EQ(VarInt::encoded_size(1000000), 4u);
    EXPECT_EQ(VarInt::encoded_size(1073741823), 4u);
}

TEST_F(VarIntSizeTest, EightBytes) {
    EXPECT_EQ(VarInt::encoded_size(1073741824), 8u);
    EXPECT_EQ(VarInt::encoded_size(10000000000ULL), 8u);
    EXPECT_EQ(VarInt::encoded_size(4611686018427387903ULL), 8u);
}

// ===========================================================================
// Encoding Tests
// ===========================================================================

class VarIntEncodeTest : public ::testing::Test {
protected:
    uint8_t buffer_[16];
    std::mt19937_64 rng_;

    void SetUp() override {
        memset(buffer_, 0, sizeof(buffer_));
        rng_.seed(std::random_device{}());
    }
};

TEST_F(VarIntEncodeTest, EncodeZero) {
    size_t len = VarInt::encode(0, buffer_);
    EXPECT_EQ(len, 1u);
    EXPECT_EQ(buffer_[0], 0x00);
}

TEST_F(VarIntEncodeTest, EncodeOneByte) {
    size_t len = VarInt::encode(37, buffer_);
    EXPECT_EQ(len, 1u);
    EXPECT_EQ(buffer_[0], 37);
}

TEST_F(VarIntEncodeTest, EncodeOneByteMax) {
    size_t len = VarInt::encode(63, buffer_);
    EXPECT_EQ(len, 1u);
    EXPECT_EQ(buffer_[0], 0x3F);
}

TEST_F(VarIntEncodeTest, EncodeTwoByteMin) {
    size_t len = VarInt::encode(64, buffer_);
    EXPECT_EQ(len, 2u);
    // Prefix 01 indicates 2 bytes
    EXPECT_EQ(buffer_[0] >> 6, 1);
}

TEST_F(VarIntEncodeTest, EncodeTwoBytes) {
    size_t len = VarInt::encode(494, buffer_);
    EXPECT_EQ(len, 2u);
    // 494 = 0x1EE, with prefix 01 = 0x41EE
    EXPECT_EQ(buffer_[0], 0x41);
    EXPECT_EQ(buffer_[1], 0xEE);
}

TEST_F(VarIntEncodeTest, EncodeTwoByteMax) {
    size_t len = VarInt::encode(16383, buffer_);
    EXPECT_EQ(len, 2u);
    // 16383 = 0x3FFF, with prefix 01 = 0x7FFF
    EXPECT_EQ(buffer_[0], 0x7F);
    EXPECT_EQ(buffer_[1], 0xFF);
}

TEST_F(VarIntEncodeTest, EncodeFourByteMin) {
    size_t len = VarInt::encode(16384, buffer_);
    EXPECT_EQ(len, 4u);
    // Prefix 10 indicates 4 bytes
    EXPECT_EQ(buffer_[0] >> 6, 2);
}

TEST_F(VarIntEncodeTest, EncodeFourBytes) {
    size_t len = VarInt::encode(15293, buffer_);
    EXPECT_EQ(len, 2u);  // Actually fits in 2 bytes
    // Let's test a proper 4-byte value
    len = VarInt::encode(100000, buffer_);
    EXPECT_EQ(len, 4u);
}

TEST_F(VarIntEncodeTest, EncodeFourByteMax) {
    size_t len = VarInt::encode(1073741823, buffer_);
    EXPECT_EQ(len, 4u);
    // 1073741823 = 0x3FFFFFFF, with prefix 10 = 0xBFFFFFFF
    EXPECT_EQ(buffer_[0], 0xBF);
    EXPECT_EQ(buffer_[1], 0xFF);
    EXPECT_EQ(buffer_[2], 0xFF);
    EXPECT_EQ(buffer_[3], 0xFF);
}

TEST_F(VarIntEncodeTest, EncodeEightByteMin) {
    size_t len = VarInt::encode(1073741824, buffer_);
    EXPECT_EQ(len, 8u);
    // Prefix 11 indicates 8 bytes
    EXPECT_EQ(buffer_[0] >> 6, 3);
}

TEST_F(VarIntEncodeTest, EncodeEightBytes) {
    size_t len = VarInt::encode(10000000000ULL, buffer_);
    EXPECT_EQ(len, 8u);
}

TEST_F(VarIntEncodeTest, EncodeEightByteMax) {
    // Maximum QUIC VarInt value: 2^62 - 1 = 4611686018427387903
    size_t len = VarInt::encode(4611686018427387903ULL, buffer_);
    EXPECT_EQ(len, 8u);
    // With prefix 11 = 0xFFFFFFFFFFFFFFFF
    EXPECT_EQ(buffer_[0], 0xFF);
}

// ===========================================================================
// Decoding Tests
// ===========================================================================

class VarIntDecodeTest : public ::testing::Test {
protected:
    uint8_t buffer_[16];
    uint64_t value_;
    std::mt19937_64 rng_;

    void SetUp() override {
        memset(buffer_, 0, sizeof(buffer_));
        value_ = 0;
        rng_.seed(std::random_device{}());
    }
};

TEST_F(VarIntDecodeTest, DecodeZero) {
    buffer_[0] = 0x00;
    int consumed = VarInt::decode(buffer_, 1, value_);
    EXPECT_EQ(consumed, 1);
    EXPECT_EQ(value_, 0u);
}

TEST_F(VarIntDecodeTest, DecodeOneByte) {
    buffer_[0] = 37;
    int consumed = VarInt::decode(buffer_, 1, value_);
    EXPECT_EQ(consumed, 1);
    EXPECT_EQ(value_, 37u);
}

TEST_F(VarIntDecodeTest, DecodeOneByteMax) {
    buffer_[0] = 0x3F;
    int consumed = VarInt::decode(buffer_, 1, value_);
    EXPECT_EQ(consumed, 1);
    EXPECT_EQ(value_, 63u);
}

TEST_F(VarIntDecodeTest, DecodeTwoBytes) {
    buffer_[0] = 0x41;
    buffer_[1] = 0xEE;
    int consumed = VarInt::decode(buffer_, 2, value_);
    EXPECT_EQ(consumed, 2);
    EXPECT_EQ(value_, 494u);
}

TEST_F(VarIntDecodeTest, DecodeTwoByteMax) {
    buffer_[0] = 0x7F;
    buffer_[1] = 0xFF;
    int consumed = VarInt::decode(buffer_, 2, value_);
    EXPECT_EQ(consumed, 2);
    EXPECT_EQ(value_, 16383u);
}

TEST_F(VarIntDecodeTest, DecodeFourBytes) {
    // Encode 100000 then decode
    VarInt::encode(100000, buffer_);
    int consumed = VarInt::decode(buffer_, 4, value_);
    EXPECT_EQ(consumed, 4);
    EXPECT_EQ(value_, 100000u);
}

TEST_F(VarIntDecodeTest, DecodeFourByteMax) {
    buffer_[0] = 0xBF;
    buffer_[1] = 0xFF;
    buffer_[2] = 0xFF;
    buffer_[3] = 0xFF;
    int consumed = VarInt::decode(buffer_, 4, value_);
    EXPECT_EQ(consumed, 4);
    EXPECT_EQ(value_, 1073741823u);
}

TEST_F(VarIntDecodeTest, DecodeEightBytes) {
    VarInt::encode(10000000000ULL, buffer_);
    int consumed = VarInt::decode(buffer_, 8, value_);
    EXPECT_EQ(consumed, 8);
    EXPECT_EQ(value_, 10000000000ULL);
}

TEST_F(VarIntDecodeTest, DecodeEightByteMax) {
    VarInt::encode(4611686018427387903ULL, buffer_);
    int consumed = VarInt::decode(buffer_, 8, value_);
    EXPECT_EQ(consumed, 8);
    EXPECT_EQ(value_, 4611686018427387903ULL);
}

TEST_F(VarIntDecodeTest, DecodeInsufficientData) {
    buffer_[0] = 0x40;  // 2-byte prefix but only 1 byte available
    int consumed = VarInt::decode(buffer_, 1, value_);
    EXPECT_EQ(consumed, -1);
}

TEST_F(VarIntDecodeTest, DecodeEmptyBuffer) {
    int consumed = VarInt::decode(buffer_, 0, value_);
    EXPECT_EQ(consumed, -1);
}

TEST_F(VarIntDecodeTest, DecodeFourByteInsufficientData) {
    buffer_[0] = 0x80;  // 4-byte prefix
    buffer_[1] = 0x00;
    int consumed = VarInt::decode(buffer_, 2, value_);  // Only 2 bytes
    EXPECT_EQ(consumed, -1);
}

TEST_F(VarIntDecodeTest, DecodeEightByteInsufficientData) {
    buffer_[0] = 0xC0;  // 8-byte prefix
    buffer_[1] = 0x00;
    buffer_[2] = 0x00;
    buffer_[3] = 0x00;
    int consumed = VarInt::decode(buffer_, 4, value_);  // Only 4 bytes
    EXPECT_EQ(consumed, -1);
}

// ===========================================================================
// Round-trip Tests
// ===========================================================================

class VarIntRoundTripTest : public ::testing::Test {
protected:
    uint8_t buffer_[16];
    std::mt19937_64 rng_;

    void SetUp() override {
        memset(buffer_, 0, sizeof(buffer_));
        rng_.seed(std::random_device{}());
    }
};

TEST_F(VarIntRoundTripTest, AllOneByteBoundaries) {
    for (uint64_t v = 0; v <= 63; v++) {
        size_t len = VarInt::encode(v, buffer_);
        uint64_t decoded;
        int consumed = VarInt::decode(buffer_, len, decoded);
        EXPECT_EQ(consumed, static_cast<int>(len));
        EXPECT_EQ(decoded, v) << "Failed for value " << v;
    }
}

TEST_F(VarIntRoundTripTest, TwoByteBoundaries) {
    std::vector<uint64_t> values = {64, 65, 100, 1000, 10000, 16382, 16383};
    for (uint64_t v : values) {
        size_t len = VarInt::encode(v, buffer_);
        uint64_t decoded;
        int consumed = VarInt::decode(buffer_, len, decoded);
        EXPECT_EQ(consumed, static_cast<int>(len));
        EXPECT_EQ(decoded, v) << "Failed for value " << v;
    }
}

TEST_F(VarIntRoundTripTest, FourByteBoundaries) {
    std::vector<uint64_t> values = {
        16384, 16385, 100000, 1000000, 100000000, 1073741822, 1073741823
    };
    for (uint64_t v : values) {
        size_t len = VarInt::encode(v, buffer_);
        uint64_t decoded;
        int consumed = VarInt::decode(buffer_, len, decoded);
        EXPECT_EQ(consumed, static_cast<int>(len));
        EXPECT_EQ(decoded, v) << "Failed for value " << v;
    }
}

TEST_F(VarIntRoundTripTest, EightByteBoundaries) {
    std::vector<uint64_t> values = {
        1073741824, 1073741825, 10000000000ULL,
        100000000000ULL, 1000000000000ULL,
        4611686018427387902ULL, 4611686018427387903ULL
    };
    for (uint64_t v : values) {
        size_t len = VarInt::encode(v, buffer_);
        uint64_t decoded;
        int consumed = VarInt::decode(buffer_, len, decoded);
        EXPECT_EQ(consumed, static_cast<int>(len));
        EXPECT_EQ(decoded, v) << "Failed for value " << v;
    }
}

TEST_F(VarIntRoundTripTest, RandomOneByte) {
    std::uniform_int_distribution<uint64_t> dist(0, 63);
    for (int i = 0; i < 100; i++) {
        uint64_t v = dist(rng_);
        size_t len = VarInt::encode(v, buffer_);
        EXPECT_EQ(len, 1u);
        uint64_t decoded;
        int consumed = VarInt::decode(buffer_, len, decoded);
        EXPECT_EQ(consumed, 1);
        EXPECT_EQ(decoded, v);
    }
}

TEST_F(VarIntRoundTripTest, RandomTwoByte) {
    std::uniform_int_distribution<uint64_t> dist(64, 16383);
    for (int i = 0; i < 100; i++) {
        uint64_t v = dist(rng_);
        size_t len = VarInt::encode(v, buffer_);
        EXPECT_EQ(len, 2u);
        uint64_t decoded;
        int consumed = VarInt::decode(buffer_, len, decoded);
        EXPECT_EQ(consumed, 2);
        EXPECT_EQ(decoded, v);
    }
}

TEST_F(VarIntRoundTripTest, RandomFourByte) {
    std::uniform_int_distribution<uint64_t> dist(16384, 1073741823);
    for (int i = 0; i < 100; i++) {
        uint64_t v = dist(rng_);
        size_t len = VarInt::encode(v, buffer_);
        EXPECT_EQ(len, 4u);
        uint64_t decoded;
        int consumed = VarInt::decode(buffer_, len, decoded);
        EXPECT_EQ(consumed, 4);
        EXPECT_EQ(decoded, v);
    }
}

TEST_F(VarIntRoundTripTest, RandomEightByte) {
    std::uniform_int_distribution<uint64_t> dist(1073741824, 4611686018427387903ULL);
    for (int i = 0; i < 100; i++) {
        uint64_t v = dist(rng_);
        size_t len = VarInt::encode(v, buffer_);
        EXPECT_EQ(len, 8u);
        uint64_t decoded;
        int consumed = VarInt::decode(buffer_, len, decoded);
        EXPECT_EQ(consumed, 8);
        EXPECT_EQ(decoded, v);
    }
}

TEST_F(VarIntRoundTripTest, RandomMixed) {
    std::uniform_int_distribution<uint64_t> dist(0, 4611686018427387903ULL);
    for (int i = 0; i < 1000; i++) {
        uint64_t v = dist(rng_);
        size_t expected_size = VarInt::encoded_size(v);
        size_t len = VarInt::encode(v, buffer_);
        EXPECT_EQ(len, expected_size);
        uint64_t decoded;
        int consumed = VarInt::decode(buffer_, len, decoded);
        EXPECT_EQ(consumed, static_cast<int>(len));
        EXPECT_EQ(decoded, v);
    }
}

// ===========================================================================
// Boundary Transition Tests
// ===========================================================================

class VarIntBoundaryTest : public ::testing::Test {
protected:
    uint8_t buffer_[16];

    void SetUp() override {
        memset(buffer_, 0, sizeof(buffer_));
    }
};

TEST_F(VarIntBoundaryTest, OneToTwoByteBoundary) {
    // 63 should be 1 byte
    EXPECT_EQ(VarInt::encoded_size(63), 1u);
    EXPECT_EQ(VarInt::encode(63, buffer_), 1u);

    // 64 should be 2 bytes
    EXPECT_EQ(VarInt::encoded_size(64), 2u);
    EXPECT_EQ(VarInt::encode(64, buffer_), 2u);
}

TEST_F(VarIntBoundaryTest, TwoToFourByteBoundary) {
    // 16383 should be 2 bytes
    EXPECT_EQ(VarInt::encoded_size(16383), 2u);
    EXPECT_EQ(VarInt::encode(16383, buffer_), 2u);

    // 16384 should be 4 bytes
    EXPECT_EQ(VarInt::encoded_size(16384), 4u);
    EXPECT_EQ(VarInt::encode(16384, buffer_), 4u);
}

TEST_F(VarIntBoundaryTest, FourToEightByteBoundary) {
    // 1073741823 should be 4 bytes
    EXPECT_EQ(VarInt::encoded_size(1073741823), 4u);
    EXPECT_EQ(VarInt::encode(1073741823, buffer_), 4u);

    // 1073741824 should be 8 bytes
    EXPECT_EQ(VarInt::encoded_size(1073741824), 8u);
    EXPECT_EQ(VarInt::encode(1073741824, buffer_), 8u);
}

// ===========================================================================
// Extra Bytes in Buffer Tests
// ===========================================================================

class VarIntExtraBytesTest : public ::testing::Test {
protected:
    uint8_t buffer_[16];
    uint64_t value_;

    void SetUp() override {
        // Fill with garbage
        for (int i = 0; i < 16; i++) {
            buffer_[i] = 0xFF;
        }
        value_ = 0;
    }
};

TEST_F(VarIntExtraBytesTest, OneByteWithExtra) {
    buffer_[0] = 37;  // 1-byte value
    // buffer_[1..] still 0xFF
    int consumed = VarInt::decode(buffer_, 16, value_);
    EXPECT_EQ(consumed, 1);
    EXPECT_EQ(value_, 37u);
}

TEST_F(VarIntExtraBytesTest, TwoByteWithExtra) {
    VarInt::encode(1000, buffer_);
    int consumed = VarInt::decode(buffer_, 16, value_);
    EXPECT_EQ(consumed, 2);
    EXPECT_EQ(value_, 1000u);
}

TEST_F(VarIntExtraBytesTest, FourByteWithExtra) {
    VarInt::encode(100000, buffer_);
    int consumed = VarInt::decode(buffer_, 16, value_);
    EXPECT_EQ(consumed, 4);
    EXPECT_EQ(value_, 100000u);
}

TEST_F(VarIntExtraBytesTest, EightByteWithExtra) {
    VarInt::encode(10000000000ULL, buffer_);
    int consumed = VarInt::decode(buffer_, 16, value_);
    EXPECT_EQ(consumed, 8);
    EXPECT_EQ(value_, 10000000000ULL);
}

// ===========================================================================
// Performance Benchmarks
// ===========================================================================

class VarIntPerformanceTest : public ::testing::Test {
protected:
    uint8_t buffer_[16];
    std::mt19937_64 rng_;

    void SetUp() override {
        rng_.seed(std::random_device{}());
    }
};

TEST_F(VarIntPerformanceTest, EncodePerformance) {
    const int iterations = 100000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        volatile size_t len1 = VarInt::encode(37, buffer_);
        volatile size_t len2 = VarInt::encode(1000, buffer_);
        volatile size_t len3 = VarInt::encode(100000, buffer_);
        volatile size_t len4 = VarInt::encode(10000000000ULL, buffer_);
        (void)len1;
        (void)len2;
        (void)len3;
        (void)len4;
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / (iterations * 4);

    std::cout << "VarInt Encode: " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 50.0);  // Should be very fast
}

TEST_F(VarIntPerformanceTest, DecodePerformance) {
    const int iterations = 100000;

    // Pre-encode test values
    uint8_t buf1[8], buf2[8], buf3[8], buf4[8];
    VarInt::encode(37, buf1);
    VarInt::encode(1000, buf2);
    VarInt::encode(100000, buf3);
    VarInt::encode(10000000000ULL, buf4);

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        uint64_t v1, v2, v3, v4;
        volatile int r1 = VarInt::decode(buf1, 8, v1);
        volatile int r2 = VarInt::decode(buf2, 8, v2);
        volatile int r3 = VarInt::decode(buf3, 8, v3);
        volatile int r4 = VarInt::decode(buf4, 8, v4);
        (void)r1;
        (void)r2;
        (void)r3;
        (void)r4;
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / (iterations * 4);

    std::cout << "VarInt Decode: " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 50.0);
}

TEST_F(VarIntPerformanceTest, RoundTripPerformance) {
    const int iterations = 50000;

    std::uniform_int_distribution<uint64_t> dist(0, 4611686018427387903ULL);

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        uint64_t v = dist(rng_);
        size_t len = VarInt::encode(v, buffer_);
        uint64_t decoded;
        VarInt::decode(buffer_, len, decoded);
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "VarInt Round-trip: " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 100.0);
}

TEST_F(VarIntPerformanceTest, EncodedSizePerformance) {
    const int iterations = 100000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        volatile size_t s1 = VarInt::encoded_size(37);
        volatile size_t s2 = VarInt::encoded_size(1000);
        volatile size_t s3 = VarInt::encoded_size(100000);
        volatile size_t s4 = VarInt::encoded_size(10000000000ULL);
        (void)s1;
        (void)s2;
        (void)s3;
        (void)s4;
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / (iterations * 4);

    std::cout << "VarInt encoded_size: " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 20.0);  // Should be extremely fast (just comparisons)
}

}  // namespace test
}  // namespace quic
}  // namespace fasterapi

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
