/**
 * QPACK Static Table Test Suite
 *
 * Comprehensive tests for RFC 9204 Appendix A compliance.
 * Verifies all 99 static table entries are correct.
 */

#include "../src/cpp/http/qpack/qpack_static_table.h"
#include <cassert>
#include <iostream>
#include <chrono>
#include <string_view>
#include <cstring>

using namespace fasterapi::qpack;
using namespace std::chrono;

// Test counters
static int test_count = 0;
static int passed_count = 0;

#define TEST_START(name) \
    do { \
        std::cout << "Test " << ++test_count << ": " << name << "... "; \
    } while(0)

#define TEST_PASS() \
    do { \
        std::cout << "PASS" << std::endl; \
        passed_count++; \
    } while(0)

#define TEST_FAIL(msg) \
    do { \
        std::cout << "FAIL: " << msg << std::endl; \
        return false; \
    } while(0)

#define ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            TEST_FAIL("Expected " << (b) << " but got " << (a)); \
        } \
    } while(0)

#define ASSERT_STR_EQ(a, b) \
    do { \
        if (std::string_view(a) != std::string_view(b)) { \
            TEST_FAIL("Expected '" << (b) << "' but got '" << (a) << "'"); \
        } \
    } while(0)

#define ASSERT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            TEST_FAIL("Expected true but got false"); \
        } \
    } while(0)

#define ASSERT_FALSE(expr) \
    do { \
        if (expr) { \
            TEST_FAIL("Expected false but got true"); \
        } \
    } while(0)

#define ASSERT_NOT_NULL(ptr) \
    do { \
        if ((ptr) == nullptr) { \
            TEST_FAIL("Expected non-null pointer"); \
        } \
    } while(0)

#define ASSERT_NULL(ptr) \
    do { \
        if ((ptr) != nullptr) { \
            TEST_FAIL("Expected null pointer"); \
        } \
    } while(0)

// ============================================================================
// RFC 9204 Appendix A - Static Table Definition
// ============================================================================

/**
 * Complete QPACK static table from RFC 9204 Appendix A.
 * Used for verification against our implementation.
 */
struct ExpectedEntry {
    size_t index;
    const char* name;
    const char* value;
};

// RFC 9204 Appendix A - Complete Static Table (99 entries, indices 0-98)
static const ExpectedEntry kRFC9204StaticTable[] = {
    {0, ":authority", ""},
    {1, ":path", "/"},
    {2, "age", "0"},
    {3, "content-disposition", ""},
    {4, "content-length", "0"},
    {5, "cookie", ""},
    {6, "date", ""},
    {7, "etag", ""},
    {8, "if-modified-since", ""},
    {9, "if-none-match", ""},
    {10, "last-modified", ""},
    {11, "link", ""},
    {12, "location", ""},
    {13, "referer", ""},
    {14, "set-cookie", ""},
    {15, ":method", "CONNECT"},
    {16, ":method", "DELETE"},
    {17, ":method", "GET"},
    {18, ":method", "HEAD"},
    {19, ":method", "OPTIONS"},
    {20, ":method", "POST"},
    {21, ":method", "PUT"},
    {22, ":scheme", "http"},
    {23, ":scheme", "https"},
    {24, ":status", "103"},
    {25, ":status", "200"},
    {26, ":status", "304"},
    {27, ":status", "404"},
    {28, ":status", "503"},
    {29, "accept", "*/*"},
    {30, "accept", "application/dns-message"},
    {31, "accept-encoding", "gzip, deflate, br"},
    {32, "accept-ranges", "bytes"},
    {33, "access-control-allow-headers", "cache-control"},
    {34, "access-control-allow-headers", "content-type"},
    {35, "access-control-allow-origin", "*"},
    {36, "cache-control", "max-age=0"},
    {37, "cache-control", "max-age=2592000"},
    {38, "cache-control", "max-age=604800"},
    {39, "cache-control", "no-cache"},
    {40, "cache-control", "no-store"},
    {41, "cache-control", "public, max-age=31536000"},
    {42, "content-encoding", "br"},
    {43, "content-encoding", "gzip"},
    {44, "content-type", "application/dns-message"},
    {45, "content-type", "application/javascript"},
    {46, "content-type", "application/json"},
    {47, "content-type", "application/x-www-form-urlencoded"},
    {48, "content-type", "image/gif"},
    {49, "content-type", "image/jpeg"},
    {50, "content-type", "image/png"},
    {51, "content-type", "text/css"},
    {52, "content-type", "text/html; charset=utf-8"},
    {53, "content-type", "text/plain"},
    {54, "content-type", "text/plain;charset=utf-8"},
    {55, "range", "bytes=0-"},
    {56, "strict-transport-security", "max-age=31536000"},
    {57, "strict-transport-security", "max-age=31536000; includesubdomains"},
    {58, "strict-transport-security", "max-age=31536000; includesubdomains; preload"},
    {59, "vary", "accept-encoding"},
    {60, "vary", "origin"},
    {61, "x-content-type-options", "nosniff"},
    {62, "x-xss-protection", "1; mode=block"},
    {63, ":status", "100"},
    {64, ":status", "204"},
    {65, ":status", "206"},
    {66, ":status", "302"},
    {67, ":status", "400"},
    {68, ":status", "403"},
    {69, ":status", "421"},
    {70, ":status", "425"},
    {71, ":status", "500"},
    {72, "accept-language", ""},
    {73, "access-control-allow-credentials", "FALSE"},
    {74, "access-control-allow-credentials", "TRUE"},
    {75, "access-control-allow-headers", "*"},
    {76, "access-control-allow-methods", "get"},
    {77, "access-control-allow-methods", "get, post, options"},
    {78, "access-control-allow-methods", "options"},
    {79, "access-control-expose-headers", "content-length"},
    {80, "access-control-request-headers", "content-type"},
    {81, "access-control-request-method", "get"},
    {82, "access-control-request-method", "post"},
    {83, "alt-svc", "clear"},
    {84, "authorization", ""},
    {85, "content-security-policy", "script-src 'none'; object-src 'none'; base-uri 'none'"},
    {86, "early-data", "1"},
    {87, "expect-ct", ""},
    {88, "forwarded", ""},
    {89, "if-range", ""},
    {90, "origin", ""},
    {91, "purpose", "prefetch"},
    {92, "server", ""},
    {93, "timing-allow-origin", "*"},
    {94, "upgrade-insecure-requests", "1"},
    {95, "user-agent", ""},
    {96, "x-forwarded-for", ""},
    {97, "x-frame-options", "deny"},
    {98, "x-frame-options", "sameorigin"},
};

static constexpr size_t kExpectedTableSize = sizeof(kRFC9204StaticTable) / sizeof(kRFC9204StaticTable[0]);

// ============================================================================
// Test 1: Table Size Verification
// ============================================================================

bool test_table_size() {
    TEST_START("Static table size is 99 entries");

    ASSERT_EQ(QPACKStaticTable::size(), 99);
    ASSERT_EQ(kExpectedTableSize, 99);

    TEST_PASS();
    return true;
}

// ============================================================================
// Test 2: All Entries Match RFC 9204 Appendix A
// ============================================================================

bool test_all_entries_rfc_compliant() {
    TEST_START("All 99 entries match RFC 9204 Appendix A");

    for (size_t i = 0; i < kExpectedTableSize; i++) {
        const auto& expected = kRFC9204StaticTable[i];
        const StaticEntry* actual = QPACKStaticTable::get(i);

        ASSERT_NOT_NULL(actual);
        ASSERT_EQ(expected.index, i);
        ASSERT_STR_EQ(actual->name, expected.name);
        ASSERT_STR_EQ(actual->value, expected.value);
    }

    TEST_PASS();
    return true;
}

// ============================================================================
// Test 3: Key Static Entries (Common HTTP Headers)
// ============================================================================

bool test_key_entries() {
    TEST_START("Key static entries are correct");

    // :authority (index 0)
    const StaticEntry* e0 = QPACKStaticTable::get(0);
    ASSERT_NOT_NULL(e0);
    ASSERT_STR_EQ(e0->name, ":authority");
    ASSERT_STR_EQ(e0->value, "");

    // :path / (index 1)
    const StaticEntry* e1 = QPACKStaticTable::get(1);
    ASSERT_NOT_NULL(e1);
    ASSERT_STR_EQ(e1->name, ":path");
    ASSERT_STR_EQ(e1->value, "/");

    // :method GET (index 17)
    const StaticEntry* e17 = QPACKStaticTable::get(17);
    ASSERT_NOT_NULL(e17);
    ASSERT_STR_EQ(e17->name, ":method");
    ASSERT_STR_EQ(e17->value, "GET");

    // :method POST (index 20)
    const StaticEntry* e20 = QPACKStaticTable::get(20);
    ASSERT_NOT_NULL(e20);
    ASSERT_STR_EQ(e20->name, ":method");
    ASSERT_STR_EQ(e20->value, "POST");

    // :scheme http (index 22)
    const StaticEntry* e22 = QPACKStaticTable::get(22);
    ASSERT_NOT_NULL(e22);
    ASSERT_STR_EQ(e22->name, ":scheme");
    ASSERT_STR_EQ(e22->value, "http");

    // :scheme https (index 23)
    const StaticEntry* e23 = QPACKStaticTable::get(23);
    ASSERT_NOT_NULL(e23);
    ASSERT_STR_EQ(e23->name, ":scheme");
    ASSERT_STR_EQ(e23->value, "https");

    // :status 200 (index 25)
    const StaticEntry* e25 = QPACKStaticTable::get(25);
    ASSERT_NOT_NULL(e25);
    ASSERT_STR_EQ(e25->name, ":status");
    ASSERT_STR_EQ(e25->value, "200");

    // :status 404 (index 27)
    const StaticEntry* e27 = QPACKStaticTable::get(27);
    ASSERT_NOT_NULL(e27);
    ASSERT_STR_EQ(e27->name, ":status");
    ASSERT_STR_EQ(e27->value, "404");

    // :status 500 (index 71)
    const StaticEntry* e71 = QPACKStaticTable::get(71);
    ASSERT_NOT_NULL(e71);
    ASSERT_STR_EQ(e71->name, ":status");
    ASSERT_STR_EQ(e71->value, "500");

    // content-type text/html; charset=utf-8 (index 52)
    const StaticEntry* e52 = QPACKStaticTable::get(52);
    ASSERT_NOT_NULL(e52);
    ASSERT_STR_EQ(e52->name, "content-type");
    ASSERT_STR_EQ(e52->value, "text/html; charset=utf-8");

    TEST_PASS();
    return true;
}

// ============================================================================
// Test 4: Out of Bounds Access
// ============================================================================

bool test_out_of_bounds() {
    TEST_START("Out of bounds access returns nullptr");

    // Valid range is 0-98
    ASSERT_NULL(QPACKStaticTable::get(99));
    ASSERT_NULL(QPACKStaticTable::get(100));
    ASSERT_NULL(QPACKStaticTable::get(1000));
    ASSERT_NULL(QPACKStaticTable::get(SIZE_MAX));

    TEST_PASS();
    return true;
}

// ============================================================================
// Test 5: Find by Name and Value (Exact Match)
// ============================================================================

bool test_find_name_value() {
    TEST_START("Find by name and value (exact match)");

    // :method GET should return 17
    int idx1 = QPACKStaticTable::find(":method", "GET");
    ASSERT_EQ(idx1, 17);

    // :method POST should return 20
    int idx2 = QPACKStaticTable::find(":method", "POST");
    ASSERT_EQ(idx2, 20);

    // :scheme https should return 23
    int idx3 = QPACKStaticTable::find(":scheme", "https");
    ASSERT_EQ(idx3, 23);

    // :status 200 should return 25
    int idx4 = QPACKStaticTable::find(":status", "200");
    ASSERT_EQ(idx4, 25);

    // :status 404 should return 27
    int idx5 = QPACKStaticTable::find(":status", "404");
    ASSERT_EQ(idx5, 27);

    // content-type application/json should return 46
    int idx6 = QPACKStaticTable::find("content-type", "application/json");
    ASSERT_EQ(idx6, 46);

    // Not found should return -1
    int idx7 = QPACKStaticTable::find("x-custom", "value");
    ASSERT_EQ(idx7, -1);

    // Name exists but value doesn't should return -1
    int idx8 = QPACKStaticTable::find(":method", "TRACE");
    ASSERT_EQ(idx8, -1);

    TEST_PASS();
    return true;
}

// ============================================================================
// Test 6: Find by Name Only (First Match)
// ============================================================================

bool test_find_name_only() {
    TEST_START("Find by name only (returns first match)");

    // :authority (index 0)
    int idx1 = QPACKStaticTable::find_name(":authority");
    ASSERT_EQ(idx1, 0);

    // :path (index 1)
    int idx2 = QPACKStaticTable::find_name(":path");
    ASSERT_EQ(idx2, 1);

    // :method should return first occurrence (CONNECT at index 15)
    int idx3 = QPACKStaticTable::find_name(":method");
    ASSERT_EQ(idx3, 15);

    // :scheme should return first occurrence (http at index 22)
    int idx4 = QPACKStaticTable::find_name(":scheme");
    ASSERT_EQ(idx4, 22);

    // :status should return first occurrence (103 at index 24)
    int idx5 = QPACKStaticTable::find_name(":status");
    ASSERT_EQ(idx5, 24);

    // content-type should return first occurrence (application/dns-message at index 44)
    int idx6 = QPACKStaticTable::find_name("content-type");
    ASSERT_EQ(idx6, 44);

    // Not found should return -1
    int idx7 = QPACKStaticTable::find_name("x-nonexistent");
    ASSERT_EQ(idx7, -1);

    TEST_PASS();
    return true;
}

// ============================================================================
// Test 7: All HTTP Methods
// ============================================================================

bool test_http_methods() {
    TEST_START("All HTTP methods in static table");

    const char* methods[] = {"CONNECT", "DELETE", "GET", "HEAD", "OPTIONS", "POST", "PUT"};
    const int expected_indices[] = {15, 16, 17, 18, 19, 20, 21};

    for (size_t i = 0; i < 7; i++) {
        int idx = QPACKStaticTable::find(":method", methods[i]);
        ASSERT_EQ(idx, expected_indices[i]);

        const StaticEntry* entry = QPACKStaticTable::get(expected_indices[i]);
        ASSERT_NOT_NULL(entry);
        ASSERT_STR_EQ(entry->name, ":method");
        ASSERT_STR_EQ(entry->value, methods[i]);
    }

    TEST_PASS();
    return true;
}

// ============================================================================
// Test 8: All HTTP Status Codes
// ============================================================================

bool test_http_status_codes() {
    TEST_START("All HTTP status codes in static table");

    struct StatusTest {
        const char* code;
        int expected_index;
    };

    const StatusTest tests[] = {
        {"103", 24}, {"200", 25}, {"304", 26}, {"404", 27}, {"503", 28},
        {"100", 63}, {"204", 64}, {"206", 65}, {"302", 66}, {"400", 67},
        {"403", 68}, {"421", 69}, {"425", 70}, {"500", 71}
    };

    for (const auto& test : tests) {
        int idx = QPACKStaticTable::find(":status", test.code);
        ASSERT_EQ(idx, test.expected_index);

        const StaticEntry* entry = QPACKStaticTable::get(test.expected_index);
        ASSERT_NOT_NULL(entry);
        ASSERT_STR_EQ(entry->name, ":status");
        ASSERT_STR_EQ(entry->value, test.code);
    }

    TEST_PASS();
    return true;
}

// ============================================================================
// Test 9: Content-Type Entries
// ============================================================================

bool test_content_types() {
    TEST_START("All content-type entries in static table");

    struct ContentTypeTest {
        const char* value;
        int expected_index;
    };

    const ContentTypeTest tests[] = {
        {"application/dns-message", 44},
        {"application/javascript", 45},
        {"application/json", 46},
        {"application/x-www-form-urlencoded", 47},
        {"image/gif", 48},
        {"image/jpeg", 49},
        {"image/png", 50},
        {"text/css", 51},
        {"text/html; charset=utf-8", 52},
        {"text/plain", 53},
        {"text/plain;charset=utf-8", 54}
    };

    for (const auto& test : tests) {
        int idx = QPACKStaticTable::find("content-type", test.value);
        ASSERT_EQ(idx, test.expected_index);

        const StaticEntry* entry = QPACKStaticTable::get(test.expected_index);
        ASSERT_NOT_NULL(entry);
        ASSERT_STR_EQ(entry->name, "content-type");
        ASSERT_STR_EQ(entry->value, test.value);
    }

    TEST_PASS();
    return true;
}

// ============================================================================
// Test 10: Security Headers
// ============================================================================

bool test_security_headers() {
    TEST_START("Security headers in static table");

    // strict-transport-security (multiple variants)
    int idx1 = QPACKStaticTable::find("strict-transport-security", "max-age=31536000");
    ASSERT_EQ(idx1, 56);

    int idx2 = QPACKStaticTable::find("strict-transport-security", "max-age=31536000; includesubdomains");
    ASSERT_EQ(idx2, 57);

    int idx3 = QPACKStaticTable::find("strict-transport-security", "max-age=31536000; includesubdomains; preload");
    ASSERT_EQ(idx3, 58);

    // x-content-type-options
    int idx4 = QPACKStaticTable::find("x-content-type-options", "nosniff");
    ASSERT_EQ(idx4, 61);

    // x-xss-protection
    int idx5 = QPACKStaticTable::find("x-xss-protection", "1; mode=block");
    ASSERT_EQ(idx5, 62);

    // x-frame-options
    int idx6 = QPACKStaticTable::find("x-frame-options", "deny");
    ASSERT_EQ(idx6, 97);

    int idx7 = QPACKStaticTable::find("x-frame-options", "sameorigin");
    ASSERT_EQ(idx7, 98);

    // content-security-policy
    int idx8 = QPACKStaticTable::find("content-security-policy", "script-src 'none'; object-src 'none'; base-uri 'none'");
    ASSERT_EQ(idx8, 85);

    TEST_PASS();
    return true;
}

// ============================================================================
// Test 11: Empty Value Entries
// ============================================================================

bool test_empty_value_entries() {
    TEST_START("Entries with empty values");

    const char* empty_value_names[] = {
        ":authority", "content-disposition", "cookie", "date", "etag",
        "if-modified-since", "if-none-match", "last-modified", "link",
        "location", "referer", "set-cookie", "accept-language",
        "authorization", "expect-ct", "forwarded", "if-range", "origin",
        "server", "user-agent", "x-forwarded-for"
    };

    for (const char* name : empty_value_names) {
        int idx = QPACKStaticTable::find(name, "");
        ASSERT_TRUE(idx >= 0); // Should find it

        const StaticEntry* entry = QPACKStaticTable::get(idx);
        ASSERT_NOT_NULL(entry);
        ASSERT_STR_EQ(entry->name, name);
        ASSERT_STR_EQ(entry->value, "");
    }

    TEST_PASS();
    return true;
}

// ============================================================================
// Test 12: Performance - Lookup by Index (<10ns target)
// ============================================================================

bool test_performance_lookup_by_index() {
    TEST_START("Performance: Lookup by index (<10ns target)");

    const size_t iterations = 1000000;
    volatile const StaticEntry* result; // Prevent optimization

    auto start = high_resolution_clock::now();

    for (size_t i = 0; i < iterations; i++) {
        // Access different indices to prevent cache effects
        size_t idx = (i * 17) % 99;
        result = QPACKStaticTable::get(idx);
    }

    auto end = high_resolution_clock::now();
    auto duration_ns = duration_cast<nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration_ns) / iterations;

    std::cout << avg_ns << "ns per lookup ";

    // Target: <10ns per lookup
    ASSERT_TRUE(avg_ns < 10.0);

    TEST_PASS();
    return true;
}

// ============================================================================
// Test 13: Performance - Find by Name and Value
// ============================================================================

bool test_performance_find_name_value() {
    TEST_START("Performance: Find by name and value");

    const size_t iterations = 100000;

    struct TestCase {
        const char* name;
        const char* value;
    };

    const TestCase cases[] = {
        {":method", "GET"},
        {":method", "POST"},
        {":scheme", "https"},
        {":status", "200"},
        {":status", "404"},
        {"content-type", "application/json"},
        {"accept-encoding", "gzip, deflate, br"},
    };

    const size_t num_cases = sizeof(cases) / sizeof(cases[0]);
    volatile int result; // Prevent optimization

    auto start = high_resolution_clock::now();

    for (size_t i = 0; i < iterations; i++) {
        const auto& test = cases[i % num_cases];
        result = QPACKStaticTable::find(test.name, test.value);
    }

    auto end = high_resolution_clock::now();
    auto duration_ns = duration_cast<nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration_ns) / iterations;

    std::cout << avg_ns << "ns per lookup ";

    // Linear search through 99 entries should still be fast
    // Target: <500ns (reasonable for linear search)
    ASSERT_TRUE(avg_ns < 500.0);

    TEST_PASS();
    return true;
}

// ============================================================================
// Test 14: Performance - Find by Name
// ============================================================================

bool test_performance_find_name() {
    TEST_START("Performance: Find by name");

    const size_t iterations = 100000;

    const char* names[] = {
        ":authority", ":path", ":method", ":scheme", ":status",
        "content-type", "accept", "accept-encoding", "cache-control"
    };

    const size_t num_names = sizeof(names) / sizeof(names[0]);
    volatile int result; // Prevent optimization

    auto start = high_resolution_clock::now();

    for (size_t i = 0; i < iterations; i++) {
        const char* name = names[i % num_names];
        result = QPACKStaticTable::find_name(name);
    }

    auto end = high_resolution_clock::now();
    auto duration_ns = duration_cast<nanoseconds>(end - start).count();

    double avg_ns = static_cast<double>(duration_ns) / iterations;

    std::cout << avg_ns << "ns per lookup ";

    // Target: <200ns (linear search should be fast for small table)
    ASSERT_TRUE(avg_ns < 200.0);

    TEST_PASS();
    return true;
}

// ============================================================================
// Test 15: Case Sensitivity
// ============================================================================

bool test_case_sensitivity() {
    TEST_START("Header names and values are case-sensitive");

    // RFC 9110: Header field names are case-insensitive in HTTP/1.1 and HTTP/2,
    // but QPACK static table entries are stored in lowercase.
    // Values are case-sensitive.

    // Should NOT find uppercase header names
    int idx1 = QPACKStaticTable::find(":METHOD", "GET");
    ASSERT_EQ(idx1, -1);

    int idx2 = QPACKStaticTable::find("Content-Type", "application/json");
    ASSERT_EQ(idx2, -1);

    // Should NOT find wrong case values
    int idx3 = QPACKStaticTable::find(":method", "get");
    ASSERT_EQ(idx3, -1);

    // Correct case should work
    int idx4 = QPACKStaticTable::find(":method", "GET");
    ASSERT_EQ(idx4, 17);

    TEST_PASS();
    return true;
}

// ============================================================================
// Test 16: Entry Size Calculation (RFC 9204 Section 3.2.1)
// ============================================================================

bool test_entry_size_calculation() {
    TEST_START("Entry size follows RFC 9204 Section 3.2.1");

    // RFC 9204 Section 3.2.1: Entry size = name.length + value.length + 32

    // Check a few entries
    const StaticEntry* e0 = QPACKStaticTable::get(0); // :authority, ""
    ASSERT_NOT_NULL(e0);
    size_t size0 = e0->name.length() + e0->value.length() + 32;
    ASSERT_EQ(size0, 10 + 0 + 32); // 42 bytes

    const StaticEntry* e1 = QPACKStaticTable::get(1); // :path, /
    ASSERT_NOT_NULL(e1);
    size_t size1 = e1->name.length() + e1->value.length() + 32;
    ASSERT_EQ(size1, 5 + 1 + 32); // 38 bytes

    const StaticEntry* e17 = QPACKStaticTable::get(17); // :method, GET
    ASSERT_NOT_NULL(e17);
    size_t size17 = e17->name.length() + e17->value.length() + 32;
    ASSERT_EQ(size17, 7 + 3 + 32); // 42 bytes

    const StaticEntry* e52 = QPACKStaticTable::get(52); // content-type, text/html; charset=utf-8
    ASSERT_NOT_NULL(e52);
    size_t size52 = e52->name.length() + e52->value.length() + 32;
    ASSERT_EQ(size52, 12 + 24 + 32); // 68 bytes

    TEST_PASS();
    return true;
}

// ============================================================================
// Test 17: Pseudo-Header Distribution (RFC 9204 Appendix A)
// ============================================================================

bool test_pseudo_headers_distribution() {
    TEST_START("Pseudo-headers follow RFC 9204 Appendix A ordering");

    // RFC 9204 Appendix A has a specific ordering that's not strictly "all pseudo-headers first"
    // Instead, it groups related headers:
    // - Indices 0-1: :authority, :path (pseudo)
    // - Indices 2-14: Common headers without values
    // - Indices 15-28: :method, :scheme, :status variants (pseudo)
    // - Indices 29+: Other headers

    // Verify key pseudo-header positions
    const StaticEntry* e0 = QPACKStaticTable::get(0);
    ASSERT_NOT_NULL(e0);
    ASSERT_TRUE(e0->name[0] == ':'); // :authority

    const StaticEntry* e1 = QPACKStaticTable::get(1);
    ASSERT_NOT_NULL(e1);
    ASSERT_TRUE(e1->name[0] == ':'); // :path

    // Indices 15-28 should be :method, :scheme, :status
    for (size_t i = 15; i <= 28; i++) {
        const StaticEntry* entry = QPACKStaticTable::get(i);
        ASSERT_NOT_NULL(entry);
        ASSERT_TRUE(entry->name[0] == ':'); // All pseudo-headers
    }

    // Verify we have both pseudo and regular headers
    int pseudo_count = 0;
    int regular_count = 0;

    for (size_t i = 0; i < QPACKStaticTable::size(); i++) {
        const StaticEntry* entry = QPACKStaticTable::get(i);
        ASSERT_NOT_NULL(entry);

        if (entry->name.length() > 0 && entry->name[0] == ':') {
            pseudo_count++;
        } else {
            regular_count++;
        }
    }

    ASSERT_TRUE(pseudo_count > 0);
    ASSERT_TRUE(regular_count > 0);

    TEST_PASS();
    return true;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "QPACK Static Table Test Suite" << std::endl;
    std::cout << "RFC 9204 Appendix A Compliance" << std::endl;
    std::cout << "========================================\n" << std::endl;

    bool all_passed = true;

    all_passed &= test_table_size();
    all_passed &= test_all_entries_rfc_compliant();
    all_passed &= test_key_entries();
    all_passed &= test_out_of_bounds();
    all_passed &= test_find_name_value();
    all_passed &= test_find_name_only();
    all_passed &= test_http_methods();
    all_passed &= test_http_status_codes();
    all_passed &= test_content_types();
    all_passed &= test_security_headers();
    all_passed &= test_empty_value_entries();
    all_passed &= test_performance_lookup_by_index();
    all_passed &= test_performance_find_name_value();
    all_passed &= test_performance_find_name();
    all_passed &= test_case_sensitivity();
    all_passed &= test_entry_size_calculation();
    all_passed &= test_pseudo_headers_distribution();

    std::cout << "\n========================================" << std::endl;
    if (all_passed) {
        std::cout << "ALL TESTS PASSED ✓ (" << passed_count << "/" << test_count << ")" << std::endl;
        std::cout << "========================================" << std::endl;
        return 0;
    } else {
        std::cout << "SOME TESTS FAILED ✗ (" << passed_count << "/" << test_count << " passed)" << std::endl;
        std::cout << "========================================" << std::endl;
        return 1;
    }
}
