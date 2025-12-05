#include "rate_limit.h"
#include <algorithm>

namespace fasterapi {
namespace mcp {
namespace security {

// ========== TokenBucketLimiter ==========

TokenBucketLimiter::TokenBucketLimiter(const Config& config)
    : config_(config)
{
}

uint64_t TokenBucketLimiter::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

TokenBucketLimiter::Bucket* TokenBucketLimiter::get_bucket(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = buckets_.find(client_id);
    if (it != buckets_.end()) {
        return it->second.get();
    }

    // Create new bucket
    auto bucket = std::make_unique<Bucket>(config_.capacity);
    bucket->last_refill_ms = now_ms();
    auto* bucket_ptr = bucket.get();
    buckets_[client_id] = std::move(bucket);

    return bucket_ptr;
}

void TokenBucketLimiter::refill_bucket(Bucket* bucket, uint64_t now_ms) {
    uint64_t last_refill = bucket->last_refill_ms.load();
    uint64_t elapsed_ms = now_ms - last_refill;

    if (elapsed_ms == 0) return;

    // Calculate tokens to add
    double tokens_to_add = (elapsed_ms / 1000.0) * config_.refill_rate;
    double current_tokens = bucket->tokens.load();
    double new_tokens = std::min(current_tokens + tokens_to_add, static_cast<double>(config_.capacity));

    bucket->tokens.store(new_tokens);
    bucket->last_refill_ms.store(now_ms);
}

RateLimitResult TokenBucketLimiter::check(const std::string& client_id, uint64_t tokens) {
    auto* bucket = get_bucket(client_id);
    uint64_t now = now_ms();

    // Refill bucket
    refill_bucket(bucket, now);

    // Check if we have enough tokens
    double current_tokens = bucket->tokens.load();
    if (current_tokens >= tokens) {
        // Consume tokens
        bucket->tokens.store(current_tokens - tokens);
        uint64_t reset_time = now + (config_.window_ms);
        return RateLimitResult::ok(static_cast<uint64_t>(current_tokens - tokens), reset_time);
    }

    // Rate limit exceeded
    uint64_t reset_time = now + static_cast<uint64_t>((tokens - current_tokens) / config_.refill_rate * 1000);
    return RateLimitResult::exceeded(reset_time);
}

void TokenBucketLimiter::reset(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    buckets_.erase(client_id);
}

uint64_t TokenBucketLimiter::get_tokens(const std::string& client_id) {
    auto* bucket = get_bucket(client_id);
    uint64_t now = now_ms();
    refill_bucket(bucket, now);
    return static_cast<uint64_t>(bucket->tokens.load());
}

// ========== SlidingWindowLimiter ==========

SlidingWindowLimiter::SlidingWindowLimiter(const Config& config)
    : config_(config)
{
}

uint64_t SlidingWindowLimiter::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

SlidingWindowLimiter::Window* SlidingWindowLimiter::get_window(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = windows_.find(client_id);
    if (it != windows_.end()) {
        return it->second.get();
    }

    auto window = std::make_unique<Window>();
    auto* window_ptr = window.get();
    windows_[client_id] = std::move(window);

    return window_ptr;
}

void SlidingWindowLimiter::cleanup_old_timestamps(Window* window, uint64_t cutoff_ms) {
    // Remove timestamps older than cutoff
    window->timestamps.erase(
        std::remove_if(window->timestamps.begin(), window->timestamps.end(),
            [cutoff_ms](uint64_t ts) { return ts < cutoff_ms; }),
        window->timestamps.end()
    );
}

RateLimitResult SlidingWindowLimiter::check(const std::string& client_id) {
    auto* window = get_window(client_id);
    uint64_t now = now_ms();
    uint64_t cutoff = now - config_.window_ms;

    std::lock_guard<std::mutex> lock(window->mutex);

    // Cleanup old timestamps
    cleanup_old_timestamps(window, cutoff);

    // Check if we can add a new request
    if (window->timestamps.size() < config_.max_requests) {
        window->timestamps.push_back(now);
        uint64_t remaining = config_.max_requests - window->timestamps.size();
        return RateLimitResult::ok(remaining, now + config_.window_ms);
    }

    // Rate limit exceeded - calculate reset time
    uint64_t oldest = window->timestamps.front();
    uint64_t reset_time = oldest + config_.window_ms;

    return RateLimitResult::exceeded(reset_time);
}

void SlidingWindowLimiter::reset(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    windows_.erase(client_id);
}

// ========== FixedWindowLimiter ==========

FixedWindowLimiter::FixedWindowLimiter(const Config& config)
    : config_(config)
{
}

uint64_t FixedWindowLimiter::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

FixedWindowLimiter::Window* FixedWindowLimiter::get_window(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = windows_.find(client_id);
    if (it != windows_.end()) {
        return it->second.get();
    }

    auto window = std::make_unique<Window>();
    auto* window_ptr = window.get();
    windows_[client_id] = std::move(window);

    return window_ptr;
}

RateLimitResult FixedWindowLimiter::check(const std::string& client_id) {
    auto* window = get_window(client_id);
    uint64_t now = now_ms();

    uint64_t window_start = window->window_start_ms.load();
    uint64_t count = window->count.load();

    // Check if we're in a new window
    if (now - window_start >= config_.window_ms) {
        // Reset window
        window->window_start_ms.store(now);
        window->count.store(1);
        return RateLimitResult::ok(config_.max_requests - 1, now + config_.window_ms);
    }

    // Check if we can increment count
    if (count < config_.max_requests) {
        window->count.fetch_add(1);
        return RateLimitResult::ok(config_.max_requests - count - 1, window_start + config_.window_ms);
    }

    // Rate limit exceeded
    return RateLimitResult::exceeded(window_start + config_.window_ms);
}

void FixedWindowLimiter::reset(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    windows_.erase(client_id);
}

// ========== RateLimitMiddleware ==========

RateLimitMiddleware::RateLimitMiddleware(const Config& config)
    : config_(config)
{
    // Create global limiter
    TokenBucketLimiter::Config global_config;
    global_config.capacity = config.global_max_requests;
    global_config.refill_rate = config.global_max_requests / (config.global_window_ms / 1000.0);
    global_config.window_ms = config.global_window_ms;
    global_limiter_ = std::make_unique<TokenBucketLimiter>(global_config);

    // Create per-client limiter
    TokenBucketLimiter::Config client_config;
    client_config.capacity = config.client_burst;
    client_config.refill_rate = config.client_max_requests / (config.client_window_ms / 1000.0);
    client_config.window_ms = config.client_window_ms;
    client_limiter_ = std::make_unique<TokenBucketLimiter>(client_config);
}

RateLimitResult RateLimitMiddleware::check(const std::string& client_id, const std::string& tool_name) {
    // Check global limit
    auto global_result = global_limiter_->check("global");
    if (!global_result.allowed) {
        return global_result;
    }

    // Check per-client limit
    auto client_result = client_limiter_->check(client_id);
    if (!client_result.allowed) {
        return client_result;
    }

    // Check per-tool limit (if enabled and tool specified)
    if (config_.enable_tool_limits && !tool_name.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tool_limiters_.find(tool_name);
        if (it != tool_limiters_.end()) {
            auto tool_result = it->second->check(client_id);
            if (!tool_result.allowed) {
                return tool_result;
            }
        }
    }

    return client_result;
}

void RateLimitMiddleware::set_tool_limit(const std::string& tool_name, uint64_t max_requests, uint64_t window_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    TokenBucketLimiter::Config tool_config;
    tool_config.capacity = max_requests;
    tool_config.refill_rate = max_requests / (window_ms / 1000.0);
    tool_config.window_ms = window_ms;

    tool_limiters_[tool_name] = std::make_unique<TokenBucketLimiter>(tool_config);
}

void RateLimitMiddleware::reset_client(const std::string& client_id) {
    client_limiter_->reset(client_id);

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [tool_name, limiter] : tool_limiters_) {
        limiter->reset(client_id);
    }
}

} // namespace security
} // namespace mcp
} // namespace fasterapi
