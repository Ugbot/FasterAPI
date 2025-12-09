/**
 * Rate Limiter Tests
 *
 * Tests for token bucket, sliding window, and fixed window rate limiting.
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/rate_limiter.h"
#include <thread>
#include <chrono>
#include <random>
#include <vector>
#include <atomic>

using namespace fasterapi::http;

// =============================================================================
// Test Fixtures
// =============================================================================

class RateLimiterTest : public ::testing::Test {
protected:
    void SetUp() override {
        gen_.seed(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    std::string random_key() {
        return "key_" + std::to_string(gen_());
    }

    std::string random_ip() {
        return std::to_string(gen_() % 256) + "." +
               std::to_string(gen_() % 256) + "." +
               std::to_string(gen_() % 256) + "." +
               std::to_string(gen_() % 256);
    }

    std::mt19937 gen_;
};

// =============================================================================
// Token Bucket Tests
// =============================================================================

TEST_F(RateLimiterTest, TokenBucketBasicAllowance) {
    RateLimitConfig config;
    config.algorithm = RateLimitAlgorithm::TOKEN_BUCKET;
    config.requests_per_window = 10;
    config.window_size_ms = 1000;  // 1 second
    config.burst_size = 10;

    RateLimiter limiter(config);
    std::string key = random_key();

    // First 10 requests should be allowed
    for (int i = 0; i < 10; i++) {
        auto result = limiter.check(key);
        EXPECT_TRUE(result.allowed) << "Request " << i << " should be allowed";
        EXPECT_EQ(result.remaining, 10 - i - 1) << "Remaining incorrect at " << i;
    }
}

TEST_F(RateLimiterTest, TokenBucketRateLimits) {
    RateLimitConfig config;
    config.algorithm = RateLimitAlgorithm::TOKEN_BUCKET;
    config.requests_per_window = 5;
    config.window_size_ms = 1000;
    config.burst_size = 5;

    RateLimiter limiter(config);
    std::string key = random_key();

    // Exhaust the bucket
    for (int i = 0; i < 5; i++) {
        EXPECT_TRUE(limiter.check(key).allowed);
    }

    // Next request should be rate limited
    auto result = limiter.check(key);
    EXPECT_FALSE(result.allowed);
    EXPECT_EQ(result.remaining, 0);
    EXPECT_GT(result.retry_after_ms, 0);
}

TEST_F(RateLimiterTest, TokenBucketReplenishment) {
    RateLimitConfig config;
    config.algorithm = RateLimitAlgorithm::TOKEN_BUCKET;
    config.requests_per_window = 10;
    config.window_size_ms = 100;  // 100ms window = 100 requests/second
    config.burst_size = 10;

    RateLimiter limiter(config);
    std::string key = random_key();

    // Exhaust bucket
    for (int i = 0; i < 10; i++) {
        limiter.check(key);
    }
    EXPECT_FALSE(limiter.check(key).allowed);

    // Wait for replenishment (enough time for ~5 tokens)
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    // Should have some tokens now
    auto result = limiter.check(key);
    EXPECT_TRUE(result.allowed);
}

TEST_F(RateLimiterTest, TokenBucketBurstCapacity) {
    RateLimitConfig config;
    config.algorithm = RateLimitAlgorithm::TOKEN_BUCKET;
    config.requests_per_window = 10;
    config.window_size_ms = 1000;
    config.burst_size = 20;  // Double the rate

    RateLimiter limiter(config);
    std::string key = random_key();

    // Should allow burst_size requests immediately
    for (int i = 0; i < 20; i++) {
        auto result = limiter.check(key);
        EXPECT_TRUE(result.allowed) << "Burst request " << i << " should be allowed";
    }

    // Now should be rate limited
    EXPECT_FALSE(limiter.check(key).allowed);
}

TEST_F(RateLimiterTest, TokenBucketDifferentKeys) {
    RateLimitConfig config;
    config.algorithm = RateLimitAlgorithm::TOKEN_BUCKET;
    config.requests_per_window = 5;
    config.burst_size = 5;

    RateLimiter limiter(config);
    std::string key1 = random_key();
    std::string key2 = random_key();

    // Exhaust key1's bucket
    for (int i = 0; i < 5; i++) {
        limiter.check(key1);
    }
    EXPECT_FALSE(limiter.check(key1).allowed);

    // key2 should still have full bucket
    for (int i = 0; i < 5; i++) {
        EXPECT_TRUE(limiter.check(key2).allowed);
    }
}

// =============================================================================
// Sliding Window Tests
// =============================================================================

TEST_F(RateLimiterTest, SlidingWindowBasicAllowance) {
    RateLimitConfig config;
    config.algorithm = RateLimitAlgorithm::SLIDING_WINDOW;
    config.requests_per_window = 10;
    config.window_size_ms = 1000;
    config.sliding_window_granularity = 5;

    RateLimiter limiter(config);
    std::string key = random_key();

    // First 10 requests should be allowed
    for (int i = 0; i < 10; i++) {
        auto result = limiter.check(key);
        EXPECT_TRUE(result.allowed) << "Request " << i << " should be allowed";
    }

    // 11th should be rate limited
    EXPECT_FALSE(limiter.check(key).allowed);
}

TEST_F(RateLimiterTest, SlidingWindowReset) {
    RateLimitConfig config;
    config.algorithm = RateLimitAlgorithm::SLIDING_WINDOW;
    config.requests_per_window = 5;
    config.window_size_ms = 100;  // Short window for testing
    config.sliding_window_granularity = 5;

    RateLimiter limiter(config);
    std::string key = random_key();

    // Exhaust limit
    for (int i = 0; i < 5; i++) {
        limiter.check(key);
    }
    EXPECT_FALSE(limiter.check(key).allowed);

    // Wait for window to expire
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Should be allowed again
    EXPECT_TRUE(limiter.check(key).allowed);
}

TEST_F(RateLimiterTest, SlidingWindowGranularity) {
    RateLimitConfig config;
    config.algorithm = RateLimitAlgorithm::SLIDING_WINDOW;
    config.requests_per_window = 10;
    config.window_size_ms = 100;
    config.sliding_window_granularity = 10;  // 10 sub-windows of 10ms each

    RateLimiter limiter(config);
    std::string key = random_key();

    // Use 5 requests
    for (int i = 0; i < 5; i++) {
        limiter.check(key);
    }

    // Wait for half window
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    // Should still count the old requests but have 5 more available
    auto result = limiter.peek(key);
    // Due to sub-window rotation, some old requests may have expired
    EXPECT_GT(result.remaining, 0);
}

// =============================================================================
// Fixed Window Tests
// =============================================================================

TEST_F(RateLimiterTest, FixedWindowBasicAllowance) {
    RateLimitConfig config;
    config.algorithm = RateLimitAlgorithm::FIXED_WINDOW;
    config.requests_per_window = 10;
    config.window_size_ms = 1000;

    RateLimiter limiter(config);
    std::string key = random_key();

    for (int i = 0; i < 10; i++) {
        auto result = limiter.check(key);
        EXPECT_TRUE(result.allowed);
        EXPECT_EQ(result.remaining, 10 - i - 1);
    }

    EXPECT_FALSE(limiter.check(key).allowed);
}

TEST_F(RateLimiterTest, FixedWindowReset) {
    RateLimitConfig config;
    config.algorithm = RateLimitAlgorithm::FIXED_WINDOW;
    config.requests_per_window = 5;
    config.window_size_ms = 100;

    RateLimiter limiter(config);
    std::string key = random_key();

    // Exhaust limit
    for (int i = 0; i < 5; i++) {
        limiter.check(key);
    }
    EXPECT_FALSE(limiter.check(key).allowed);

    // Wait for window to expire
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Should be allowed again
    EXPECT_TRUE(limiter.check(key).allowed);
}

// =============================================================================
// Result Metadata Tests
// =============================================================================

TEST_F(RateLimiterTest, ResultContainsCorrectLimit) {
    RateLimitConfig config;
    config.requests_per_window = 42;

    RateLimiter limiter(config);
    auto result = limiter.check(random_key());

    EXPECT_EQ(result.limit, 42);
}

TEST_F(RateLimiterTest, ResultContainsResetTime) {
    RateLimitConfig config;
    config.window_size_ms = 60000;  // 1 minute

    RateLimiter limiter(config);
    auto result = limiter.check(random_key());

    // Reset time should be in the future
    auto now = std::chrono::system_clock::now();
    auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()
    ).count();

    // The reset_at is based on steady_clock, so we can't compare directly
    // Just check it's a reasonable value (non-zero)
    EXPECT_GT(result.reset_at, 0);
}

TEST_F(RateLimiterTest, RetryAfterWhenLimited) {
    // Use fixed window for deterministic rate limiting
    RateLimitConfig config;
    config.algorithm = RateLimitAlgorithm::FIXED_WINDOW;
    config.requests_per_window = 1;
    config.window_size_ms = 5000;  // 5 seconds

    RateLimiter limiter(config);
    std::string key = random_key();

    limiter.check(key);  // Use the only allowed request
    auto result = limiter.check(key);

    EXPECT_FALSE(result.allowed);
    EXPECT_GT(result.retry_after_ms, 0);
    EXPECT_LE(result.retry_after_ms, 5000);  // Should be less than window size
}

// =============================================================================
// Peek Tests
// =============================================================================

TEST_F(RateLimiterTest, PeekDoesNotConsumeQuota) {
    // Use fixed window for deterministic behavior
    RateLimitConfig config;
    config.algorithm = RateLimitAlgorithm::FIXED_WINDOW;
    config.requests_per_window = 5;
    config.window_size_ms = 60000;  // Long window

    RateLimiter limiter(config);
    std::string key = random_key();

    // First, make one check to establish the key
    limiter.check(key);  // Uses 1

    // Peek multiple times - should always show 4 remaining
    for (int i = 0; i < 10; i++) {
        auto result = limiter.peek(key);
        EXPECT_TRUE(result.allowed);
        EXPECT_EQ(result.remaining, 4);  // 5 - 1 = 4
    }

    // Actually consume another
    limiter.check(key);  // Uses 2

    // Peek should show further reduced quota
    auto result = limiter.peek(key);
    EXPECT_TRUE(result.allowed);
    EXPECT_EQ(result.remaining, 3);  // 5 - 2 = 3
}

// =============================================================================
// Reset Tests
// =============================================================================

TEST_F(RateLimiterTest, ResetResetsQuota) {
    // Use fixed window for deterministic behavior
    RateLimitConfig config;
    config.algorithm = RateLimitAlgorithm::FIXED_WINDOW;
    config.requests_per_window = 5;
    config.window_size_ms = 60000;

    RateLimiter limiter(config);
    std::string key = random_key();

    // Exhaust quota
    for (int i = 0; i < 5; i++) {
        limiter.check(key);
    }
    EXPECT_FALSE(limiter.check(key).allowed);

    // Reset
    limiter.reset(key);

    // Should have full quota again
    EXPECT_TRUE(limiter.check(key).allowed);
}

TEST_F(RateLimiterTest, ClearRemovesAllKeys) {
    RateLimitConfig config;
    config.requests_per_window = 5;

    RateLimiter limiter(config);

    // Add multiple keys
    for (int i = 0; i < 10; i++) {
        for (int j = 0; j < 5; j++) {
            limiter.check("key_" + std::to_string(i));
        }
    }

    EXPECT_EQ(limiter.size(), 10);

    // Clear all
    limiter.clear();

    EXPECT_EQ(limiter.size(), 0);
}

// =============================================================================
// Middleware Tests
// =============================================================================

TEST_F(RateLimiterTest, MiddlewareExtractsForwardedFor) {
    // Use fixed window for deterministic behavior
    RateLimitConfig config;
    config.algorithm = RateLimitAlgorithm::FIXED_WINDOW;
    config.requests_per_window = 5;
    config.window_size_ms = 60000;

    RateLimitMiddleware middleware(config);

    std::unordered_map<std::string, std::string> headers;
    headers["X-Forwarded-For"] = "192.168.1.1, 10.0.0.1";

    for (int i = 0; i < 5; i++) {
        EXPECT_TRUE(middleware.check("GET", "/api/test", headers).allowed);
    }

    // Same IP should be rate limited
    EXPECT_FALSE(middleware.check("GET", "/api/test", headers).allowed);

    // Different IP should be allowed
    headers["X-Forwarded-For"] = "192.168.1.2";
    EXPECT_TRUE(middleware.check("GET", "/api/test", headers).allowed);
}

TEST_F(RateLimiterTest, MiddlewareExtractsRealIP) {
    // Use fixed window for deterministic behavior
    RateLimitConfig config;
    config.algorithm = RateLimitAlgorithm::FIXED_WINDOW;
    config.requests_per_window = 3;
    config.window_size_ms = 60000;

    RateLimitMiddleware middleware(config);

    std::unordered_map<std::string, std::string> headers;
    headers["X-Real-IP"] = "10.0.0.1";

    for (int i = 0; i < 3; i++) {
        middleware.check("GET", "/", headers);
    }

    EXPECT_FALSE(middleware.check("GET", "/", headers).allowed);
}

TEST_F(RateLimiterTest, MiddlewareCustomKeyExtractor) {
    // Use fixed window for deterministic behavior
    RateLimitConfig config;
    config.algorithm = RateLimitAlgorithm::FIXED_WINDOW;
    config.requests_per_window = 5;
    config.window_size_ms = 60000;

    // Key by API key header
    RateLimitMiddleware middleware(config, [](
        const std::string& /*method*/,
        const std::string& /*path*/,
        const std::unordered_map<std::string, std::string>& headers
    ) -> std::string {
        auto it = headers.find("X-API-Key");
        return it != headers.end() ? it->second : "anonymous";
    });

    std::unordered_map<std::string, std::string> headers1;
    headers1["X-API-Key"] = "key_abc";

    std::unordered_map<std::string, std::string> headers2;
    headers2["X-API-Key"] = "key_xyz";

    // Exhaust key_abc
    for (int i = 0; i < 5; i++) {
        middleware.check("GET", "/", headers1);
    }
    EXPECT_FALSE(middleware.check("GET", "/", headers1).allowed);

    // key_xyz should still work
    EXPECT_TRUE(middleware.check("GET", "/", headers2).allowed);
}

TEST_F(RateLimiterTest, MiddlewareAddsHeaders) {
    RateLimitResult result;
    result.allowed = true;
    result.remaining = 42;
    result.limit = 100;
    result.reset_at = 1234567890;
    result.retry_after_ms = 0;

    std::unordered_map<std::string, std::string> headers;
    RateLimitMiddleware::add_headers(result, headers);

    EXPECT_EQ(headers["X-RateLimit-Limit"], "100");
    EXPECT_EQ(headers["X-RateLimit-Remaining"], "42");
    EXPECT_EQ(headers["X-RateLimit-Reset"], "1234567890");
    EXPECT_EQ(headers.count("Retry-After"), 0);
}

TEST_F(RateLimiterTest, MiddlewareAddsRetryAfter) {
    RateLimitResult result;
    result.allowed = false;
    result.remaining = 0;
    result.limit = 100;
    result.reset_at = 1234567890;
    result.retry_after_ms = 5500;  // 5.5 seconds

    std::unordered_map<std::string, std::string> headers;
    RateLimitMiddleware::add_headers(result, headers);

    EXPECT_EQ(headers["Retry-After"], "6");  // Rounded up
}

// =============================================================================
// Endpoint Rate Limiter Tests
// =============================================================================

TEST_F(RateLimiterTest, EndpointLimiterExactMatch) {
    EndpointRateLimiter limiter;

    // Use fixed window for deterministic behavior
    RateLimitConfig strict;
    strict.algorithm = RateLimitAlgorithm::FIXED_WINDOW;
    strict.requests_per_window = 3;
    strict.window_size_ms = 60000;
    limiter.add_rule("/api/login", strict);

    std::string ip = random_ip();

    // Exact match should use the rule
    for (int i = 0; i < 3; i++) {
        auto result = limiter.check("/api/login", ip);
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(result->allowed);
    }

    auto result = limiter.check("/api/login", ip);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->allowed);
}

TEST_F(RateLimiterTest, EndpointLimiterWildcard) {
    EndpointRateLimiter limiter;

    RateLimitConfig api_limit;
    api_limit.requests_per_window = 5;
    limiter.add_rule("/api/*", api_limit);

    std::string ip = random_ip();

    // Should match various API endpoints
    EXPECT_TRUE(limiter.check("/api/users", ip).has_value());
    EXPECT_TRUE(limiter.check("/api/products", ip).has_value());
    EXPECT_TRUE(limiter.check("/api/v1/items", ip).has_value());

    // Should not match non-API paths
    EXPECT_FALSE(limiter.check("/static/file.js", ip).has_value());
    EXPECT_FALSE(limiter.check("/", ip).has_value());
}

TEST_F(RateLimiterTest, EndpointLimiterMultipleRules) {
    EndpointRateLimiter limiter;

    // Use fixed window for deterministic behavior
    RateLimitConfig strict;
    strict.algorithm = RateLimitAlgorithm::FIXED_WINDOW;
    strict.requests_per_window = 2;
    strict.window_size_ms = 60000;
    limiter.add_rule("/api/login", strict);

    RateLimitConfig normal;
    normal.algorithm = RateLimitAlgorithm::FIXED_WINDOW;
    normal.requests_per_window = 10;
    normal.window_size_ms = 60000;
    limiter.add_rule("/api/*", normal);

    std::string ip = random_ip();

    // Login uses strict limit (first matching rule)
    for (int i = 0; i < 2; i++) {
        auto result = limiter.check("/api/login", ip);
        EXPECT_TRUE(result->allowed);
    }
    EXPECT_FALSE(limiter.check("/api/login", ip)->allowed);

    // Other API uses normal limit
    for (int i = 0; i < 10; i++) {
        auto result = limiter.check("/api/users", ip);
        EXPECT_TRUE(result->allowed);
    }
    EXPECT_FALSE(limiter.check("/api/users", ip)->allowed);
}

// =============================================================================
// Concurrency Tests
// =============================================================================

TEST_F(RateLimiterTest, ConcurrentAccessSafe) {
    // Use fixed window for deterministic behavior
    RateLimitConfig config;
    config.algorithm = RateLimitAlgorithm::FIXED_WINDOW;
    config.requests_per_window = 1000;
    config.window_size_ms = 60000;  // Long window to avoid resets during test

    RateLimiter limiter(config);
    std::string key = "shared_key";

    std::atomic<int> allowed_count{0};
    std::atomic<int> denied_count{0};

    // Spawn multiple threads making requests
    std::vector<std::thread> threads;
    for (int t = 0; t < 10; t++) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 200; i++) {
                auto result = limiter.check(key);
                if (result.allowed) {
                    allowed_count++;
                } else {
                    denied_count++;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Total requests = 10 threads * 200 requests = 2000
    // Limit = 1000, so exactly 1000 should be allowed
    EXPECT_EQ(allowed_count + denied_count, 2000);
    EXPECT_EQ(allowed_count.load(), 1000);  // Exactly 1000 should be allowed
    EXPECT_EQ(denied_count.load(), 1000);   // Exactly 1000 should be denied
}

TEST_F(RateLimiterTest, ConcurrentDifferentKeys) {
    RateLimitConfig config;
    config.requests_per_window = 10;
    config.burst_size = 10;

    RateLimiter limiter(config);

    std::atomic<int> total_allowed{0};

    // Each thread uses a different key
    std::vector<std::thread> threads;
    for (int t = 0; t < 10; t++) {
        threads.emplace_back([&, t]() {
            std::string key = "key_" + std::to_string(t);
            for (int i = 0; i < 15; i++) {
                if (limiter.check(key).allowed) {
                    total_allowed++;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Each thread should get 10 allowed (total = 100)
    EXPECT_EQ(total_allowed.load(), 100);
}

// =============================================================================
// Performance Tests
// =============================================================================

TEST_F(RateLimiterTest, PerformanceTokenBucket) {
    RateLimitConfig config;
    config.algorithm = RateLimitAlgorithm::TOKEN_BUCKET;
    config.requests_per_window = 1000000;  // High limit so we're measuring overhead, not blocking
    config.burst_size = 1000000;

    RateLimiter limiter(config);

    auto start = std::chrono::high_resolution_clock::now();
    const int iterations = 100000;

    for (int i = 0; i < iterations; i++) {
        limiter.check("perf_key");
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    double ns_per_check = (double(duration_us) * 1000) / iterations;
    double checks_per_second = 1000000000.0 / ns_per_check;

    std::cout << "Token bucket: " << ns_per_check << " ns/check, "
              << (checks_per_second / 1000000) << " M checks/sec\n";

    // Should be able to do at least 1M checks/second
    EXPECT_GT(checks_per_second, 1000000);
}

TEST_F(RateLimiterTest, PerformanceSlidingWindow) {
    RateLimitConfig config;
    config.algorithm = RateLimitAlgorithm::SLIDING_WINDOW;
    config.requests_per_window = 1000000;
    config.sliding_window_granularity = 10;

    RateLimiter limiter(config);

    auto start = std::chrono::high_resolution_clock::now();
    const int iterations = 100000;

    for (int i = 0; i < iterations; i++) {
        limiter.check("perf_key");
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    double ns_per_check = (double(duration_us) * 1000) / iterations;
    double checks_per_second = 1000000000.0 / ns_per_check;

    std::cout << "Sliding window: " << ns_per_check << " ns/check, "
              << (checks_per_second / 1000000) << " M checks/sec\n";

    EXPECT_GT(checks_per_second, 500000);
}

TEST_F(RateLimiterTest, PerformanceFixedWindow) {
    RateLimitConfig config;
    config.algorithm = RateLimitAlgorithm::FIXED_WINDOW;
    config.requests_per_window = 1000000;

    RateLimiter limiter(config);

    auto start = std::chrono::high_resolution_clock::now();
    const int iterations = 100000;

    for (int i = 0; i < iterations; i++) {
        limiter.check("perf_key");
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    double ns_per_check = (double(duration_us) * 1000) / iterations;
    double checks_per_second = 1000000000.0 / ns_per_check;

    std::cout << "Fixed window: " << ns_per_check << " ns/check, "
              << (checks_per_second / 1000000) << " M checks/sec\n";

    EXPECT_GT(checks_per_second, 1000000);
}

TEST_F(RateLimiterTest, PerformanceManyKeys) {
    RateLimitConfig config;
    config.requests_per_window = 100;

    RateLimiter limiter(config);

    auto start = std::chrono::high_resolution_clock::now();
    const int num_keys = 10000;
    const int requests_per_key = 10;

    for (int k = 0; k < num_keys; k++) {
        std::string key = "key_" + std::to_string(k);
        for (int r = 0; r < requests_per_key; r++) {
            limiter.check(key);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    int total_checks = num_keys * requests_per_key;
    double checks_per_ms = double(total_checks) / duration_ms;

    std::cout << "Many keys: " << total_checks << " checks in " << duration_ms << " ms ("
              << (checks_per_ms * 1000) << " checks/sec)\n";

    EXPECT_EQ(limiter.size(), num_keys);
    EXPECT_GT(checks_per_ms * 1000, 100000);  // At least 100k checks/sec
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
