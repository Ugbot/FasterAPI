/**
 * @file test_utils.h
 * @brief Test utilities for FasterAPI Google Test suite
 *
 * Provides:
 * - RandomGenerator: Generate randomized test data (per CLAUDE.md requirements)
 * - FasterAPITest: Base test fixture with common setup/teardown
 * - Timing utilities for performance assertions
 * - Memory tracking helpers
 */

#pragma once

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <random>
#include <string>
#include <vector>
#include <chrono>
#include <functional>
#include <array>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <thread>
#include <atomic>

namespace fasterapi {
namespace testing {

/**
 * @class RandomGenerator
 * @brief Generate randomized test data for comprehensive testing
 *
 * Per CLAUDE.md: "Tests must involve more than one route, different HTTP verbs
 * and randomised input data"
 */
class RandomGenerator {
public:
    RandomGenerator() : rng_(std::random_device{}()) {}

    explicit RandomGenerator(uint64_t seed) : rng_(seed) {}

    /**
     * Generate a random string of specified length
     */
    std::string random_string(size_t len) {
        static constexpr char chars[] =
            "abcdefghijklmnopqrstuvwxyz"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "0123456789";
        std::uniform_int_distribution<size_t> dist(0, sizeof(chars) - 2);

        std::string result;
        result.reserve(len);
        for (size_t i = 0; i < len; ++i) {
            result += chars[dist(rng_)];
        }
        return result;
    }

    /**
     * Generate a random URL path
     * Examples: /api/v1/users/abc123, /health, /items/xyz789/details
     */
    std::string random_path() {
        static const std::vector<std::string> prefixes = {
            "/", "/api/", "/api/v1/", "/api/v2/", "/v1/", "/v2/"
        };
        static const std::vector<std::string> resources = {
            "users", "items", "posts", "comments", "orders", "products",
            "health", "status", "metrics", "config", "settings"
        };

        std::uniform_int_distribution<size_t> prefix_dist(0, prefixes.size() - 1);
        std::uniform_int_distribution<size_t> resource_dist(0, resources.size() - 1);
        std::uniform_int_distribution<int> depth_dist(0, 3);

        std::string path = prefixes[prefix_dist(rng_)];
        int depth = depth_dist(rng_);

        for (int i = 0; i < depth; ++i) {
            path += resources[resource_dist(rng_)];
            if (random_bool()) {
                path += "/" + random_string(8);
            }
            if (i < depth - 1) {
                path += "/";
            }
        }

        if (path.empty() || path.back() == '/') {
            path += resources[resource_dist(rng_)];
        }

        return path;
    }

    /**
     * Generate a random HTTP method
     */
    std::string random_method() {
        static const std::vector<std::string> methods = {
            "GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"
        };
        std::uniform_int_distribution<size_t> dist(0, methods.size() - 1);
        return methods[dist(rng_)];
    }

    /**
     * Generate random bytes
     */
    std::vector<uint8_t> random_bytes(size_t len) {
        std::vector<uint8_t> result(len);
        std::uniform_int_distribution<int> dist(0, 255);
        for (size_t i = 0; i < len; ++i) {
            result[i] = static_cast<uint8_t>(dist(rng_));
        }
        return result;
    }

    /**
     * Generate a random JSON body
     */
    std::string random_json_body() {
        std::ostringstream oss;
        oss << R"({"id":")" << random_string(8) << R"(",)";
        oss << R"("name":")" << random_string(16) << R"(",)";
        oss << R"("value":)" << random_int(0, 10000) << R"(,)";
        oss << R"("active":)" << (random_bool() ? "true" : "false") << "}";
        return oss.str();
    }

    /**
     * Generate a random HTTP header name
     */
    std::string random_header_name() {
        static const std::vector<std::string> headers = {
            "Content-Type", "Accept", "Authorization", "X-Request-ID",
            "X-Correlation-ID", "Cache-Control", "User-Agent", "Accept-Encoding",
            "Accept-Language", "Host", "Connection", "X-Custom-Header"
        };
        std::uniform_int_distribution<size_t> dist(0, headers.size() - 1);
        return headers[dist(rng_)];
    }

    /**
     * Generate a random HTTP header value
     */
    std::string random_header_value() {
        static const std::vector<std::string> values = {
            "application/json", "text/plain", "text/html", "*/*",
            "gzip, deflate", "en-US,en;q=0.9", "keep-alive", "close",
            "no-cache", "max-age=3600"
        };
        std::uniform_int_distribution<size_t> dist(0, values.size() - 1);
        return values[dist(rng_)];
    }

    /**
     * Generate a random integer in range [min, max]
     */
    int random_int(int min, int max) {
        std::uniform_int_distribution<int> dist(min, max);
        return dist(rng_);
    }

    /**
     * Generate a random size_t in range [min, max]
     */
    size_t random_size(size_t min, size_t max) {
        std::uniform_int_distribution<size_t> dist(min, max);
        return dist(rng_);
    }

    /**
     * Generate a random boolean
     */
    bool random_bool() {
        std::uniform_int_distribution<int> dist(0, 1);
        return dist(rng_) == 1;
    }

    /**
     * Generate a random port number (high range to avoid conflicts)
     */
    uint16_t random_port() {
        std::uniform_int_distribution<int> dist(10000, 60000);
        return static_cast<uint16_t>(dist(rng_));
    }

    /**
     * Shuffle a vector in place
     */
    template<typename T>
    void shuffle(std::vector<T>& vec) {
        std::shuffle(vec.begin(), vec.end(), rng_);
    }

    /**
     * Pick a random element from a vector
     */
    template<typename T>
    const T& pick(const std::vector<T>& vec) {
        std::uniform_int_distribution<size_t> dist(0, vec.size() - 1);
        return vec[dist(rng_)];
    }

private:
    std::mt19937_64 rng_;
};

/**
 * @class Timer
 * @brief High-resolution timer for performance assertions
 */
class Timer {
public:
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;
    using Duration = std::chrono::nanoseconds;

    void start() {
        start_ = Clock::now();
    }

    void stop() {
        end_ = Clock::now();
    }

    int64_t elapsed_ns() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(end_ - start_).count();
    }

    double elapsed_us() const {
        return elapsed_ns() / 1000.0;
    }

    double elapsed_ms() const {
        return elapsed_ns() / 1000000.0;
    }

private:
    TimePoint start_;
    TimePoint end_;
};

/**
 * @class ScopedTimer
 * @brief RAII timer that records duration on destruction
 */
class ScopedTimer {
public:
    explicit ScopedTimer(int64_t& target_ns) : target_(target_ns) {
        start_ = Timer::Clock::now();
    }

    ~ScopedTimer() {
        auto end = Timer::Clock::now();
        target_ = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_).count();
    }

    // Non-copyable, non-movable
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    int64_t& target_;
    Timer::TimePoint start_;
};

/**
 * @struct BenchmarkStats
 * @brief Statistics from a micro-benchmark run
 */
struct BenchmarkStats {
    int64_t min_ns = INT64_MAX;
    int64_t max_ns = 0;
    int64_t total_ns = 0;
    size_t iterations = 0;

    double mean_ns() const {
        return iterations > 0 ? static_cast<double>(total_ns) / iterations : 0.0;
    }

    void record(int64_t ns) {
        min_ns = std::min(min_ns, ns);
        max_ns = std::max(max_ns, ns);
        total_ns += ns;
        ++iterations;
    }
};

/**
 * Run a micro-benchmark with warmup
 */
template<typename Func>
BenchmarkStats run_benchmark(Func&& func, size_t warmup = 100, size_t iterations = 10000) {
    // Warmup
    for (size_t i = 0; i < warmup; ++i) {
        func();
    }

    BenchmarkStats stats;
    Timer timer;

    for (size_t i = 0; i < iterations; ++i) {
        timer.start();
        func();
        timer.stop();
        stats.record(timer.elapsed_ns());
    }

    return stats;
}

/**
 * @class FasterAPITest
 * @brief Base test fixture for FasterAPI tests
 *
 * Provides:
 * - Random data generator
 * - Common setup/teardown
 * - Performance assertion helpers
 */
class FasterAPITest : public ::testing::Test {
protected:
    RandomGenerator rng_;

    void SetUp() override {
        // Reset RNG with random seed for each test
        rng_ = RandomGenerator();
    }

    void TearDown() override {
        // Cleanup if needed
    }

    /**
     * Assert that an operation completes within a time limit
     */
    template<typename Func>
    void assert_completes_within(Func&& func, int64_t max_ns, const char* msg = nullptr) {
        Timer timer;
        timer.start();
        func();
        timer.stop();

        ASSERT_LT(timer.elapsed_ns(), max_ns)
            << (msg ? msg : "Operation exceeded time limit")
            << " (actual: " << timer.elapsed_ns() << " ns, limit: " << max_ns << " ns)";
    }

    /**
     * Assert that average operation time is within limit
     */
    template<typename Func>
    void assert_average_within(Func&& func, int64_t max_avg_ns, size_t iterations = 1000) {
        auto stats = run_benchmark(std::forward<Func>(func), 100, iterations);

        ASSERT_LT(stats.mean_ns(), static_cast<double>(max_avg_ns))
            << "Average operation time exceeded limit"
            << " (actual: " << stats.mean_ns() << " ns, limit: " << max_avg_ns << " ns)";
    }
};

/**
 * @class ConcurrencyTest
 * @brief Base fixture for concurrency tests
 */
class ConcurrencyTest : public FasterAPITest {
protected:
    /**
     * Run a function concurrently from multiple threads
     */
    template<typename Func>
    void run_concurrent(Func&& func, size_t num_threads, size_t iterations_per_thread) {
        std::vector<std::thread> threads;
        threads.reserve(num_threads);

        std::atomic<size_t> completed{0};
        std::atomic<bool> has_error{false};

        for (size_t t = 0; t < num_threads; ++t) {
            threads.emplace_back([&, t]() {
                try {
                    for (size_t i = 0; i < iterations_per_thread && !has_error.load(); ++i) {
                        func(t, i);
                    }
                    ++completed;
                } catch (...) {
                    has_error.store(true);
                }
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }

        ASSERT_FALSE(has_error.load()) << "Exception occurred in concurrent execution";
        ASSERT_EQ(completed.load(), num_threads) << "Not all threads completed";
    }
};

// Macros for common test patterns

/**
 * Test multiple routes with randomized methods
 * Usage: TEST_MULTI_ROUTE(TestSuite, test_name) { ... use routes and methods ... }
 */
#define TEST_MULTI_ROUTE_F(test_fixture, test_name) \
    TEST_F(test_fixture, test_name)

/**
 * Performance test macro with timing assertion
 */
#define EXPECT_FASTER_THAN_NS(expr, max_ns) \
    do { \
        ::fasterapi::testing::Timer __timer; \
        __timer.start(); \
        (expr); \
        __timer.stop(); \
        EXPECT_LT(__timer.elapsed_ns(), max_ns) \
            << "Expected " #expr " to complete in < " << max_ns << " ns, " \
            << "but took " << __timer.elapsed_ns() << " ns"; \
    } while (0)

/**
 * Memory allocation check (placeholder - implement with custom allocator)
 */
#define EXPECT_NO_ALLOCATIONS(expr) \
    do { \
        (expr); \
        /* TODO: Implement allocation tracking */ \
    } while (0)

} // namespace testing
} // namespace fasterapi

// Bring testing namespace into global scope for convenience
using namespace fasterapi::testing;
