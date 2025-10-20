#pragma once

#include <string>
#include <unordered_map>
#include <chrono>
#include <atomic>
#include <mutex>
#include <memory>

namespace fasterapi {
namespace mcp {
namespace security {

/**
 * Rate limit result
 */
struct RateLimitResult {
    bool allowed;
    uint64_t remaining;      // Remaining requests
    uint64_t reset_time_ms;  // When limit resets (epoch ms)
    std::string error_message;

    static RateLimitResult ok(uint64_t remaining, uint64_t reset_time_ms) {
        return RateLimitResult{true, remaining, reset_time_ms, ""};
    }

    static RateLimitResult exceeded(uint64_t reset_time_ms) {
        return RateLimitResult{false, 0, reset_time_ms, "Rate limit exceeded"};
    }
};

/**
 * Token bucket rate limiter.
 *
 * Algorithm:
 * - Each client has a bucket with capacity C
 * - Bucket refills at rate R tokens/second
 * - Each request consumes 1 token
 * - Request blocked if bucket empty
 *
 * Example:
 *   100 req/min with burst 20 =
 *   capacity: 20, refill_rate: 100/60 = 1.67 tokens/sec
 */
class TokenBucketLimiter {
public:
    struct Config {
        uint64_t capacity;           // Max tokens (burst size)
        double refill_rate;          // Tokens per second
        uint64_t window_ms = 60000;  // Window size (default 1 minute)
    };

    explicit TokenBucketLimiter(const Config& config);

    /**
     * Check if request is allowed for a client.
     *
     * @param client_id Client identifier
     * @param tokens Number of tokens to consume (default 1)
     * @return Rate limit result
     */
    RateLimitResult check(const std::string& client_id, uint64_t tokens = 1);

    /**
     * Reset rate limit for a client.
     *
     * @param client_id Client identifier
     */
    void reset(const std::string& client_id);

    /**
     * Get current token count for a client.
     *
     * @param client_id Client identifier
     * @return Current tokens available
     */
    uint64_t get_tokens(const std::string& client_id);

private:
    struct Bucket {
        std::atomic<double> tokens;
        std::atomic<uint64_t> last_refill_ms;

        Bucket(double initial_tokens)
            : tokens(initial_tokens)
            , last_refill_ms(0)
        {}
    };

    Config config_;
    std::unordered_map<std::string, std::unique_ptr<Bucket>> buckets_;
    mutable std::mutex mutex_;

    // Get or create bucket for client
    Bucket* get_bucket(const std::string& client_id);

    // Refill bucket based on time elapsed
    void refill_bucket(Bucket* bucket, uint64_t now_ms);

    // Get current time in milliseconds
    static uint64_t now_ms();
};

/**
 * Sliding window rate limiter.
 *
 * Algorithm:
 * - Track requests in a sliding time window
 * - Count requests in last N seconds
 * - Block if count exceeds limit
 *
 * More accurate than fixed window, but higher memory usage.
 */
class SlidingWindowLimiter {
public:
    struct Config {
        uint64_t max_requests;       // Max requests per window
        uint64_t window_ms;          // Window size in milliseconds
    };

    explicit SlidingWindowLimiter(const Config& config);

    RateLimitResult check(const std::string& client_id);
    void reset(const std::string& client_id);

private:
    struct Window {
        std::vector<uint64_t> timestamps;
        mutable std::mutex mutex;
    };

    Config config_;
    std::unordered_map<std::string, std::unique_ptr<Window>> windows_;
    mutable std::mutex mutex_;

    Window* get_window(const std::string& client_id);
    void cleanup_old_timestamps(Window* window, uint64_t cutoff_ms);
    static uint64_t now_ms();
};

/**
 * Fixed window rate limiter.
 *
 * Algorithm:
 * - Count requests in fixed time windows (e.g., per minute)
 * - Reset counter at window boundary
 * - Fast and memory efficient
 * - Can have "burst" at window boundaries
 */
class FixedWindowLimiter {
public:
    struct Config {
        uint64_t max_requests;  // Max requests per window
        uint64_t window_ms;     // Window size in milliseconds
    };

    explicit FixedWindowLimiter(const Config& config);

    RateLimitResult check(const std::string& client_id);
    void reset(const std::string& client_id);

private:
    struct Window {
        std::atomic<uint64_t> count;
        std::atomic<uint64_t> window_start_ms;

        Window() : count(0), window_start_ms(0) {}
    };

    Config config_;
    std::unordered_map<std::string, std::unique_ptr<Window>> windows_;
    mutable std::mutex mutex_;

    Window* get_window(const std::string& client_id);
    static uint64_t now_ms();
};

/**
 * Rate limit middleware for MCP server.
 *
 * Features:
 * - Per-client rate limiting
 * - Per-tool rate limiting
 * - Global rate limiting
 * - Configurable algorithms
 */
class RateLimitMiddleware {
public:
    enum class Algorithm {
        TOKEN_BUCKET,
        SLIDING_WINDOW,
        FIXED_WINDOW
    };

    struct Config {
        Algorithm algorithm = Algorithm::TOKEN_BUCKET;

        // Global limits
        uint64_t global_max_requests = 1000;
        uint64_t global_window_ms = 60000;

        // Per-client limits
        uint64_t client_max_requests = 100;
        uint64_t client_window_ms = 60000;
        uint64_t client_burst = 20;

        // Per-tool limits (optional)
        bool enable_tool_limits = false;
    };

    explicit RateLimitMiddleware(const Config& config);

    /**
     * Check if request is allowed.
     *
     * @param client_id Client identifier
     * @param tool_name Tool name (optional, for per-tool limits)
     * @return Rate limit result
     */
    RateLimitResult check(const std::string& client_id, const std::string& tool_name = "");

    /**
     * Set per-tool rate limit.
     *
     * @param tool_name Tool name
     * @param max_requests Max requests per window
     * @param window_ms Window size in ms
     */
    void set_tool_limit(const std::string& tool_name, uint64_t max_requests, uint64_t window_ms);

    /**
     * Reset rate limit for a client.
     *
     * @param client_id Client identifier
     */
    void reset_client(const std::string& client_id);

private:
    Config config_;

    // Limiters
    std::unique_ptr<TokenBucketLimiter> global_limiter_;
    std::unique_ptr<TokenBucketLimiter> client_limiter_;
    std::unordered_map<std::string, std::unique_ptr<TokenBucketLimiter>> tool_limiters_;

    mutable std::mutex mutex_;
};

} // namespace security
} // namespace mcp
} // namespace fasterapi
