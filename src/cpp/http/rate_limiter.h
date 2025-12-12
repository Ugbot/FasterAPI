/**
 * HTTP Rate Limiter
 *
 * Production-grade rate limiting with multiple algorithms:
 * - Token bucket: Smooth rate limiting with burst support
 * - Sliding window: Fixed time window with smooth rollover
 * - Fixed window: Simple counter per time window
 *
 * Features:
 * - Per-IP and per-endpoint rate limiting
 * - Configurable limits and windows
 * - Thread-safe with lock-free atomics where possible
 * - 429 Too Many Requests with Retry-After header
 * - Extensible key extraction (IP, user ID, API key, etc.)
 */

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <atomic>
#include <functional>
#include <vector>
#include <array>
#include <memory>
#include <optional>
#include <cstdint>

#include "../core/string_hash.h"

namespace fasterapi {
namespace http {

/**
 * Rate limiting algorithm type
 */
enum class RateLimitAlgorithm {
    TOKEN_BUCKET,       // Smooth rate limiting with burst capacity
    SLIDING_WINDOW,     // Fixed window with sub-window granularity
    FIXED_WINDOW        // Simple counter per time window
};

/**
 * Rate limit result
 */
struct RateLimitResult {
    bool allowed{true};              // Request is allowed
    uint32_t remaining{0};           // Remaining requests in current window
    uint32_t limit{0};               // Total limit
    uint64_t reset_at{0};            // Unix timestamp when limit resets
    uint32_t retry_after_ms{0};      // Milliseconds until retry is allowed
};

/**
 * Rate limiter configuration
 */
struct RateLimitConfig {
    // Limits
    uint32_t requests_per_window = 100;     // Max requests per window
    uint32_t window_size_ms = 60000;        // Window size in milliseconds (1 minute)
    uint32_t burst_size = 10;               // Max burst for token bucket (0 = use requests_per_window)

    // Algorithm
    RateLimitAlgorithm algorithm = RateLimitAlgorithm::TOKEN_BUCKET;

    // Sliding window granularity (number of sub-windows)
    uint32_t sliding_window_granularity = 10;

    // Cleanup
    uint32_t cleanup_interval_ms = 60000;   // Cleanup expired entries every N ms
    uint32_t max_entries = 100000;          // Max entries before forced cleanup

    // Response behavior
    bool include_headers = true;            // Include X-RateLimit-* headers
    bool include_retry_after = true;        // Include Retry-After header on 429

    RateLimitConfig() = default;
};

/**
 * Token bucket state for a single key
 */
struct TokenBucket {
    std::atomic<double> tokens{0.0};
    std::atomic<uint64_t> last_update_us{0};  // Microseconds since epoch

    TokenBucket() = default;
    TokenBucket(double initial_tokens, uint64_t now_us)
        : tokens(initial_tokens), last_update_us(now_us) {}

    // Move constructor for unordered_map
    TokenBucket(TokenBucket&& other) noexcept
        : tokens(other.tokens.load())
        , last_update_us(other.last_update_us.load()) {}

    TokenBucket& operator=(TokenBucket&& other) noexcept {
        tokens.store(other.tokens.load());
        last_update_us.store(other.last_update_us.load());
        return *this;
    }

    // Disable copy
    TokenBucket(const TokenBucket&) = delete;
    TokenBucket& operator=(const TokenBucket&) = delete;
};

/**
 * Sliding window state for a single key
 *
 * Uses a fixed-size array of atomic counters for sub-windows.
 * Maximum granularity is 64 sub-windows.
 */
struct SlidingWindow {
    static constexpr uint32_t MAX_GRANULARITY = 64;
    std::array<std::atomic<uint32_t>, MAX_GRANULARITY> sub_windows;
    std::atomic<uint64_t> window_start_us{0};
    uint32_t granularity{10};

    SlidingWindow() {
        for (auto& sw : sub_windows) {
            sw.store(0);
        }
    }

    explicit SlidingWindow(uint32_t gran)
        : granularity(std::min(gran, MAX_GRANULARITY)) {
        for (auto& sw : sub_windows) {
            sw.store(0);
        }
    }

    // Move constructor
    SlidingWindow(SlidingWindow&& other) noexcept
        : window_start_us(other.window_start_us.load())
        , granularity(other.granularity) {
        for (size_t i = 0; i < MAX_GRANULARITY; i++) {
            sub_windows[i].store(other.sub_windows[i].load());
        }
    }

    SlidingWindow& operator=(SlidingWindow&& other) noexcept {
        window_start_us.store(other.window_start_us.load());
        granularity = other.granularity;
        for (size_t i = 0; i < MAX_GRANULARITY; i++) {
            sub_windows[i].store(other.sub_windows[i].load());
        }
        return *this;
    }

    // Disable copy
    SlidingWindow(const SlidingWindow&) = delete;
    SlidingWindow& operator=(const SlidingWindow&) = delete;
};

/**
 * Fixed window state for a single key
 */
struct FixedWindow {
    std::atomic<uint32_t> count{0};
    std::atomic<uint64_t> window_start_us{0};

    FixedWindow() = default;
    FixedWindow(uint32_t initial_count, uint64_t start_us)
        : count(initial_count), window_start_us(start_us) {}

    // Move constructor
    FixedWindow(FixedWindow&& other) noexcept
        : count(other.count.load())
        , window_start_us(other.window_start_us.load()) {}

    FixedWindow& operator=(FixedWindow&& other) noexcept {
        count.store(other.count.load());
        window_start_us.store(other.window_start_us.load());
        return *this;
    }

    // Disable copy
    FixedWindow(const FixedWindow&) = delete;
    FixedWindow& operator=(const FixedWindow&) = delete;
};

/**
 * Rate limiter implementation
 *
 * Thread-safe rate limiter supporting multiple algorithms.
 *
 * Usage:
 *   RateLimitConfig config;
 *   config.requests_per_window = 100;
 *   config.window_size_ms = 60000;  // 1 minute
 *
 *   RateLimiter limiter(config);
 *
 *   // Check if request is allowed
 *   auto result = limiter.check("192.168.1.1");
 *   if (!result.allowed) {
 *       // Return 429 Too Many Requests
 *   }
 */
class RateLimiter {
public:
    /**
     * Create rate limiter with configuration
     */
    explicit RateLimiter(const RateLimitConfig& config = RateLimitConfig{})
        : config_(config)
        , last_cleanup_us_(now_us()) {
        // Initialize burst size if not set
        if (config_.burst_size == 0) {
            config_.burst_size = config_.requests_per_window;
        }
    }

    /**
     * Check if request is allowed and consume quota
     *
     * @param key Rate limit key (e.g., IP address, user ID)
     * @return Rate limit result with allowed status and metadata
     */
    RateLimitResult check(const std::string& key) noexcept {
        uint64_t now = now_us();

        // Periodic cleanup
        maybe_cleanup(now);

        switch (config_.algorithm) {
            case RateLimitAlgorithm::TOKEN_BUCKET:
                return check_token_bucket(key, now);
            case RateLimitAlgorithm::SLIDING_WINDOW:
                return check_sliding_window(key, now);
            case RateLimitAlgorithm::FIXED_WINDOW:
            default:
                return check_fixed_window(key, now);
        }
    }

    /**
     * Check without consuming quota (peek)
     */
    RateLimitResult peek(const std::string& key) const noexcept {
        uint64_t now = now_us();

        switch (config_.algorithm) {
            case RateLimitAlgorithm::TOKEN_BUCKET:
                return peek_token_bucket(key, now);
            case RateLimitAlgorithm::SLIDING_WINDOW:
                return peek_sliding_window(key, now);
            case RateLimitAlgorithm::FIXED_WINDOW:
            default:
                return peek_fixed_window(key, now);
        }
    }

    /**
     * Reset rate limit for a key
     */
    void reset(const std::string& key) noexcept {
        std::unique_lock lock(mutex_);
        token_buckets_.erase(key);
        sliding_windows_.erase(key);
        fixed_windows_.erase(key);
    }

    /**
     * Clear all rate limit state
     */
    void clear() noexcept {
        std::unique_lock lock(mutex_);
        token_buckets_.clear();
        sliding_windows_.clear();
        fixed_windows_.clear();
    }

    /**
     * Get current number of tracked keys
     */
    size_t size() const noexcept {
        std::shared_lock lock(mutex_);
        switch (config_.algorithm) {
            case RateLimitAlgorithm::TOKEN_BUCKET:
                return token_buckets_.size();
            case RateLimitAlgorithm::SLIDING_WINDOW:
                return sliding_windows_.size();
            default:
                return fixed_windows_.size();
        }
    }

    /**
     * Get configuration (for introspection)
     */
    const RateLimitConfig& config() const noexcept {
        return config_;
    }

private:
    RateLimitConfig config_;
    mutable std::shared_mutex mutex_;

    // State for each algorithm (using StringHash for heterogeneous lookup)
    std::unordered_map<std::string, TokenBucket, core::StringHash, std::equal_to<>> token_buckets_;
    std::unordered_map<std::string, SlidingWindow, core::StringHash, std::equal_to<>> sliding_windows_;
    std::unordered_map<std::string, FixedWindow, core::StringHash, std::equal_to<>> fixed_windows_;

    uint64_t last_cleanup_us_{0};

    /**
     * Get current time in microseconds
     */
    static uint64_t now_us() noexcept {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()
        ).count();
    }

    /**
     * Token bucket algorithm
     *
     * Tokens are replenished at a constant rate.
     * Requests consume one token each.
     * Allows bursts up to burst_size.
     */
    RateLimitResult check_token_bucket(const std::string& key, uint64_t now) noexcept {
        // Calculate token replenishment rate (tokens per microsecond)
        double tokens_per_us = double(config_.requests_per_window) /
                               (double(config_.window_size_ms) * 1000.0);

        std::unique_lock lock(mutex_);

        auto it = token_buckets_.find(key);
        if (it == token_buckets_.end()) {
            // New key - start with full bucket
            auto [inserted_it, _] = token_buckets_.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(key),
                std::forward_as_tuple(double(config_.burst_size), now)
            );
            it = inserted_it;
        }

        TokenBucket& bucket = it->second;

        // Replenish tokens
        uint64_t last_update = bucket.last_update_us.load();
        uint64_t elapsed_us = now - last_update;
        double current_tokens = bucket.tokens.load();

        // Add tokens based on elapsed time
        double new_tokens = current_tokens + (elapsed_us * tokens_per_us);
        new_tokens = std::min(new_tokens, double(config_.burst_size));

        // Try to consume a token
        if (new_tokens >= 1.0) {
            bucket.tokens.store(new_tokens - 1.0);
            bucket.last_update_us.store(now);

            return make_result(
                true,
                uint32_t(new_tokens - 1.0),
                now + (config_.window_size_ms * 1000)
            );
        }

        // Not enough tokens - calculate retry time
        double needed = 1.0 - new_tokens;
        uint64_t wait_us = uint64_t(needed / tokens_per_us);

        bucket.tokens.store(new_tokens);
        bucket.last_update_us.store(now);

        return make_result(
            false,
            0,
            now + wait_us,
            uint32_t(wait_us / 1000)
        );
    }

    RateLimitResult peek_token_bucket(const std::string& key, uint64_t now) const noexcept {
        double tokens_per_us = double(config_.requests_per_window) /
                               (double(config_.window_size_ms) * 1000.0);

        std::shared_lock lock(mutex_);

        auto it = token_buckets_.find(key);
        if (it == token_buckets_.end()) {
            return make_result(true, config_.burst_size, now + (config_.window_size_ms * 1000));
        }

        const TokenBucket& bucket = it->second;
        uint64_t elapsed_us = now - bucket.last_update_us.load();
        double current_tokens = bucket.tokens.load();
        double new_tokens = std::min(
            current_tokens + (elapsed_us * tokens_per_us),
            double(config_.burst_size)
        );

        return make_result(
            new_tokens >= 1.0,
            uint32_t(new_tokens),
            now + (config_.window_size_ms * 1000)
        );
    }

    /**
     * Sliding window algorithm
     *
     * Divides the window into sub-windows for smoother rate limiting.
     * Provides better accuracy than fixed window.
     */
    RateLimitResult check_sliding_window(const std::string& key, uint64_t now) noexcept {
        uint64_t window_us = uint64_t(config_.window_size_ms) * 1000;
        uint64_t sub_window_us = window_us / config_.sliding_window_granularity;

        std::unique_lock lock(mutex_);

        auto it = sliding_windows_.find(key);
        if (it == sliding_windows_.end()) {
            auto [inserted_it, _] = sliding_windows_.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(key),
                std::forward_as_tuple(config_.sliding_window_granularity)
            );
            it = inserted_it;
            it->second.window_start_us.store(now);
        }

        SlidingWindow& window = it->second;

        // Check if we need to advance the window
        uint64_t window_start = window.window_start_us.load();
        if (now >= window_start + window_us) {
            // Window expired - reset
            for (uint32_t i = 0; i < window.granularity; i++) {
                window.sub_windows[i].store(0);
            }
            window.window_start_us.store(now);
            window_start = now;
        }

        // Calculate current sub-window index
        uint64_t elapsed = now - window_start;
        size_t current_idx = (elapsed / sub_window_us) % window.granularity;

        // Count requests across all sub-windows
        uint32_t total = 0;
        for (uint32_t i = 0; i < window.granularity; i++) {
            total += window.sub_windows[i].load();
        }

        if (total < config_.requests_per_window) {
            // Allowed - increment current sub-window
            window.sub_windows[current_idx].fetch_add(1);

            return make_result(
                true,
                config_.requests_per_window - total - 1,
                (window_start + window_us) / 1000000
            );
        }

        // Rate limited
        uint64_t reset_us = window_start + window_us;
        return make_result(
            false,
            0,
            reset_us / 1000000,
            uint32_t((reset_us - now) / 1000)
        );
    }

    RateLimitResult peek_sliding_window(const std::string& key, uint64_t now) const noexcept {
        std::shared_lock lock(mutex_);

        auto it = sliding_windows_.find(key);
        if (it == sliding_windows_.end()) {
            return make_result(true, config_.requests_per_window, (now + config_.window_size_ms * 1000) / 1000000);
        }

        const SlidingWindow& window = it->second;
        uint32_t total = 0;
        for (uint32_t i = 0; i < window.granularity; i++) {
            total += window.sub_windows[i].load();
        }

        uint64_t window_start = window.window_start_us.load();
        uint64_t reset_us = window_start + (uint64_t(config_.window_size_ms) * 1000);

        return make_result(
            total < config_.requests_per_window,
            total < config_.requests_per_window ? config_.requests_per_window - total : 0,
            reset_us / 1000000
        );
    }

    /**
     * Fixed window algorithm
     *
     * Simple counter per time window.
     * May allow burst at window boundaries.
     */
    RateLimitResult check_fixed_window(const std::string& key, uint64_t now) noexcept {
        uint64_t window_us = uint64_t(config_.window_size_ms) * 1000;

        std::unique_lock lock(mutex_);

        auto it = fixed_windows_.find(key);
        if (it == fixed_windows_.end()) {
            auto [inserted_it, _] = fixed_windows_.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(key),
                std::forward_as_tuple(1, now)
            );

            return make_result(
                true,
                config_.requests_per_window - 1,
                (now + window_us) / 1000000
            );
        }

        FixedWindow& window = it->second;

        // Check if window expired
        uint64_t window_start = window.window_start_us.load();
        if (now >= window_start + window_us) {
            // New window
            window.count.store(1);
            window.window_start_us.store(now);

            return make_result(
                true,
                config_.requests_per_window - 1,
                (now + window_us) / 1000000
            );
        }

        // Same window - check limit
        uint32_t count = window.count.load();
        if (count < config_.requests_per_window) {
            window.count.fetch_add(1);

            return make_result(
                true,
                config_.requests_per_window - count - 1,
                (window_start + window_us) / 1000000
            );
        }

        // Rate limited
        uint64_t reset_us = window_start + window_us;
        return make_result(
            false,
            0,
            reset_us / 1000000,
            uint32_t((reset_us - now) / 1000)
        );
    }

    RateLimitResult peek_fixed_window(const std::string& key, uint64_t now) const noexcept {
        std::shared_lock lock(mutex_);

        auto it = fixed_windows_.find(key);
        if (it == fixed_windows_.end()) {
            return make_result(true, config_.requests_per_window, (now + config_.window_size_ms * 1000) / 1000000);
        }

        const FixedWindow& window = it->second;
        uint32_t count = window.count.load();
        uint64_t window_start = window.window_start_us.load();
        uint64_t reset_us = window_start + (uint64_t(config_.window_size_ms) * 1000);

        return make_result(
            count < config_.requests_per_window,
            count < config_.requests_per_window ? config_.requests_per_window - count : 0,
            reset_us / 1000000
        );
    }

    /**
     * Create rate limit result
     */
    RateLimitResult make_result(bool allowed, uint32_t remaining, uint64_t reset_at, uint32_t retry_after_ms = 0) const noexcept {
        return RateLimitResult{
            .allowed = allowed,
            .remaining = remaining,
            .limit = config_.requests_per_window,
            .reset_at = reset_at,
            .retry_after_ms = retry_after_ms
        };
    }

    /**
     * Periodic cleanup of expired entries
     */
    void maybe_cleanup(uint64_t now) noexcept {
        uint64_t cleanup_interval_us = uint64_t(config_.cleanup_interval_ms) * 1000;

        if (now - last_cleanup_us_ < cleanup_interval_us) {
            return;
        }

        std::unique_lock lock(mutex_);

        // Double-check under lock
        if (now - last_cleanup_us_ < cleanup_interval_us) {
            return;
        }

        last_cleanup_us_ = now;
        uint64_t window_us = uint64_t(config_.window_size_ms) * 1000;
        uint64_t expire_threshold = now - (window_us * 2);  // Entries older than 2 windows

        // Cleanup each map
        for (auto it = token_buckets_.begin(); it != token_buckets_.end(); ) {
            if (it->second.last_update_us.load() < expire_threshold) {
                it = token_buckets_.erase(it);
            } else {
                ++it;
            }
        }

        for (auto it = sliding_windows_.begin(); it != sliding_windows_.end(); ) {
            if (it->second.window_start_us.load() < expire_threshold) {
                it = sliding_windows_.erase(it);
            } else {
                ++it;
            }
        }

        for (auto it = fixed_windows_.begin(); it != fixed_windows_.end(); ) {
            if (it->second.window_start_us.load() < expire_threshold) {
                it = fixed_windows_.erase(it);
            } else {
                ++it;
            }
        }
    }
};

/**
 * Rate limit middleware for HTTP responses
 *
 * Integrates rate limiter with HTTP request/response handling.
 */
class RateLimitMiddleware {
public:
    using KeyExtractor = std::function<std::string(
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers
    )>;

    /**
     * Create rate limit middleware
     */
    explicit RateLimitMiddleware(const RateLimitConfig& config = RateLimitConfig{})
        : limiter_(config)
        , key_extractor_(default_key_extractor) {}

    /**
     * Create with custom key extractor
     */
    RateLimitMiddleware(const RateLimitConfig& config, KeyExtractor extractor)
        : limiter_(config)
        , key_extractor_(std::move(extractor)) {}

    /**
     * Check rate limit for request
     *
     * @param method HTTP method
     * @param path Request path
     * @param headers Request headers
     * @return Rate limit result
     */
    RateLimitResult check(
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers
    ) noexcept {
        std::string key = key_extractor_(method, path, headers);
        return limiter_.check(key);
    }

    /**
     * Add rate limit headers to response
     *
     * @param result Rate limit result
     * @param response_headers Response headers to modify
     */
    static void add_headers(
        const RateLimitResult& result,
        std::unordered_map<std::string, std::string>& response_headers
    ) noexcept {
        response_headers["X-RateLimit-Limit"] = std::to_string(result.limit);
        response_headers["X-RateLimit-Remaining"] = std::to_string(result.remaining);
        response_headers["X-RateLimit-Reset"] = std::to_string(result.reset_at);

        if (!result.allowed && result.retry_after_ms > 0) {
            // Convert ms to seconds, rounding up
            uint32_t retry_after_s = (result.retry_after_ms + 999) / 1000;
            response_headers["Retry-After"] = std::to_string(retry_after_s);
        }
    }

    /**
     * Get underlying rate limiter (for testing/introspection)
     */
    RateLimiter& limiter() noexcept {
        return limiter_;
    }

    const RateLimiter& limiter() const noexcept {
        return limiter_;
    }

    /**
     * Set custom key extractor
     */
    void set_key_extractor(KeyExtractor extractor) noexcept {
        key_extractor_ = std::move(extractor);
    }

private:
    RateLimiter limiter_;
    KeyExtractor key_extractor_;

    /**
     * Default key extractor - uses X-Forwarded-For or X-Real-IP headers,
     * falling back to a default key
     */
    static std::string default_key_extractor(
        const std::string& /*method*/,
        const std::string& /*path*/,
        const std::unordered_map<std::string, std::string>& headers
    ) noexcept {
        // Try X-Forwarded-For first (common for proxied requests)
        auto xff_it = headers.find("X-Forwarded-For");
        if (xff_it == headers.end()) {
            xff_it = headers.find("x-forwarded-for");
        }
        if (xff_it != headers.end() && !xff_it->second.empty()) {
            // Take first IP from comma-separated list
            const auto& xff = xff_it->second;
            auto comma = xff.find(',');
            if (comma != std::string::npos) {
                return xff.substr(0, comma);
            }
            return xff;
        }

        // Try X-Real-IP
        auto xri_it = headers.find("X-Real-IP");
        if (xri_it == headers.end()) {
            xri_it = headers.find("x-real-ip");
        }
        if (xri_it != headers.end() && !xri_it->second.empty()) {
            return xri_it->second;
        }

        // No IP available - use a default key
        // In production, the server would inject the client IP
        return "_default_";
    }
};

/**
 * Per-endpoint rate limiter
 *
 * Allows different rate limits for different endpoints.
 */
class EndpointRateLimiter {
public:
    /**
     * Add rate limit rule for an endpoint pattern
     *
     * @param pattern Endpoint pattern (e.g., "/api/v1/users", "/api/*")
     * @param config Rate limit configuration for this pattern
     */
    void add_rule(const std::string& pattern, const RateLimitConfig& config) {
        rules_.emplace_back(pattern, std::make_unique<RateLimiter>(config));
    }

    /**
     * Check rate limit for request
     *
     * @param path Request path
     * @param key Rate limit key (e.g., IP address)
     * @return Rate limit result, or nullopt if no matching rule
     */
    std::optional<RateLimitResult> check(const std::string& path, const std::string& key) noexcept {
        for (auto& [pattern, limiter] : rules_) {
            if (matches_pattern(path, pattern)) {
                return limiter->check(key);
            }
        }
        return std::nullopt;
    }

private:
    std::vector<std::pair<std::string, std::unique_ptr<RateLimiter>>> rules_;

    /**
     * Simple pattern matching (supports * wildcard at end)
     */
    static bool matches_pattern(const std::string& path, const std::string& pattern) noexcept {
        if (pattern.empty()) return false;

        // Exact match
        if (pattern == path) return true;

        // Wildcard at end (e.g., "/api/*" matches "/api/users")
        if (pattern.back() == '*') {
            std::string_view prefix(pattern.data(), pattern.size() - 1);
            return path.compare(0, prefix.size(), prefix) == 0;
        }

        return false;
    }
};

} // namespace http
} // namespace fasterapi
