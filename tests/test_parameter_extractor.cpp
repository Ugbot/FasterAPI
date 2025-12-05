/**
 * Parameter Extractor Unit Tests
 *
 * Tests for query parameter and path parameter extraction.
 * Validates the C++ layer works correctly in isolation.
 */

#include "../src/cpp/http/parameter_extractor.h"
#include <iostream>
#include <cassert>
#include <string>
#include <unordered_map>

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
        current_test_error = std::string("Expected: ") + std::to_string(b) + ", Got: " + std::to_string(a); \
        return; \
    }

#define ASSERT_STR_EQ(a, b) \
    if ((a) != (b)) { \
        current_test_failed = true; \
        current_test_error = std::string("Expected: '") + (b) + "', Got: '" + (a) + "'"; \
        return; \
    }

// Helper to print query params map
void print_params(const std::unordered_map<std::string, std::string>& params) {
    std::cout << "{";
    for (const auto& [key, value] : params) {
        std::cout << key << ":" << value << " ";
    }
    std::cout << "}";
}

// ============================================================================
// Query Parameter Extraction Tests
// ============================================================================

TEST(query_params_simple) {
    auto params = ParameterExtractor::get_query_params("/search?q=test&limit=10");

    ASSERT_EQ(params.size(), 2);
    ASSERT_STR_EQ(params["q"], "test");
    ASSERT_STR_EQ(params["limit"], "10");
}

TEST(query_params_no_query) {
    auto params = ParameterExtractor::get_query_params("/search");

    ASSERT_EQ(params.size(), 0);
}

TEST(query_params_plus_to_space) {
    auto params = ParameterExtractor::get_query_params("?q=hello+world&limit=99");

    ASSERT_EQ(params.size(), 2);
    ASSERT_STR_EQ(params["q"], "hello world");
    ASSERT_STR_EQ(params["limit"], "99");
}

TEST(query_params_with_path) {
    auto params = ParameterExtractor::get_query_params("/path/to/resource?key=value");

    ASSERT_EQ(params.size(), 1);
    ASSERT_STR_EQ(params["key"], "value");
}

TEST(query_params_multiple) {
    auto params = ParameterExtractor::get_query_params("?key1=val1&key2=val2&key3=val3");

    ASSERT_EQ(params.size(), 3);
    ASSERT_STR_EQ(params["key1"], "val1");
    ASSERT_STR_EQ(params["key2"], "val2");
    ASSERT_STR_EQ(params["key3"], "val3");
}

TEST(query_params_url_encoded) {
    auto params = ParameterExtractor::get_query_params("?encoded=%20%21%22");

    ASSERT_EQ(params.size(), 1);
    ASSERT_STR_EQ(params["encoded"], " !\"");
}

TEST(query_params_real_world_search) {
    auto params = ParameterExtractor::get_query_params("/search?q=fastapi&limit=99");

    std::cout << "Extracted: ";
    print_params(params);
    std::cout << " ";

    ASSERT_EQ(params.size(), 2);
    ASSERT_STR_EQ(params["q"], "fastapi");
    ASSERT_STR_EQ(params["limit"], "99");
}

TEST(query_params_real_world_pagination) {
    auto params = ParameterExtractor::get_query_params("/users/42/posts?page=5&size=10");

    std::cout << "Extracted: ";
    print_params(params);
    std::cout << " ";

    ASSERT_EQ(params.size(), 2);
    ASSERT_STR_EQ(params["page"], "5");
    ASSERT_STR_EQ(params["size"], "10");
}

// ============================================================================
// Path Parameter Extraction Tests
// ============================================================================

TEST(path_params_single) {
    CompiledRoutePattern pattern("/items/{item_id}");
    auto params = pattern.extract("/items/123");

    ASSERT_EQ(params.size(), 1);
    ASSERT_STR_EQ(params["item_id"], "123");
}

TEST(path_params_nested) {
    CompiledRoutePattern pattern("/users/{user_id}/posts");
    auto params = pattern.extract("/users/42/posts");

    ASSERT_EQ(params.size(), 1);
    ASSERT_STR_EQ(params["user_id"], "42");
}

TEST(path_params_multiple) {
    CompiledRoutePattern pattern("/a/{b}/c/{d}");
    auto params = pattern.extract("/a/123/c/456");

    ASSERT_EQ(params.size(), 2);
    ASSERT_STR_EQ(params["b"], "123");
    ASSERT_STR_EQ(params["d"], "456");
}

TEST(path_params_no_match) {
    CompiledRoutePattern pattern("/items/{item_id}");
    auto params = pattern.extract("/users/123");

    ASSERT_EQ(params.size(), 0);  // No match returns empty map
}

TEST(path_params_real_world_item) {
    CompiledRoutePattern pattern("/items/{item_id}");
    auto params = pattern.extract("/items/12345");

    std::cout << "Extracted: ";
    print_params(params);
    std::cout << " ";

    ASSERT_EQ(params.size(), 1);
    ASSERT_STR_EQ(params["item_id"], "12345");
}

TEST(path_params_real_world_user_posts) {
    CompiledRoutePattern pattern("/users/{user_id}/posts");
    auto params = pattern.extract("/users/88/posts");

    std::cout << "Extracted: ";
    print_params(params);
    std::cout << " ";

    ASSERT_EQ(params.size(), 1);
    ASSERT_STR_EQ(params["user_id"], "88");
}

// ============================================================================
// Integration Tests
// ============================================================================

TEST(integration_path_and_query_split) {
    // Simulate: GET /users/42/posts?page=5&size=10
    std::string full_url = "/users/42/posts?page=5&size=10";

    // Split into path and query
    std::string route_path = full_url;
    size_t query_pos = full_url.find('?');
    if (query_pos != std::string::npos) {
        route_path = full_url.substr(0, query_pos);
    }

    std::cout << "Route path: " << route_path << " ";

    // Extract path params
    CompiledRoutePattern pattern("/users/{user_id}/posts");
    auto path_params = pattern.extract(route_path);

    // Extract query params
    auto query_params = ParameterExtractor::get_query_params(full_url);

    std::cout << "Path: ";
    print_params(path_params);
    std::cout << " Query: ";
    print_params(query_params);
    std::cout << " ";

    // Verify path params
    ASSERT_EQ(path_params.size(), 1);
    ASSERT_STR_EQ(path_params["user_id"], "42");

    // Verify query params
    ASSERT_EQ(query_params.size(), 2);
    ASSERT_STR_EQ(query_params["page"], "5");
    ASSERT_STR_EQ(query_params["size"], "10");
}

TEST(integration_search_query_only) {
    // Simulate: GET /search?q=fastapi&limit=33
    std::string full_url = "/search?q=fastapi&limit=33";

    // Split into path and query
    std::string route_path = full_url;
    size_t query_pos = full_url.find('?');
    if (query_pos != std::string::npos) {
        route_path = full_url.substr(0, query_pos);
    }

    std::cout << "Route path: " << route_path << " ";

    // Extract query params
    auto query_params = ParameterExtractor::get_query_params(full_url);

    std::cout << "Query: ";
    print_params(query_params);
    std::cout << " ";

    // Verify query params
    ASSERT_EQ(query_params.size(), 2);
    ASSERT_STR_EQ(query_params["q"], "fastapi");
    ASSERT_STR_EQ(query_params["limit"], "33");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "Parameter Extractor Tests\n";
    std::cout << "========================================\n\n";

    // Tests run automatically via static initialization

    std::cout << "\n========================================\n";
    std::cout << "Results: " << tests_passed << " passed, " << tests_failed << " failed\n";
    std::cout << "========================================\n";

    return tests_failed > 0 ? 1 : 0;
}
