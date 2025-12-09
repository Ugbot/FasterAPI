/**
 * Response Cache Middleware Tests
 *
 * Comprehensive tests for HTTP response caching.
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/response_cache.h"
#include <chrono>
#include <random>
#include <thread>

using namespace fasterapi::http;

class ResponseCacheTest : public ::testing::Test {
protected:
    // Generate random string
    std::string random_string(size_t length) {
        static const char charset[] =
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, sizeof(charset) - 2);

        std::string result;
        result.reserve(length);
        for (size_t i = 0; i < length; ++i) {
            result += charset[dis(gen)];
        }
        return result;
    }

    // Create a simple response
    Http1Response create_response(const std::string& body, int status = 200) {
        Http1Response response;
        response.status = status;
        response.status_message = (status == 200) ? "OK" : "Error";
        response.body = body;
        response.headers["Content-Type"] = "application/json";
        return response;
    }
};

// ============================================================================
// Cache-Control Tests
// ============================================================================

TEST_F(ResponseCacheTest, CacheControlToHeader) {
    CacheControl cc;
    cc.public_cache = true;
    cc.max_age = 3600;

    EXPECT_EQ(cc.to_header(), "public, max-age=3600");
}

TEST_F(ResponseCacheTest, CacheControlNoStore) {
    CacheControl cc;
    cc.no_store = true;
    cc.max_age = 3600;  // Should be ignored

    EXPECT_EQ(cc.to_header(), "no-store");
}

TEST_F(ResponseCacheTest, CacheControlAllDirectives) {
    CacheControl cc;
    cc.public_cache = true;
    cc.must_revalidate = true;
    cc.max_age = 300;
    cc.s_maxage = 600;
    cc.stale_while_revalidate = 60;

    std::string header = cc.to_header();
    EXPECT_NE(header.find("public"), std::string::npos);
    EXPECT_NE(header.find("must-revalidate"), std::string::npos);
    EXPECT_NE(header.find("max-age=300"), std::string::npos);
    EXPECT_NE(header.find("s-maxage=600"), std::string::npos);
    EXPECT_NE(header.find("stale-while-revalidate=60"), std::string::npos);
}

TEST_F(ResponseCacheTest, CacheControlParse) {
    std::string header = "public, max-age=3600, must-revalidate";
    auto cc = CacheControl::parse(header);

    EXPECT_TRUE(cc.public_cache);
    EXPECT_TRUE(cc.must_revalidate);
    EXPECT_EQ(cc.max_age, 3600);
}

TEST_F(ResponseCacheTest, CacheControlParseNoStore) {
    auto cc = CacheControl::parse("no-store");
    EXPECT_TRUE(cc.no_store);
}

// ============================================================================
// Basic Cache Tests
// ============================================================================

TEST_F(ResponseCacheTest, CacheMissOnEmptyCache) {
    ResponseCache cache;
    std::unordered_map<std::string, std::string> headers;
    Http1Response response;

    auto result = cache.check("GET", "/api/test", headers, response);
    EXPECT_EQ(result, CacheCheckResult::MISS);
}

TEST_F(ResponseCacheTest, CacheHitAfterStore) {
    ResponseCache cache;
    std::unordered_map<std::string, std::string> headers;

    // Store response
    Http1Response stored = create_response("{\"data\":\"test\"}");
    cache.store("GET", "/api/test", headers, stored);

    // Check cache
    Http1Response response;
    auto result = cache.check("GET", "/api/test", headers, response);

    EXPECT_EQ(result, CacheCheckResult::HIT);
    EXPECT_EQ(response.body, "{\"data\":\"test\"}");
    EXPECT_EQ(response.status, 200);
}

TEST_F(ResponseCacheTest, CacheMissForDifferentPath) {
    ResponseCache cache;
    std::unordered_map<std::string, std::string> headers;

    // Store response for one path
    Http1Response stored = create_response("{\"data\":\"test\"}");
    cache.store("GET", "/api/test1", headers, stored);

    // Check different path
    Http1Response response;
    auto result = cache.check("GET", "/api/test2", headers, response);

    EXPECT_EQ(result, CacheCheckResult::MISS);
}

TEST_F(ResponseCacheTest, PostRequestNotCacheable) {
    ResponseCache cache;
    std::unordered_map<std::string, std::string> headers;
    Http1Response response;

    auto result = cache.check("POST", "/api/test", headers, response);
    EXPECT_EQ(result, CacheCheckResult::NOT_CACHEABLE);
}

TEST_F(ResponseCacheTest, NonCacheableStatusNotStored) {
    ResponseCache cache;
    std::unordered_map<std::string, std::string> headers;

    // Store 404 response
    Http1Response stored = create_response("Not found", 404);
    cache.store("GET", "/api/test", headers, stored);

    // Should still miss
    Http1Response response;
    auto result = cache.check("GET", "/api/test", headers, response);
    EXPECT_EQ(result, CacheCheckResult::MISS);
}

// ============================================================================
// ETag Tests
// ============================================================================

TEST_F(ResponseCacheTest, ETagIsGenerated) {
    ResponseCache cache;
    std::unordered_map<std::string, std::string> headers;

    Http1Response response = create_response("{\"data\":\"test\"}");
    cache.store("GET", "/api/test", headers, response);

    EXPECT_NE(response.headers.find("ETag"), response.headers.end());
    EXPECT_TRUE(response.headers["ETag"].front() == '"');
    EXPECT_TRUE(response.headers["ETag"].back() == '"');
}

TEST_F(ResponseCacheTest, ConditionalRequestReturns304) {
    ResponseCache cache;
    std::unordered_map<std::string, std::string> headers;

    // Store response
    Http1Response stored = create_response("{\"data\":\"test\"}");
    cache.store("GET", "/api/test", headers, stored);
    std::string etag = stored.headers["ETag"];

    // Check with If-None-Match
    std::unordered_map<std::string, std::string> conditional_headers;
    conditional_headers["If-None-Match"] = etag;

    Http1Response response;
    auto result = cache.check("GET", "/api/test", conditional_headers, response);

    EXPECT_EQ(result, CacheCheckResult::NOT_MODIFIED);
    EXPECT_EQ(response.status, 304);
    EXPECT_EQ(response.headers["ETag"], etag);
}

TEST_F(ResponseCacheTest, ConditionalRequestWithWildcard) {
    ResponseCache cache;
    std::unordered_map<std::string, std::string> headers;

    // Store response
    Http1Response stored = create_response("{\"data\":\"test\"}");
    cache.store("GET", "/api/test", headers, stored);

    // Check with If-None-Match: *
    std::unordered_map<std::string, std::string> conditional_headers;
    conditional_headers["If-None-Match"] = "*";

    Http1Response response;
    auto result = cache.check("GET", "/api/test", conditional_headers, response);

    EXPECT_EQ(result, CacheCheckResult::NOT_MODIFIED);
}

TEST_F(ResponseCacheTest, ConditionalRequestMismatchReturnsHit) {
    ResponseCache cache;
    std::unordered_map<std::string, std::string> headers;

    // Store response
    Http1Response stored = create_response("{\"data\":\"test\"}");
    cache.store("GET", "/api/test", headers, stored);

    // Check with different ETag
    std::unordered_map<std::string, std::string> conditional_headers;
    conditional_headers["If-None-Match"] = "\"different-etag\"";

    Http1Response response;
    auto result = cache.check("GET", "/api/test", conditional_headers, response);

    EXPECT_EQ(result, CacheCheckResult::HIT);
    EXPECT_EQ(response.status, 200);
}

// ============================================================================
// TTL and Expiration Tests
// ============================================================================

TEST_F(ResponseCacheTest, ExpiredEntryReturnsStale) {
    ResponseCacheConfig config;
    config.default_ttl_seconds = 1;  // 1 second TTL
    ResponseCache cache(config);

    std::unordered_map<std::string, std::string> headers;

    // Store response
    Http1Response stored = create_response("{\"data\":\"test\"}");
    cache.store("GET", "/api/test", headers, stored);

    // Wait for expiration
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    // Check - should be stale
    Http1Response response;
    auto result = cache.check("GET", "/api/test", headers, response);

    EXPECT_EQ(result, CacheCheckResult::STALE);
}

TEST_F(ResponseCacheTest, CustomTTLOverridesDefault) {
    ResponseCacheConfig config;
    config.default_ttl_seconds = 1;
    ResponseCache cache(config);

    std::unordered_map<std::string, std::string> headers;

    // Store with longer TTL
    Http1Response stored = create_response("{\"data\":\"test\"}");
    cache.store("GET", "/api/test", headers, stored, 10);  // 10 seconds

    // Wait past default TTL
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    // Should still hit
    Http1Response response;
    auto result = cache.check("GET", "/api/test", headers, response);

    EXPECT_EQ(result, CacheCheckResult::HIT);
}

// ============================================================================
// Vary Header Tests
// ============================================================================

TEST_F(ResponseCacheTest, VaryHeaderAffectsCacheKey) {
    ResponseCacheConfig config;
    config.default_vary = {"Accept-Encoding"};
    ResponseCache cache(config);

    // Store response for gzip
    std::unordered_map<std::string, std::string> gzip_headers;
    gzip_headers["Accept-Encoding"] = "gzip";
    Http1Response gzip_response = create_response("gzip content");
    cache.store("GET", "/api/test", gzip_headers, gzip_response);

    // Store response for br
    std::unordered_map<std::string, std::string> br_headers;
    br_headers["Accept-Encoding"] = "br";
    Http1Response br_response = create_response("br content");
    cache.store("GET", "/api/test", br_headers, br_response);

    // Check gzip
    Http1Response response1;
    auto result1 = cache.check("GET", "/api/test", gzip_headers, response1);
    EXPECT_EQ(result1, CacheCheckResult::HIT);
    EXPECT_EQ(response1.body, "gzip content");

    // Check br
    Http1Response response2;
    auto result2 = cache.check("GET", "/api/test", br_headers, response2);
    EXPECT_EQ(result2, CacheCheckResult::HIT);
    EXPECT_EQ(response2.body, "br content");
}

TEST_F(ResponseCacheTest, VaryHeaderAddedToResponse) {
    ResponseCacheConfig config;
    config.default_vary = {"Accept-Encoding", "Accept-Language"};
    ResponseCache cache(config);

    std::unordered_map<std::string, std::string> headers;
    Http1Response response = create_response("test");
    cache.store("GET", "/api/test", headers, response);

    auto vary_header = response.headers.find("Vary");
    ASSERT_NE(vary_header, response.headers.end());
    EXPECT_NE(vary_header->second.find("Accept-Encoding"), std::string::npos);
    EXPECT_NE(vary_header->second.find("Accept-Language"), std::string::npos);
}

// ============================================================================
// LRU Eviction Tests
// ============================================================================

TEST_F(ResponseCacheTest, LRUEvictsOldEntries) {
    ResponseCacheConfig config;
    config.max_cache_size_bytes = 1000;  // Small cache
    config.max_entries = 100;
    config.max_entry_size_bytes = 500;
    ResponseCache cache(config);

    std::unordered_map<std::string, std::string> headers;

    // Fill cache with entries
    for (int i = 0; i < 10; i++) {
        Http1Response response = create_response(std::string(100, 'a'));
        cache.store("GET", "/api/test" + std::to_string(i), headers, response);
    }

    // First entries should be evicted
    Http1Response response;
    auto result = cache.check("GET", "/api/test0", headers, response);
    EXPECT_EQ(result, CacheCheckResult::MISS);

    // Later entries should still be there
    result = cache.check("GET", "/api/test9", headers, response);
    EXPECT_EQ(result, CacheCheckResult::HIT);
}

TEST_F(ResponseCacheTest, LargeEntryNotCached) {
    ResponseCacheConfig config;
    config.max_entry_size_bytes = 100;
    ResponseCache cache(config);

    std::unordered_map<std::string, std::string> headers;

    // Try to store large response
    Http1Response response = create_response(std::string(200, 'x'));
    cache.store("GET", "/api/test", headers, response);

    // Should not be cached
    Http1Response check_response;
    auto result = cache.check("GET", "/api/test", headers, check_response);
    EXPECT_EQ(result, CacheCheckResult::MISS);
}

// ============================================================================
// Invalidation Tests
// ============================================================================

TEST_F(ResponseCacheTest, InvalidateRemovesEntry) {
    ResponseCache cache;
    std::unordered_map<std::string, std::string> headers;

    // Store response
    Http1Response stored = create_response("{\"data\":\"test\"}");
    cache.store("GET", "/api/test", headers, stored);

    // Verify it's cached
    Http1Response response1;
    EXPECT_EQ(cache.check("GET", "/api/test", headers, response1), CacheCheckResult::HIT);

    // Invalidate
    cache.invalidate("GET", "/api/test");

    // Should miss now
    Http1Response response2;
    EXPECT_EQ(cache.check("GET", "/api/test", headers, response2), CacheCheckResult::MISS);
}

TEST_F(ResponseCacheTest, ClearRemovesAllEntries) {
    ResponseCache cache;
    std::unordered_map<std::string, std::string> headers;

    // Store multiple responses
    for (int i = 0; i < 5; i++) {
        Http1Response response = create_response("test" + std::to_string(i));
        cache.store("GET", "/api/test" + std::to_string(i), headers, response);
    }

    // Clear
    cache.clear();

    // All should miss
    for (int i = 0; i < 5; i++) {
        Http1Response response;
        auto result = cache.check("GET", "/api/test" + std::to_string(i), headers, response);
        EXPECT_EQ(result, CacheCheckResult::MISS);
    }
}

// ============================================================================
// Statistics Tests
// ============================================================================

TEST_F(ResponseCacheTest, StatsTrackHitsAndMisses) {
    ResponseCache cache;
    std::unordered_map<std::string, std::string> headers;

    // Miss
    Http1Response response;
    cache.check("GET", "/api/miss", headers, response);

    // Store and hit
    Http1Response stored = create_response("test");
    cache.store("GET", "/api/hit", headers, stored);
    cache.check("GET", "/api/hit", headers, response);

    auto stats = cache.stats();
    EXPECT_EQ(stats.hits, 1);
    EXPECT_EQ(stats.misses, 1);
    EXPECT_EQ(stats.entry_count, 1);
}

// ============================================================================
// Preset Tests
// ============================================================================

TEST_F(ResponseCacheTest, StaticAssetsPreset) {
    auto config = cache_presets::static_assets();
    ResponseCache cache(config);

    std::unordered_map<std::string, std::string> headers;
    Http1Response response = create_response("static content");
    cache.store("GET", "/static/app.js", headers, response);

    auto cc_header = response.headers.find("Cache-Control");
    ASSERT_NE(cc_header, response.headers.end());
    EXPECT_NE(cc_header->second.find("immutable"), std::string::npos);
}

TEST_F(ResponseCacheTest, ApiResponsesPreset) {
    auto config = cache_presets::api_responses();
    ResponseCache cache(config);

    std::unordered_map<std::string, std::string> headers;
    Http1Response response = create_response("{\"api\":\"data\"}");
    cache.store("GET", "/api/users", headers, response);

    auto cc_header = response.headers.find("Cache-Control");
    ASSERT_NE(cc_header, response.headers.end());
    EXPECT_NE(cc_header->second.find("must-revalidate"), std::string::npos);
}

TEST_F(ResponseCacheTest, NoCachePreset) {
    auto config = cache_presets::no_cache();
    ResponseCache cache(config);

    std::unordered_map<std::string, std::string> headers;
    Http1Response response = create_response("sensitive data");
    cache.store("GET", "/api/secret", headers, response);

    // Should not be cached on server
    Http1Response check;
    auto result = cache.check("GET", "/api/secret", headers, check);
    EXPECT_EQ(result, CacheCheckResult::MISS);

    // But should have no-store header
    EXPECT_EQ(response.headers["Cache-Control"], "no-store");
}

// ============================================================================
// Custom Key Generator Tests
// ============================================================================

TEST_F(ResponseCacheTest, CustomKeyGenerator) {
    ResponseCacheConfig config;
    config.key_generator = [](const std::string& method, const std::string& path,
                             const std::unordered_map<std::string, std::string>& headers) {
        auto user_it = headers.find("X-User-ID");
        std::string user = user_it != headers.end() ? user_it->second : "anonymous";
        return method + ":" + path + ":" + user;
    };
    ResponseCache cache(config);

    // Store for user1
    std::unordered_map<std::string, std::string> user1_headers;
    user1_headers["X-User-ID"] = "user1";
    Http1Response user1_response = create_response("user1 data");
    cache.store("GET", "/api/profile", user1_headers, user1_response);

    // Store for user2
    std::unordered_map<std::string, std::string> user2_headers;
    user2_headers["X-User-ID"] = "user2";
    Http1Response user2_response = create_response("user2 data");
    cache.store("GET", "/api/profile", user2_headers, user2_response);

    // Check user1
    Http1Response check1;
    auto result1 = cache.check("GET", "/api/profile", user1_headers, check1);
    EXPECT_EQ(result1, CacheCheckResult::HIT);
    EXPECT_EQ(check1.body, "user1 data");

    // Check user2
    Http1Response check2;
    auto result2 = cache.check("GET", "/api/profile", user2_headers, check2);
    EXPECT_EQ(result2, CacheCheckResult::HIT);
    EXPECT_EQ(check2.body, "user2 data");
}

// ============================================================================
// Performance Tests
// ============================================================================

TEST_F(ResponseCacheTest, PerformanceCacheHit) {
    ResponseCache cache;
    std::unordered_map<std::string, std::string> headers;

    // Pre-populate cache
    Http1Response stored = create_response(std::string(1000, 'x'));
    cache.store("GET", "/api/test", headers, stored);

    const int iterations = 100000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        Http1Response response;
        cache.check("GET", "/api/test", headers, response);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    double per_op_ns = static_cast<double>(duration) * 1000.0 / iterations;
    double ops_per_sec = 1000000000.0 / per_op_ns;

    std::cout << "Cache hit: " << per_op_ns << " ns/op, "
              << ops_per_sec / 1000000.0 << " M ops/sec" << std::endl;

    // Should handle at least 1M ops/sec
    EXPECT_GT(ops_per_sec, 1000000);
}

TEST_F(ResponseCacheTest, PerformanceCacheMiss) {
    ResponseCache cache;
    std::unordered_map<std::string, std::string> headers;

    const int iterations = 100000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        Http1Response response;
        cache.check("GET", "/api/miss" + std::to_string(i % 1000), headers, response);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    double per_op_ns = static_cast<double>(duration) * 1000.0 / iterations;
    double ops_per_sec = 1000000000.0 / per_op_ns;

    std::cout << "Cache miss: " << per_op_ns << " ns/op, "
              << ops_per_sec / 1000000.0 << " M ops/sec" << std::endl;

    // Misses should be fast too
    EXPECT_GT(ops_per_sec, 500000);
}

TEST_F(ResponseCacheTest, PerformanceStore) {
    ResponseCache cache;
    std::unordered_map<std::string, std::string> headers;

    const int iterations = 10000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        Http1Response response = create_response(std::string(500, 'x'));
        cache.store("GET", "/api/test" + std::to_string(i % 1000), headers, response);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    double per_op_us = static_cast<double>(duration) / iterations;
    double ops_per_sec = 1000000.0 / per_op_us;

    std::cout << "Cache store: " << per_op_us << " us/op, "
              << ops_per_sec / 1000.0 << " K ops/sec" << std::endl;

    // Store is slower due to hashing, but should be at least 50K/sec
    EXPECT_GT(ops_per_sec, 50000);
}

// ============================================================================
// Thread Safety Tests
// ============================================================================

TEST_F(ResponseCacheTest, ConcurrentAccess) {
    ResponseCache cache;

    // Pre-populate some entries
    for (int i = 0; i < 100; i++) {
        std::unordered_map<std::string, std::string> headers;
        Http1Response response = create_response("test" + std::to_string(i));
        cache.store("GET", "/api/test" + std::to_string(i), headers, response);
    }

    std::atomic<int> hits{0};
    std::atomic<int> misses{0};

    auto worker = [&](int id) {
        std::unordered_map<std::string, std::string> headers;
        for (int i = 0; i < 1000; i++) {
            int path_id = (id * 1000 + i) % 200;  // Mix of hits and misses

            Http1Response response;
            auto result = cache.check("GET", "/api/test" + std::to_string(path_id), headers, response);

            if (result == CacheCheckResult::HIT) {
                hits++;
            } else {
                misses++;
            }

            // Occasionally store new entries
            if (i % 10 == 0) {
                Http1Response new_response = create_response("new" + std::to_string(path_id));
                cache.store("GET", "/api/new" + std::to_string(path_id), headers, new_response);
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; i++) {
        threads.emplace_back(worker, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    // Just verify it didn't crash and we got some hits/misses
    EXPECT_GT(hits.load(), 0);
    EXPECT_GT(misses.load(), 0);
    std::cout << "Concurrent test: " << hits.load() << " hits, "
              << misses.load() << " misses" << std::endl;
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
