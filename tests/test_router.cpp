/**
 * Router Correctness Tests
 * 
 * Comprehensive test suite for the radix tree router.
 * Focus: Correctness over performance.
 */

#include "../src/cpp/http/router.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <string>

using namespace fasterapi::http;

// Test counter
static int tests_passed = 0;
static int tests_failed = 0;
static bool current_test_failed = false;
static std::string current_test_error;

#define TEST(name) \
    void test_##name(); \
    struct Test##name { \
        Test##name() { \
            std::cout << "Running " << #name << "... "; \
            current_test_failed = false; \
            current_test_error = ""; \
            test_##name(); \
            if (current_test_failed) { \
                std::cout << "❌ FAIL: " << current_test_error << std::endl; \
                tests_failed++; \
            } else { \
                std::cout << "✅ PASS" << std::endl; \
                tests_passed++; \
            } \
        } \
    } test_instance_##name; \
    void test_##name()

#define ASSERT(condition) \
    if (!(condition)) { \
        current_test_failed = true; \
        current_test_error = "Assertion failed: " #condition; \
        return; \
    }

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) { \
        current_test_failed = true; \
        current_test_error = std::string("Expected ") + std::to_string(b) + " but got " + std::to_string(a); \
        return; \
    }

// Dummy handler for testing
static RouteHandler make_handler(int id) {
    return [id](HttpRequest*, HttpResponse*, const RouteParams&) {
        // Handler ID stored for verification
    };
}

// ============================================================================
// Basic Route Registration
// ============================================================================

TEST(static_route_registration) {
    Router router;
    
    int result = router.add_route("GET", "/", make_handler(1));
    ASSERT_EQ(result, 0);
    
    result = router.add_route("GET", "/users", make_handler(2));
    ASSERT_EQ(result, 0);
    
    ASSERT_EQ(router.total_routes(), 2);
}

TEST(invalid_path_rejected) {
    Router router;
    
    // Path must start with '/'
    int result = router.add_route("GET", "users", make_handler(1));
    ASSERT_EQ(result, 1);  // Should fail
}

TEST(multiple_methods) {
    Router router;
    
    router.add_route("GET", "/users", make_handler(1));
    router.add_route("POST", "/users", make_handler(2));
    router.add_route("PUT", "/users", make_handler(3));
    router.add_route("DELETE", "/users", make_handler(4));
    
    ASSERT_EQ(router.total_routes(), 4);
}

// ============================================================================
// Static Route Matching
// ============================================================================

TEST(match_root) {
    Router router;
    router.add_route("GET", "/", make_handler(1));
    
    RouteParams params;
    auto handler = router.match("GET", "/", params);
    
    ASSERT(handler != nullptr);
    ASSERT(params.empty());
}

TEST(match_simple_path) {
    Router router;
    router.add_route("GET", "/users", make_handler(1));
    
    RouteParams params;
    auto handler = router.match("GET", "/users", params);
    
    ASSERT(handler != nullptr);
    ASSERT(params.empty());
}

TEST(match_nested_path) {
    Router router;
    router.add_route("GET", "/api/v1/users", make_handler(1));
    
    RouteParams params;
    auto handler = router.match("GET", "/api/v1/users", params);
    
    ASSERT(handler != nullptr);
}

TEST(no_match_wrong_path) {
    Router router;
    router.add_route("GET", "/users", make_handler(1));
    
    RouteParams params;
    auto handler = router.match("GET", "/posts", params);
    
    ASSERT(handler == nullptr);
}

TEST(no_match_wrong_method) {
    Router router;
    router.add_route("GET", "/users", make_handler(1));
    
    RouteParams params;
    auto handler = router.match("POST", "/users", params);
    
    ASSERT(handler == nullptr);
}

// ============================================================================
// Path Parameters
// ============================================================================

TEST(single_param) {
    Router router;
    router.add_route("GET", "/users/{id}", make_handler(1));
    
    RouteParams params;
    auto handler = router.match("GET", "/users/123", params);
    
    ASSERT(handler != nullptr);
    ASSERT_EQ(params.size(), 1);
    ASSERT(params.get("id") == "123");
}

TEST(multiple_params) {
    Router router;
    router.add_route("GET", "/users/{userId}/posts/{postId}", make_handler(1));
    
    RouteParams params;
    auto handler = router.match("GET", "/users/42/posts/100", params);
    
    ASSERT(handler != nullptr);
    ASSERT_EQ(params.size(), 2);
    ASSERT(params.get("userId") == "42");
    ASSERT(params.get("postId") == "100");
}

TEST(param_with_special_chars) {
    Router router;
    router.add_route("GET", "/users/{id}", make_handler(1));
    
    RouteParams params;
    auto handler = router.match("GET", "/users/abc-123_xyz", params);
    
    ASSERT(handler != nullptr);
    ASSERT(params.get("id") == "abc-123_xyz");
}

// ============================================================================
// Wildcard Routes
// ============================================================================

TEST(wildcard_basic) {
    Router router;
    router.add_route("GET", "/files/*path", make_handler(1));
    
    RouteParams params;
    auto handler = router.match("GET", "/files/a/b/c.txt", params);
    
    ASSERT(handler != nullptr);
    ASSERT_EQ(params.size(), 1);
    ASSERT(params.get("path") == "a/b/c.txt");
}

TEST(wildcard_empty) {
    Router router;
    router.add_route("GET", "/files/*path", make_handler(1));
    
    RouteParams params;
    auto handler = router.match("GET", "/files/", params);
    
    ASSERT(handler != nullptr);
    ASSERT(params.get("path") == "");
}

// ============================================================================
// Priority Matching
// ============================================================================

TEST(static_over_param) {
    Router router;
    router.add_route("GET", "/users/{id}", make_handler(1));
    router.add_route("GET", "/users/me", make_handler(2));
    
    RouteParams params;
    
    // Should match static "/users/me" not param
    auto handler = router.match("GET", "/users/me", params);
    ASSERT(handler != nullptr);
    // Static route should match (no params extracted)
    ASSERT(params.empty());
}

TEST(param_over_wildcard) {
    Router router;
    router.add_route("GET", "/files/*path", make_handler(1));
    router.add_route("GET", "/files/{id}", make_handler(2));
    
    RouteParams params;
    
    // Should match param "/files/{id}" not wildcard
    auto handler = router.match("GET", "/files/123", params);
    ASSERT(handler != nullptr);
    ASSERT(params.get("id") == "123");
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(trailing_slash_matters) {
    Router router;
    router.add_route("GET", "/users", make_handler(1));
    
    RouteParams params;
    
    // Exact match
    auto handler1 = router.match("GET", "/users", params);
    ASSERT(handler1 != nullptr);
    
    // With trailing slash - should NOT match
    auto handler2 = router.match("GET", "/users/", params);
    ASSERT(handler2 == nullptr);
}

TEST(empty_param_not_matched) {
    Router router;
    router.add_route("GET", "/users/{id}", make_handler(1));
    
    RouteParams params;
    
    // "/users/" should not match "/users/{id}"
    auto handler = router.match("GET", "/users/", params);
    ASSERT(handler == nullptr);
}

TEST(overlapping_static_routes) {
    Router router;
    router.add_route("GET", "/user", make_handler(1));
    router.add_route("GET", "/users", make_handler(2));
    router.add_route("GET", "/users/active", make_handler(3));
    
    RouteParams params;
    
    auto h1 = router.match("GET", "/user", params);
    ASSERT(h1 != nullptr);
    
    auto h2 = router.match("GET", "/users", params);
    ASSERT(h2 != nullptr);
    
    auto h3 = router.match("GET", "/users/active", params);
    ASSERT(h3 != nullptr);
}

TEST(param_with_slash) {
    Router router;
    router.add_route("GET", "/users/{id}/profile", make_handler(1));
    
    RouteParams params;
    auto handler = router.match("GET", "/users/123/profile", params);
    
    ASSERT(handler != nullptr);
    ASSERT(params.get("id") == "123");
}

// ============================================================================
// RouteParams Tests
// ============================================================================

TEST(params_clear) {
    RouteParams params;
    params.add("key1", "value1");
    params.add("key2", "value2");
    
    ASSERT_EQ(params.size(), 2);
    
    params.clear();
    ASSERT_EQ(params.size(), 0);
    ASSERT(params.empty());
}

TEST(params_get_missing) {
    RouteParams params;
    params.add("key1", "value1");
    
    ASSERT(params.get("missing") == "");
}

TEST(params_index_access) {
    RouteParams params;
    params.add("first", "value1");
    params.add("second", "value2");
    
    ASSERT(params[0].key == "first");
    ASSERT(params[0].value == "value1");
    ASSERT(params[1].key == "second");
    ASSERT(params[1].value == "value2");
}

// ============================================================================
// Complex Scenarios
// ============================================================================

TEST(complex_api_routes) {
    Router router;
    
    // Register realistic API routes
    router.add_route("GET", "/api/v1/users", make_handler(1));
    router.add_route("GET", "/api/v1/users/{id}", make_handler(2));
    router.add_route("POST", "/api/v1/users", make_handler(3));
    router.add_route("PUT", "/api/v1/users/{id}", make_handler(4));
    router.add_route("DELETE", "/api/v1/users/{id}", make_handler(5));
    router.add_route("GET", "/api/v1/users/{id}/posts", make_handler(6));
    router.add_route("GET", "/api/v1/users/{id}/posts/{postId}", make_handler(7));
    
    RouteParams params;
    
    // Test each route
    auto h1 = router.match("GET", "/api/v1/users", params);
    ASSERT(h1 != nullptr);
    
    auto h2 = router.match("GET", "/api/v1/users/123", params);
    ASSERT(h2 != nullptr);
    ASSERT(params.get("id") == "123");
    
    params.clear();
    auto h7 = router.match("GET", "/api/v1/users/42/posts/100", params);
    ASSERT(h7 != nullptr);
    ASSERT(params.get("id") == "42");
    ASSERT(params.get("postId") == "100");
}

TEST(route_introspection) {
    Router router;
    
    router.add_route("GET", "/users", make_handler(1));
    router.add_route("GET", "/users/{id}", make_handler(2));
    router.add_route("POST", "/users", make_handler(3));
    
    auto routes = router.get_routes();
    
    // Debug: print all routes
    std::cout << "\n  Registered routes (" << routes.size() << "):" << std::endl;
    for (const auto& route : routes) {
        std::cout << "    " << route.method << " " << route.path << std::endl;
    }
    std::cout << "  ";  // Return to test output line
    
    ASSERT_EQ(routes.size(), 3);
    
    // Check that all routes are present
    bool found_get_users = false;
    bool found_get_user_id = false;
    bool found_post_users = false;
    
    for (const auto& route : routes) {
        if (route.method == "GET" && route.path == "/users") found_get_users = true;
        if (route.method == "GET" && route.path == "/users/{id}") found_get_user_id = true;
        if (route.method == "POST" && route.path == "/users") found_post_users = true;
    }
    
    if (!found_get_users) std::cout << " Missing GET /users ";
    if (!found_get_user_id) std::cout << " Missing GET /users/{id} ";
    if (!found_post_users) std::cout << " Missing POST /users ";
    
    ASSERT(found_get_users);
    ASSERT(found_get_user_id);
    ASSERT(found_post_users);
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "╔══════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║          Router Correctness Test Suite                  ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;
    
    // Tests run automatically via static constructors
    
    std::cout << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout << "Tests: " << (tests_passed + tests_failed) << std::endl;
    std::cout << "Passed: " << tests_passed << std::endl;
    std::cout << "Failed: " << tests_failed << std::endl;
    
    if (tests_failed == 0) {
        std::cout << std::endl;
        std::cout << "🎉 All tests passed!" << std::endl;
        return 0;
    } else {
        std::cout << std::endl;
        std::cout << "❌ Some tests failed" << std::endl;
        return 1;
    }
}

