/**
 * QPACK (HTTP/3 Header Compression) Unit Tests
 *
 * Tests the QPACK implementation (RFC 9204):
 * - Static table (99 entries)
 * - Dynamic table operations
 * - Encoder (indexed, literal with name ref, literal)
 * - Decoder (indexed, literal with name ref, literal)
 * - Huffman encoding integration
 * - Round-trip encode/decode
 * - Performance benchmarks
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/qpack/qpack_encoder.h"
#include "../../src/cpp/http/qpack/qpack_decoder.h"
#include "../../src/cpp/http/qpack/qpack_static_table.h"
#include "../../src/cpp/http/qpack/qpack_dynamic_table.h"
#include <random>
#include <chrono>
#include <vector>
#include <string>

namespace fasterapi {
namespace qpack {
namespace test {

// ===========================================================================
// Static Table Tests
// ===========================================================================

class QPACKStaticTableTest : public ::testing::Test {
protected:
    std::mt19937 rng_;

    void SetUp() override {
        rng_.seed(std::random_device{}());
    }
};

TEST_F(QPACKStaticTableTest, Size) {
    EXPECT_EQ(QPACKStaticTable::size(), 99u);
}

TEST_F(QPACKStaticTableTest, GetValidIndices) {
    // Test a sampling of entries
    const StaticEntry* entry0 = QPACKStaticTable::get(0);
    ASSERT_NE(entry0, nullptr);
    EXPECT_EQ(entry0->name, ":authority");
    EXPECT_EQ(entry0->value, "");

    const StaticEntry* entry1 = QPACKStaticTable::get(1);
    ASSERT_NE(entry1, nullptr);
    EXPECT_EQ(entry1->name, ":path");
    EXPECT_EQ(entry1->value, "/");

    const StaticEntry* entry17 = QPACKStaticTable::get(17);
    ASSERT_NE(entry17, nullptr);
    EXPECT_EQ(entry17->name, ":method");
    EXPECT_EQ(entry17->value, "GET");

    const StaticEntry* entry25 = QPACKStaticTable::get(25);
    ASSERT_NE(entry25, nullptr);
    EXPECT_EQ(entry25->name, ":status");
    EXPECT_EQ(entry25->value, "200");

    const StaticEntry* entry98 = QPACKStaticTable::get(98);
    ASSERT_NE(entry98, nullptr);
    EXPECT_EQ(entry98->name, "x-frame-options");
    EXPECT_EQ(entry98->value, "sameorigin");
}

TEST_F(QPACKStaticTableTest, GetInvalidIndex) {
    EXPECT_EQ(QPACKStaticTable::get(99), nullptr);
    EXPECT_EQ(QPACKStaticTable::get(100), nullptr);
    EXPECT_EQ(QPACKStaticTable::get(1000), nullptr);
}

TEST_F(QPACKStaticTableTest, FindExact) {
    EXPECT_EQ(QPACKStaticTable::find(":method", "GET"), 17);
    EXPECT_EQ(QPACKStaticTable::find(":method", "POST"), 20);
    EXPECT_EQ(QPACKStaticTable::find(":status", "200"), 25);
    EXPECT_EQ(QPACKStaticTable::find(":status", "404"), 27);
    EXPECT_EQ(QPACKStaticTable::find("content-type", "application/json"), 46);
    EXPECT_EQ(QPACKStaticTable::find(":path", "/"), 1);
}

TEST_F(QPACKStaticTableTest, FindNotFound) {
    EXPECT_EQ(QPACKStaticTable::find(":method", "PATCH"), -1);
    EXPECT_EQ(QPACKStaticTable::find("x-custom", "value"), -1);
    EXPECT_EQ(QPACKStaticTable::find(":status", "201"), -1);
}

TEST_F(QPACKStaticTableTest, FindName) {
    // :method appears at 15, 16, 17, 18, 19, 20, 21 - should return first
    EXPECT_EQ(QPACKStaticTable::find_name(":method"), 15);
    EXPECT_EQ(QPACKStaticTable::find_name(":status"), 24);  // First :status
    EXPECT_EQ(QPACKStaticTable::find_name("content-type"), 44);
    EXPECT_EQ(QPACKStaticTable::find_name(":authority"), 0);
}

TEST_F(QPACKStaticTableTest, FindNameNotFound) {
    EXPECT_EQ(QPACKStaticTable::find_name("x-custom-header"), -1);
    EXPECT_EQ(QPACKStaticTable::find_name("nonexistent"), -1);
}

TEST_F(QPACKStaticTableTest, AllEntriesAccessible) {
    for (size_t i = 0; i < QPACKStaticTable::size(); i++) {
        const StaticEntry* entry = QPACKStaticTable::get(i);
        ASSERT_NE(entry, nullptr) << "Entry " << i << " is null";
        EXPECT_FALSE(entry->name.empty()) << "Entry " << i << " has empty name";
    }
}

// ===========================================================================
// Dynamic Table Tests
// ===========================================================================

class QPACKDynamicTableTest : public ::testing::Test {
protected:
    std::unique_ptr<QPACKDynamicTable> table_;
    std::mt19937 rng_;

    void SetUp() override {
        table_ = std::make_unique<QPACKDynamicTable>(1024);  // 1KB capacity
        rng_.seed(std::random_device{}());
    }

    std::string random_string(size_t length) {
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

TEST_F(QPACKDynamicTableTest, InitialState) {
    EXPECT_EQ(table_->size(), 0u);
    EXPECT_EQ(table_->capacity(), 1024u);
    EXPECT_EQ(table_->count(), 0u);
    EXPECT_EQ(table_->insert_count(), 0u);
    EXPECT_EQ(table_->drop_count(), 0u);
}

TEST_F(QPACKDynamicTableTest, InsertSingle) {
    EXPECT_TRUE(table_->insert("x-custom", "value123"));
    EXPECT_EQ(table_->count(), 1u);
    EXPECT_EQ(table_->insert_count(), 1u);
    EXPECT_GT(table_->size(), 0u);
}

TEST_F(QPACKDynamicTableTest, InsertMultiple) {
    for (int i = 0; i < 10; i++) {
        std::string name = "header-" + std::to_string(i);
        std::string value = "value-" + std::to_string(i);
        EXPECT_TRUE(table_->insert(name, value));
    }
    EXPECT_EQ(table_->count(), 10u);
    EXPECT_EQ(table_->insert_count(), 10u);
}

TEST_F(QPACKDynamicTableTest, FindByNameAndValue) {
    table_->insert("x-test", "value1");
    table_->insert("x-test", "value2");
    table_->insert("x-other", "value1");

    EXPECT_GE(table_->find("x-test", "value1"), 0);
    EXPECT_GE(table_->find("x-test", "value2"), 0);
    EXPECT_GE(table_->find("x-other", "value1"), 0);
    EXPECT_EQ(table_->find("x-test", "value3"), -1);
    EXPECT_EQ(table_->find("x-missing", "value1"), -1);
}

TEST_F(QPACKDynamicTableTest, FindByName) {
    table_->insert("x-custom", "value1");
    table_->insert("x-custom", "value2");

    EXPECT_GE(table_->find_name("x-custom"), 0);
    EXPECT_EQ(table_->find_name("x-missing"), -1);
}

TEST_F(QPACKDynamicTableTest, GetByAbsoluteIndex) {
    table_->insert("header1", "value1");
    table_->insert("header2", "value2");
    table_->insert("header3", "value3");

    const DynamicEntry* entry0 = table_->get(0);
    ASSERT_NE(entry0, nullptr);
    EXPECT_EQ(entry0->name, "header1");

    const DynamicEntry* entry1 = table_->get(1);
    ASSERT_NE(entry1, nullptr);
    EXPECT_EQ(entry1->name, "header2");

    const DynamicEntry* entry2 = table_->get(2);
    ASSERT_NE(entry2, nullptr);
    EXPECT_EQ(entry2->name, "header3");

    EXPECT_EQ(table_->get(3), nullptr);
}

TEST_F(QPACKDynamicTableTest, GetByRelativeIndex) {
    table_->insert("oldest", "value1");
    table_->insert("middle", "value2");
    table_->insert("newest", "value3");

    // Relative 0 = most recent
    const DynamicEntry* rel0 = table_->get_relative(0);
    ASSERT_NE(rel0, nullptr);
    EXPECT_EQ(rel0->name, "newest");

    const DynamicEntry* rel1 = table_->get_relative(1);
    ASSERT_NE(rel1, nullptr);
    EXPECT_EQ(rel1->name, "middle");

    const DynamicEntry* rel2 = table_->get_relative(2);
    ASSERT_NE(rel2, nullptr);
    EXPECT_EQ(rel2->name, "oldest");

    EXPECT_EQ(table_->get_relative(3), nullptr);
}

TEST_F(QPACKDynamicTableTest, Eviction) {
    // Small capacity table
    QPACKDynamicTable small_table(100);

    // Insert entries until eviction happens
    // Each entry is roughly name.len + value.len + 32
    small_table.insert("a", "12345");  // ~38 bytes
    small_table.insert("b", "12345");  // ~38 bytes - should evict first
    small_table.insert("c", "12345");  // ~38 bytes - should evict second

    EXPECT_LT(small_table.count(), 3u);
    EXPECT_GT(small_table.drop_count(), 0u);
}

TEST_F(QPACKDynamicTableTest, EntryTooLarge) {
    QPACKDynamicTable small_table(50);

    // Entry size = name + value + 32 overhead
    // "big" + (50 chars) + 32 = 85 bytes > 50 capacity
    std::string big_value(50, 'x');
    EXPECT_FALSE(small_table.insert("big", big_value));
    EXPECT_EQ(small_table.count(), 0u);
}

TEST_F(QPACKDynamicTableTest, SetCapacity) {
    table_->insert("header1", "value1");
    table_->insert("header2", "value2");
    table_->insert("header3", "value3");

    size_t original_count = table_->count();
    EXPECT_EQ(original_count, 3u);

    // Shrink capacity to force eviction
    table_->set_capacity(100);
    EXPECT_LT(table_->count(), original_count);
}

TEST_F(QPACKDynamicTableTest, ReferenceCount) {
    table_->insert("header", "value");

    EXPECT_TRUE(table_->increment_reference(0));
    EXPECT_TRUE(table_->increment_reference(0));

    // Entry should have ref_count = 2
    const DynamicEntry* entry = table_->get(0);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->ref_count, 2u);

    EXPECT_TRUE(table_->decrement_reference(0));
    EXPECT_EQ(table_->get(0)->ref_count, 1u);
}

TEST_F(QPACKDynamicTableTest, Clear) {
    table_->insert("header1", "value1");
    table_->insert("header2", "value2");

    table_->clear();

    EXPECT_EQ(table_->count(), 0u);
    EXPECT_EQ(table_->size(), 0u);
    EXPECT_EQ(table_->insert_count(), 0u);
    EXPECT_EQ(table_->drop_count(), 0u);
}

TEST_F(QPACKDynamicTableTest, IndexConversions) {
    table_->insert("first", "v1");
    table_->insert("second", "v2");
    table_->insert("third", "v3");

    // Absolute 0 = oldest (first), relative = 2
    EXPECT_EQ(table_->absolute_to_relative(0), 2);
    // Absolute 2 = newest (third), relative = 0
    EXPECT_EQ(table_->absolute_to_relative(2), 0);

    // Relative 0 = newest, absolute = 2
    EXPECT_EQ(table_->relative_to_absolute(0), 2);
    // Relative 2 = oldest, absolute = 0
    EXPECT_EQ(table_->relative_to_absolute(2), 0);

    // Invalid indices
    EXPECT_EQ(table_->absolute_to_relative(100), -1);
    EXPECT_EQ(table_->relative_to_absolute(100), -1);
}

// ===========================================================================
// Encoder Tests
// ===========================================================================

class QPACKEncoderTest : public ::testing::Test {
protected:
    std::unique_ptr<QPACKEncoder> encoder_;
    uint8_t buffer_[4096];
    std::mt19937 rng_;

    void SetUp() override {
        encoder_ = std::make_unique<QPACKEncoder>(4096, 100);
        rng_.seed(std::random_device{}());
    }

    std::string random_string(size_t length) {
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

TEST_F(QPACKEncoderTest, EncodeStaticIndexed) {
    // :method: GET should be fully indexed (static table entry 17)
    std::pair<std::string_view, std::string_view> headers[] = {
        {":method", "GET"}
    };

    size_t encoded_len = 0;
    int result = encoder_->encode_field_section(headers, 1, buffer_,
                                                sizeof(buffer_), encoded_len);
    EXPECT_EQ(result, 0);
    EXPECT_GT(encoded_len, 0u);

    // Should be very compact - prefix + indexed field
    EXPECT_LE(encoded_len, 5u);
}

TEST_F(QPACKEncoderTest, EncodeMultipleStaticIndexed) {
    std::pair<std::string_view, std::string_view> headers[] = {
        {":method", "GET"},
        {":path", "/"},
        {":scheme", "https"},
        {":status", "200"}
    };

    size_t encoded_len = 0;
    int result = encoder_->encode_field_section(headers, 4, buffer_,
                                                sizeof(buffer_), encoded_len);
    EXPECT_EQ(result, 0);
    EXPECT_GT(encoded_len, 0u);
}

TEST_F(QPACKEncoderTest, EncodeLiteralWithNameRef) {
    // :authority with custom value - name is in static table
    std::pair<std::string_view, std::string_view> headers[] = {
        {":authority", "example.com"}
    };

    size_t encoded_len = 0;
    int result = encoder_->encode_field_section(headers, 1, buffer_,
                                                sizeof(buffer_), encoded_len);
    EXPECT_EQ(result, 0);
    EXPECT_GT(encoded_len, 0u);
}

TEST_F(QPACKEncoderTest, EncodeLiteralWithLiteralName) {
    // Completely custom header
    std::pair<std::string_view, std::string_view> headers[] = {
        {"x-custom-header", "custom-value-123"}
    };

    size_t encoded_len = 0;
    int result = encoder_->encode_field_section(headers, 1, buffer_,
                                                sizeof(buffer_), encoded_len);
    EXPECT_EQ(result, 0);
    EXPECT_GT(encoded_len, 0u);
}

TEST_F(QPACKEncoderTest, EncodeWithHuffman) {
    encoder_->set_huffman_encoding(true);

    std::pair<std::string_view, std::string_view> headers[] = {
        {"x-custom", "this is a longer value that should compress well"}
    };

    size_t encoded_len = 0;
    int result = encoder_->encode_field_section(headers, 1, buffer_,
                                                sizeof(buffer_), encoded_len);
    EXPECT_EQ(result, 0);
    EXPECT_GT(encoded_len, 0u);
}

TEST_F(QPACKEncoderTest, EncodeWithoutHuffman) {
    encoder_->set_huffman_encoding(false);

    std::pair<std::string_view, std::string_view> headers[] = {
        {"x-custom", "value"}
    };

    size_t encoded_len = 0;
    int result = encoder_->encode_field_section(headers, 1, buffer_,
                                                sizeof(buffer_), encoded_len);
    EXPECT_EQ(result, 0);
    EXPECT_GT(encoded_len, 0u);
}

TEST_F(QPACKEncoderTest, EncodeBufferTooSmall) {
    std::pair<std::string_view, std::string_view> headers[] = {
        {"x-custom", "this is a very long value that will not fit in a tiny buffer"}
    };

    size_t encoded_len = 0;
    int result = encoder_->encode_field_section(headers, 1, buffer_, 5, encoded_len);
    EXPECT_EQ(result, -1);  // Should fail
}

TEST_F(QPACKEncoderTest, EncodeEmptyHeaders) {
    size_t encoded_len = 0;
    int result = encoder_->encode_field_section(nullptr, 0, buffer_,
                                                sizeof(buffer_), encoded_len);
    EXPECT_EQ(result, 0);
    // Should only contain prefix
    EXPECT_GT(encoded_len, 0u);
    EXPECT_LE(encoded_len, 4u);
}

TEST_F(QPACKEncoderTest, EncodeManyHeaders) {
    std::vector<std::pair<std::string, std::string>> storage;
    std::vector<std::pair<std::string_view, std::string_view>> headers;

    for (int i = 0; i < 20; i++) {
        storage.emplace_back("x-header-" + std::to_string(i),
                            "value-" + std::to_string(i));
    }
    for (const auto& h : storage) {
        headers.emplace_back(h.first, h.second);
    }

    size_t encoded_len = 0;
    int result = encoder_->encode_field_section(headers.data(), headers.size(),
                                                buffer_, sizeof(buffer_), encoded_len);
    EXPECT_EQ(result, 0);
    EXPECT_GT(encoded_len, 0u);
}

// ===========================================================================
// Decoder Tests
// ===========================================================================

class QPACKDecoderTest : public ::testing::Test {
protected:
    std::unique_ptr<QPACKDecoder> decoder_;
    std::pair<std::string, std::string> headers_[256];
    std::mt19937 rng_;

    void SetUp() override {
        decoder_ = std::make_unique<QPACKDecoder>(4096);
        rng_.seed(std::random_device{}());
    }
};

TEST_F(QPACKDecoderTest, DecodeEmptyInput) {
    size_t count = 0;
    int result = decoder_->decode_field_section(nullptr, 0, headers_, count);
    EXPECT_EQ(result, -1);  // Empty input is an error
}

TEST_F(QPACKDecoderTest, DecodeMalformedInput) {
    // Just garbage bytes
    uint8_t garbage[] = {0xFF, 0xFF, 0xFF};
    size_t count = 0;
    int result = decoder_->decode_field_section(garbage, sizeof(garbage), headers_, count);
    // May or may not decode depending on interpretation - just shouldn't crash
    (void)result;
}

// ===========================================================================
// Round-trip Encode/Decode Tests
// ===========================================================================

class QPACKRoundTripTest : public ::testing::Test {
protected:
    std::unique_ptr<QPACKEncoder> encoder_;
    std::unique_ptr<QPACKDecoder> decoder_;
    uint8_t buffer_[8192];
    std::pair<std::string, std::string> decoded_headers_[256];
    std::mt19937 rng_;

    void SetUp() override {
        encoder_ = std::make_unique<QPACKEncoder>(4096, 100);
        decoder_ = std::make_unique<QPACKDecoder>(4096);
        rng_.seed(std::random_device{}());
    }

    std::string random_string(size_t length) {
        static const char charset[] = "abcdefghijklmnopqrstuvwxyz0123456789-_";
        std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);
        std::string result;
        result.reserve(length);
        for (size_t i = 0; i < length; ++i) {
            result += charset[dist(rng_)];
        }
        return result;
    }
};

TEST_F(QPACKRoundTripTest, StaticTableHeaders) {
    std::pair<std::string_view, std::string_view> headers[] = {
        {":method", "GET"},
        {":path", "/"},
        {":scheme", "https"}
    };

    // Encode
    size_t encoded_len = 0;
    int enc_result = encoder_->encode_field_section(headers, 3, buffer_,
                                                    sizeof(buffer_), encoded_len);
    EXPECT_EQ(enc_result, 0);

    // Decode
    size_t decoded_count = 0;
    int dec_result = decoder_->decode_field_section(buffer_, encoded_len,
                                                    decoded_headers_, decoded_count);
    EXPECT_EQ(dec_result, 0);
    EXPECT_EQ(decoded_count, 3u);

    EXPECT_EQ(decoded_headers_[0].first, ":method");
    EXPECT_EQ(decoded_headers_[0].second, "GET");
    EXPECT_EQ(decoded_headers_[1].first, ":path");
    EXPECT_EQ(decoded_headers_[1].second, "/");
    EXPECT_EQ(decoded_headers_[2].first, ":scheme");
    EXPECT_EQ(decoded_headers_[2].second, "https");
}

TEST_F(QPACKRoundTripTest, LiteralWithNameRef) {
    std::pair<std::string_view, std::string_view> headers[] = {
        {":authority", "api.example.com"},
        {"content-type", "application/json"}
    };

    size_t encoded_len = 0;
    int enc_result = encoder_->encode_field_section(headers, 2, buffer_,
                                                    sizeof(buffer_), encoded_len);
    EXPECT_EQ(enc_result, 0);

    size_t decoded_count = 0;
    int dec_result = decoder_->decode_field_section(buffer_, encoded_len,
                                                    decoded_headers_, decoded_count);
    EXPECT_EQ(dec_result, 0);
    EXPECT_EQ(decoded_count, 2u);

    EXPECT_EQ(decoded_headers_[0].first, ":authority");
    EXPECT_EQ(decoded_headers_[0].second, "api.example.com");
}

TEST_F(QPACKRoundTripTest, CustomHeaders) {
    std::pair<std::string_view, std::string_view> headers[] = {
        {"x-request-id", "abc-123-def-456"},
        {"x-correlation-id", "corr-789"},
        {"x-timestamp", "1234567890"}
    };

    size_t encoded_len = 0;
    int enc_result = encoder_->encode_field_section(headers, 3, buffer_,
                                                    sizeof(buffer_), encoded_len);
    EXPECT_EQ(enc_result, 0);

    size_t decoded_count = 0;
    int dec_result = decoder_->decode_field_section(buffer_, encoded_len,
                                                    decoded_headers_, decoded_count);
    EXPECT_EQ(dec_result, 0);
    EXPECT_EQ(decoded_count, 3u);

    EXPECT_EQ(decoded_headers_[0].first, "x-request-id");
    EXPECT_EQ(decoded_headers_[0].second, "abc-123-def-456");
    EXPECT_EQ(decoded_headers_[1].first, "x-correlation-id");
    EXPECT_EQ(decoded_headers_[1].second, "corr-789");
}

TEST_F(QPACKRoundTripTest, MixedHeaders) {
    std::pair<std::string_view, std::string_view> headers[] = {
        {":method", "POST"},                    // Fully indexed
        {":path", "/api/v1/users"},             // Name ref + literal value
        {"content-type", "application/json"},   // Fully indexed
        {"x-api-key", "secret-key-12345"},      // All literal
        {"authorization", "Bearer token123"}    // Name ref + literal value
    };

    size_t encoded_len = 0;
    int enc_result = encoder_->encode_field_section(headers, 5, buffer_,
                                                    sizeof(buffer_), encoded_len);
    EXPECT_EQ(enc_result, 0);

    size_t decoded_count = 0;
    int dec_result = decoder_->decode_field_section(buffer_, encoded_len,
                                                    decoded_headers_, decoded_count);
    EXPECT_EQ(dec_result, 0);
    EXPECT_EQ(decoded_count, 5u);

    EXPECT_EQ(decoded_headers_[0].first, ":method");
    EXPECT_EQ(decoded_headers_[0].second, "POST");
    EXPECT_EQ(decoded_headers_[1].first, ":path");
    EXPECT_EQ(decoded_headers_[1].second, "/api/v1/users");
    EXPECT_EQ(decoded_headers_[3].first, "x-api-key");
    EXPECT_EQ(decoded_headers_[3].second, "secret-key-12345");
}

TEST_F(QPACKRoundTripTest, LongValues) {
    std::string long_value = random_string(500);
    std::pair<std::string, std::string> storage[] = {
        {"x-long-header", long_value}
    };
    std::pair<std::string_view, std::string_view> headers[] = {
        {storage[0].first, storage[0].second}
    };

    size_t encoded_len = 0;
    int enc_result = encoder_->encode_field_section(headers, 1, buffer_,
                                                    sizeof(buffer_), encoded_len);
    EXPECT_EQ(enc_result, 0);

    size_t decoded_count = 0;
    int dec_result = decoder_->decode_field_section(buffer_, encoded_len,
                                                    decoded_headers_, decoded_count);
    EXPECT_EQ(dec_result, 0);
    EXPECT_EQ(decoded_count, 1u);
    EXPECT_EQ(decoded_headers_[0].second, long_value);
}

TEST_F(QPACKRoundTripTest, RandomHeaders) {
    std::uniform_int_distribution<> count_dist(5, 20);
    std::uniform_int_distribution<> name_len_dist(5, 30);
    std::uniform_int_distribution<> value_len_dist(5, 100);

    int header_count = count_dist(rng_);

    std::vector<std::pair<std::string, std::string>> storage;
    std::vector<std::pair<std::string_view, std::string_view>> headers;

    for (int i = 0; i < header_count; i++) {
        std::string name = "x-" + random_string(name_len_dist(rng_));
        std::string value = random_string(value_len_dist(rng_));
        storage.emplace_back(name, value);
    }

    for (const auto& h : storage) {
        headers.emplace_back(h.first, h.second);
    }

    size_t encoded_len = 0;
    int enc_result = encoder_->encode_field_section(headers.data(), headers.size(),
                                                    buffer_, sizeof(buffer_), encoded_len);
    EXPECT_EQ(enc_result, 0);

    size_t decoded_count = 0;
    int dec_result = decoder_->decode_field_section(buffer_, encoded_len,
                                                    decoded_headers_, decoded_count);
    EXPECT_EQ(dec_result, 0);
    EXPECT_EQ(decoded_count, static_cast<size_t>(header_count));

    // Verify all headers match
    for (int i = 0; i < header_count; i++) {
        EXPECT_EQ(decoded_headers_[i].first, storage[i].first)
            << "Header " << i << " name mismatch";
        EXPECT_EQ(decoded_headers_[i].second, storage[i].second)
            << "Header " << i << " value mismatch";
    }
}

TEST_F(QPACKRoundTripTest, HuffmanEnabled) {
    encoder_->set_huffman_encoding(true);

    std::pair<std::string_view, std::string_view> headers[] = {
        {"x-test", "this is a test value that should compress nicely"},
        {"x-another", "more compressible text here"}
    };

    size_t encoded_len = 0;
    int enc_result = encoder_->encode_field_section(headers, 2, buffer_,
                                                    sizeof(buffer_), encoded_len);
    EXPECT_EQ(enc_result, 0);

    size_t decoded_count = 0;
    int dec_result = decoder_->decode_field_section(buffer_, encoded_len,
                                                    decoded_headers_, decoded_count);
    EXPECT_EQ(dec_result, 0);
    EXPECT_EQ(decoded_count, 2u);

    EXPECT_EQ(decoded_headers_[0].second, "this is a test value that should compress nicely");
    EXPECT_EQ(decoded_headers_[1].second, "more compressible text here");
}

TEST_F(QPACKRoundTripTest, HuffmanDisabled) {
    encoder_->set_huffman_encoding(false);

    std::pair<std::string_view, std::string_view> headers[] = {
        {"x-test", "plain text value"}
    };

    size_t encoded_len = 0;
    int enc_result = encoder_->encode_field_section(headers, 1, buffer_,
                                                    sizeof(buffer_), encoded_len);
    EXPECT_EQ(enc_result, 0);

    size_t decoded_count = 0;
    int dec_result = decoder_->decode_field_section(buffer_, encoded_len,
                                                    decoded_headers_, decoded_count);
    EXPECT_EQ(dec_result, 0);
    EXPECT_EQ(decoded_count, 1u);

    EXPECT_EQ(decoded_headers_[0].second, "plain text value");
}

TEST_F(QPACKRoundTripTest, CommonHTTPRequest) {
    std::pair<std::string_view, std::string_view> headers[] = {
        {":method", "GET"},
        {":path", "/api/users?limit=10"},
        {":scheme", "https"},
        {":authority", "api.example.com"},
        {"accept", "application/json"},
        {"accept-encoding", "gzip, deflate, br"},
        {"user-agent", ""},
        {"authorization", "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"}
    };

    size_t encoded_len = 0;
    int enc_result = encoder_->encode_field_section(headers, 8, buffer_,
                                                    sizeof(buffer_), encoded_len);
    EXPECT_EQ(enc_result, 0);

    size_t decoded_count = 0;
    int dec_result = decoder_->decode_field_section(buffer_, encoded_len,
                                                    decoded_headers_, decoded_count);
    EXPECT_EQ(dec_result, 0);
    EXPECT_EQ(decoded_count, 8u);
}

TEST_F(QPACKRoundTripTest, CommonHTTPResponse) {
    std::pair<std::string_view, std::string_view> headers[] = {
        {":status", "200"},
        {"content-type", "application/json"},
        {"content-length", "0"},
        {"cache-control", "no-cache"},
        {"x-request-id", "uuid-12345-abcde"}
    };

    size_t encoded_len = 0;
    int enc_result = encoder_->encode_field_section(headers, 5, buffer_,
                                                    sizeof(buffer_), encoded_len);
    EXPECT_EQ(enc_result, 0);

    size_t decoded_count = 0;
    int dec_result = decoder_->decode_field_section(buffer_, encoded_len,
                                                    decoded_headers_, decoded_count);
    EXPECT_EQ(dec_result, 0);
    EXPECT_EQ(decoded_count, 5u);

    EXPECT_EQ(decoded_headers_[0].first, ":status");
    EXPECT_EQ(decoded_headers_[0].second, "200");
}

// ===========================================================================
// Performance Benchmarks
// ===========================================================================

class QPACKPerformanceTest : public ::testing::Test {
protected:
    std::unique_ptr<QPACKEncoder> encoder_;
    std::unique_ptr<QPACKDecoder> decoder_;
    uint8_t buffer_[8192];
    std::pair<std::string, std::string> decoded_headers_[256];

    void SetUp() override {
        encoder_ = std::make_unique<QPACKEncoder>(4096, 100);
        decoder_ = std::make_unique<QPACKDecoder>(4096);
    }
};

TEST_F(QPACKPerformanceTest, EncodePerformance) {
    std::pair<std::string_view, std::string_view> headers[] = {
        {":method", "GET"},
        {":path", "/api/v1/users"},
        {":scheme", "https"},
        {":authority", "api.example.com"},
        {"content-type", "application/json"},
        {"accept-encoding", "gzip, deflate, br"}
    };

    const int iterations = 10000;
    size_t total_encoded = 0;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        size_t encoded_len = 0;
        encoder_->encode_field_section(headers, 6, buffer_, sizeof(buffer_), encoded_len);
        total_encoded += encoded_len;
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "QPACK Encode (6 headers): " << ns_per_op << " ns/op\n";
    std::cout << "Total encoded: " << total_encoded << " bytes\n";
    EXPECT_LT(ns_per_op, 10000.0);  // Should be well under 10us
}

TEST_F(QPACKPerformanceTest, DecodePerformance) {
    std::pair<std::string_view, std::string_view> headers[] = {
        {":method", "GET"},
        {":path", "/api/v1/users"},
        {":scheme", "https"},
        {":authority", "api.example.com"},
        {"content-type", "application/json"},
        {"accept-encoding", "gzip, deflate, br"}
    };

    // First encode
    size_t encoded_len = 0;
    encoder_->encode_field_section(headers, 6, buffer_, sizeof(buffer_), encoded_len);

    const int iterations = 10000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        size_t count = 0;
        decoder_->decode_field_section(buffer_, encoded_len, decoded_headers_, count);
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "QPACK Decode (6 headers): " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 10000.0);
}

TEST_F(QPACKPerformanceTest, RoundTripPerformance) {
    std::pair<std::string_view, std::string_view> headers[] = {
        {":method", "POST"},
        {":path", "/api/v2/data"},
        {":scheme", "https"},
        {":authority", "api.example.com"},
        {"content-type", "application/json"},
        {"content-length", "0"},
        {"x-request-id", "uuid-12345"},
        {"authorization", "Bearer token"}
    };

    const int iterations = 5000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        size_t encoded_len = 0;
        encoder_->encode_field_section(headers, 8, buffer_, sizeof(buffer_), encoded_len);

        size_t count = 0;
        decoder_->decode_field_section(buffer_, encoded_len, decoded_headers_, count);
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "QPACK Round-trip (8 headers): " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 20000.0);
}

TEST_F(QPACKPerformanceTest, StaticTableLookup) {
    const int iterations = 100000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        volatile int idx1 = QPACKStaticTable::find(":method", "GET");
        volatile int idx2 = QPACKStaticTable::find(":status", "200");
        volatile int idx3 = QPACKStaticTable::find("content-type", "application/json");
        (void)idx1;
        (void)idx2;
        (void)idx3;
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "Static Table Lookup (3 finds): " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 1000.0);  // Should be very fast
}

}  // namespace test
}  // namespace qpack
}  // namespace fasterapi

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
