/**
 * RequestBodyBuffer Unit Tests
 *
 * Tests the shared request body buffer abstraction used by HTTP/1.1, HTTP/2, HTTP/3.
 * Backed by thread-local BodyBufferArena.
 */

#include "../src/cpp/http/request_body_buffer.h"
#include <iostream>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace fasterapi::http;

static int tests_passed = 0;
static int tests_failed = 0;
static bool current_test_failed = false;
static std::string current_test_error;

#define TEST(name) void test_##name()

#define RUN_TEST(name) \
    do { \
        std::cout << "Running " << #name << "... "; \
        current_test_failed = false; \
        current_test_error = ""; \
        test_##name(); \
        if (current_test_failed) { \
            std::cout << "FAIL: " << current_test_error << std::endl; \
            tests_failed++; \
        } else { \
            std::cout << "PASS" << std::endl; \
            tests_passed++; \
        } \
    } while(0)

#define ASSERT(condition) \
    if (!(condition)) { \
        current_test_failed = true; \
        current_test_error = "Assertion failed: " #condition " (line " + std::to_string(__LINE__) + ")"; \
        return; \
    }

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) { \
        current_test_failed = true; \
        current_test_error = "Expected " + std::to_string(b) + " but got " + std::to_string(a) + " (line " + std::to_string(__LINE__) + ")"; \
        return; \
    }

// ============================================================================
// Basic Append
// ============================================================================

TEST(append_small_data) {
    RequestBodyBuffer buf;
    const char* data = "Hello, World!";
    buf.append(data, strlen(data));

    ASSERT_EQ(buf.size(), strlen(data));
    ASSERT(buf.view() == "Hello, World!");
    ASSERT(buf.to_string() == "Hello, World!");
}

TEST(append_multiple_chunks) {
    RequestBodyBuffer buf;
    buf.append("chunk1", 6);
    buf.append("chunk2", 6);
    buf.append("chunk3", 6);

    ASSERT_EQ(buf.size(), static_cast<size_t>(18));
    ASSERT(buf.view() == "chunk1chunk2chunk3");
}

TEST(append_with_uint8_pointer) {
    RequestBodyBuffer buf;
    uint8_t data[] = {0x48, 0x65, 0x6C, 0x6C, 0x6F};  // "Hello"
    buf.append(data, 5);

    ASSERT_EQ(buf.size(), static_cast<size_t>(5));
    ASSERT(buf.view() == "Hello");
}

// ============================================================================
// Reserve
// ============================================================================

TEST(reserve_and_append) {
    RequestBodyBuffer buf;
    size_t body_size = 50000;
    size_t max_body = 10 * 1024 * 1024;

    bool ok = buf.reserve(body_size, max_body);
    ASSERT(ok);
    ASSERT(buf.capacity() >= body_size);
    ASSERT(buf.is_arena_backed());

    // Fill with random data
    std::mt19937 rng(123);
    std::vector<uint8_t> expected(body_size);
    for (auto& b : expected) b = static_cast<uint8_t>(rng() & 0xFF);

    buf.append(expected.data(), expected.size());
    ASSERT_EQ(buf.size(), body_size);

    // Verify content
    ASSERT(std::memcmp(buf.data(), expected.data(), body_size) == 0);
}

TEST(reserve_rejects_oversized) {
    RequestBodyBuffer buf;
    bool ok = buf.reserve(100, 50);  // 100 > max 50
    ASSERT(!ok);
    ASSERT_EQ(buf.size(), static_cast<size_t>(0));
}

TEST(reserve_noop_when_fits) {
    RequestBodyBuffer buf;
    // Reserve small amount that fits in already-acquired buffer
    buf.reserve(64 * 1024, 10 * 1024 * 1024);
    bool was_arena = buf.is_arena_backed();

    // Reserve same or smaller — should be a no-op
    bool ok = buf.reserve(32 * 1024, 10 * 1024 * 1024);
    ASSERT(ok);
    // Arena state unchanged
    ASSERT(buf.is_arena_backed() == was_arena);
}

// ============================================================================
// Adopt (HTTP/1.1 two-phase pattern)
// ============================================================================

TEST(adopt_from_stack_buffer) {
    // Simulate HTTP/1.1: partial read into stack buffer, then adopt into arena
    uint8_t stack_buf[8192];
    std::mt19937 rng(456);
    for (auto& b : stack_buf) b = static_cast<uint8_t>(rng() & 0xFF);

    RequestBodyBuffer buf;
    buf.adopt(stack_buf, 4096);

    ASSERT_EQ(buf.size(), static_cast<size_t>(4096));
    ASSERT(std::memcmp(buf.data(), stack_buf, 4096) == 0);

    // Then append more data (simulating continued socket reads)
    uint8_t more[2048];
    for (auto& b : more) b = static_cast<uint8_t>(rng() & 0xFF);
    buf.append(more, sizeof(more));

    ASSERT_EQ(buf.size(), static_cast<size_t>(4096 + 2048));
}

TEST(adopt_overwrites_previous) {
    RequestBodyBuffer buf;
    buf.append("initial", 7);
    ASSERT_EQ(buf.size(), static_cast<size_t>(7));

    uint8_t new_data[] = "replaced";
    buf.adopt(new_data, 8);
    ASSERT_EQ(buf.size(), static_cast<size_t>(8));
    ASSERT(buf.view() == "replaced");
}

// ============================================================================
// Write Head / Advance (Direct socket read pattern)
// ============================================================================

TEST(write_head_advance) {
    RequestBodyBuffer buf;
    buf.reserve(1024, 10 * 1024 * 1024);

    auto [ptr, avail] = buf.write_head();
    ASSERT(ptr != nullptr);
    ASSERT(avail >= 1024);

    // Simulate socket read writing directly into buffer
    std::memcpy(ptr, "direct_write", 12);
    buf.advance(12);

    ASSERT_EQ(buf.size(), static_cast<size_t>(12));
    ASSERT(buf.view() == "direct_write");

    // Second write
    auto [ptr2, avail2] = buf.write_head();
    ASSERT(ptr2 == ptr + 12);
    std::memcpy(ptr2, "_more", 5);
    buf.advance(5);

    ASSERT_EQ(buf.size(), static_cast<size_t>(17));
    ASSERT(buf.view() == "direct_write_more");
}

// ============================================================================
// Reset
// ============================================================================

TEST(reset_releases_arena) {
    RequestBodyBuffer buf;
    buf.reserve(64 * 1024, 10 * 1024 * 1024);
    buf.append("data", 4);
    ASSERT(buf.is_arena_backed());
    ASSERT_EQ(buf.size(), static_cast<size_t>(4));

    buf.reset();
    ASSERT(!buf.is_arena_backed());
    ASSERT_EQ(buf.size(), static_cast<size_t>(0));
    ASSERT_EQ(buf.capacity(), static_cast<size_t>(0));
    ASSERT(buf.data() == nullptr);
}

// ============================================================================
// Auto-Grow
// ============================================================================

TEST(grow_on_append_without_reserve) {
    RequestBodyBuffer buf;

    // Append 100KB without reserving — should auto-grow via arena
    std::mt19937 rng(789);
    std::vector<uint8_t> data(100 * 1024);
    for (auto& b : data) b = static_cast<uint8_t>(rng() & 0xFF);

    // Append in small chunks
    size_t chunk = 4096;
    for (size_t off = 0; off < data.size(); off += chunk) {
        size_t len = std::min(chunk, data.size() - off);
        buf.append(data.data() + off, len);
    }

    ASSERT_EQ(buf.size(), data.size());
    ASSERT(std::memcmp(buf.data(), data.data(), data.size()) == 0);
}

TEST(grow_preserves_existing_data) {
    RequestBodyBuffer buf;

    // Write small data
    buf.append("HEADER", 6);

    // Now append enough to trigger growth
    std::vector<uint8_t> big(70 * 1024, 0xBB);
    buf.append(big.data(), big.size());

    ASSERT_EQ(buf.size(), 6 + big.size());
    // Original data preserved
    ASSERT(std::memcmp(buf.data(), "HEADER", 6) == 0);
    // New data correct
    ASSERT(buf.data()[6] == 0xBB);
    ASSERT(buf.data()[6 + big.size() - 1] == 0xBB);
}

// ============================================================================
// Move Semantics
// ============================================================================

TEST(move_construct) {
    RequestBodyBuffer buf1;
    buf1.reserve(64 * 1024, 10 * 1024 * 1024);
    buf1.append("test_data", 9);

    RequestBodyBuffer buf2(std::move(buf1));
    ASSERT_EQ(buf2.size(), static_cast<size_t>(9));
    ASSERT(buf2.view() == "test_data");
    ASSERT(buf2.is_arena_backed());
}

TEST(move_assign) {
    RequestBodyBuffer buf1;
    buf1.reserve(64 * 1024, 10 * 1024 * 1024);
    buf1.append("first", 5);

    RequestBodyBuffer buf2;
    buf2.append("second", 6);

    buf2 = std::move(buf1);
    ASSERT_EQ(buf2.size(), static_cast<size_t>(5));
    ASSERT(buf2.view() == "first");
}

// ============================================================================
// Large Payload Simulation
// ============================================================================

TEST(simulate_es_bulk_payload) {
    // Simulate a realistic ES bulk API payload: ~300KB of NDJSON
    RequestBodyBuffer buf;
    std::mt19937 rng(101);

    std::string payload;
    for (int i = 0; i < 1000; i++) {
        // Index action line
        payload += "{\"index\":{\"_id\":\"doc_" + std::to_string(i) + "\"}}\n";
        // Document line
        payload += "{\"title\":\"document " + std::to_string(i) + "\",\"count\":" +
                   std::to_string(rng() % 10000) + ",\"body\":\"";
        for (int j = 0; j < 200; j++) payload += 'x';
        payload += "\"}\n";
    }

    // Reserve with Content-Length known
    bool ok = buf.reserve(payload.size(), 10 * 1024 * 1024);
    ASSERT(ok);

    // Simulate chunked socket reads
    size_t chunk_size = 8192;
    for (size_t off = 0; off < payload.size(); off += chunk_size) {
        size_t len = std::min(chunk_size, payload.size() - off);
        buf.append(payload.data() + off, len);
    }

    ASSERT_EQ(buf.size(), payload.size());
    ASSERT(buf.view() == payload);
}

TEST(simulate_http11_two_phase) {
    // Simulate the exact HTTP/1.1 two-phase pattern:
    // 1. Read into 8KB stack buffer
    // 2. Parse headers, find Content-Length
    // 3. Adopt into arena buffer
    // 4. Continue reading into arena

    static constexpr size_t STACK_SIZE = 8192;
    uint8_t stack[STACK_SIZE];

    // Build a fake HTTP request: headers + body
    std::string headers = "POST /test/_bulk HTTP/1.1\r\n"
                          "Content-Length: 50000\r\n"
                          "Content-Type: application/x-ndjson\r\n\r\n";

    std::mt19937 rng(202);
    std::string body(50000, '\0');
    for (auto& c : body) c = 'A' + static_cast<char>(rng() % 26);

    std::string full_request = headers + body;

    // Phase 1: First 8KB goes into stack
    size_t first_read = std::min(STACK_SIZE, full_request.size());
    std::memcpy(stack, full_request.data(), first_read);

    // Phase 2: Parser found Content-Length = 50000
    size_t content_length = 50000;
    size_t needed = STACK_SIZE + content_length;  // headers max + body

    RequestBodyBuffer buf;
    buf.reserve(needed, 10 * 1024 * 1024);
    buf.adopt(stack, first_read);

    // Phase 3: Continue reading remaining data
    size_t remaining = full_request.size() - first_read;
    if (remaining > 0) {
        buf.append(
            reinterpret_cast<const uint8_t*>(full_request.data() + first_read),
            remaining
        );
    }

    ASSERT_EQ(buf.size(), full_request.size());
    ASSERT(std::memcmp(buf.data(), full_request.data(), full_request.size()) == 0);
}

// ============================================================================
// Remaining / Capacity
// ============================================================================

TEST(remaining_tracks_correctly) {
    RequestBodyBuffer buf;
    buf.reserve(1024, 10 * 1024 * 1024);
    size_t cap = buf.capacity();

    ASSERT_EQ(buf.remaining(), cap);
    buf.append("12345", 5);
    ASSERT_EQ(buf.remaining(), cap - 5);
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "=== RequestBodyBuffer Tests ===" << std::endl;

    RUN_TEST(append_small_data);
    RUN_TEST(append_multiple_chunks);
    RUN_TEST(append_with_uint8_pointer);
    RUN_TEST(reserve_and_append);
    RUN_TEST(reserve_rejects_oversized);
    RUN_TEST(reserve_noop_when_fits);
    RUN_TEST(adopt_from_stack_buffer);
    RUN_TEST(adopt_overwrites_previous);
    RUN_TEST(write_head_advance);
    RUN_TEST(reset_releases_arena);
    RUN_TEST(grow_on_append_without_reserve);
    RUN_TEST(grow_preserves_existing_data);
    RUN_TEST(move_construct);
    RUN_TEST(move_assign);
    RUN_TEST(simulate_es_bulk_payload);
    RUN_TEST(simulate_http11_two_phase);
    RUN_TEST(remaining_tracks_correctly);

    std::cout << "\n=== Results: " << tests_passed << " passed, "
              << tests_failed << " failed ===" << std::endl;
    return tests_failed > 0 ? 1 : 0;
}
