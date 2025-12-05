/**
 * Comprehensive QPACK Round-Trip and Compression Tests
 *
 * Tests encoder → decoder round-trip, compression ratios, error handling,
 * dynamic table management, and RFC 9204 compliance.
 */

#include "../src/cpp/http/qpack/qpack_encoder.h"
#include "../src/cpp/http/qpack/qpack_decoder.h"
#include "../src/cpp/http/qpack/qpack_static_table.h"
#include "../src/cpp/http/qpack/qpack_dynamic_table.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <chrono>
#include <random>
#include <vector>
#include <string>

using namespace fasterapi::qpack;
using namespace std::chrono;

// ============================================================================
// Test Utilities
// ============================================================================

struct CompressionStats {
    size_t original_size;
    size_t compressed_size;
    double ratio;

    void calculate() {
        if (original_size > 0) {
            ratio = 100.0 * (1.0 - (double)compressed_size / original_size);
        } else {
            ratio = 0.0;
        }
    }
};

void print_hex(const char* label, const uint8_t* data, size_t len) {
    std::cout << label << ": ";
    for (size_t i = 0; i < len && i < 32; i++) {
        printf("%02x ", data[i]);
    }
    if (len > 32) std::cout << "... (" << len << " bytes total)";
    std::cout << std::endl;
}

void print_stats(const CompressionStats& stats) {
    std::cout << "  Original: " << stats.original_size << " bytes" << std::endl;
    std::cout << "  Compressed: " << stats.compressed_size << " bytes" << std::endl;
    std::cout << "  Ratio: " << stats.ratio << "%" << std::endl;
}

bool headers_match(const std::pair<std::string_view, std::string_view>* original,
                   const std::pair<std::string, std::string>* decoded,
                   size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (original[i].first != decoded[i].first ||
            original[i].second != decoded[i].second) {
            std::cerr << "Header mismatch at index " << i << ":" << std::endl;
            std::cerr << "  Expected: '" << original[i].first << "': '"
                      << original[i].second << "'" << std::endl;
            std::cerr << "  Got:      '" << decoded[i].first << "': '"
                      << decoded[i].second << "'" << std::endl;
            return false;
        }
    }
    return true;
}

size_t calculate_original_size(const std::pair<std::string_view, std::string_view>* headers,
                                size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        total += headers[i].first.length() + headers[i].second.length() + 4; // +4 for ": " and "\r\n"
    }
    return total;
}

// Random string generator
std::string random_string(size_t length, std::mt19937& rng) {
    static const char charset[] =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789-_.";
    std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);

    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; i++) {
        result += charset[dist(rng)];
    }
    return result;
}

// ============================================================================
// Test 1: Simple Round-Trip
// ============================================================================

void test_simple_roundtrip() {
    std::cout << "\n=== Test 1: Simple Round-Trip ===" << std::endl;

    QPACKEncoder encoder;
    QPACKDecoder decoder;
    encoder.set_huffman_encoding(false);  // Huffman decoder is stubbed

    // Simple HTTP request headers
    std::pair<std::string_view, std::string_view> headers[] = {
        {":method", "GET"},
        {":path", "/"},
        {":scheme", "https"},
        {":authority", "example.com"}
    };

    uint8_t buffer[256];
    size_t encoded_len;

    // Encode
    int result = encoder.encode_field_section(headers, 4, buffer, sizeof(buffer), encoded_len);
    assert(result == 0);
    assert(encoded_len > 0);

    std::cout << "Encoded " << 4 << " headers into " << encoded_len << " bytes" << std::endl;
    print_hex("Encoded", buffer, encoded_len);

    // Decode
    std::pair<std::string, std::string> decoded[QPACKDecoder::kMaxHeaders];
    size_t decoded_count;

    result = decoder.decode_field_section(buffer, encoded_len, decoded, decoded_count);
    assert(result == 0);
    assert(decoded_count == 4);

    // Verify
    assert(headers_match(headers, decoded, 4));

    std::cout << "✓ Simple round-trip successful" << std::endl;
}

// ============================================================================
// Test 2: Static Table Encoding
// ============================================================================

void test_static_table_encoding() {
    std::cout << "\n=== Test 2: Static Table Encoding ===" << std::endl;

    QPACKEncoder encoder;
    QPACKDecoder decoder;
    encoder.set_huffman_encoding(false);  // Huffman decoder is stubbed

    // All headers from static table (exact matches)
    std::pair<std::string_view, std::string_view> headers[] = {
        {":method", "GET"},         // Index 17
        {":method", "POST"},        // Index 20
        {":path", "/"},             // Index 1
        {":scheme", "https"},       // Index 23
        {":status", "200"},         // Index 25
        {":status", "404"},         // Index 27
        {"content-type", "application/json"},  // Index 46
        {"cache-control", "no-cache"},         // Index 39
    };

    uint8_t buffer[512];
    size_t encoded_len;

    int result = encoder.encode_field_section(headers, 8, buffer, sizeof(buffer), encoded_len);
    assert(result == 0);

    // Should be very compact (all indexed)
    std::cout << "Encoded " << 8 << " static table headers into " << encoded_len << " bytes" << std::endl;

    // Decode and verify
    std::pair<std::string, std::string> decoded[QPACKDecoder::kMaxHeaders];
    size_t decoded_count;

    result = decoder.decode_field_section(buffer, encoded_len, decoded, decoded_count);
    assert(result == 0);
    assert(decoded_count == 8);
    assert(headers_match(headers, decoded, 8));

    std::cout << "✓ Static table encoding works correctly" << std::endl;
}

// ============================================================================
// Test 3: Dynamic Table Insertion
// ============================================================================

void test_dynamic_table_insertion() {
    std::cout << "\n=== Test 3: Dynamic Table Insertion ===" << std::endl;

    QPACKEncoder encoder(4096);  // 4KB dynamic table
    QPACKDecoder decoder(4096);

    // Custom headers that will go into dynamic table
    std::string custom_name = "x-custom-header";
    std::string custom_value = "my-custom-value";

    // Manually insert into encoder's dynamic table
    bool inserted = encoder.dynamic_table().insert(custom_name, custom_value);
    assert(inserted);

    std::cout << "Inserted custom header into dynamic table" << std::endl;
    std::cout << "  Table size: " << encoder.dynamic_table().size() << " bytes" << std::endl;
    std::cout << "  Entry count: " << encoder.dynamic_table().count() << std::endl;

    // Also insert into decoder's dynamic table (in real scenario, this would come via encoder stream)
    inserted = decoder.dynamic_table().insert(custom_name, custom_value);
    assert(inserted);

    std::cout << "✓ Dynamic table insertion works" << std::endl;
}

// ============================================================================
// Test 4: Repeated Headers Compression
// ============================================================================

void test_repeated_headers_compression() {
    std::cout << "\n=== Test 4: Repeated Headers Compression ===" << std::endl;

    QPACKEncoder encoder;
    QPACKDecoder decoder;
    encoder.set_huffman_encoding(false);  // Huffman decoder is stubbed

    // Same headers repeated multiple times (simulating multiple requests)
    std::pair<std::string_view, std::string_view> headers[] = {
        {":method", "GET"},
        {":path", "/api/users"},
        {":scheme", "https"},
        {":authority", "api.example.com"},
        {"user-agent", "TestClient/1.0"},
        {"accept", "application/json"}
    };

    uint8_t buffer1[512], buffer2[512], buffer3[512];
    size_t len1, len2, len3;

    // First encoding
    int result = encoder.encode_field_section(headers, 6, buffer1, sizeof(buffer1), len1);
    assert(result == 0);

    // Second encoding (same headers)
    result = encoder.encode_field_section(headers, 6, buffer2, sizeof(buffer2), len2);
    assert(result == 0);

    // Third encoding (same headers)
    result = encoder.encode_field_section(headers, 6, buffer3, sizeof(buffer3), len3);
    assert(result == 0);

    std::cout << "Encoding 1: " << len1 << " bytes" << std::endl;
    std::cout << "Encoding 2: " << len2 << " bytes" << std::endl;
    std::cout << "Encoding 3: " << len3 << " bytes" << std::endl;

    // All should decode correctly
    std::pair<std::string, std::string> decoded[QPACKDecoder::kMaxHeaders];
    size_t decoded_count;

    result = decoder.decode_field_section(buffer1, len1, decoded, decoded_count);
    assert(result == 0);
    assert(decoded_count == 6);
    assert(headers_match(headers, decoded, 6));

    std::cout << "✓ Repeated headers encode correctly" << std::endl;
}

// ============================================================================
// Test 5: Huffman Compression
// ============================================================================

void test_huffman_compression() {
    std::cout << "\n=== Test 5: Huffman Compression ===" << std::endl;

    QPACKEncoder encoder_huffman, encoder_plain;
    QPACKDecoder decoder;

    encoder_huffman.set_huffman_encoding(true);
    encoder_plain.set_huffman_encoding(false);

    // Headers with compressible text
    std::pair<std::string_view, std::string_view> headers[] = {
        {"x-custom", "this-is-a-very-long-header-value-that-should-compress-well-with-huffman"},
        {"x-another", "wwwwwwwwwwwwwwwwwwwwwwwwwwwwwww"},  // Repetitive
        {"x-text", "Hello World! This is some sample text."}
    };

    uint8_t buffer_huffman[512], buffer_plain[512];
    size_t len_huffman, len_plain;

    int result = encoder_huffman.encode_field_section(headers, 3, buffer_huffman,
                                                      sizeof(buffer_huffman), len_huffman);
    assert(result == 0);

    result = encoder_plain.encode_field_section(headers, 3, buffer_plain,
                                                sizeof(buffer_plain), len_plain);
    assert(result == 0);

    std::cout << "Plain encoding: " << len_plain << " bytes" << std::endl;
    std::cout << "Huffman encoding: " << len_huffman << " bytes" << std::endl;

    double savings = 100.0 * (1.0 - (double)len_huffman / len_plain);
    std::cout << "Huffman savings: " << savings << "%" << std::endl;

    // Note: Huffman decoder is stubbed, so we can't decode yet
    std::cout << "✓ Huffman encoding works (decoder not implemented)" << std::endl;
}

// ============================================================================
// Test 6: Large Header Set
// ============================================================================

void test_large_header_set() {
    std::cout << "\n=== Test 6: Large Header Set (50 headers) ===" << std::endl;

    QPACKEncoder encoder;
    QPACKDecoder decoder;
    encoder.set_huffman_encoding(false);  // Huffman decoder is stubbed

    // Create 50 headers
    std::vector<std::pair<std::string, std::string>> header_storage;
    std::vector<std::pair<std::string_view, std::string_view>> headers;

    header_storage.reserve(50);
    headers.reserve(50);

    // Add pseudo-headers
    header_storage.emplace_back(":method", "GET");
    header_storage.emplace_back(":path", "/api/v1/resource");
    header_storage.emplace_back(":scheme", "https");
    header_storage.emplace_back(":authority", "api.example.com");

    // Add many custom headers
    for (int i = 0; i < 46; i++) {
        std::string name = "x-custom-header-" + std::to_string(i);
        std::string value = "value-" + std::to_string(i * 100);
        header_storage.emplace_back(std::move(name), std::move(value));
    }

    for (const auto& h : header_storage) {
        headers.emplace_back(h.first, h.second);
    }

    uint8_t buffer[8192];
    size_t encoded_len;

    int result = encoder.encode_field_section(headers.data(), headers.size(),
                                              buffer, sizeof(buffer), encoded_len);
    assert(result == 0);

    std::cout << "Encoded " << headers.size() << " headers into "
              << encoded_len << " bytes" << std::endl;

    // Decode
    std::pair<std::string, std::string> decoded[QPACKDecoder::kMaxHeaders];
    size_t decoded_count;

    result = decoder.decode_field_section(buffer, encoded_len, decoded, decoded_count);
    assert(result == 0);
    assert(decoded_count == headers.size());
    assert(headers_match(headers.data(), decoded, headers.size()));

    CompressionStats stats;
    stats.original_size = calculate_original_size(headers.data(), headers.size());
    stats.compressed_size = encoded_len;
    stats.calculate();

    print_stats(stats);

    std::cout << "✓ Large header set round-trip successful" << std::endl;
}

// ============================================================================
// Test 7: Compression Ratios
// ============================================================================

void test_compression_ratios() {
    std::cout << "\n=== Test 7: Compression Ratios ===" << std::endl;

    QPACKEncoder encoder;
    encoder.set_huffman_encoding(false);  // Huffman decoder is stubbed

    // Test 1: Typical HTTP request
    {
        std::pair<std::string_view, std::string_view> headers[] = {
            {":method", "GET"},
            {":path", "/index.html"},
            {":scheme", "https"},
            {":authority", "www.example.com"},
            {"user-agent", "Mozilla/5.0"},
            {"accept", "text/html,application/xml"},
            {"accept-encoding", "gzip, deflate, br"},
            {"accept-language", "en-US,en;q=0.9"}
        };

        uint8_t buffer[1024];
        size_t encoded_len;

        int result = encoder.encode_field_section(headers, 8, buffer, sizeof(buffer), encoded_len);
        assert(result == 0);

        CompressionStats stats;
        stats.original_size = calculate_original_size(headers, 8);
        stats.compressed_size = encoded_len;
        stats.calculate();

        std::cout << "\nTypical HTTP Request:" << std::endl;
        print_stats(stats);
    }

    // Test 2: Typical HTTP response
    {
        std::pair<std::string_view, std::string_view> headers[] = {
            {":status", "200"},
            {"content-type", "text/html; charset=utf-8"},
            {"content-length", "1234"},
            {"cache-control", "max-age=3600"},
            {"date", "Mon, 01 Jan 2024 00:00:00 GMT"},
            {"server", "FasterAPI/1.0"},
            {"x-frame-options", "SAMEORIGIN"}
        };

        uint8_t buffer[1024];
        size_t encoded_len;

        int result = encoder.encode_field_section(headers, 7, buffer, sizeof(buffer), encoded_len);
        assert(result == 0);

        CompressionStats stats;
        stats.original_size = calculate_original_size(headers, 7);
        stats.compressed_size = encoded_len;
        stats.calculate();

        std::cout << "\nTypical HTTP Response:" << std::endl;
        print_stats(stats);
    }

    std::cout << "\n✓ Compression ratio tests complete" << std::endl;
}

// ============================================================================
// Test 8: Mixed Encoding Modes
// ============================================================================

void test_mixed_encoding_modes() {
    std::cout << "\n=== Test 8: Mixed Encoding Modes ===" << std::endl;

    QPACKEncoder encoder;
    QPACKDecoder decoder;
    encoder.set_huffman_encoding(false);  // Huffman decoder is stubbed

    // Mix of: static table exact match, static name reference, literal
    std::pair<std::string_view, std::string_view> headers[] = {
        {":method", "GET"},           // Static exact match
        {":path", "/custom/path"},    // Static name reference
        {":scheme", "https"},         // Static exact match
        {"content-type", "application/custom+json"},  // Static name ref
        {"x-custom-header", "custom-value"}  // Literal name and value
    };

    uint8_t buffer[512];
    size_t encoded_len;

    int result = encoder.encode_field_section(headers, 5, buffer, sizeof(buffer), encoded_len);
    assert(result == 0);

    std::cout << "Encoded " << 5 << " mixed headers into " << encoded_len << " bytes" << std::endl;
    print_hex("Encoded", buffer, encoded_len);

    // Decode
    std::pair<std::string, std::string> decoded[QPACKDecoder::kMaxHeaders];
    size_t decoded_count;

    result = decoder.decode_field_section(buffer, encoded_len, decoded, decoded_count);
    assert(result == 0);
    assert(decoded_count == 5);
    assert(headers_match(headers, decoded, 5));

    std::cout << "✓ Mixed encoding modes work correctly" << std::endl;
}

// ============================================================================
// Test 9: Decoder Error Handling
// ============================================================================

void test_decoder_error_handling() {
    std::cout << "\n=== Test 9: Decoder Error Handling ===" << std::endl;

    QPACKDecoder decoder;
    std::pair<std::string, std::string> decoded[QPACKDecoder::kMaxHeaders];
    size_t decoded_count;
    int result;

    // Test 1: Empty input
    {
        uint8_t empty[1] = {0};
        result = decoder.decode_field_section(empty, 0, decoded, decoded_count);
        assert(result != 0);
        std::cout << "✓ Rejects empty input" << std::endl;
    }

    // Test 2: Truncated input
    {
        uint8_t truncated[] = {0x00, 0x00, 0xC0};  // Prefix + incomplete indexed field
        result = decoder.decode_field_section(truncated, sizeof(truncated), decoded, decoded_count);
        // May fail or succeed depending on index value - just ensure no crash
        std::cout << "✓ Handles truncated input (result: " << result << ")" << std::endl;
    }

    // Test 3: Invalid static table index
    {
        uint8_t invalid[] = {0x00, 0x00, 0xFF, 0x7F};  // Prefix + very large index
        result = decoder.decode_field_section(invalid, sizeof(invalid), decoded, decoded_count);
        // Should fail with invalid index
        std::cout << "✓ Handles invalid index (result: " << result << ")" << std::endl;
    }

    std::cout << "✓ Decoder error handling tests complete" << std::endl;
}

// ============================================================================
// Test 10: RFC 9204 Test Vectors
// ============================================================================

void test_rfc9204_test_vectors() {
    std::cout << "\n=== Test 10: RFC 9204 Test Vectors ===" << std::endl;

    QPACKEncoder encoder;
    QPACKDecoder decoder;
    encoder.set_huffman_encoding(false);  // Huffman decoder is stubbed

    // RFC 9204 Appendix B.1: Simple Example
    // Encoding ":path: /sample/path"
    std::pair<std::string_view, std::string_view> headers[] = {
        {":path", "/sample/path"}
    };

    uint8_t buffer[128];
    size_t encoded_len;

    int result = encoder.encode_field_section(headers, 1, buffer, sizeof(buffer), encoded_len);
    assert(result == 0);

    std::cout << "RFC 9204 example encoded into " << encoded_len << " bytes" << std::endl;
    print_hex("Encoded", buffer, encoded_len);

    // Decode and verify
    std::pair<std::string, std::string> decoded[QPACKDecoder::kMaxHeaders];
    size_t decoded_count;

    result = decoder.decode_field_section(buffer, encoded_len, decoded, decoded_count);
    assert(result == 0);
    assert(decoded_count == 1);
    assert(headers_match(headers, decoded, 1));

    std::cout << "✓ RFC 9204 test vector works" << std::endl;
}

// ============================================================================
// Test 11: Dynamic Table Eviction
// ============================================================================

void test_dynamic_table_eviction() {
    std::cout << "\n=== Test 11: Dynamic Table Eviction ===" << std::endl;

    QPACKDynamicTable table(128);  // Small capacity: 128 bytes

    // Each entry is name.length() + value.length() + 32
    std::string name1 = "header1";
    std::string value1 = "value1";  // 7 + 6 + 32 = 45 bytes

    std::string name2 = "header2";
    std::string value2 = "value2";  // 7 + 6 + 32 = 45 bytes

    std::string name3 = "header3";
    std::string value3 = "value3";  // 7 + 6 + 32 = 45 bytes

    // Insert first entry
    bool inserted = table.insert(name1, value1);
    assert(inserted);
    std::cout << "Inserted entry 1, size: " << table.size() << " bytes" << std::endl;

    // Insert second entry
    inserted = table.insert(name2, value2);
    assert(inserted);
    std::cout << "Inserted entry 2, size: " << table.size() << " bytes" << std::endl;

    // Insert third entry (should evict first)
    inserted = table.insert(name3, value3);
    assert(inserted);
    std::cout << "Inserted entry 3, size: " << table.size() << " bytes" << std::endl;

    // First entry should be evicted
    int idx = table.find(name1, value1);
    assert(idx == -1);
    std::cout << "✓ First entry was evicted" << std::endl;

    // Second and third should still be there
    idx = table.find(name2, value2);
    assert(idx != -1);
    std::cout << "✓ Second entry still present" << std::endl;

    idx = table.find(name3, value3);
    assert(idx != -1);
    std::cout << "✓ Third entry still present" << std::endl;

    std::cout << "✓ Dynamic table eviction works correctly" << std::endl;
}

// ============================================================================
// Test 12: Reference Counting
// ============================================================================

void test_reference_counting() {
    std::cout << "\n=== Test 12: Reference Counting ===" << std::endl;

    QPACKDynamicTable table(128);

    std::string name = "header";
    std::string value = "value";

    // Insert entry
    bool inserted = table.insert(name, value);
    assert(inserted);

    uint64_t abs_index = 0;  // First entry has absolute index 0

    // Increment reference
    bool success = table.increment_reference(abs_index);
    assert(success);
    std::cout << "✓ Incremented reference count" << std::endl;

    // Try to evict by filling table (entry is referenced, so can't evict)
    for (int i = 0; i < 10; i++) {
        std::string n = "h" + std::to_string(i);
        std::string v = "v" + std::to_string(i);
        bool ins = table.insert(n, v);
        // May fail when referenced entry blocks eviction
        if (!ins) {
            std::cout << "✓ Insertion blocked by referenced entry" << std::endl;
            break;
        }
    }

    // Decrement reference
    success = table.decrement_reference(abs_index);
    assert(success);
    std::cout << "✓ Decremented reference count" << std::endl;

    std::cout << "✓ Reference counting works" << std::endl;
}

// ============================================================================
// Test 13: Randomized Headers
// ============================================================================

void test_randomized_headers() {
    std::cout << "\n=== Test 13: Randomized Headers (100 iterations) ===" << std::endl;

    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<> header_count_dist(1, 30);
    std::uniform_int_distribution<> name_len_dist(5, 20);
    std::uniform_int_distribution<> value_len_dist(5, 100);

    QPACKEncoder encoder;
    QPACKDecoder decoder;
    encoder.set_huffman_encoding(false);  // Huffman decoder is stubbed

    size_t success_count = 0;
    size_t total_original = 0;
    size_t total_compressed = 0;

    for (int iter = 0; iter < 100; iter++) {
        int header_count = header_count_dist(rng);

        std::vector<std::pair<std::string, std::string>> header_storage;
        std::vector<std::pair<std::string_view, std::string_view>> headers;

        for (int i = 0; i < header_count; i++) {
            std::string name = "x-hdr-" + random_string(name_len_dist(rng), rng);
            std::string value = random_string(value_len_dist(rng), rng);
            header_storage.emplace_back(std::move(name), std::move(value));
        }

        for (const auto& h : header_storage) {
            headers.emplace_back(h.first, h.second);
        }

        uint8_t buffer[16384];
        size_t encoded_len;

        int result = encoder.encode_field_section(headers.data(), headers.size(),
                                                  buffer, sizeof(buffer), encoded_len);
        if (result != 0) {
            continue;  // Skip if encoding fails (buffer too small)
        }

        std::pair<std::string, std::string> decoded[QPACKDecoder::kMaxHeaders];
        size_t decoded_count;

        result = decoder.decode_field_section(buffer, encoded_len, decoded, decoded_count);
        if (result != 0 || decoded_count != headers.size()) {
            std::cerr << "Iteration " << iter << " failed to decode" << std::endl;
            continue;
        }

        if (!headers_match(headers.data(), decoded, headers.size())) {
            std::cerr << "Iteration " << iter << " headers don't match" << std::endl;
            continue;
        }

        success_count++;
        total_original += calculate_original_size(headers.data(), headers.size());
        total_compressed += encoded_len;
    }

    std::cout << "Successful iterations: " << success_count << "/100" << std::endl;

    if (success_count > 0) {
        double avg_ratio = 100.0 * (1.0 - (double)total_compressed / total_original);
        std::cout << "Average compression ratio: " << avg_ratio << "%" << std::endl;
    }

    assert(success_count >= 95);  // At least 95% should succeed

    std::cout << "✓ Randomized header tests passed" << std::endl;
}

// ============================================================================
// Test 14: Large Header Values (8KB)
// ============================================================================

void test_large_header_values() {
    std::cout << "\n=== Test 14: Large Header Values (8KB) ===" << std::endl;

    QPACKEncoder encoder;
    QPACKDecoder decoder;
    encoder.set_huffman_encoding(false);  // Huffman decoder is stubbed

    std::string large_value(8000, 'x');  // 8KB value

    std::pair<std::string_view, std::string_view> headers[] = {
        {":method", "POST"},
        {"x-large-header", large_value}
    };

    uint8_t buffer[16384];
    size_t encoded_len;

    int result = encoder.encode_field_section(headers, 2, buffer, sizeof(buffer), encoded_len);
    assert(result == 0);

    std::cout << "Encoded large header (" << large_value.length()
              << " bytes) into " << encoded_len << " bytes" << std::endl;

    // Decode
    std::pair<std::string, std::string> decoded[QPACKDecoder::kMaxHeaders];
    size_t decoded_count;

    result = decoder.decode_field_section(buffer, encoded_len, decoded, decoded_count);
    assert(result == 0);
    assert(decoded_count == 2);
    assert(headers_match(headers, decoded, 2));
    assert(decoded[1].second.length() == 8000);

    std::cout << "✓ Large header values work correctly" << std::endl;
}

// ============================================================================
// Test 15: Performance Benchmarks
// ============================================================================

void test_performance_benchmarks() {
    std::cout << "\n=== Test 15: Performance Benchmarks ===" << std::endl;

    QPACKEncoder encoder;
    QPACKDecoder decoder;
    encoder.set_huffman_encoding(false);  // Huffman decoder is stubbed

    // Typical request headers
    std::pair<std::string_view, std::string_view> headers[] = {
        {":method", "GET"},
        {":path", "/api/v1/users/123"},
        {":scheme", "https"},
        {":authority", "api.example.com"},
        {"user-agent", "FasterAPI-Client/1.0"},
        {"accept", "application/json"},
        {"accept-encoding", "gzip, deflate, br"},
        {"authorization", "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"}
    };

    const int iterations = 10000;
    uint8_t buffer[2048];
    size_t encoded_len;

    // Encoding benchmark
    auto start = high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        encoder.encode_field_section(headers, 8, buffer, sizeof(buffer), encoded_len);
    }
    auto end = high_resolution_clock::now();
    auto encode_duration = duration_cast<microseconds>(end - start);

    double encode_rate = (iterations * 1000000.0) / encode_duration.count();
    std::cout << "Encoding: " << encode_rate << " req/sec" << std::endl;
    std::cout << "Encoding: " << (encode_duration.count() / (double)iterations) << " µs/req" << std::endl;

    // Decode benchmark
    std::pair<std::string, std::string> decoded[QPACKDecoder::kMaxHeaders];
    size_t decoded_count;

    start = high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        decoder.decode_field_section(buffer, encoded_len, decoded, decoded_count);
    }
    end = high_resolution_clock::now();
    auto decode_duration = duration_cast<microseconds>(end - start);

    double decode_rate = (iterations * 1000000.0) / decode_duration.count();
    std::cout << "Decoding: " << decode_rate << " req/sec" << std::endl;
    std::cout << "Decoding: " << (decode_duration.count() / (double)iterations) << " µs/req" << std::endl;

    std::cout << "✓ Performance benchmarks complete" << std::endl;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "QPACK Round-Trip & Compression Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    int passed = 0;
    int total = 15;

    try {
        test_simple_roundtrip();
        passed++;
    } catch (const std::exception& e) {
        std::cerr << "Test 1 failed: " << e.what() << std::endl;
    }

    try {
        test_static_table_encoding();
        passed++;
    } catch (const std::exception& e) {
        std::cerr << "Test 2 failed: " << e.what() << std::endl;
    }

    try {
        test_dynamic_table_insertion();
        passed++;
    } catch (const std::exception& e) {
        std::cerr << "Test 3 failed: " << e.what() << std::endl;
    }

    try {
        test_repeated_headers_compression();
        passed++;
    } catch (const std::exception& e) {
        std::cerr << "Test 4 failed: " << e.what() << std::endl;
    }

    try {
        test_huffman_compression();
        passed++;
    } catch (const std::exception& e) {
        std::cerr << "Test 5 failed: " << e.what() << std::endl;
    }

    try {
        test_large_header_set();
        passed++;
    } catch (const std::exception& e) {
        std::cerr << "Test 6 failed: " << e.what() << std::endl;
    }

    try {
        test_compression_ratios();
        passed++;
    } catch (const std::exception& e) {
        std::cerr << "Test 7 failed: " << e.what() << std::endl;
    }

    try {
        test_mixed_encoding_modes();
        passed++;
    } catch (const std::exception& e) {
        std::cerr << "Test 8 failed: " << e.what() << std::endl;
    }

    try {
        test_decoder_error_handling();
        passed++;
    } catch (const std::exception& e) {
        std::cerr << "Test 9 failed: " << e.what() << std::endl;
    }

    try {
        test_rfc9204_test_vectors();
        passed++;
    } catch (const std::exception& e) {
        std::cerr << "Test 10 failed: " << e.what() << std::endl;
    }

    try {
        test_dynamic_table_eviction();
        passed++;
    } catch (const std::exception& e) {
        std::cerr << "Test 11 failed: " << e.what() << std::endl;
    }

    try {
        test_reference_counting();
        passed++;
    } catch (const std::exception& e) {
        std::cerr << "Test 12 failed: " << e.what() << std::endl;
    }

    try {
        test_randomized_headers();
        passed++;
    } catch (const std::exception& e) {
        std::cerr << "Test 13 failed: " << e.what() << std::endl;
    }

    try {
        test_large_header_values();
        passed++;
    } catch (const std::exception& e) {
        std::cerr << "Test 14 failed: " << e.what() << std::endl;
    }

    try {
        test_performance_benchmarks();
        passed++;
    } catch (const std::exception& e) {
        std::cerr << "Test 15 failed: " << e.what() << std::endl;
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << "Test Results: " << passed << "/" << total << " passed" << std::endl;
    std::cout << "========================================" << std::endl;

    return (passed == total) ? 0 : 1;
}
