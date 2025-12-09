/**
 * HTTP Caching and Range Request Unit Tests
 *
 * Tests conditional GET (304 Not Modified) and range requests (206 Partial Content):
 * - ETag generation and matching
 * - Last-Modified header handling
 * - If-None-Match validation
 * - If-Modified-Since validation
 * - Range request parsing
 * - Partial content responses
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/response.h"
#include <random>
#include <chrono>

// ===========================================================================
// ETag Tests
// ===========================================================================

class ETagTest : public ::testing::Test {};

TEST_F(ETagTest, SetSimpleETag) {
    HttpResponse res;
    res.etag("abc123");

    EXPECT_EQ(res.get_etag(), "\"abc123\"");
    EXPECT_EQ(res.get_headers().at("etag"), "\"abc123\"");
}

TEST_F(ETagTest, SetQuotedETag) {
    HttpResponse res;
    res.etag("\"already-quoted\"");

    EXPECT_EQ(res.get_etag(), "\"already-quoted\"");
}

TEST_F(ETagTest, SetWeakETag) {
    HttpResponse res;
    res.etag("W/\"weak-tag\"");

    EXPECT_EQ(res.get_etag(), "W/\"weak-tag\"");
}

TEST_F(ETagTest, ClearETag) {
    HttpResponse res;
    res.etag("abc123");
    res.etag("");

    EXPECT_TRUE(res.get_etag().empty());
}

TEST_F(ETagTest, MatchExactETag) {
    HttpResponse res;
    res.etag("abc123");

    EXPECT_TRUE(res.matches_etag("\"abc123\""));
}

TEST_F(ETagTest, MatchWildcard) {
    HttpResponse res;
    res.etag("abc123");

    EXPECT_TRUE(res.matches_etag("*"));
}

TEST_F(ETagTest, NoMatchDifferentETag) {
    HttpResponse res;
    res.etag("abc123");

    EXPECT_FALSE(res.matches_etag("\"xyz789\""));
}

TEST_F(ETagTest, MatchMultipleETags) {
    HttpResponse res;
    res.etag("tag2");

    EXPECT_TRUE(res.matches_etag("\"tag1\", \"tag2\", \"tag3\""));
}

TEST_F(ETagTest, MatchWeakETag) {
    HttpResponse res;
    res.etag("abc123");

    // Weak comparison should match
    EXPECT_TRUE(res.matches_etag("W/\"abc123\""));
}

TEST_F(ETagTest, NoMatchEmptyClientETag) {
    HttpResponse res;
    res.etag("abc123");

    EXPECT_FALSE(res.matches_etag(""));
}

TEST_F(ETagTest, NoMatchEmptyServerETag) {
    HttpResponse res;
    // Don't set ETag

    EXPECT_FALSE(res.matches_etag("\"abc123\""));
}

// ===========================================================================
// Last-Modified Tests
// ===========================================================================

class LastModifiedTest : public ::testing::Test {};

TEST_F(LastModifiedTest, SetFromTimestamp) {
    HttpResponse res;
    // Use a known timestamp: 2024-01-15 12:00:00 UTC = 1705320000
    res.last_modified(1705320000);

    EXPECT_EQ(res.get_last_modified(), 1705320000u);
    EXPECT_TRUE(res.get_headers().count("last-modified") > 0);
}

TEST_F(LastModifiedTest, SetFromDateString) {
    HttpResponse res;
    res.last_modified("Sat, 29 Oct 1994 19:43:31 GMT");

    EXPECT_TRUE(res.get_last_modified() > 0);
    EXPECT_EQ(res.get_headers().at("last-modified"), "Sat, 29 Oct 1994 19:43:31 GMT");
}

TEST_F(LastModifiedTest, NotModifiedSinceOlderDate) {
    HttpResponse res;
    // Server file is from 2024-01-15
    res.last_modified(1705320000);

    // Client has version from 2024-01-20 (newer) - should be not modified
    EXPECT_TRUE(res.not_modified_since("Sat, 20 Jan 2024 12:00:00 GMT"));
}

TEST_F(LastModifiedTest, ModifiedSinceOlderDate) {
    HttpResponse res;
    // Server file is from 2024-01-15
    res.last_modified(1705320000);

    // Client has version from 2024-01-10 (older) - should be modified
    EXPECT_FALSE(res.not_modified_since("Wed, 10 Jan 2024 12:00:00 GMT"));
}

TEST_F(LastModifiedTest, NotModifiedSinceEmptyHeader) {
    HttpResponse res;
    res.last_modified(1705320000);

    EXPECT_FALSE(res.not_modified_since(""));
}

TEST_F(LastModifiedTest, NotModifiedSinceNoTimestamp) {
    HttpResponse res;
    // Don't set last_modified

    EXPECT_FALSE(res.not_modified_since("Sat, 20 Jan 2024 12:00:00 GMT"));
}

// ===========================================================================
// Cache-Control Tests
// ===========================================================================

class CacheControlTest : public ::testing::Test {};

TEST_F(CacheControlTest, SetPublicMaxAge) {
    HttpResponse res;
    res.cache_control("public, max-age=3600");

    EXPECT_EQ(res.get_headers().at("cache-control"), "public, max-age=3600");
}

TEST_F(CacheControlTest, SetNoCache) {
    HttpResponse res;
    res.cache_control("no-cache");

    EXPECT_EQ(res.get_headers().at("cache-control"), "no-cache");
}

TEST_F(CacheControlTest, SetNoStore) {
    HttpResponse res;
    res.cache_control("no-store");

    EXPECT_EQ(res.get_headers().at("cache-control"), "no-store");
}

TEST_F(CacheControlTest, SetPrivateMaxAge) {
    HttpResponse res;
    res.cache_control("private, max-age=86400, must-revalidate");

    EXPECT_EQ(res.get_headers().at("cache-control"), "private, max-age=86400, must-revalidate");
}

// ===========================================================================
// 304 Not Modified Tests
// ===========================================================================

class NotModifiedTest : public ::testing::Test {};

TEST_F(NotModifiedTest, BasicNotModified) {
    HttpResponse res;
    res.text("Hello World");
    res.etag("abc123");
    res.not_modified();

    EXPECT_EQ(res.get_status_code(), HttpResponse::Status::NOT_MODIFIED);
    EXPECT_TRUE(res.get_body().empty());
}

TEST_F(NotModifiedTest, NotModifiedPreservesETag) {
    HttpResponse res;
    res.text("Hello World");
    res.etag("abc123");
    res.not_modified();

    EXPECT_EQ(res.get_etag(), "\"abc123\"");
    EXPECT_EQ(res.get_headers().at("etag"), "\"abc123\"");
}

TEST_F(NotModifiedTest, NotModifiedClearsBody) {
    HttpResponse res;
    res.text("This is a long body that should be cleared");
    res.not_modified();

    EXPECT_TRUE(res.get_body().empty());
}

TEST_F(NotModifiedTest, ConditionalETagMatch) {
    HttpResponse res;
    res.text("Hello World");
    res.etag("abc123");

    // Simulate checking If-None-Match
    std::string client_etag = "\"abc123\"";
    if (res.matches_etag(client_etag)) {
        res.not_modified();
    }

    EXPECT_EQ(res.get_status_code(), HttpResponse::Status::NOT_MODIFIED);
}

TEST_F(NotModifiedTest, ConditionalETagNoMatch) {
    HttpResponse res;
    res.text("Hello World");
    res.etag("abc123");

    std::string client_etag = "\"different-tag\"";
    if (res.matches_etag(client_etag)) {
        res.not_modified();
    }

    // Should still be 200 OK
    EXPECT_EQ(res.get_status_code(), HttpResponse::Status::OK);
    EXPECT_EQ(res.get_body(), "Hello World");
}

// ===========================================================================
// Range Request Tests
// ===========================================================================

class RangeRequestTest : public ::testing::Test {};

TEST_F(RangeRequestTest, AcceptRanges) {
    HttpResponse res;
    res.accept_ranges();

    EXPECT_EQ(res.get_headers().at("accept-ranges"), "bytes");
}

TEST_F(RangeRequestTest, AcceptRangesNone) {
    HttpResponse res;
    res.accept_ranges("none");

    EXPECT_EQ(res.get_headers().at("accept-ranges"), "none");
}

TEST_F(RangeRequestTest, PartialContentString) {
    HttpResponse res;
    std::string data = "Hello World!";  // 12 bytes
    res.partial_content(data, 0, 4, data.size());  // "Hello"

    EXPECT_EQ(res.get_status_code(), HttpResponse::Status::PARTIAL_CONTENT);
    EXPECT_EQ(res.get_body(), "Hello");
    EXPECT_EQ(res.get_headers().at("content-range"), "bytes 0-4/12");
}

TEST_F(RangeRequestTest, PartialContentMiddle) {
    HttpResponse res;
    std::string data = "Hello World!";
    res.partial_content(data, 6, 10, data.size());  // "World"

    EXPECT_EQ(res.get_body(), "World");
    EXPECT_EQ(res.get_headers().at("content-range"), "bytes 6-10/12");
}

TEST_F(RangeRequestTest, PartialContentBinary) {
    HttpResponse res;
    std::vector<uint8_t> data = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    res.partial_content(data, 2, 5, data.size());

    EXPECT_EQ(res.get_status_code(), HttpResponse::Status::PARTIAL_CONTENT);
    EXPECT_EQ(res.get_headers().at("content-range"), "bytes 2-5/8");
}

TEST_F(RangeRequestTest, RangeNotSatisfiableStartTooLarge) {
    HttpResponse res;
    std::string data = "Hello";
    res.partial_content(data, 10, 15, data.size());  // Start beyond end

    EXPECT_EQ(res.get_status_code(), HttpResponse::Status::RANGE_NOT_SATISFIABLE);
    EXPECT_EQ(res.get_headers().at("content-range"), "bytes */5");
}

TEST_F(RangeRequestTest, RangeNotSatisfiableEndTooLarge) {
    HttpResponse res;
    std::string data = "Hello";
    res.partial_content(data, 0, 10, data.size());  // End beyond total

    EXPECT_EQ(res.get_status_code(), HttpResponse::Status::RANGE_NOT_SATISFIABLE);
}

TEST_F(RangeRequestTest, RangeNotSatisfiableInverted) {
    HttpResponse res;
    std::string data = "Hello World!";
    res.partial_content(data, 10, 5, data.size());  // Start > End

    EXPECT_EQ(res.get_status_code(), HttpResponse::Status::RANGE_NOT_SATISFIABLE);
}

TEST_F(RangeRequestTest, PartialContentFullRange) {
    HttpResponse res;
    std::string data = "Complete";
    res.partial_content(data, 0, data.size() - 1, data.size());

    EXPECT_EQ(res.get_status_code(), HttpResponse::Status::PARTIAL_CONTENT);
    EXPECT_EQ(res.get_body(), "Complete");
    EXPECT_EQ(res.get_headers().at("content-range"), "bytes 0-7/8");
}

// ===========================================================================
// Wire Format Tests
// ===========================================================================

class WireFormatTest : public ::testing::Test {};

TEST_F(WireFormatTest, NotModifiedWireFormat) {
    HttpResponse res;
    res.etag("abc123");
    res.not_modified();

    std::string wire = res.to_http_wire_format();

    EXPECT_TRUE(wire.find("HTTP/1.1 304 Not Modified") != std::string::npos);
    EXPECT_TRUE(wire.find("etag: \"abc123\"") != std::string::npos);
    // Should have minimal body
}

TEST_F(WireFormatTest, PartialContentWireFormat) {
    HttpResponse res;
    std::string data = "Hello World!";
    res.partial_content(data, 0, 4, data.size());
    res.content_type("text/plain");

    std::string wire = res.to_http_wire_format();

    EXPECT_TRUE(wire.find("HTTP/1.1 206 Partial Content") != std::string::npos);
    EXPECT_TRUE(wire.find("content-range: bytes 0-4/12") != std::string::npos);
    EXPECT_TRUE(wire.find("Hello") != std::string::npos);
}

// ===========================================================================
// Integration Tests
// ===========================================================================

class CachingIntegrationTest : public ::testing::Test {};

TEST_F(CachingIntegrationTest, FullCachingFlow) {
    // First request - returns full content
    HttpResponse res1;
    res1.text("Hello World");
    res1.etag("v1");
    res1.cache_control("public, max-age=3600");

    EXPECT_EQ(res1.get_status_code(), HttpResponse::Status::OK);
    EXPECT_EQ(res1.get_body(), "Hello World");
    std::string etag1 = res1.get_etag();

    // Second request with If-None-Match - returns 304
    HttpResponse res2;
    res2.text("Hello World");
    res2.etag("v1");
    res2.cache_control("public, max-age=3600");

    if (res2.matches_etag(etag1)) {
        res2.not_modified();
    }

    EXPECT_EQ(res2.get_status_code(), HttpResponse::Status::NOT_MODIFIED);
    EXPECT_TRUE(res2.get_body().empty());
}

TEST_F(CachingIntegrationTest, FullRangeFlow) {
    std::string full_content = "This is a large file content for testing range requests.";

    // First request - full content with Accept-Ranges
    HttpResponse res1;
    res1.text(full_content);
    res1.accept_ranges();

    EXPECT_EQ(res1.get_headers().at("accept-ranges"), "bytes");

    // Second request - partial content
    HttpResponse res2;
    res2.partial_content(full_content, 0, 3, full_content.size());  // "This"
    res2.content_type("text/plain");

    EXPECT_EQ(res2.get_status_code(), HttpResponse::Status::PARTIAL_CONTENT);
    EXPECT_EQ(res2.get_body(), "This");
}

TEST_F(CachingIntegrationTest, CombinedETagAndLastModified) {
    HttpResponse res;
    res.text("Content");
    res.etag("abc123");
    res.last_modified(1705320000);
    res.cache_control("public, max-age=3600");

    // Both ETag and Last-Modified should be set
    EXPECT_FALSE(res.get_etag().empty());
    EXPECT_GT(res.get_last_modified(), 0u);
    EXPECT_TRUE(res.get_headers().count("etag") > 0);
    EXPECT_TRUE(res.get_headers().count("last-modified") > 0);
    EXPECT_TRUE(res.get_headers().count("cache-control") > 0);
}

// ===========================================================================
// Edge Cases
// ===========================================================================

class CachingEdgeCaseTest : public ::testing::Test {};

TEST_F(CachingEdgeCaseTest, EmptyBodyPartialContent) {
    HttpResponse res;
    std::string data = "";
    // This should fail - can't get range from empty content
    res.partial_content(data, 0, 0, 0);

    // Implementation note: 0-0/0 is technically invalid
    // Most implementations return 416 for empty content
}

TEST_F(CachingEdgeCaseTest, MultipleETagsInIfNoneMatch) {
    HttpResponse res;
    res.etag("third");

    // Client sends multiple ETags, one matches
    EXPECT_TRUE(res.matches_etag("\"first\", \"second\", \"third\", \"fourth\""));
}

TEST_F(CachingEdgeCaseTest, ETagWithSpecialCharacters) {
    HttpResponse res;
    res.etag("abc/123+456=");

    EXPECT_EQ(res.get_etag(), "\"abc/123+456=\"");
    EXPECT_TRUE(res.matches_etag("\"abc/123+456=\""));
}

TEST_F(CachingEdgeCaseTest, WeakETagComparison) {
    HttpResponse res;
    res.etag("W/\"weak-tag\"");

    // Strong ETag should match weak using weak comparison
    EXPECT_TRUE(res.matches_etag("\"weak-tag\""));
    EXPECT_TRUE(res.matches_etag("W/\"weak-tag\""));
}

TEST_F(CachingEdgeCaseTest, ChainedMethods) {
    HttpResponse res;
    res.etag("v1")
       .last_modified(1705320000)
       .cache_control("public, max-age=3600")
       .accept_ranges()
       .text("Content");

    EXPECT_EQ(res.get_etag(), "\"v1\"");
    EXPECT_EQ(res.get_last_modified(), 1705320000u);
    EXPECT_EQ(res.get_headers().at("cache-control"), "public, max-age=3600");
    EXPECT_EQ(res.get_headers().at("accept-ranges"), "bytes");
}

// ===========================================================================
// Performance Tests
// ===========================================================================

class CachingPerformanceTest : public ::testing::Test {};

TEST_F(CachingPerformanceTest, ETagMatchingPerformance) {
    HttpResponse res;
    res.etag("target-etag");

    // Build a large If-None-Match header with many ETags
    std::string if_none_match;
    for (int i = 0; i < 99; i++) {
        if_none_match += "\"etag-" + std::to_string(i) + "\", ";
    }
    if_none_match += "\"target-etag\"";  // Target is last

    auto start = std::chrono::high_resolution_clock::now();
    bool matched = res.matches_etag(if_none_match);
    auto end = std::chrono::high_resolution_clock::now();

    EXPECT_TRUE(matched);

    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    std::cout << "ETag matching 100 ETags: " << duration.count() << " us\n";
    EXPECT_LT(duration.count(), 1000);  // Should complete in under 1ms
}

TEST_F(CachingPerformanceTest, LargePartialContent) {
    // Create 10MB of data
    std::string large_data(10 * 1024 * 1024, 'X');

    auto start = std::chrono::high_resolution_clock::now();
    HttpResponse res;
    res.partial_content(large_data, 1000000, 2000000, large_data.size());  // 1MB slice
    auto end = std::chrono::high_resolution_clock::now();

    EXPECT_EQ(res.get_status_code(), HttpResponse::Status::PARTIAL_CONTENT);
    EXPECT_EQ(res.get_body().size(), 1000001u);  // inclusive range

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::cout << "1MB partial content from 10MB: " << duration.count() << " ms\n";
    EXPECT_LT(duration.count(), 100);  // Should complete in under 100ms
}
