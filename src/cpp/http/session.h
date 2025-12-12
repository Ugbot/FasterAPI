/**
 * HTTP Session Management Middleware
 *
 * Production-grade session handling for web applications.
 *
 * Features:
 * - Cryptographically secure session ID generation
 * - Cookie-based session tracking
 * - Pluggable storage backends (in-memory, Redis-compatible interface)
 * - TTL-based expiration with automatic cleanup
 * - Thread-safe concurrent access
 * - Secure cookie attributes (HttpOnly, Secure, SameSite)
 */

#pragma once

#include "http1_connection.h"
#include "../core/string_hash.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <functional>
#include <optional>
#include <memory>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <random>
#include <any>
#include <atomic>
#include <list>

// For secure random generation
#include <openssl/rand.h>

namespace fasterapi {
namespace http {

/**
 * Session data container
 */
class SessionData {
public:
    SessionData() = default;

    /**
     * Get a value from session
     */
    template<typename T>
    std::optional<T> get(const std::string& key) const {
        std::shared_lock lock(mutex_);
        auto it = data_.find(key);
        if (it != data_.end()) {
            try {
                return std::any_cast<T>(it->second);
            } catch (const std::bad_any_cast&) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    /**
     * Get string value (common case)
     */
    std::optional<std::string> get_string(const std::string& key) const {
        return get<std::string>(key);
    }

    /**
     * Get integer value
     */
    std::optional<int64_t> get_int(const std::string& key) const {
        return get<int64_t>(key);
    }

    /**
     * Get boolean value
     */
    std::optional<bool> get_bool(const std::string& key) const {
        return get<bool>(key);
    }

    /**
     * Set a value in session
     */
    template<typename T>
    void set(const std::string& key, T value) {
        std::unique_lock lock(mutex_);
        data_[key] = std::move(value);
        modified_ = true;
    }

    /**
     * Remove a value from session
     */
    void remove(const std::string& key) {
        std::unique_lock lock(mutex_);
        data_.erase(key);
        modified_ = true;
    }

    /**
     * Check if key exists
     */
    bool has(const std::string& key) const {
        std::shared_lock lock(mutex_);
        return data_.find(key) != data_.end();
    }

    /**
     * Clear all session data
     */
    void clear() {
        std::unique_lock lock(mutex_);
        data_.clear();
        modified_ = true;
    }

    /**
     * Get all keys
     */
    std::vector<std::string> keys() const {
        std::shared_lock lock(mutex_);
        std::vector<std::string> result;
        result.reserve(data_.size());
        for (const auto& [key, _] : data_) {
            result.push_back(key);
        }
        return result;
    }

    /**
     * Check if session was modified
     */
    bool is_modified() const {
        return modified_;
    }

    /**
     * Reset modified flag
     */
    void reset_modified() {
        modified_ = false;
    }

    /**
     * Get number of items
     */
    size_t size() const {
        std::shared_lock lock(mutex_);
        return data_.size();
    }

    /**
     * Check if empty
     */
    bool empty() const {
        std::shared_lock lock(mutex_);
        return data_.empty();
    }

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::any> data_;
    std::atomic<bool> modified_{false};
};

/**
 * Session entry with metadata
 */
struct SessionEntry {
    std::string session_id;
    std::shared_ptr<SessionData> data;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_accessed;
    std::chrono::steady_clock::time_point expires_at;
    std::string user_agent;
    std::string ip_address;

    bool is_expired() const {
        return std::chrono::steady_clock::now() > expires_at;
    }

    void touch(std::chrono::seconds ttl) {
        auto now = std::chrono::steady_clock::now();
        last_accessed = now;
        expires_at = now + ttl;
    }
};

/**
 * Session storage interface
 */
class SessionStore {
public:
    virtual ~SessionStore() = default;

    virtual std::optional<SessionEntry> get(const std::string& session_id) = 0;
    virtual void set(const std::string& session_id, const SessionEntry& entry) = 0;
    virtual void remove(const std::string& session_id) = 0;
    virtual void cleanup_expired() = 0;
    virtual size_t count() const = 0;
};

/**
 * In-memory session store (for development/single-instance)
 */
class InMemorySessionStore : public SessionStore {
public:
    explicit InMemorySessionStore(size_t max_sessions = 100000)
        : max_sessions_(max_sessions) {}

    std::optional<SessionEntry> get(const std::string& session_id) override {
        // Use unique_lock since we modify LRU list on access
        std::unique_lock lock(mutex_);
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            if (!it->second.is_expired()) {
                // Move to front of LRU list
                auto list_it = lru_map_.find(session_id);
                if (list_it != lru_map_.end()) {
                    lru_list_.splice(lru_list_.begin(), lru_list_, list_it->second);
                }
                return it->second;
            }
        }
        return std::nullopt;
    }

    void set(const std::string& session_id, const SessionEntry& entry) override {
        std::unique_lock lock(mutex_);

        // Remove existing if present
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            auto list_it = lru_map_.find(session_id);
            if (list_it != lru_map_.end()) {
                lru_list_.erase(list_it->second);
                lru_map_.erase(list_it);
            }
        }

        // Evict if at capacity
        while (sessions_.size() >= max_sessions_ && !lru_list_.empty()) {
            const std::string& oldest = lru_list_.back();
            sessions_.erase(oldest);
            lru_map_.erase(oldest);
            lru_list_.pop_back();
        }

        // Insert new entry
        sessions_[session_id] = entry;
        lru_list_.push_front(session_id);
        lru_map_[session_id] = lru_list_.begin();
    }

    void remove(const std::string& session_id) override {
        std::unique_lock lock(mutex_);
        sessions_.erase(session_id);
        auto list_it = lru_map_.find(session_id);
        if (list_it != lru_map_.end()) {
            lru_list_.erase(list_it->second);
            lru_map_.erase(list_it);
        }
    }

    void cleanup_expired() override {
        std::unique_lock lock(mutex_);
        auto now = std::chrono::steady_clock::now();

        std::vector<std::string> expired;
        for (const auto& [id, entry] : sessions_) {
            if (entry.is_expired()) {
                expired.push_back(id);
            }
        }

        for (const auto& id : expired) {
            sessions_.erase(id);
            auto list_it = lru_map_.find(id);
            if (list_it != lru_map_.end()) {
                lru_list_.erase(list_it->second);
                lru_map_.erase(list_it);
            }
        }
    }

    size_t count() const override {
        std::shared_lock lock(mutex_);
        return sessions_.size();
    }

private:
    mutable std::shared_mutex mutex_;
    size_t max_sessions_;
    std::unordered_map<std::string, SessionEntry, core::StringHash, std::equal_to<>> sessions_;
    std::list<std::string> lru_list_;
    std::unordered_map<std::string, std::list<std::string>::iterator, core::StringHash, std::equal_to<>> lru_map_;
};

/**
 * Cookie SameSite attribute
 */
enum class SameSite {
    STRICT,     // Only sent in first-party context
    LAX,        // Sent with top-level navigations
    NONE        // Sent in all contexts (requires Secure)
};

/**
 * Session configuration
 */
struct SessionConfig {
    // Cookie settings
    std::string cookie_name = "session_id";
    std::string cookie_path = "/";
    std::string cookie_domain;           // Empty = current domain
    bool cookie_secure = true;           // Require HTTPS
    bool cookie_http_only = true;        // Not accessible via JavaScript
    SameSite cookie_same_site = SameSite::LAX;

    // Session settings
    uint32_t ttl_seconds = 3600;         // 1 hour default
    uint32_t max_sessions = 100000;      // Maximum sessions in store
    bool rolling_sessions = true;        // Extend TTL on each request
    uint32_t cleanup_interval_seconds = 300;  // Cleanup every 5 minutes

    // Session ID settings
    size_t session_id_length = 32;       // 256-bit session ID

    // Optional: Custom session store
    std::shared_ptr<SessionStore> store;

    SessionConfig() = default;
};

/**
 * Session middleware result
 */
struct SessionResult {
    bool has_session{false};
    std::string session_id;
    std::shared_ptr<SessionData> data;
    bool is_new{false};
};

/**
 * Session Middleware
 *
 * Manages HTTP sessions with secure cookie handling and pluggable storage.
 *
 * Usage:
 *   SessionConfig config;
 *   config.ttl_seconds = 7200;  // 2 hours
 *   config.cookie_secure = true;
 *
 *   SessionMiddleware sessions(config);
 *
 *   // In request handler:
 *   auto session = sessions.get_or_create(request_headers, response);
 *   session.data->set("user_id", user.id);
 *   session.data->set("username", user.name);
 *
 *   // Later:
 *   auto user_id = session.data->get_int("user_id");
 */
class SessionMiddleware {
public:
    explicit SessionMiddleware(const SessionConfig& config = SessionConfig())
        : config_(config) {
        // Use provided store or create in-memory store
        if (config_.store) {
            store_ = config_.store;
        } else {
            store_ = std::make_shared<InMemorySessionStore>(config_.max_sessions);
        }

        // Start cleanup timer if interval is set
        if (config_.cleanup_interval_seconds > 0) {
            last_cleanup_ = std::chrono::steady_clock::now();
        }
    }

    /**
     * Get existing session from request
     *
     * @param headers Request headers
     * @return Session result (has_session = false if no valid session)
     */
    SessionResult get_session(
        const std::unordered_map<std::string, std::string>& headers
    ) {
        maybe_cleanup();

        SessionResult result;

        // Extract session ID from cookie
        auto session_id = extract_session_id(headers);
        if (!session_id) {
            return result;
        }

        // Look up session
        auto entry = store_->get(*session_id);
        if (!entry) {
            return result;
        }

        // Session found
        result.has_session = true;
        result.session_id = *session_id;
        result.data = entry->data;
        result.is_new = false;

        // Update last accessed if rolling sessions
        if (config_.rolling_sessions) {
            entry->touch(std::chrono::seconds(config_.ttl_seconds));
            store_->set(*session_id, *entry);
        }

        return result;
    }

    /**
     * Get existing session or create a new one
     *
     * @param headers Request headers
     * @param response Response to add Set-Cookie header to (if new session)
     * @param client_ip Client IP address (optional, for session binding)
     * @param user_agent User agent string (optional)
     * @return Session result with data
     */
    SessionResult get_or_create(
        const std::unordered_map<std::string, std::string>& headers,
        Http1Response& response,
        const std::string& client_ip = "",
        const std::string& user_agent = ""
    ) {
        maybe_cleanup();

        // Try to get existing session
        auto result = get_session(headers);
        if (result.has_session) {
            return result;
        }

        // Create new session
        return create_session(response, client_ip, user_agent);
    }

    /**
     * Create a new session (replacing any existing)
     *
     * @param response Response to add Set-Cookie header to
     * @param client_ip Client IP address (optional)
     * @param user_agent User agent string (optional)
     * @return Session result with new session
     */
    SessionResult create_session(
        Http1Response& response,
        const std::string& client_ip = "",
        const std::string& user_agent = ""
    ) {
        SessionResult result;

        // Generate secure session ID
        std::string session_id = generate_session_id();

        // Create session entry
        SessionEntry entry;
        entry.session_id = session_id;
        entry.data = std::make_shared<SessionData>();
        entry.created_at = std::chrono::steady_clock::now();
        entry.last_accessed = entry.created_at;
        entry.expires_at = entry.created_at + std::chrono::seconds(config_.ttl_seconds);
        entry.ip_address = client_ip;
        entry.user_agent = user_agent;

        // Store session
        store_->set(session_id, entry);

        // Set cookie in response
        add_session_cookie(response, session_id);

        result.has_session = true;
        result.session_id = session_id;
        result.data = entry.data;
        result.is_new = true;

        return result;
    }

    /**
     * Destroy a session
     *
     * @param session_id Session ID to destroy
     * @param response Response to add cookie deletion to
     */
    void destroy_session(
        const std::string& session_id,
        Http1Response& response
    ) {
        store_->remove(session_id);
        delete_session_cookie(response);
    }

    /**
     * Destroy session from request headers
     */
    void destroy_session(
        const std::unordered_map<std::string, std::string>& headers,
        Http1Response& response
    ) {
        auto session_id = extract_session_id(headers);
        if (session_id) {
            destroy_session(*session_id, response);
        } else {
            delete_session_cookie(response);
        }
    }

    /**
     * Regenerate session ID (for security, e.g., after login)
     *
     * @param old_session_id Current session ID
     * @param response Response to add new cookie to
     * @return New session ID
     */
    std::string regenerate_session_id(
        const std::string& old_session_id,
        Http1Response& response
    ) {
        // Get old session data
        auto old_entry = store_->get(old_session_id);
        if (!old_entry) {
            return "";
        }

        // Generate new ID
        std::string new_session_id = generate_session_id();

        // Create new entry with same data
        SessionEntry new_entry = *old_entry;
        new_entry.session_id = new_session_id;
        new_entry.last_accessed = std::chrono::steady_clock::now();
        new_entry.expires_at = new_entry.last_accessed +
                              std::chrono::seconds(config_.ttl_seconds);

        // Store new, remove old
        store_->set(new_session_id, new_entry);
        store_->remove(old_session_id);

        // Update cookie
        add_session_cookie(response, new_session_id);

        return new_session_id;
    }

    /**
     * Get session count
     */
    size_t session_count() const {
        return store_->count();
    }

    /**
     * Force cleanup of expired sessions
     */
    void cleanup() {
        store_->cleanup_expired();
    }

    /**
     * Get configuration
     */
    const SessionConfig& config() const {
        return config_;
    }

private:
    SessionConfig config_;
    std::shared_ptr<SessionStore> store_;
    std::chrono::steady_clock::time_point last_cleanup_;
    std::mutex cleanup_mutex_;

    /**
     * Generate cryptographically secure session ID
     */
    std::string generate_session_id() const {
        std::vector<uint8_t> bytes(config_.session_id_length);

        if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
            // Fallback to std::random_device if OpenSSL fails
            std::random_device rd;
            std::mt19937_64 gen(rd());
            std::uniform_int_distribution<uint8_t> dis(0, 255);
            for (auto& b : bytes) {
                b = dis(gen);
            }
        }

        // Convert to hex string
        static const char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(bytes.size() * 2);
        for (uint8_t b : bytes) {
            result += hex[b >> 4];
            result += hex[b & 0x0f];
        }
        return result;
    }

    /**
     * Extract session ID from Cookie header
     */
    std::optional<std::string> extract_session_id(
        const std::unordered_map<std::string, std::string>& headers
    ) const {
        auto it = headers.find("Cookie");
        if (it == headers.end()) {
            it = headers.find("cookie");
        }
        if (it == headers.end()) {
            return std::nullopt;
        }

        return parse_cookie(it->second, config_.cookie_name);
    }

    /**
     * Parse a specific cookie from Cookie header
     */
    static std::optional<std::string> parse_cookie(
        const std::string& cookie_header,
        const std::string& cookie_name
    ) {
        size_t pos = 0;
        while (pos < cookie_header.size()) {
            // Skip whitespace
            while (pos < cookie_header.size() &&
                   (cookie_header[pos] == ' ' || cookie_header[pos] == '\t')) {
                pos++;
            }

            // Find =
            size_t eq_pos = cookie_header.find('=', pos);
            if (eq_pos == std::string::npos) break;

            std::string name = cookie_header.substr(pos, eq_pos - pos);

            // Find end of value
            size_t value_start = eq_pos + 1;
            size_t value_end = cookie_header.find(';', value_start);
            if (value_end == std::string::npos) {
                value_end = cookie_header.size();
            }

            // Trim trailing whitespace from value
            while (value_end > value_start &&
                   (cookie_header[value_end - 1] == ' ' ||
                    cookie_header[value_end - 1] == '\t')) {
                value_end--;
            }

            if (name == cookie_name) {
                return cookie_header.substr(value_start, value_end - value_start);
            }

            pos = cookie_header.find(';', eq_pos);
            if (pos == std::string::npos) break;
            pos++;
        }

        return std::nullopt;
    }

    /**
     * Add session cookie to response
     */
    void add_session_cookie(Http1Response& response, const std::string& session_id) const {
        std::string cookie = config_.cookie_name + "=" + session_id;

        // Add path
        cookie += "; Path=" + config_.cookie_path;

        // Add domain if specified
        if (!config_.cookie_domain.empty()) {
            cookie += "; Domain=" + config_.cookie_domain;
        }

        // Add expiration
        cookie += "; Max-Age=" + std::to_string(config_.ttl_seconds);

        // Security attributes
        if (config_.cookie_secure) {
            cookie += "; Secure";
        }
        if (config_.cookie_http_only) {
            cookie += "; HttpOnly";
        }

        // SameSite
        switch (config_.cookie_same_site) {
            case SameSite::STRICT:
                cookie += "; SameSite=Strict";
                break;
            case SameSite::LAX:
                cookie += "; SameSite=Lax";
                break;
            case SameSite::NONE:
                cookie += "; SameSite=None";
                break;
        }

        response.headers["Set-Cookie"] = cookie;
    }

    /**
     * Delete session cookie
     */
    void delete_session_cookie(Http1Response& response) const {
        std::string cookie = config_.cookie_name + "=";
        cookie += "; Path=" + config_.cookie_path;
        cookie += "; Max-Age=0";
        cookie += "; Expires=Thu, 01 Jan 1970 00:00:00 GMT";

        if (!config_.cookie_domain.empty()) {
            cookie += "; Domain=" + config_.cookie_domain;
        }
        if (config_.cookie_secure) {
            cookie += "; Secure";
        }
        if (config_.cookie_http_only) {
            cookie += "; HttpOnly";
        }

        response.headers["Set-Cookie"] = cookie;
    }

    /**
     * Maybe run cleanup if interval has passed
     */
    void maybe_cleanup() {
        if (config_.cleanup_interval_seconds == 0) return;

        auto now = std::chrono::steady_clock::now();
        auto interval = std::chrono::seconds(config_.cleanup_interval_seconds);

        if (now - last_cleanup_ > interval) {
            std::unique_lock lock(cleanup_mutex_, std::try_to_lock);
            if (lock.owns_lock()) {
                if (now - last_cleanup_ > interval) {
                    store_->cleanup_expired();
                    last_cleanup_ = now;
                }
            }
        }
    }
};

/**
 * Session configuration presets
 */
namespace session_presets {

/**
 * Development preset - relaxed security for local development
 */
inline SessionConfig development() {
    SessionConfig config;
    config.cookie_secure = false;          // Allow HTTP
    config.cookie_same_site = SameSite::LAX;
    config.ttl_seconds = 86400;            // 24 hours
    config.rolling_sessions = true;
    return config;
}

/**
 * Production preset - strict security
 */
inline SessionConfig production() {
    SessionConfig config;
    config.cookie_secure = true;           // Require HTTPS
    config.cookie_http_only = true;
    config.cookie_same_site = SameSite::STRICT;
    config.ttl_seconds = 3600;             // 1 hour
    config.rolling_sessions = true;
    config.session_id_length = 32;         // 256-bit
    return config;
}

/**
 * Short-lived sessions (e.g., for sensitive operations)
 */
inline SessionConfig short_lived() {
    SessionConfig config;
    config.cookie_secure = true;
    config.cookie_http_only = true;
    config.cookie_same_site = SameSite::STRICT;
    config.ttl_seconds = 300;              // 5 minutes
    config.rolling_sessions = false;       // Don't extend
    return config;
}

/**
 * Remember me sessions (long-lived)
 */
inline SessionConfig remember_me() {
    SessionConfig config;
    config.cookie_secure = true;
    config.cookie_http_only = true;
    config.cookie_same_site = SameSite::LAX;
    config.ttl_seconds = 86400 * 30;       // 30 days
    config.rolling_sessions = true;
    return config;
}

} // namespace session_presets

} // namespace http
} // namespace fasterapi
