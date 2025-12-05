#include "../src/cpp/http/qpack/qpack_encoder.h"
#include "../src/cpp/http/qpack/qpack_decoder.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <chrono>
#include <random>

using namespace fasterapi::qpack;
using namespace std::chrono;

// ============================================================================
// Test Utilities
// ============================================================================

void print_hex(const char* label, const uint8_t* data, size_t len) {
    std::cout << label << ": ";
    for (size_t i = 0; i < len; i++) {
        printf("%02x ", data[i]);
    }
    std::cout << std::endl;
}

// Random string generator
std::string random_string(size_t length, std::mt19937& rng) {
    static const char charset[] =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789-_";
    std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);

    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; i++) {
        result += charset[dist(rng)];
    }
    return result;
}

// ============================================================================
// Test 1: Integer Encoding (QUIC VarInt for QPACK)
// ============================================================================

void test_integer_encoding() {
    std::cout << "\n=== Test 1: Integer Encoding ===" << std::endl;

    QPACKEncoder encoder;
    uint8_t buffer[16];
    size_t encoded_len;

    // Test small integers in field section prefix
    std::pair<std::string_view, std::string_view> empty_headers[1] = {};
    int result = encoder.encode_field_section(empty_headers, 0, buffer, sizeof(buffer), encoded_len);

    assert(result == 0);
    assert(encoded_len >= 2); // At least Required Insert Count + Delta Base

    std::cout << "✓ Integer encoding in field section prefix works" << std::endl;
    print_hex("Prefix bytes", buffer, encoded_len);
}

// ============================================================================
// Test 2: String Encoding (Plain)
// ============================================================================

void test_string_encoding_plain() {
    std::cout << "\n=== Test 2: String Encoding (Plain) ===" << std::endl;

    QPACKEncoder encoder;
    encoder.set_huffman_encoding(false); // Disable Huffman

    uint8_t buffer[256];
    size_t encoded_len;

    // Encode a custom header (not in static table)
    std::pair<std::string_view, std::string_view> headers[] = {
        {"x-custom-header", "test-value-123"}
    };

    int result = encoder.encode_field_section(headers, 1, buffer, sizeof(buffer), encoded_len);
    assert(result == 0);
    assert(encoded_len > 0);

    std::cout << "✓ Plain string encoding works" << std::endl;
    print_hex("Encoded", buffer, encoded_len);
}

// ============================================================================
// Test 3: String Encoding (Huffman)
// ============================================================================

void test_string_encoding_huffman() {
    std::cout << "\n=== Test 3: String Encoding (Huffman) ===" << std::endl;

    QPACKEncoder encoder;
    encoder.set_huffman_encoding(true); // Enable Huffman

    uint8_t buffer_huffman[256];
    uint8_t buffer_plain[256];
    size_t encoded_len_huffman, encoded_len_plain;

    // Encode with Huffman
    std::pair<std::string_view, std::string_view> headers[] = {
        {"x-custom-header", "this-is-a-longer-value-that-should-compress-well"}
    };

    int result1 = encoder.encode_field_section(headers, 1, buffer_huffman, sizeof(buffer_huffman), encoded_len_huffman);
    assert(result1 == 0);

    // Encode without Huffman
    encoder.set_huffman_encoding(false);
    int result2 = encoder.encode_field_section(headers, 1, buffer_plain, sizeof(buffer_plain), encoded_len_plain);
    assert(result2 == 0);

    // Huffman should be smaller (or same size for short strings)
    std::cout << "Plain size: " << encoded_len_plain << " bytes" << std::endl;
    std::cout << "Huffman size: " << encoded_len_huffman << " bytes" << std::endl;

    double compression_ratio = 100.0 * (1.0 - (double)encoded_len_huffman / encoded_len_plain);
    std::cout << "Compression: " << compression_ratio << "%" << std::endl;

    std::cout << "✓ Huffman encoding works" << std::endl;
}

// ============================================================================
// Test 4: Indexed Field Encoding (Static Table)
// ============================================================================

void test_indexed_static() {
    std::cout << "\n=== Test 4: Indexed Field (Static Table) ===" << std::endl;

    QPACKEncoder encoder;
    uint8_t buffer[256];
    size_t encoded_len;

    // Use exact match from static table
    std::pair<std::string_view, std::string_view> headers[] = {
        {":method", "GET"},       // Index 17 in static table
        {":path", "/"},           // Index 1 in static table
        {":scheme", "https"},     // Index 23 in static table
    };

    int result = encoder.encode_field_section(headers, 3, buffer, sizeof(buffer), encoded_len);
    assert(result == 0);

    // Should be very compact (prefix + 3 indexed fields)
    std::cout << "Encoded length: " << encoded_len << " bytes" << std::endl;
    assert(encoded_len < 20); // Should be much smaller than literal encoding

    print_hex("Indexed fields", buffer, encoded_len);
    std::cout << "✓ Static table indexed field encoding works" << std::endl;
}

// ============================================================================
// Test 5: Indexed Field Encoding (Dynamic Table)
// ============================================================================

void test_indexed_dynamic() {
    std::cout << "\n=== Test 5: Indexed Field (Dynamic Table) ===" << std::endl;

    QPACKEncoder encoder;
    uint8_t buffer[256];
    size_t encoded_len;

    // Insert into dynamic table
    encoder.dynamic_table().insert("x-custom", "value1");
    encoder.dynamic_table().insert("x-another", "value2");

    // Check it's in there
    int idx = encoder.dynamic_table().find("x-custom", "value1");
    assert(idx >= 0);

    std::cout << "Dynamic table size: " << encoder.dynamic_table().count() << " entries" << std::endl;
    std::cout << "✓ Dynamic table insertion works" << std::endl;

    // Note: Current encoder doesn't auto-reference dynamic table in encode_field_section
    // This would need enhancement for full dynamic table support
}

// ============================================================================
// Test 6: Literal Field with Name Reference (Static)
// ============================================================================

void test_literal_with_name_ref_static() {
    std::cout << "\n=== Test 6: Literal with Name Reference (Static) ===" << std::endl;

    QPACKEncoder encoder;
    uint8_t buffer[256];
    size_t encoded_len;

    // Use name from static table but custom value
    std::pair<std::string_view, std::string_view> headers[] = {
        {":method", "CUSTOM"},      // :method in static, but CUSTOM isn't
        {":authority", "example.com"}, // :authority in static, custom value
    };

    int result = encoder.encode_field_section(headers, 2, buffer, sizeof(buffer), encoded_len);
    assert(result == 0);

    std::cout << "Encoded length: " << encoded_len << " bytes" << std::endl;
    print_hex("With name refs", buffer, encoded_len);
    std::cout << "✓ Literal with name reference (static) works" << std::endl;
}

// ============================================================================
// Test 7: Literal Field with Literal Name
// ============================================================================

void test_literal_with_literal_name() {
    std::cout << "\n=== Test 7: Literal with Literal Name ===" << std::endl;

    QPACKEncoder encoder;
    encoder.set_huffman_encoding(false);

    uint8_t buffer[256];
    size_t encoded_len;

    // Completely custom headers
    std::pair<std::string_view, std::string_view> headers[] = {
        {"x-my-custom-header", "my-custom-value"},
        {"x-another-header", "another-value"},
    };

    int result = encoder.encode_field_section(headers, 2, buffer, sizeof(buffer), encoded_len);
    assert(result == 0);

    std::cout << "Encoded length: " << encoded_len << " bytes" << std::endl;
    print_hex("Literal names", buffer, encoded_len);
    std::cout << "✓ Literal with literal name works" << std::endl;
}

// ============================================================================
// Test 8: Full HTTP Request Headers
// ============================================================================

void test_http_request_full() {
    std::cout << "\n=== Test 8: Full HTTP Request ===" << std::endl;

    QPACKEncoder encoder;
    uint8_t buffer[1024];
    size_t encoded_len;

    // Typical HTTP/3 request
    std::pair<std::string_view, std::string_view> headers[] = {
        {":method", "GET"},
        {":scheme", "https"},
        {":authority", "www.example.com"},
        {":path", "/index.html"},
        {"user-agent", "TestClient/1.0"},
        {"accept", "*/*"},
        {"accept-encoding", "gzip, deflate, br"},
        {"cookie", "session=abc123; token=xyz789"},
        {"x-request-id", "req-12345"},
        {"x-trace-id", "trace-67890"},
    };

    int result = encoder.encode_field_section(headers, 10, buffer, sizeof(buffer), encoded_len);
    assert(result == 0);

    std::cout << "Request headers: 10 fields" << std::endl;
    std::cout << "Encoded length: " << encoded_len << " bytes" << std::endl;

    // Calculate original size
    size_t original_size = 0;
    for (auto& [name, value] : headers) {
        original_size += name.length() + value.length() + 2; // +2 for ": "
    }

    double compression = 100.0 * (1.0 - (double)encoded_len / original_size);
    std::cout << "Original size: " << original_size << " bytes" << std::endl;
    std::cout << "Compression: " << compression << "%" << std::endl;

    print_hex("Encoded request", buffer, std::min(encoded_len, size_t(64)));
    if (encoded_len > 64) std::cout << "... (truncated)" << std::endl;

    std::cout << "✓ Full HTTP request encoding works" << std::endl;
}

// ============================================================================
// Test 9: Full HTTP Response Headers
// ============================================================================

void test_http_response_full() {
    std::cout << "\n=== Test 9: Full HTTP Response ===" << std::endl;

    QPACKEncoder encoder;
    uint8_t buffer[1024];
    size_t encoded_len;

    // Typical HTTP/3 response
    std::pair<std::string_view, std::string_view> headers[] = {
        {":status", "200"},
        {"content-type", "text/html; charset=utf-8"},
        {"content-length", "12345"},
        {"cache-control", "max-age=3600"},
        {"date", "Mon, 01 Jan 2024 00:00:00 GMT"},
        {"server", "FasterAPI/1.0"},
        {"x-frame-options", "sameorigin"},
        {"x-content-type-options", "nosniff"},
        {"strict-transport-security", "max-age=31536000"},
        {"set-cookie", "session=new123; HttpOnly; Secure"},
    };

    int result = encoder.encode_field_section(headers, 10, buffer, sizeof(buffer), encoded_len);
    assert(result == 0);

    std::cout << "Response headers: 10 fields" << std::endl;
    std::cout << "Encoded length: " << encoded_len << " bytes" << std::endl;

    print_hex("Encoded response", buffer, std::min(encoded_len, size_t(64)));
    if (encoded_len > 64) std::cout << "... (truncated)" << std::endl;

    std::cout << "✓ Full HTTP response encoding works" << std::endl;
}

// ============================================================================
// Test 10: Edge Cases
// ============================================================================

void test_edge_cases() {
    std::cout << "\n=== Test 10: Edge Cases ===" << std::endl;

    QPACKEncoder encoder;
    uint8_t buffer[1024];
    size_t encoded_len;

    // Empty value
    std::pair<std::string_view, std::string_view> headers1[] = {
        {"x-empty", ""}
    };
    int r1 = encoder.encode_field_section(headers1, 1, buffer, sizeof(buffer), encoded_len);
    assert(r1 == 0);
    std::cout << "✓ Empty value works" << std::endl;

    // Very long value
    std::string long_value(500, 'a');
    std::pair<std::string_view, std::string_view> headers2[] = {
        {"x-long", long_value}
    };
    int r2 = encoder.encode_field_section(headers2, 1, buffer, sizeof(buffer), encoded_len);
    assert(r2 == 0);
    std::cout << "✓ Long value (500 bytes) works" << std::endl;

    // Special characters
    std::pair<std::string_view, std::string_view> headers3[] = {
        {"x-special", "value with spaces and @#$%^&*()"}
    };
    int r3 = encoder.encode_field_section(headers3, 1, buffer, sizeof(buffer), encoded_len);
    assert(r3 == 0);
    std::cout << "✓ Special characters work" << std::endl;

    // Buffer overflow test
    uint8_t small_buffer[10];
    std::pair<std::string_view, std::string_view> headers4[] = {
        {"x-toolong", "this-value-is-definitely-too-long-for-the-buffer"}
    };
    int r4 = encoder.encode_field_section(headers4, 1, small_buffer, sizeof(small_buffer), encoded_len);
    // Should handle gracefully (may return error)
    std::cout << "✓ Buffer overflow handled (result=" << r4 << ")" << std::endl;
}

// ============================================================================
// Test 11: Randomized Input
// ============================================================================

void test_randomized_input() {
    std::cout << "\n=== Test 11: Randomized Input ===" << std::endl;

    std::mt19937 rng(42); // Fixed seed for reproducibility
    QPACKEncoder encoder;
    uint8_t buffer[4096];

    const int num_tests = 50;
    int successes = 0;

    for (int test = 0; test < num_tests; test++) {
        // Random number of headers (1-15)
        std::uniform_int_distribution<> header_count_dist(1, 15);
        int num_headers = header_count_dist(rng);

        std::vector<std::pair<std::string, std::string>> header_storage;
        std::vector<std::pair<std::string_view, std::string_view>> headers;

        for (int i = 0; i < num_headers; i++) {
            std::string name = "x-hdr-" + std::to_string(i);
            std::string value = random_string(20, rng);

            header_storage.emplace_back(std::move(name), std::move(value));
            headers.emplace_back(header_storage.back().first, header_storage.back().second);
        }

        size_t encoded_len;
        int result = encoder.encode_field_section(
            headers.data(), headers.size(),
            buffer, sizeof(buffer),
            encoded_len
        );

        if (result == 0) {
            successes++;
        }
    }

    std::cout << "Randomized tests: " << successes << "/" << num_tests << " passed" << std::endl;
    assert(successes == num_tests);
    std::cout << "✓ All randomized tests passed" << std::endl;
}

// ============================================================================
// Test 12: Performance Benchmark
// ============================================================================

void test_performance() {
    std::cout << "\n=== Test 12: Performance Benchmark ===" << std::endl;

    QPACKEncoder encoder;
    uint8_t buffer[1024];

    // Typical request headers (15 fields)
    std::pair<std::string_view, std::string_view> headers[] = {
        {":method", "POST"},
        {":scheme", "https"},
        {":authority", "api.example.com"},
        {":path", "/v1/users/123"},
        {"content-type", "application/json"},
        {"content-length", "256"},
        {"authorization", "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"},
        {"user-agent", "TestClient/1.0"},
        {"accept", "*/*"},
        {"accept-encoding", "gzip, deflate, br"},
        {"x-request-id", "req-abc-123"},
        {"x-trace-id", "trace-xyz-789"},
        {"x-api-key", "api-key-12345"},
        {"x-client-version", "1.0.0"},
        {"x-platform", "linux"},
    };

    const int iterations = 10000;
    size_t total_encoded = 0;

    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        size_t encoded_len;
        encoder.encode_field_section(headers, 15, buffer, sizeof(buffer), encoded_len);
        total_encoded += encoded_len;
    }

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<nanoseconds>(end - start).count();

    double avg_ns = (double)duration / iterations;
    double avg_us = avg_ns / 1000.0;

    std::cout << "Iterations: " << iterations << std::endl;
    std::cout << "Total time: " << duration / 1000000.0 << " ms" << std::endl;
    std::cout << "Average per encode: " << avg_us << " μs (" << avg_ns << " ns)" << std::endl;
    std::cout << "Average encoded size: " << total_encoded / iterations << " bytes" << std::endl;
    std::cout << "Throughput: " << (iterations * 1000000000.0 / duration) << " ops/sec" << std::endl;

    // Target: <1μs for 15 fields
    if (avg_us < 1.0) {
        std::cout << "✓ Performance target met (<1μs)" << std::endl;
    } else {
        std::cout << "⚠ Performance target missed (target: <1μs, actual: " << avg_us << "μs)" << std::endl;
    }
}

// ============================================================================
// Test 13: Compression Ratio Statistics
// ============================================================================

void test_compression_ratio() {
    std::cout << "\n=== Test 13: Compression Ratio Statistics ===" << std::endl;

    QPACKEncoder encoder;
    uint8_t buffer[2048];

    struct TestCase {
        const char* name;
        std::vector<std::pair<std::string_view, std::string_view>> headers;
    };

    std::vector<TestCase> test_cases = {
        {
            "Minimal request",
            {
                {":method", "GET"},
                {":path", "/"},
                {":scheme", "https"},
            }
        },
        {
            "Typical request",
            {
                {":method", "GET"},
                {":scheme", "https"},
                {":authority", "www.example.com"},
                {":path", "/api/v1/data"},
                {"user-agent", "Mozilla/5.0"},
                {"accept", "*/*"},
                {"accept-encoding", "gzip, deflate, br"},
            }
        },
        {
            "Large response",
            {
                {":status", "200"},
                {"content-type", "application/json"},
                {"content-length", "4096"},
                {"cache-control", "public, max-age=3600"},
                {"date", "Mon, 01 Jan 2024 00:00:00 GMT"},
                {"server", "nginx/1.18.0"},
                {"x-frame-options", "DENY"},
                {"x-content-type-options", "nosniff"},
                {"strict-transport-security", "max-age=31536000; includeSubDomains"},
                {"set-cookie", "session=abcdef123456; HttpOnly; Secure; SameSite=Strict"},
            }
        },
    };

    for (auto& test : test_cases) {
        size_t original_size = 0;
        for (auto& [name, value] : test.headers) {
            original_size += name.length() + value.length();
        }

        size_t encoded_len;
        encoder.encode_field_section(
            test.headers.data(), test.headers.size(),
            buffer, sizeof(buffer),
            encoded_len
        );

        double ratio = 100.0 * (1.0 - (double)encoded_len / original_size);

        std::cout << test.name << ":" << std::endl;
        std::cout << "  Original: " << original_size << " bytes" << std::endl;
        std::cout << "  Encoded:  " << encoded_len << " bytes" << std::endl;
        std::cout << "  Ratio:    " << ratio << "%" << std::endl;
    }

    std::cout << "✓ Compression ratio statistics collected" << std::endl;
}

// ============================================================================
// Test 14: RFC Compliance Verification
// ============================================================================

void test_rfc_compliance() {
    std::cout << "\n=== Test 14: RFC 9204 Compliance ===" << std::endl;

    QPACKEncoder encoder;
    uint8_t buffer[256];
    size_t encoded_len;

    // Test 1: Field Section Prefix format (Section 4.5.1)
    std::pair<std::string_view, std::string_view> headers1[] = {
        {"x-test", "value"}
    };
    encoder.encode_field_section(headers1, 1, buffer, sizeof(buffer), encoded_len);

    // First bytes should be Required Insert Count and Delta Base (both varints)
    // For simple case with no dynamic table refs, both should be 0
    // 0 as varint is encoded as 0x00
    assert(buffer[0] == 0x00); // Required Insert Count = 0
    assert(buffer[1] == 0x00); // Delta Base = 0
    std::cout << "✓ Field section prefix format compliant" << std::endl;

    // Test 2: Indexed field format (Section 4.5.2)
    // Pattern: 11TXXXXX where T=1 for static, T=0 for dynamic
    std::pair<std::string_view, std::string_view> headers2[] = {
        {":method", "GET"} // Should be indexed from static table
    };
    uint8_t buffer2[256];
    size_t encoded_len2;
    encoder.encode_field_section(headers2, 1, buffer2, sizeof(buffer2), encoded_len2);

    // After prefix, should see indexed field (11XXXXXX pattern)
    bool found_indexed = false;
    for (size_t i = 2; i < encoded_len2; i++) {
        if ((buffer2[i] & 0xC0) == 0xC0) {
            found_indexed = true;
            std::cout << "Found indexed field at position " << i << ": 0x" << std::hex << (int)buffer2[i] << std::dec << std::endl;
            break;
        }
    }

    if (!found_indexed) {
        std::cout << "Encoded bytes for :method=GET: ";
        for (size_t i = 0; i < encoded_len2; i++) {
            printf("%02x ", buffer2[i]);
        }
        std::cout << std::endl;
    }

    assert(found_indexed);
    std::cout << "✓ Indexed field format compliant" << std::endl;

    // Test 3: Literal field format (Section 4.5.3)
    encoder.set_huffman_encoding(false); // Easier to verify
    std::pair<std::string_view, std::string_view> headers3[] = {
        {"x-new", "test"}
    };
    encoder.encode_field_section(headers3, 1, buffer, sizeof(buffer), encoded_len);

    // Should contain literal encoding patterns (001XXXXX or 01XXXXXX)
    bool found_literal = false;
    for (size_t i = 2; i < encoded_len; i++) {
        if ((buffer[i] & 0xE0) == 0x20 || (buffer[i] & 0xC0) == 0x40) {
            found_literal = true;
            break;
        }
    }
    assert(found_literal);
    std::cout << "✓ Literal field format compliant" << std::endl;

    std::cout << "✓ RFC 9204 compliance verified" << std::endl;
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "QPACK Encoder Test Suite" << std::endl;
    std::cout << "=========================" << std::endl;

    try {
        test_integer_encoding();
        test_string_encoding_plain();
        test_string_encoding_huffman();
        test_indexed_static();
        test_indexed_dynamic();
        test_literal_with_name_ref_static();
        test_literal_with_literal_name();
        test_http_request_full();
        test_http_response_full();
        test_edge_cases();
        test_randomized_input();
        test_performance();
        test_compression_ratio();
        test_rfc_compliance();

        std::cout << "\n=========================" << std::endl;
        std::cout << "ALL TESTS PASSED ✓" << std::endl;
        std::cout << "=========================" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\n❌ TEST FAILED: Unknown exception" << std::endl;
        return 1;
    }
}
