/**
 * HTTP Response Caching Middleware
 *
 * Production-grade HTTP caching for APIs and web servers.
 *
 * Features:
 * - HTTP cache headers (ETag, Last-Modified, Cache-Control)
 * - Client-side caching validation (304 Not Modified)
 * - Server-side in-memory LRU cache
 * - Cache key customization
 * - Vary header support
 * - Cache invalidation
 * - TTL-based expiration
 */

#pragma once

#include "http1_connection.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>
#include <optional>
#include <memory>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <list>
#include <cstring>

// For hashing (SHA-256 via OpenSSL)
#include <openssl/sha.h>

namespace fasterapi {
namespace http {

/**
 * Cache-Control directives
 */
struct CacheControl {
    bool no_store{false};           // Don't cache at all
    bool no_cache{false};           // Revalidate before using
    bool must_revalidate{false};    // Must revalidate after expiry
    bool public_cache{false};       // Can be cached by shared caches
    bool private_cache{false};      // Only browser can cache
    bool immutable{false};          // Never changes

    int32_t max_age{-1};            // Max age in seconds (-1 = not set)
    int32_t s_maxage{-1};           // Shared cache max age (-1 = not set)
    int32_t stale_while_revalidate{-1};  // Allow stale while revalidating
    int32_t stale_if_error{-1};     // Allow stale on error

    /**
     * Build Cache-Control header value
     */
    std::string to_header() const {
        if (no_store) return "no-store";

        std::string result;
        auto append = [&](const char* directive) {
            if (!result.empty()) result += ", ";
            result += directive;
        };

        if (no_cache) append("no-cache");
        if (must_revalidate) append("must-revalidate");
        if (public_cache) append("public");
        if (private_cache) append("private");
        if (immutable) append("immutable");

        if (max_age >= 0) {
            if (!result.empty()) result += ", ";
            result += "max-age=" + std::to_string(max_age);
        }
        if (s_maxage >= 0) {
            if (!result.empty()) result += ", ";
            result += "s-maxage=" + std::to_string(s_maxage);
        }
        if (stale_while_revalidate >= 0) {
            if (!result.empty()) result += ", ";
            result += "stale-while-revalidate=" + std::to_string(stale_while_revalidate);
        }
        if (stale_if_error >= 0) {
            if (!result.empty()) result += ", ";
            result += "stale-if-error=" + std::to_string(stale_if_error);
        }

        return result.empty() ? "no-cache" : result;
    }

    /**
     * Parse Cache-Control header value
     */
    static CacheControl parse(const std::string& header) {
        CacheControl cc;
        size_t pos = 0;

        while (pos < header.size()) {
            // Skip whitespace and commas
            while (pos < header.size() && (header[pos] == ' ' || header[pos] == ',')) {
                pos++;
            }
            if (pos >= header.size()) break;

            // Find directive
            size_t end = header.find_first_of(",= ", pos);
            if (end == std::string::npos) end = header.size();

            std::string directive = header.substr(pos, end - pos);

            // Check for value
            int32_t value = -1;
            if (end < header.size() && header[end] == '=') {
                size_t val_start = end + 1;
                size_t val_end = header.find(',', val_start);
                if (val_end == std::string::npos) val_end = header.size();
                value = std::atoi(header.substr(val_start, val_end - val_start).c_str());
                pos = val_end;
            } else {
                pos = end;
            }

            // Apply directive
            if (directive == "no-store") cc.no_store = true;
            else if (directive == "no-cache") cc.no_cache = true;
            else if (directive == "must-revalidate") cc.must_revalidate = true;
            else if (directive == "public") cc.public_cache = true;
            else if (directive == "private") cc.private_cache = true;
            else if (directive == "immutable") cc.immutable = true;
            else if (directive == "max-age") cc.max_age = value;
            else if (directive == "s-maxage") cc.s_maxage = value;
            else if (directive == "stale-while-revalidate") cc.stale_while_revalidate = value;
            else if (directive == "stale-if-error") cc.stale_if_error = value;
        }

        return cc;
    }
};

/**
 * Cached response entry
 */
struct CacheEntry {
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    int status{200};

    std::string etag;
    std::string last_modified;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point expires_at;

    // Vary header values for this entry
    std::unordered_map<std::string, std::string> vary_values;

    bool is_expired() const {
        return std::chrono::steady_clock::now() > expires_at;
    }

    size_t memory_size() const {
        size_t size = body.size() + etag.size() + last_modified.size();
        for (const auto& [k, v] : headers) {
            size += k.size() + v.size();
        }
        for (const auto& [k, v] : vary_values) {
            size += k.size() + v.size();
        }
        return size;
    }
};

/**
 * Response cache configuration
 */
struct ResponseCacheConfig {
    // Server-side cache settings
    bool enable_server_cache{true};      // Enable in-memory caching
    size_t max_cache_size_bytes{64 * 1024 * 1024};  // 64MB default
    size_t max_entry_size_bytes{1024 * 1024};  // 1MB max per entry
    uint32_t default_ttl_seconds{300};    // 5 minutes default TTL
    uint32_t max_entries{10000};          // Max number of entries

    // HTTP cache headers
    bool add_etag{true};                  // Generate ETag headers
    bool add_last_modified{false};        // Add Last-Modified (requires source)
    bool handle_conditional{true};        // Handle If-None-Match, If-Modified-Since

    // Default cache control
    CacheControl default_cache_control;

    // Cacheable methods (by default only GET and HEAD)
    std::vector<std::string> cacheable_methods{"GET", "HEAD"};

    // Non-cacheable status codes
    std::vector<int> non_cacheable_statuses{
        401, 403, 404, 500, 502, 503, 504
    };

    // Headers to include in Vary
    std::vector<std::string> default_vary{"Accept-Encoding"};

    // Cache key generator (nullptr = use default: method + path + query)
    std::function<std::string(
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers
    )> key_generator = nullptr;

    ResponseCacheConfig() {
        default_cache_control.public_cache = true;
        default_cache_control.max_age = 300;
    }
};

/**
 * Cache check result
 */
enum class CacheCheckResult {
    MISS,               // No cache entry
    HIT,                // Cache hit, use cached response
    STALE,              // Cache entry is stale
    NOT_MODIFIED,       // Client cache is valid (304)
    NOT_CACHEABLE       // Request is not cacheable
};

/**
 * LRU Cache for HTTP responses
 */
class LRUCache {
public:
    explicit LRUCache(size_t max_size_bytes, size_t max_entries)
        : max_size_bytes_(max_size_bytes)
        , max_entries_(max_entries) {}

    /**
     * Get entry from cache
     */
    std::optional<CacheEntry> get(const std::string& key) {
        std::unique_lock lock(mutex_);

        auto it = cache_map_.find(key);
        if (it == cache_map_.end()) {
            return std::nullopt;
        }

        // Move to front (most recently used)
        auto list_it = it->second;
        lru_list_.splice(lru_list_.begin(), lru_list_, list_it);

        return list_it->second;
    }

    /**
     * Put entry in cache
     */
    void put(const std::string& key, const CacheEntry& entry) {
        std::unique_lock lock(mutex_);

        size_t entry_size = key.size() + entry.memory_size();

        // Check if entry is too large
        if (entry_size > max_size_bytes_ / 4) {
            return;  // Don't cache entries larger than 25% of max size
        }

        // Remove existing entry if present
        auto it = cache_map_.find(key);
        if (it != cache_map_.end()) {
            current_size_ -= it->second->first.size() + it->second->second.memory_size();
            lru_list_.erase(it->second);
            cache_map_.erase(it);
        }

        // Evict entries to make room
        while ((current_size_ + entry_size > max_size_bytes_ ||
                cache_map_.size() >= max_entries_) &&
               !lru_list_.empty()) {
            auto& back = lru_list_.back();
            current_size_ -= back.first.size() + back.second.memory_size();
            cache_map_.erase(back.first);
            lru_list_.pop_back();
        }

        // Insert new entry
        lru_list_.push_front({key, entry});
        cache_map_[key] = lru_list_.begin();
        current_size_ += entry_size;
    }

    /**
     * Remove entry from cache
     */
    void remove(const std::string& key) {
        std::unique_lock lock(mutex_);

        auto it = cache_map_.find(key);
        if (it != cache_map_.end()) {
            current_size_ -= it->second->first.size() + it->second->second.memory_size();
            lru_list_.erase(it->second);
            cache_map_.erase(it);
        }
    }

    /**
     * Clear all entries
     */
    void clear() {
        std::unique_lock lock(mutex_);
        cache_map_.clear();
        lru_list_.clear();
        current_size_ = 0;
    }

    /**
     * Get cache statistics
     */
    struct Stats {
        size_t entry_count;
        size_t current_size_bytes;
        size_t max_size_bytes;
        uint64_t hits;
        uint64_t misses;
    };

    Stats stats() const {
        std::shared_lock lock(mutex_);
        return {
            cache_map_.size(),
            current_size_,
            max_size_bytes_,
            hits_,
            misses_
        };
    }

    void record_hit() { hits_++; }
    void record_miss() { misses_++; }

private:
    mutable std::shared_mutex mutex_;
    size_t max_size_bytes_;
    size_t max_entries_;
    size_t current_size_{0};

    // LRU list: front = most recently used, back = least recently used
    std::list<std::pair<std::string, CacheEntry>> lru_list_;
    std::unordered_map<std::string, decltype(lru_list_)::iterator> cache_map_;

    std::atomic<uint64_t> hits_{0};
    std::atomic<uint64_t> misses_{0};
};

/**
 * Response Cache Middleware
 *
 * Handles HTTP caching for responses including:
 * - ETag generation and validation
 * - Cache-Control headers
 * - Server-side LRU caching
 * - Conditional request handling (304 Not Modified)
 *
 * Usage:
 *   ResponseCacheConfig config;
 *   config.default_ttl_seconds = 600;
 *
 *   ResponseCache cache(config);
 *
 *   // Check cache before handling request
 *   auto result = cache.check(method, path, headers, response);
 *   if (result == CacheCheckResult::HIT || result == CacheCheckResult::NOT_MODIFIED) {
 *       return response;  // Response is already populated
 *   }
 *
 *   // Handle request and generate response...
 *   handler(request, response);
 *
 *   // Store in cache and add headers
 *   cache.store(method, path, headers, response);
 */
class ResponseCache {
public:
    explicit ResponseCache(const ResponseCacheConfig& config = ResponseCacheConfig())
        : config_(config)
        , cache_(config.max_cache_size_bytes, config.max_entries) {
        build_cacheable_methods_set();
        build_non_cacheable_statuses_set();
    }

    /**
     * Check cache and handle conditional requests
     *
     * @param method HTTP method
     * @param path Request path
     * @param headers Request headers
     * @param response Response to populate if cache hit
     * @return Cache check result
     */
    CacheCheckResult check(
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        Http1Response& response
    ) {
        // Check if method is cacheable
        if (!is_method_cacheable(method)) {
            return CacheCheckResult::NOT_CACHEABLE;
        }

        // Generate cache key
        std::string key = generate_key(method, path, headers);

        // Try to get from cache
        auto entry = cache_.get(key);
        if (!entry) {
            cache_.record_miss();

            // Check for conditional request (client might have cached version)
            if (config_.handle_conditional) {
                return CacheCheckResult::MISS;
            }
            return CacheCheckResult::MISS;
        }

        // Check if entry is expired
        if (entry->is_expired()) {
            cache_.record_miss();
            cache_.remove(key);
            return CacheCheckResult::STALE;
        }

        // Check for conditional request
        if (config_.handle_conditional) {
            auto if_none_match = get_header(headers, "If-None-Match");
            if (if_none_match && !entry->etag.empty()) {
                if (*if_none_match == entry->etag || *if_none_match == "*") {
                    cache_.record_hit();
                    response.status = 304;
                    response.status_message = "Not Modified";
                    response.headers["ETag"] = entry->etag;
                    if (!entry->last_modified.empty()) {
                        response.headers["Last-Modified"] = entry->last_modified;
                    }
                    return CacheCheckResult::NOT_MODIFIED;
                }
            }

            auto if_modified_since = get_header(headers, "If-Modified-Since");
            if (if_modified_since && !entry->last_modified.empty()) {
                if (*if_modified_since == entry->last_modified) {
                    cache_.record_hit();
                    response.status = 304;
                    response.status_message = "Not Modified";
                    if (!entry->etag.empty()) {
                        response.headers["ETag"] = entry->etag;
                    }
                    response.headers["Last-Modified"] = entry->last_modified;
                    return CacheCheckResult::NOT_MODIFIED;
                }
            }
        }

        // Verify Vary headers match
        if (!verify_vary(headers, *entry)) {
            cache_.record_miss();
            return CacheCheckResult::MISS;
        }

        // Cache hit - populate response
        cache_.record_hit();
        response.status = entry->status;
        response.status_message = "OK";
        response.body = entry->body;
        response.headers = entry->headers;

        return CacheCheckResult::HIT;
    }

    /**
     * Store response in cache and add caching headers
     *
     * @param method HTTP method
     * @param path Request path
     * @param headers Request headers (for Vary)
     * @param response Response to store and modify
     * @param ttl_seconds Custom TTL (0 = use default, -1 = don't cache)
     */
    void store(
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& request_headers,
        Http1Response& response,
        int32_t ttl_seconds = 0
    ) {
        // Check if response is cacheable
        if (!is_response_cacheable(method, response.status)) {
            add_no_cache_headers(response);
            return;
        }

        // Generate ETag if enabled and not present
        if (config_.add_etag && response.headers.find("ETag") == response.headers.end()) {
            response.headers["ETag"] = generate_etag(response);
        }

        // Add Cache-Control if not present
        if (response.headers.find("Cache-Control") == response.headers.end()) {
            response.headers["Cache-Control"] = config_.default_cache_control.to_header();
        }

        // Add Vary header
        add_vary_header(response);

        // Don't store if server caching is disabled
        if (!config_.enable_server_cache) {
            return;
        }

        // Check response size
        if (response.body.size() > config_.max_entry_size_bytes) {
            return;
        }

        // Determine TTL
        int32_t actual_ttl = (ttl_seconds > 0) ? ttl_seconds :
                            (ttl_seconds == 0) ? static_cast<int32_t>(config_.default_ttl_seconds) :
                            -1;
        if (actual_ttl < 0) {
            return;  // Don't cache
        }

        // Create cache entry
        CacheEntry entry;
        entry.body = response.body;
        entry.headers = response.headers;
        entry.status = response.status;
        entry.etag = response.headers.count("ETag") ? response.headers.at("ETag") : "";
        entry.last_modified = response.headers.count("Last-Modified") ?
                             response.headers.at("Last-Modified") : "";
        entry.created_at = std::chrono::steady_clock::now();
        entry.expires_at = entry.created_at + std::chrono::seconds(actual_ttl);

        // Store Vary header values
        for (const auto& vary_header : config_.default_vary) {
            auto value = get_header(request_headers, vary_header);
            if (value) {
                entry.vary_values[vary_header] = *value;
            }
        }

        // Store in cache
        std::string key = generate_key(method, path, request_headers);
        cache_.put(key, entry);
    }

    /**
     * Invalidate cache entry
     */
    void invalidate(
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers = {}
    ) {
        std::string key = generate_key(method, path, headers);
        cache_.remove(key);
    }

    /**
     * Clear entire cache
     */
    void clear() {
        cache_.clear();
    }

    /**
     * Get cache statistics
     */
    LRUCache::Stats stats() const {
        return cache_.stats();
    }

    /**
     * Get configuration
     */
    const ResponseCacheConfig& config() const {
        return config_;
    }

    /**
     * Update configuration
     */
    void set_config(const ResponseCacheConfig& config) {
        config_ = config;
        build_cacheable_methods_set();
        build_non_cacheable_statuses_set();
    }

private:
    ResponseCacheConfig config_;
    LRUCache cache_;
    std::unordered_set<std::string> cacheable_methods_set_;
    std::unordered_set<int> non_cacheable_statuses_set_;

    void build_cacheable_methods_set() {
        cacheable_methods_set_.clear();
        for (const auto& method : config_.cacheable_methods) {
            cacheable_methods_set_.insert(method);
        }
    }

    void build_non_cacheable_statuses_set() {
        non_cacheable_statuses_set_.clear();
        for (int status : config_.non_cacheable_statuses) {
            non_cacheable_statuses_set_.insert(status);
        }
    }

    bool is_method_cacheable(const std::string& method) const {
        return cacheable_methods_set_.count(method) > 0;
    }

    bool is_response_cacheable(const std::string& method, int status) const {
        if (!is_method_cacheable(method)) return false;
        if (non_cacheable_statuses_set_.count(status) > 0) return false;
        return true;
    }

    /**
     * Generate cache key
     */
    std::string generate_key(
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers
    ) const {
        if (config_.key_generator) {
            return config_.key_generator(method, path, headers);
        }

        // Default: method + path + sorted vary headers
        std::string key = method + ":" + path;

        for (const auto& vary_header : config_.default_vary) {
            auto value = get_header(headers, vary_header);
            if (value) {
                key += ":" + vary_header + "=" + *value;
            }
        }

        return key;
    }

    /**
     * Generate ETag from response body
     */
    std::string generate_etag(const Http1Response& response) const {
        // Use SHA-256 hash of body
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(response.body.data()),
               response.body.size(), hash);

        // Convert to hex string
        char hex[SHA256_DIGEST_LENGTH * 2 + 1];
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
            snprintf(hex + i * 2, 3, "%02x", hash[i]);
        }

        // Use first 16 characters for shorter ETag
        return "\"" + std::string(hex, 16) + "\"";
    }

    /**
     * Verify Vary header values match
     */
    bool verify_vary(
        const std::unordered_map<std::string, std::string>& headers,
        const CacheEntry& entry
    ) const {
        for (const auto& [vary_header, cached_value] : entry.vary_values) {
            auto current_value = get_header(headers, vary_header);
            std::string current = current_value ? *current_value : "";
            if (current != cached_value) {
                return false;
            }
        }
        return true;
    }

    /**
     * Add Vary header to response
     */
    void add_vary_header(Http1Response& response) const {
        if (config_.default_vary.empty()) return;

        std::string vary;
        for (const auto& header : config_.default_vary) {
            if (!vary.empty()) vary += ", ";
            vary += header;
        }

        auto existing = response.headers.find("Vary");
        if (existing != response.headers.end()) {
            // Merge with existing Vary
            response.headers["Vary"] = existing->second + ", " + vary;
        } else {
            response.headers["Vary"] = vary;
        }
    }

    /**
     * Add no-cache headers
     */
    void add_no_cache_headers(Http1Response& response) const {
        response.headers["Cache-Control"] = "no-store";
    }

    /**
     * Get header value (case-insensitive)
     */
    static std::optional<std::string> get_header(
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& name
    ) {
        auto it = headers.find(name);
        if (it != headers.end()) {
            return it->second;
        }

        // Try lowercase
        std::string lower = name;
        for (char& c : lower) {
            if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        }
        it = headers.find(lower);
        if (it != headers.end()) {
            return it->second;
        }

        return std::nullopt;
    }
};

/**
 * Cache configuration presets
 */
namespace cache_presets {

/**
 * Static assets (long cache, immutable)
 */
inline ResponseCacheConfig static_assets() {
    ResponseCacheConfig config;
    config.default_ttl_seconds = 86400 * 365;  // 1 year
    config.default_cache_control.public_cache = true;
    config.default_cache_control.max_age = 86400 * 365;
    config.default_cache_control.immutable = true;
    return config;
}

/**
 * API responses (short cache, must revalidate)
 */
inline ResponseCacheConfig api_responses() {
    ResponseCacheConfig config;
    config.default_ttl_seconds = 60;  // 1 minute
    config.default_cache_control.public_cache = true;
    config.default_cache_control.max_age = 60;
    config.default_cache_control.must_revalidate = true;
    return config;
}

/**
 * Private data (no shared caching)
 */
inline ResponseCacheConfig private_data() {
    ResponseCacheConfig config;
    config.default_ttl_seconds = 300;  // 5 minutes
    config.default_cache_control.private_cache = true;
    config.default_cache_control.max_age = 300;
    config.enable_server_cache = false;  // Don't cache on server
    return config;
}

/**
 * No caching
 */
inline ResponseCacheConfig no_cache() {
    ResponseCacheConfig config;
    config.enable_server_cache = false;
    config.default_cache_control.no_store = true;
    return config;
}

} // namespace cache_presets

} // namespace http
} // namespace fasterapi
