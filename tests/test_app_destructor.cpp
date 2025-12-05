/**
 * @file test_app_destructor.cpp
 * @brief Unit tests for App destructor memory safety
 *
 * These tests verify that the App destructor correctly handles cleanup
 * without crashes, double-frees, or use-after-free errors.
 *
 * Test Coverage:
 * - Basic destruction without routes
 * - Destruction with single route
 * - Destruction after calling routes()
 * - Destruction with multiple routes
 * - Destruction with handlers capturing references
 * - Stress test with many routes
 */

#include "../src/cpp/http/app.h"

#include <iostream>
#include <chrono>
#include <random>
#include <atomic>

using namespace fasterapi;

// =============================================================================
// Test Infrastructure (exception-free)
// =============================================================================

static int tests_passed = 0;
static int tests_failed = 0;
static bool current_test_failed = false;
static std::string current_test_error;

struct TestResult {
    std::string name;
    bool passed;
    std::string error;
    int64_t duration_us;
};

std::vector<TestResult> test_results;

#define ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            current_test_failed = true; \
            current_test_error = std::string("Assertion failed: ") + message; \
            return; \
        } \
    } while(0)

#define RUN_TEST(test_func) \
    do { \
        auto start = std::chrono::steady_clock::now(); \
        TestResult result; \
        result.name = #test_func; \
        current_test_failed = false; \
        current_test_error = ""; \
        test_func(); \
        if (current_test_failed) { \
            result.passed = false; \
            result.error = current_test_error; \
            tests_failed++; \
        } else { \
            result.passed = true; \
            tests_passed++; \
        } \
        auto end = std::chrono::steady_clock::now(); \
        result.duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count(); \
        test_results.push_back(result); \
    } while(0)

// =============================================================================
// Test Cases
// =============================================================================

void test_basic_destruction_no_routes() {
    std::cout << "  Testing basic destruction without routes..." << std::endl;

    App::Config config;
    config.enable_docs = false;

    {
        App app(config);
        // No routes registered
    }  // App destroyed here - should not crash

    // If we get here, destruction succeeded
    ASSERT(true, "Basic destruction should succeed");
}

void test_destruction_with_single_route() {
    std::cout << "  Testing destruction with single route..." << std::endl;

    App::Config config;
    config.enable_docs = false;

    {
        App app(config);
        app.get("/", [](Request& req, Response& res) {
            res.json({{"message", "root"}});
        });
    }  // App destroyed here - should not crash

    ASSERT(true, "Destruction with single route should succeed");
}

void test_destruction_after_calling_routes() {
    std::cout << "  Testing destruction after calling routes()..." << std::endl;

    App::Config config;
    config.enable_docs = false;

    {
        App app(config);
        app.get("/", [](Request& req, Response& res) {
            res.json({{"message", "root"}});
        });

        // This used to trigger the crash
        auto routes = app.routes();
        ASSERT(routes.size() == 1, "Should have 1 route");
    }  // App destroyed here - should not crash

    ASSERT(true, "Destruction after routes() should succeed");
}

void test_destruction_with_multiple_routes() {
    std::cout << "  Testing destruction with multiple routes..." << std::endl;

    App::Config config;
    config.enable_docs = false;

    {
        App app(config);

        app.get("/", [](Request& req, Response& res) {
            res.json({{"message", "root"}});
        });

        app.post("/users", [](Request& req, Response& res) {
            res.json({{"action", "create"}});
        });

        app.get("/users/{id}", [](Request& req, Response& res) {
            auto id = req.path_param("id");
            res.json({{"id", id}});
        });

        app.put("/users/{id}", [](Request& req, Response& res) {
            auto id = req.path_param("id");
            res.json({{"action", "update"}, {"id", id}});
        });

        app.del("/users/{id}", [](Request& req, Response& res) {
            auto id = req.path_param("id");
            res.json({{"action", "delete"}, {"id", id}});
        });

        auto routes = app.routes();
        ASSERT(routes.size() == 5, "Should have 5 routes");
    }  // App destroyed here - should not crash

    ASSERT(true, "Destruction with multiple routes should succeed");
}

void test_destruction_with_http3_enabled() {
    std::cout << "  Testing destruction with HTTP/3 enabled..." << std::endl;

    App::Config config;
    config.enable_http3 = true;
    config.enable_docs = false;
    config.http3_port = 9443;

    {
        App app(config);
        app.get("/test", [](Request& req, Response& res) {
            res.json({{"test", "value"}});
        });

        auto routes = app.routes();
        ASSERT(routes.size() == 1, "Should have 1 route");
    }  // App destroyed here - should not crash

    ASSERT(true, "Destruction with HTTP/3 should succeed");
}

void test_destruction_stress_many_routes() {
    std::cout << "  Testing destruction stress test (100 routes)..." << std::endl;

    App::Config config;
    config.enable_docs = false;

    {
        App app(config);

        // Register 100 routes with randomized data
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(1000, 9999);

        for (int i = 0; i < 100; i++) {
            int random_val = dis(gen);
            std::string path = "/route" + std::to_string(i);

            app.get(path, [i, random_val](Request& req, Response& res) {
                res.json({{"index", std::to_string(i)}, {"value", std::to_string(random_val)}});
            });
        }

        auto routes = app.routes();
        ASSERT(routes.size() == 100, "Should have 100 routes");
    }  // App destroyed here - should not crash

    ASSERT(true, "Destruction with 100 routes should succeed");
}

void test_destruction_multiple_apps_lifecycle() {
    std::cout << "  Testing multiple App creation/destruction cycles..." << std::endl;

    App::Config config;
    config.enable_docs = false;

    // Create and destroy app 10 times
    for (int cycle = 0; cycle < 10; cycle++) {
        App app(config);

        app.get("/cycle" + std::to_string(cycle), [cycle](Request& req, Response& res) {
            res.json({{"cycle", std::to_string(cycle)}});
        });

        auto routes = app.routes();
        ASSERT(routes.size() == 1, "Should have 1 route in cycle " + std::to_string(cycle));

        // App destroyed at end of loop iteration
    }

    ASSERT(true, "Multiple App lifecycle should succeed");
}

void test_destruction_with_all_http_methods() {
    std::cout << "  Testing destruction with all HTTP methods..." << std::endl;

    App::Config config;
    config.enable_docs = false;

    {
        App app(config);

        app.get("/resource", [](Request& req, Response& res) {
            res.json({{"method", "GET"}});
        });

        app.post("/resource", [](Request& req, Response& res) {
            res.json({{"method", "POST"}});
        });

        app.put("/resource", [](Request& req, Response& res) {
            res.json({{"method", "PUT"}});
        });

        app.del("/resource", [](Request& req, Response& res) {
            res.json({{"method", "DELETE"}});
        });

        app.patch("/resource", [](Request& req, Response& res) {
            res.json({{"method", "PATCH"}});
        });

        app.head("/resource", [](Request& req, Response& res) {
            res.status(200);
        });

        app.options("/resource", [](Request& req, Response& res) {
            res.json({{"method", "OPTIONS"}});
        });

        auto routes = app.routes();
        ASSERT(routes.size() == 7, "Should have 7 routes");
    }  // App destroyed here - should not crash

    ASSERT(true, "Destruction with all HTTP methods should succeed");
}

void test_destruction_with_parameterized_routes() {
    std::cout << "  Testing destruction with parameterized routes..." << std::endl;

    App::Config config;
    config.enable_docs = false;

    {
        App app(config);

        app.get("/users/{user_id}", [](Request& req, Response& res) {
            res.json({{"user_id", req.path_param("user_id")}});
        });

        app.get("/users/{user_id}/posts/{post_id}", [](Request& req, Response& res) {
            res.json({
                {"user_id", req.path_param("user_id")},
                {"post_id", req.path_param("post_id")}
            });
        });

        app.get("/files/*path", [](Request& req, Response& res) {
            res.json({{"path", req.path_param("path")}});
        });

        auto routes = app.routes();
        ASSERT(routes.size() == 3, "Should have 3 routes");
    }  // App destroyed here - should not crash

    ASSERT(true, "Destruction with parameterized routes should succeed");
}

void test_destruction_with_multi_protocol() {
    std::cout << "  Testing destruction with multi-protocol config..." << std::endl;

    App::Config config;
    config.enable_http2 = true;
    config.enable_http3 = true;
    config.enable_docs = false;
    config.http3_port = 9443;

    {
        App app(config);

        app.get("/test", [](Request& req, Response& res) {
            res.json({{"multi_protocol", "true"}});
        });

        auto routes = app.routes();
        ASSERT(routes.size() == 1, "Should have 1 route");
    }  // App destroyed here - should not crash

    ASSERT(true, "Destruction with multi-protocol should succeed");
}

// =============================================================================
// Main Test Runner
// =============================================================================

int main(int argc, char** argv) {
    std::cout << "========================================" << std::endl;
    std::cout << "FasterAPI App Destructor Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    std::cout << "Running tests..." << std::endl;
    std::cout << std::endl;

    RUN_TEST(test_basic_destruction_no_routes);
    RUN_TEST(test_destruction_with_single_route);
    RUN_TEST(test_destruction_after_calling_routes);
    RUN_TEST(test_destruction_with_multiple_routes);
    RUN_TEST(test_destruction_with_http3_enabled);
    RUN_TEST(test_destruction_stress_many_routes);
    RUN_TEST(test_destruction_multiple_apps_lifecycle);
    RUN_TEST(test_destruction_with_all_http_methods);
    RUN_TEST(test_destruction_with_parameterized_routes);
    RUN_TEST(test_destruction_with_multi_protocol);

    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Test Results" << std::endl;
    std::cout << "========================================" << std::endl;

    for (const auto& result : test_results) {
        std::string status = result.passed ? "✅ PASS" : "❌ FAIL";
        std::cout << status << " " << result.name;
        std::cout << " (" << (result.duration_us / 1000.0) << " ms)";

        if (!result.passed) {
            std::cout << std::endl;
            std::cout << "    Error: " << result.error;
        }
        std::cout << std::endl;
    }

    std::cout << std::endl;
    std::cout << "Summary:" << std::endl;
    int total_tests = tests_passed + tests_failed;
    std::cout << "  Total:  " << total_tests << std::endl;
    std::cout << "  Passed: " << tests_passed << " ✅" << std::endl;
    std::cout << "  Failed: " << tests_failed << " ❌" << std::endl;
    std::cout << "  Rate:   " << (tests_passed * 100 / std::max(1, total_tests)) << "%" << std::endl;

    return (tests_failed == 0) ? 0 : 1;
}
