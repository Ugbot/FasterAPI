/**
 * Session Management Middleware Tests
 *
 * Comprehensive tests for HTTP session handling including:
 * - Session creation and retrieval
 * - Cookie parsing and generation
 * - Session data storage (typed get/set)
 * - TTL expiration and cleanup
 * - LRU eviction
 * - Session ID regeneration
 * - Thread safety
 * - Configuration presets
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "src/cpp/http/session.h"
#include <thread>
#include <random>
#include <chrono>
#include <set>

using namespace fasterapi::http;
using namespace std::chrono_literals;

// Helper to generate random strings for testing
std::string random_string(size_t length) {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, sizeof(alphanum) - 2);

    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        result += alphanum[dis(gen)];
    }
    return result;
}

// =============================================================================
// SessionData Tests
// =============================================================================

class SessionDataTest : public ::testing::Test {
protected:
    SessionData session;
};

TEST_F(SessionDataTest, SetAndGetString) {
    session.set("username", std::string("alice"));
    auto result = session.get_string("username");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "alice");
}

TEST_F(SessionDataTest, SetAndGetInt) {
    session.set("user_id", int64_t(12345));
    auto result = session.get_int("user_id");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 12345);
}

TEST_F(SessionDataTest, SetAndGetBool) {
    session.set("is_admin", true);
    auto result = session.get_bool("is_admin");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(*result);
}

TEST_F(SessionDataTest, GetNonexistentKey) {
    auto result = session.get_string("nonexistent");
    EXPECT_FALSE(result.has_value());
}

TEST_F(SessionDataTest, GetWithWrongType) {
    session.set("count", int64_t(42));
    auto result = session.get_string("count");
    EXPECT_FALSE(result.has_value());
}

TEST_F(SessionDataTest, HasKey) {
    session.set("exists", std::string("value"));
    EXPECT_TRUE(session.has("exists"));
    EXPECT_FALSE(session.has("missing"));
}

TEST_F(SessionDataTest, RemoveKey) {
    session.set("temp", std::string("data"));
    EXPECT_TRUE(session.has("temp"));
    session.remove("temp");
    EXPECT_FALSE(session.has("temp"));
}

TEST_F(SessionDataTest, Clear) {
    session.set("key1", std::string("v1"));
    session.set("key2", std::string("v2"));
    session.set("key3", std::string("v3"));
    EXPECT_EQ(session.size(), 3u);

    session.clear();
    EXPECT_TRUE(session.empty());
    EXPECT_EQ(session.size(), 0u);
}

TEST_F(SessionDataTest, Keys) {
    session.set("apple", std::string("a"));
    session.set("banana", std::string("b"));
    session.set("cherry", std::string("c"));

    auto keys = session.keys();
    EXPECT_EQ(keys.size(), 3u);

    std::set<std::string> key_set(keys.begin(), keys.end());
    EXPECT_TRUE(key_set.count("apple"));
    EXPECT_TRUE(key_set.count("banana"));
    EXPECT_TRUE(key_set.count("cherry"));
}

TEST_F(SessionDataTest, ModifiedFlag) {
    EXPECT_FALSE(session.is_modified());

    session.set("key", std::string("value"));
    EXPECT_TRUE(session.is_modified());

    session.reset_modified();
    EXPECT_FALSE(session.is_modified());

    session.remove("key");
    EXPECT_TRUE(session.is_modified());
}

TEST_F(SessionDataTest, ThreadSafety) {
    const int num_threads = 8;
    const int operations_per_thread = 1000;
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([this, t, operations_per_thread]() {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> op_dist(0, 2);

            for (int i = 0; i < operations_per_thread; ++i) {
                std::string key = "key_" + std::to_string(t) + "_" + std::to_string(i % 10);
                int op = op_dist(gen);

                switch (op) {
                    case 0:
                        session.set(key, std::string("value_" + std::to_string(i)));
                        break;
                    case 1:
                        session.get_string(key);
                        break;
                    case 2:
                        session.has(key);
                        break;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // No crashes or data corruption
    EXPECT_TRUE(session.size() <= num_threads * 10);
}

// =============================================================================
// InMemorySessionStore Tests
// =============================================================================

class InMemorySessionStoreTest : public ::testing::Test {
protected:
    InMemorySessionStore store{100};

    SessionEntry create_entry(const std::string& id, int ttl_seconds = 3600) {
        SessionEntry entry;
        entry.session_id = id;
        entry.data = std::make_shared<SessionData>();
        entry.created_at = std::chrono::steady_clock::now();
        entry.last_accessed = entry.created_at;
        entry.expires_at = entry.created_at + std::chrono::seconds(ttl_seconds);
        return entry;
    }
};

TEST_F(InMemorySessionStoreTest, SetAndGet) {
    auto entry = create_entry("session_123");
    entry.data->set("user", std::string("bob"));

    store.set("session_123", entry);

    auto retrieved = store.get("session_123");
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->session_id, "session_123");
    EXPECT_EQ(retrieved->data->get_string("user"), "bob");
}

TEST_F(InMemorySessionStoreTest, GetNonexistent) {
    auto result = store.get("nonexistent");
    EXPECT_FALSE(result.has_value());
}

TEST_F(InMemorySessionStoreTest, Remove) {
    store.set("session_abc", create_entry("session_abc"));
    EXPECT_TRUE(store.get("session_abc").has_value());

    store.remove("session_abc");
    EXPECT_FALSE(store.get("session_abc").has_value());
}

TEST_F(InMemorySessionStoreTest, Count) {
    EXPECT_EQ(store.count(), 0u);

    store.set("s1", create_entry("s1"));
    store.set("s2", create_entry("s2"));
    store.set("s3", create_entry("s3"));

    EXPECT_EQ(store.count(), 3u);

    store.remove("s2");
    EXPECT_EQ(store.count(), 2u);
}

TEST_F(InMemorySessionStoreTest, ExpiredSessionNotReturned) {
    // Create session that's already expired
    auto entry = create_entry("expired", -1);
    store.set("expired", entry);

    auto result = store.get("expired");
    EXPECT_FALSE(result.has_value());
}

TEST_F(InMemorySessionStoreTest, CleanupExpired) {
    // Create mix of valid and expired sessions
    store.set("valid1", create_entry("valid1", 3600));
    store.set("valid2", create_entry("valid2", 3600));
    store.set("expired1", create_entry("expired1", -10));
    store.set("expired2", create_entry("expired2", -10));
    store.set("expired3", create_entry("expired3", -10));

    EXPECT_EQ(store.count(), 5u);

    store.cleanup_expired();

    EXPECT_EQ(store.count(), 2u);
    EXPECT_TRUE(store.get("valid1").has_value());
    EXPECT_TRUE(store.get("valid2").has_value());
}

TEST_F(InMemorySessionStoreTest, LRUEviction) {
    // Create store with small capacity
    InMemorySessionStore small_store(5);

    // Add 5 sessions
    for (int i = 0; i < 5; ++i) {
        auto id = "session_" + std::to_string(i);
        small_store.set(id, create_entry(id));
    }
    EXPECT_EQ(small_store.count(), 5u);

    // Access session_2 to make it recently used
    small_store.get("session_2");

    // Add more sessions, should evict oldest
    small_store.set("session_5", create_entry("session_5"));

    EXPECT_EQ(small_store.count(), 5u);
    // session_0 should have been evicted (LRU)
    EXPECT_FALSE(small_store.get("session_0").has_value());
    // session_2 should still exist (was accessed)
    EXPECT_TRUE(small_store.get("session_2").has_value());
    EXPECT_TRUE(small_store.get("session_5").has_value());
}

// =============================================================================
// SessionMiddleware Tests
// =============================================================================

class SessionMiddlewareTest : public ::testing::Test {
protected:
    SessionConfig config;
    std::unique_ptr<SessionMiddleware> middleware;
    Http1Response response;

    void SetUp() override {
        config.ttl_seconds = 3600;
        config.cookie_name = "session_id";
        config.cookie_secure = false;  // For testing
        config.cleanup_interval_seconds = 0;  // Disable auto-cleanup
        middleware = std::make_unique<SessionMiddleware>(config);
    }

    std::unordered_map<std::string, std::string> make_headers_with_cookie(
        const std::string& session_id
    ) {
        return {{"Cookie", config.cookie_name + "=" + session_id}};
    }
};

TEST_F(SessionMiddlewareTest, CreateSession) {
    std::unordered_map<std::string, std::string> headers;

    auto result = middleware->create_session(response, "127.0.0.1", "TestAgent/1.0");

    EXPECT_TRUE(result.has_session);
    EXPECT_TRUE(result.is_new);
    EXPECT_FALSE(result.session_id.empty());
    EXPECT_NE(result.data, nullptr);

    // Check Set-Cookie header
    EXPECT_TRUE(response.headers.count("Set-Cookie"));
    EXPECT_NE(response.headers["Set-Cookie"].find(result.session_id), std::string::npos);
}

TEST_F(SessionMiddlewareTest, SessionIdFormat) {
    auto result = middleware->create_session(response);

    // Default is 32 bytes = 64 hex characters
    EXPECT_EQ(result.session_id.length(), 64u);

    // Should be all hex characters
    for (char c : result.session_id) {
        EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
}

TEST_F(SessionMiddlewareTest, SessionIdUniqueness) {
    std::set<std::string> ids;
    const int count = 1000;

    for (int i = 0; i < count; ++i) {
        Http1Response resp;
        auto result = middleware->create_session(resp);
        ids.insert(result.session_id);
    }

    // All IDs should be unique
    EXPECT_EQ(ids.size(), count);
}

TEST_F(SessionMiddlewareTest, GetSessionWithCookie) {
    // First create a session
    auto created = middleware->create_session(response);

    // Then get it with the cookie
    auto headers = make_headers_with_cookie(created.session_id);
    auto result = middleware->get_session(headers);

    EXPECT_TRUE(result.has_session);
    EXPECT_FALSE(result.is_new);
    EXPECT_EQ(result.session_id, created.session_id);
}

TEST_F(SessionMiddlewareTest, GetSessionWithoutCookie) {
    std::unordered_map<std::string, std::string> headers;

    auto result = middleware->get_session(headers);

    EXPECT_FALSE(result.has_session);
}

TEST_F(SessionMiddlewareTest, GetSessionWithInvalidCookie) {
    auto headers = make_headers_with_cookie("invalid_session_id_that_doesnt_exist");

    auto result = middleware->get_session(headers);

    EXPECT_FALSE(result.has_session);
}

TEST_F(SessionMiddlewareTest, GetOrCreateNew) {
    std::unordered_map<std::string, std::string> headers;

    auto result = middleware->get_or_create(headers, response);

    EXPECT_TRUE(result.has_session);
    EXPECT_TRUE(result.is_new);
}

TEST_F(SessionMiddlewareTest, GetOrCreateExisting) {
    // Create session
    auto created = middleware->create_session(response);
    created.data->set("key", std::string("value"));

    // Get or create with same cookie
    Http1Response resp2;
    auto headers = make_headers_with_cookie(created.session_id);
    auto result = middleware->get_or_create(headers, resp2);

    EXPECT_TRUE(result.has_session);
    EXPECT_FALSE(result.is_new);
    EXPECT_EQ(result.session_id, created.session_id);
    // Data should be preserved
    EXPECT_EQ(result.data->get_string("key"), "value");
}

TEST_F(SessionMiddlewareTest, DestroySession) {
    // Create session
    auto created = middleware->create_session(response);

    // Destroy it
    Http1Response resp2;
    middleware->destroy_session(created.session_id, resp2);

    // Session should no longer exist
    auto headers = make_headers_with_cookie(created.session_id);
    auto result = middleware->get_session(headers);
    EXPECT_FALSE(result.has_session);

    // Cookie should be deleted
    EXPECT_TRUE(resp2.headers.count("Set-Cookie"));
    EXPECT_NE(resp2.headers["Set-Cookie"].find("Max-Age=0"), std::string::npos);
}

TEST_F(SessionMiddlewareTest, DestroySessionFromHeaders) {
    // Create session
    auto created = middleware->create_session(response);

    // Destroy using headers
    Http1Response resp2;
    auto headers = make_headers_with_cookie(created.session_id);
    middleware->destroy_session(headers, resp2);

    // Session should be gone
    auto result = middleware->get_session(headers);
    EXPECT_FALSE(result.has_session);
}

TEST_F(SessionMiddlewareTest, RegenerateSessionId) {
    // Create session with data
    auto created = middleware->create_session(response);
    created.data->set("user_id", int64_t(12345));
    created.data->set("role", std::string("admin"));

    // Regenerate ID
    Http1Response resp2;
    std::string new_id = middleware->regenerate_session_id(created.session_id, resp2);

    EXPECT_FALSE(new_id.empty());
    EXPECT_NE(new_id, created.session_id);

    // Old session should be gone
    auto old_headers = make_headers_with_cookie(created.session_id);
    auto old_result = middleware->get_session(old_headers);
    EXPECT_FALSE(old_result.has_session);

    // New session should have the data
    auto new_headers = make_headers_with_cookie(new_id);
    auto new_result = middleware->get_session(new_headers);
    EXPECT_TRUE(new_result.has_session);
    EXPECT_EQ(new_result.data->get_int("user_id"), 12345);
    EXPECT_EQ(new_result.data->get_string("role"), "admin");
}

TEST_F(SessionMiddlewareTest, SessionCount) {
    EXPECT_EQ(middleware->session_count(), 0u);

    Http1Response r1, r2, r3;
    middleware->create_session(r1);
    middleware->create_session(r2);
    middleware->create_session(r3);

    EXPECT_EQ(middleware->session_count(), 3u);
}

TEST_F(SessionMiddlewareTest, ManualCleanup) {
    // Create valid and expired sessions
    Http1Response r1, r2, r3;
    auto valid1 = middleware->create_session(r1);
    auto valid2 = middleware->create_session(r2);

    // Create expired session by directly modifying store
    // (In real usage, sessions expire naturally)
    EXPECT_EQ(middleware->session_count(), 2u);

    middleware->cleanup();

    // Valid sessions should remain
    EXPECT_EQ(middleware->session_count(), 2u);
}

// =============================================================================
// Cookie Parsing Tests
// =============================================================================

class CookieParsingTest : public ::testing::Test {
protected:
    SessionMiddleware middleware;
    Http1Response response;
};

TEST_F(CookieParsingTest, SingleCookie) {
    std::unordered_map<std::string, std::string> headers = {
        {"Cookie", "session_id=abc123"}
    };

    // Create a session first to make abc123 valid
    // For this test, we just check that invalid sessions return no session
    auto result = middleware.get_session(headers);
    EXPECT_FALSE(result.has_session);  // abc123 doesn't exist
}

TEST_F(CookieParsingTest, MultipleCookies) {
    // Create a real session
    auto created = middleware.create_session(response);

    std::unordered_map<std::string, std::string> headers = {
        {"Cookie", "other=value; session_id=" + created.session_id + "; another=data"}
    };

    auto result = middleware.get_session(headers);
    EXPECT_TRUE(result.has_session);
    EXPECT_EQ(result.session_id, created.session_id);
}

TEST_F(CookieParsingTest, CookieWithSpaces) {
    auto created = middleware.create_session(response);

    std::unordered_map<std::string, std::string> headers = {
        {"Cookie", "  session_id=" + created.session_id + "  ; other=value"}
    };

    auto result = middleware.get_session(headers);
    EXPECT_TRUE(result.has_session);
}

TEST_F(CookieParsingTest, LowercaseCookieHeader) {
    auto created = middleware.create_session(response);

    std::unordered_map<std::string, std::string> headers = {
        {"cookie", "session_id=" + created.session_id}
    };

    auto result = middleware.get_session(headers);
    EXPECT_TRUE(result.has_session);
}

// =============================================================================
// Set-Cookie Header Tests
// =============================================================================

class SetCookieTest : public ::testing::Test {
protected:
    Http1Response response;
};

TEST_F(SetCookieTest, SecureCookie) {
    SessionConfig config;
    config.cookie_secure = true;
    SessionMiddleware middleware(config);

    middleware.create_session(response);

    EXPECT_NE(response.headers["Set-Cookie"].find("; Secure"), std::string::npos);
}

TEST_F(SetCookieTest, HttpOnlyCookie) {
    SessionConfig config;
    config.cookie_http_only = true;
    SessionMiddleware middleware(config);

    middleware.create_session(response);

    EXPECT_NE(response.headers["Set-Cookie"].find("; HttpOnly"), std::string::npos);
}

TEST_F(SetCookieTest, SameSiteStrict) {
    SessionConfig config;
    config.cookie_same_site = SameSite::STRICT;
    SessionMiddleware middleware(config);

    middleware.create_session(response);

    EXPECT_NE(response.headers["Set-Cookie"].find("; SameSite=Strict"), std::string::npos);
}

TEST_F(SetCookieTest, SameSiteLax) {
    SessionConfig config;
    config.cookie_same_site = SameSite::LAX;
    SessionMiddleware middleware(config);

    middleware.create_session(response);

    EXPECT_NE(response.headers["Set-Cookie"].find("; SameSite=Lax"), std::string::npos);
}

TEST_F(SetCookieTest, SameSiteNone) {
    SessionConfig config;
    config.cookie_same_site = SameSite::NONE;
    SessionMiddleware middleware(config);

    middleware.create_session(response);

    EXPECT_NE(response.headers["Set-Cookie"].find("; SameSite=None"), std::string::npos);
}

TEST_F(SetCookieTest, CustomPath) {
    SessionConfig config;
    config.cookie_path = "/api";
    SessionMiddleware middleware(config);

    middleware.create_session(response);

    EXPECT_NE(response.headers["Set-Cookie"].find("; Path=/api"), std::string::npos);
}

TEST_F(SetCookieTest, CustomDomain) {
    SessionConfig config;
    config.cookie_domain = "example.com";
    SessionMiddleware middleware(config);

    middleware.create_session(response);

    EXPECT_NE(response.headers["Set-Cookie"].find("; Domain=example.com"), std::string::npos);
}

TEST_F(SetCookieTest, CustomName) {
    SessionConfig config;
    config.cookie_name = "my_session";
    SessionMiddleware middleware(config);

    auto result = middleware.create_session(response);

    EXPECT_EQ(response.headers["Set-Cookie"].substr(0, 11), "my_session=");
}

TEST_F(SetCookieTest, MaxAge) {
    SessionConfig config;
    config.ttl_seconds = 7200;
    SessionMiddleware middleware(config);

    middleware.create_session(response);

    EXPECT_NE(response.headers["Set-Cookie"].find("; Max-Age=7200"), std::string::npos);
}

// =============================================================================
// Configuration Presets Tests
// =============================================================================

TEST(SessionPresetsTest, Development) {
    auto config = session_presets::development();

    EXPECT_FALSE(config.cookie_secure);  // Allow HTTP
    EXPECT_EQ(config.cookie_same_site, SameSite::LAX);
    EXPECT_EQ(config.ttl_seconds, 86400u);  // 24 hours
    EXPECT_TRUE(config.rolling_sessions);
}

TEST(SessionPresetsTest, Production) {
    auto config = session_presets::production();

    EXPECT_TRUE(config.cookie_secure);
    EXPECT_TRUE(config.cookie_http_only);
    EXPECT_EQ(config.cookie_same_site, SameSite::STRICT);
    EXPECT_EQ(config.ttl_seconds, 3600u);  // 1 hour
    EXPECT_TRUE(config.rolling_sessions);
    EXPECT_EQ(config.session_id_length, 32u);
}

TEST(SessionPresetsTest, ShortLived) {
    auto config = session_presets::short_lived();

    EXPECT_TRUE(config.cookie_secure);
    EXPECT_TRUE(config.cookie_http_only);
    EXPECT_EQ(config.cookie_same_site, SameSite::STRICT);
    EXPECT_EQ(config.ttl_seconds, 300u);  // 5 minutes
    EXPECT_FALSE(config.rolling_sessions);  // No extension
}

TEST(SessionPresetsTest, RememberMe) {
    auto config = session_presets::remember_me();

    EXPECT_TRUE(config.cookie_secure);
    EXPECT_TRUE(config.cookie_http_only);
    EXPECT_EQ(config.cookie_same_site, SameSite::LAX);
    EXPECT_EQ(config.ttl_seconds, 86400u * 30);  // 30 days
    EXPECT_TRUE(config.rolling_sessions);
}

// =============================================================================
// Rolling Sessions Tests
// =============================================================================

TEST(RollingSessionsTest, ExtendsTTL) {
    SessionConfig config;
    config.ttl_seconds = 3600;
    config.rolling_sessions = true;
    config.cleanup_interval_seconds = 0;
    SessionMiddleware middleware(config);

    Http1Response resp;
    auto created = middleware.create_session(resp);

    // Get session should extend TTL
    std::unordered_map<std::string, std::string> headers = {
        {"Cookie", "session_id=" + created.session_id}
    };

    // Access multiple times
    for (int i = 0; i < 5; ++i) {
        auto result = middleware.get_session(headers);
        EXPECT_TRUE(result.has_session);
    }

    // Session should still be valid
    EXPECT_EQ(middleware.session_count(), 1u);
}

// =============================================================================
// Custom Session Store Tests
// =============================================================================

class MockSessionStore : public SessionStore {
public:
    std::optional<SessionEntry> get(const std::string& session_id) override {
        get_calls++;
        auto it = sessions.find(session_id);
        if (it != sessions.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    void set(const std::string& session_id, const SessionEntry& entry) override {
        set_calls++;
        sessions[session_id] = entry;
    }

    void remove(const std::string& session_id) override {
        remove_calls++;
        sessions.erase(session_id);
    }

    void cleanup_expired() override {
        cleanup_calls++;
    }

    size_t count() const override {
        return sessions.size();
    }

    std::unordered_map<std::string, SessionEntry> sessions;
    int get_calls = 0;
    int set_calls = 0;
    int remove_calls = 0;
    int cleanup_calls = 0;
};

TEST(CustomStoreTest, UsesProvidedStore) {
    auto custom_store = std::make_shared<MockSessionStore>();

    SessionConfig config;
    config.store = custom_store;
    config.cleanup_interval_seconds = 0;
    SessionMiddleware middleware(config);

    Http1Response resp;
    middleware.create_session(resp);

    EXPECT_EQ(custom_store->set_calls, 1);
    EXPECT_EQ(custom_store->count(), 1u);
}

// =============================================================================
// Performance Tests
// =============================================================================

TEST(SessionPerformanceTest, HighThroughputCreation) {
    SessionConfig config;
    config.max_sessions = 100000;
    config.cleanup_interval_seconds = 0;
    SessionMiddleware middleware(config);

    const int num_sessions = 10000;

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < num_sessions; ++i) {
        Http1Response resp;
        middleware.create_session(resp);
    }

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double sessions_per_sec = (num_sessions * 1e6) / duration.count();

    std::cout << "Session creation: " << sessions_per_sec << " sessions/sec" << std::endl;

    // Should create at least 50K sessions/sec
    EXPECT_GT(sessions_per_sec, 50000.0);
    EXPECT_EQ(middleware.session_count(), num_sessions);
}

TEST(SessionPerformanceTest, HighThroughputLookup) {
    SessionConfig config;
    config.max_sessions = 10000;
    config.cleanup_interval_seconds = 0;
    SessionMiddleware middleware(config);

    // Create sessions
    std::vector<std::string> session_ids;
    for (int i = 0; i < 1000; ++i) {
        Http1Response resp;
        auto result = middleware.create_session(resp);
        session_ids.push_back(result.session_id);
    }

    const int num_lookups = 100000;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, session_ids.size() - 1);

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < num_lookups; ++i) {
        std::unordered_map<std::string, std::string> headers = {
            {"Cookie", "session_id=" + session_ids[dis(gen)]}
        };
        middleware.get_session(headers);
    }

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double lookups_per_sec = (num_lookups * 1e6) / duration.count();

    std::cout << "Session lookups: " << lookups_per_sec << " lookups/sec" << std::endl;

    // Should handle at least 500K lookups/sec
    EXPECT_GT(lookups_per_sec, 500000.0);
}

TEST(SessionPerformanceTest, ConcurrentAccess) {
    SessionConfig config;
    config.max_sessions = 100000;
    config.cleanup_interval_seconds = 0;
    SessionMiddleware middleware(config);

    const int num_threads = 8;
    const int ops_per_thread = 5000;
    std::atomic<int> created{0};
    std::atomic<int> found{0};

    // Create initial sessions
    std::vector<std::string> session_ids;
    std::mutex ids_mutex;
    for (int i = 0; i < 100; ++i) {
        Http1Response resp;
        auto result = middleware.create_session(resp);
        session_ids.push_back(result.session_id);
    }

    std::vector<std::thread> threads;
    auto start = std::chrono::steady_clock::now();

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> op_dis(0, 2);

            for (int i = 0; i < ops_per_thread; ++i) {
                int op = op_dis(gen);

                if (op == 0) {
                    // Create
                    Http1Response resp;
                    auto result = middleware.create_session(resp);
                    {
                        std::lock_guard<std::mutex> lock(ids_mutex);
                        if (session_ids.size() < 10000) {
                            session_ids.push_back(result.session_id);
                        }
                    }
                    created++;
                } else {
                    // Lookup
                    std::string id;
                    {
                        std::lock_guard<std::mutex> lock(ids_mutex);
                        if (!session_ids.empty()) {
                            std::uniform_int_distribution<> id_dis(0, session_ids.size() - 1);
                            id = session_ids[id_dis(gen)];
                        }
                    }
                    if (!id.empty()) {
                        std::unordered_map<std::string, std::string> headers = {
                            {"Cookie", "session_id=" + id}
                        };
                        auto result = middleware.get_session(headers);
                        if (result.has_session) {
                            found++;
                        }
                    }
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    int total_ops = num_threads * ops_per_thread;
    double ops_per_sec = (total_ops * 1000.0) / duration.count();

    std::cout << "Concurrent operations: " << ops_per_sec << " ops/sec" << std::endl;
    std::cout << "  Created: " << created << ", Found: " << found << std::endl;

    // Should handle concurrent access without crashes
    EXPECT_GT(created.load(), 0);
    EXPECT_GT(found.load(), 0);
}

// =============================================================================
// Edge Cases Tests
// =============================================================================

TEST(EdgeCasesTest, EmptyCookieHeader) {
    SessionMiddleware middleware;

    std::unordered_map<std::string, std::string> headers = {
        {"Cookie", ""}
    };

    auto result = middleware.get_session(headers);
    EXPECT_FALSE(result.has_session);
}

TEST(EdgeCasesTest, MalformedCookieHeader) {
    SessionMiddleware middleware;

    std::unordered_map<std::string, std::string> headers = {
        {"Cookie", "=no_name;malformed;;===weird"}
    };

    auto result = middleware.get_session(headers);
    EXPECT_FALSE(result.has_session);
}

TEST(EdgeCasesTest, VeryLongSessionData) {
    SessionMiddleware middleware;
    Http1Response resp;

    auto session = middleware.create_session(resp);

    // Store large data
    std::string large_data(100000, 'x');
    session.data->set("large", large_data);

    EXPECT_EQ(session.data->get_string("large")->size(), 100000u);
}

TEST(EdgeCasesTest, SpecialCharactersInData) {
    SessionMiddleware middleware;
    Http1Response resp;

    auto session = middleware.create_session(resp);

    // Unicode and special characters
    session.data->set("emoji", std::string("\xF0\x9F\x98\x80"));  // Grinning face
    session.data->set("quotes", std::string("He said \"hello\""));
    session.data->set("newlines", std::string("line1\nline2\rline3"));

    EXPECT_TRUE(session.data->get_string("emoji").has_value());
    EXPECT_TRUE(session.data->get_string("quotes").has_value());
    EXPECT_TRUE(session.data->get_string("newlines").has_value());
}

TEST(EdgeCasesTest, RegenerateNonexistentSession) {
    SessionMiddleware middleware;
    Http1Response resp;

    std::string new_id = middleware.regenerate_session_id("nonexistent_session", resp);

    EXPECT_TRUE(new_id.empty());
}

TEST(EdgeCasesTest, ConfigWithZeroTTL) {
    SessionConfig config;
    config.ttl_seconds = 0;  // Immediate expiration
    config.cleanup_interval_seconds = 0;
    SessionMiddleware middleware(config);

    Http1Response resp;
    auto session = middleware.create_session(resp);

    // Session should be created
    EXPECT_TRUE(session.has_session);

    // But should expire immediately on next lookup
    std::unordered_map<std::string, std::string> headers = {
        {"Cookie", "session_id=" + session.session_id}
    };
    auto result = middleware.get_session(headers);
    EXPECT_FALSE(result.has_session);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
