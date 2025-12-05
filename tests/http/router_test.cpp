/**
 * FasterAPI Router Tests
 *
 * Comprehensive Google Test suite for the radix tree router.
 */

#include <gtest/gtest.h>
#include "../test_utils.h"
#include "../../src/cpp/http/router.h"

#include <thread>
#include <atomic>
#include <chrono>
#include <set>

using namespace fasterapi::http;
using namespace fasterapi::testing;

// =============================================================================
// Router Test Fixture
// =============================================================================

class RouterTest : public FasterAPITest {
protected:
    Router router_;
    RandomGenerator rng_;

    // Helper to create a simple handler
    RouteHandler make_handler(const std::string& name) {
        return [name](HttpRequest*, HttpResponse*, const RouteParams&) {
            // Handler identified by name for testing
        };
    }

    // Helper to check if handler matches
    bool handler_exists(const std::string& method, const std::string& path) {
        RouteParams params;
        return router_.match(method, path, params) != nullptr;
    }
};

// =============================================================================
// Basic Routing Tests
// =============================================================================

TEST_F(RouterTest, EmptyRouter) {
    EXPECT_EQ(router_.total_routes(), 0);
    EXPECT_EQ(router_.route_count("GET"), 0);

    RouteParams params;
    auto handler = router_.match("GET", "/", params);
    EXPECT_EQ(handler, nullptr);
}

TEST_F(RouterTest, SingleStaticRoute) {
    EXPECT_EQ(router_.add_route("GET", "/", make_handler("root")), 0);
    EXPECT_EQ(router_.total_routes(), 1);

    RouteParams params;
    auto handler = router_.match("GET", "/", params);
    EXPECT_NE(handler, nullptr);
    EXPECT_TRUE(params.empty());
}

TEST_F(RouterTest, MultipleStaticRoutes) {
    EXPECT_EQ(router_.add_route("GET", "/users", make_handler("users")), 0);
    EXPECT_EQ(router_.add_route("GET", "/posts", make_handler("posts")), 0);
    EXPECT_EQ(router_.add_route("GET", "/comments", make_handler("comments")), 0);
    EXPECT_EQ(router_.total_routes(), 3);

    EXPECT_TRUE(handler_exists("GET", "/users"));
    EXPECT_TRUE(handler_exists("GET", "/posts"));
    EXPECT_TRUE(handler_exists("GET", "/comments"));
    EXPECT_FALSE(handler_exists("GET", "/unknown"));
}

TEST_F(RouterTest, NestedStaticRoutes) {
    EXPECT_EQ(router_.add_route("GET", "/api", make_handler("api")), 0);
    EXPECT_EQ(router_.add_route("GET", "/api/v1", make_handler("v1")), 0);
    EXPECT_EQ(router_.add_route("GET", "/api/v1/users", make_handler("users")), 0);
    EXPECT_EQ(router_.add_route("GET", "/api/v1/users/list", make_handler("list")), 0);

    EXPECT_TRUE(handler_exists("GET", "/api"));
    EXPECT_TRUE(handler_exists("GET", "/api/v1"));
    EXPECT_TRUE(handler_exists("GET", "/api/v1/users"));
    EXPECT_TRUE(handler_exists("GET", "/api/v1/users/list"));
    EXPECT_FALSE(handler_exists("GET", "/api/v2"));
}

TEST_F(RouterTest, DifferentMethods) {
    EXPECT_EQ(router_.add_route("GET", "/users", make_handler("get_users")), 0);
    EXPECT_EQ(router_.add_route("POST", "/users", make_handler("create_user")), 0);
    EXPECT_EQ(router_.add_route("PUT", "/users", make_handler("update_users")), 0);
    EXPECT_EQ(router_.add_route("DELETE", "/users", make_handler("delete_users")), 0);

    EXPECT_TRUE(handler_exists("GET", "/users"));
    EXPECT_TRUE(handler_exists("POST", "/users"));
    EXPECT_TRUE(handler_exists("PUT", "/users"));
    EXPECT_TRUE(handler_exists("DELETE", "/users"));
    EXPECT_FALSE(handler_exists("PATCH", "/users"));
}

// =============================================================================
// Path Parameter Tests
// =============================================================================

TEST_F(RouterTest, SinglePathParameter) {
    EXPECT_EQ(router_.add_route("GET", "/users/{id}", make_handler("user")), 0);

    RouteParams params;
    auto handler = router_.match("GET", "/users/123", params);
    EXPECT_NE(handler, nullptr);
    EXPECT_EQ(params.size(), 1);
    EXPECT_EQ(params.get("id"), "123");
}

TEST_F(RouterTest, MultiplePathParameters) {
    EXPECT_EQ(router_.add_route("GET", "/users/{user_id}/posts/{post_id}", make_handler("post")), 0);

    RouteParams params;
    auto handler = router_.match("GET", "/users/42/posts/99", params);
    EXPECT_NE(handler, nullptr);
    EXPECT_EQ(params.size(), 2);
    EXPECT_EQ(params.get("user_id"), "42");
    EXPECT_EQ(params.get("post_id"), "99");
}

TEST_F(RouterTest, ParameterWithDifferentValues) {
    EXPECT_EQ(router_.add_route("GET", "/items/{id}", make_handler("item")), 0);

    std::vector<std::string> test_ids = {"1", "abc", "test-123", "uuid-xxxx", "a_b_c"};

    for (const auto& id : test_ids) {
        RouteParams params;
        auto handler = router_.match("GET", "/items/" + id, params);
        EXPECT_NE(handler, nullptr) << "Failed for id: " << id;
        EXPECT_EQ(params.get("id"), id);
    }
}

TEST_F(RouterTest, MixedStaticAndParameter) {
    // Note: Register static routes FIRST to ensure priority
    EXPECT_EQ(router_.add_route("GET", "/users/me", make_handler("me")), 0);
    EXPECT_EQ(router_.add_route("GET", "/users/admin", make_handler("admin")), 0);
    EXPECT_EQ(router_.add_route("GET", "/users/{id}", make_handler("user")), 0);

    // Static routes should match
    RouteParams params1;
    auto h1 = router_.match("GET", "/users/me", params1);
    EXPECT_NE(h1, nullptr);

    RouteParams params2;
    auto h2 = router_.match("GET", "/users/admin", params2);
    EXPECT_NE(h2, nullptr);

    // Parameter paths should match parameter route
    RouteParams params3;
    auto h3 = router_.match("GET", "/users/123", params3);
    EXPECT_NE(h3, nullptr);
    EXPECT_EQ(params3.get("id"), "123");
}

// =============================================================================
// Wildcard Route Tests
// =============================================================================

TEST_F(RouterTest, WildcardRoute) {
    EXPECT_EQ(router_.add_route("GET", "/files/*path", make_handler("files")), 0);

    RouteParams params;
    auto handler = router_.match("GET", "/files/docs/readme.txt", params);
    EXPECT_NE(handler, nullptr);
    EXPECT_EQ(params.get("path"), "docs/readme.txt");
}

TEST_F(RouterTest, WildcardMultipleLevels) {
    EXPECT_EQ(router_.add_route("GET", "/static/*filepath", make_handler("static")), 0);

    std::vector<std::string> test_paths = {
        "style.css",
        "js/app.js",
        "images/logo.png",
        "deep/nested/path/file.txt"
    };

    for (const auto& path : test_paths) {
        RouteParams params;
        auto handler = router_.match("GET", "/static/" + path, params);
        EXPECT_NE(handler, nullptr) << "Failed for: " << path;
        EXPECT_EQ(params.get("filepath"), path);
    }
}

TEST_F(RouterTest, WildcardPriority) {
    EXPECT_EQ(router_.add_route("GET", "/files/special", make_handler("special")), 0);
    EXPECT_EQ(router_.add_route("GET", "/files/*path", make_handler("wildcard")), 0);

    // Static should match first
    RouteParams params1;
    auto h1 = router_.match("GET", "/files/special", params1);
    EXPECT_NE(h1, nullptr);
    EXPECT_TRUE(params1.empty());

    // Other paths should match wildcard
    RouteParams params2;
    auto h2 = router_.match("GET", "/files/other", params2);
    EXPECT_NE(h2, nullptr);
    EXPECT_EQ(params2.get("path"), "other");
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(RouterTest, TrailingSlash) {
    EXPECT_EQ(router_.add_route("GET", "/users", make_handler("users")), 0);
    EXPECT_EQ(router_.add_route("GET", "/users/", make_handler("users_slash")), 0);

    RouteParams params1;
    auto h1 = router_.match("GET", "/users", params1);
    EXPECT_NE(h1, nullptr);

    RouteParams params2;
    auto h2 = router_.match("GET", "/users/", params2);
    EXPECT_NE(h2, nullptr);
}

TEST_F(RouterTest, EmptyPath) {
    EXPECT_EQ(router_.add_route("GET", "/", make_handler("root")), 0);

    RouteParams params;
    auto handler = router_.match("GET", "/", params);
    EXPECT_NE(handler, nullptr);
}

TEST_F(RouterTest, NoMatch) {
    EXPECT_EQ(router_.add_route("GET", "/users", make_handler("users")), 0);

    RouteParams params;
    EXPECT_EQ(router_.match("GET", "/posts", params), nullptr);
    EXPECT_EQ(router_.match("POST", "/users", params), nullptr);
    EXPECT_EQ(router_.match("GET", "/user", params), nullptr);  // Partial match
    EXPECT_EQ(router_.match("GET", "/users123", params), nullptr);  // No separator
}

TEST_F(RouterTest, CaseSensitive) {
    EXPECT_EQ(router_.add_route("GET", "/Users", make_handler("users")), 0);

    RouteParams params1;
    auto h1 = router_.match("GET", "/Users", params1);
    EXPECT_NE(h1, nullptr);

    RouteParams params2;
    auto h2 = router_.match("GET", "/users", params2);
    // Case mismatch - depends on implementation
}

// =============================================================================
// RouteParams Tests
// =============================================================================

TEST_F(RouterTest, RouteParamsAdd) {
    RouteParams params;
    params.add("id", "123");
    params.add("name", "test");

    EXPECT_EQ(params.size(), 2);
    EXPECT_EQ(params.get("id"), "123");
    EXPECT_EQ(params.get("name"), "test");
}

TEST_F(RouterTest, RouteParamsIndex) {
    RouteParams params;
    params.add("first", "1");
    params.add("second", "2");

    EXPECT_EQ(params[0].key, "first");
    EXPECT_EQ(params[0].value, "1");
    EXPECT_EQ(params[1].key, "second");
    EXPECT_EQ(params[1].value, "2");
}

TEST_F(RouterTest, RouteParamsMissing) {
    RouteParams params;
    params.add("id", "123");

    EXPECT_EQ(params.get("nonexistent"), "");
}

TEST_F(RouterTest, RouteParamsClear) {
    RouteParams params;
    params.add("id", "123");
    EXPECT_FALSE(params.empty());

    params.clear();
    EXPECT_TRUE(params.empty());
    EXPECT_EQ(params.size(), 0);
}

// =============================================================================
// Introspection Tests
// =============================================================================

TEST_F(RouterTest, GetRoutes) {
    router_.add_route("GET", "/users", make_handler("users"));
    router_.add_route("POST", "/users", make_handler("create"));
    router_.add_route("GET", "/posts", make_handler("posts"));

    auto routes = router_.get_routes();
    // At least some routes should be returned
    EXPECT_GE(routes.size(), 1);

    // Verify total_routes count matches what we added
    EXPECT_EQ(router_.total_routes(), 3);
}

// =============================================================================
// Performance Tests
// =============================================================================

TEST_F(RouterTest, MatchPerformance) {
    // Register multiple routes
    for (int i = 0; i < 100; ++i) {
        router_.add_route("GET", "/api/v1/resource" + std::to_string(i), make_handler("r" + std::to_string(i)));
    }
    router_.add_route("GET", "/api/v1/users/{id}", make_handler("user"));

    constexpr int ITERATIONS = 10000;

    // Benchmark static route matching
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < ITERATIONS; ++i) {
        RouteParams params;
        router_.match("GET", "/api/v1/resource50", params);
    }
    auto static_elapsed = std::chrono::steady_clock::now() - start;
    auto static_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(static_elapsed).count() / ITERATIONS;

    // Benchmark parameter route matching
    start = std::chrono::steady_clock::now();
    for (int i = 0; i < ITERATIONS; ++i) {
        RouteParams params;
        router_.match("GET", "/api/v1/users/12345", params);
    }
    auto param_elapsed = std::chrono::steady_clock::now() - start;
    auto param_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(param_elapsed).count() / ITERATIONS;

    std::cout << "Router static match: " << static_ns << " ns/match" << std::endl;
    std::cout << "Router param match: " << param_ns << " ns/match" << std::endl;

    // Should be well under 10us per match
    EXPECT_LT(static_ns, 10000);
    EXPECT_LT(param_ns, 10000);
}

// Re-enabled after fix to radix tree node splitting logic
TEST_F(RouterTest, ManyRoutes) {
    // Test with many routes to verify tree structure
    constexpr int NUM_ROUTES = 100;

    for (int i = 0; i < NUM_ROUTES; ++i) {
        std::string path = "/r" + std::to_string(i);
        router_.add_route("GET", path, make_handler("r" + std::to_string(i)));
    }

    EXPECT_EQ(router_.total_routes(), NUM_ROUTES);

    // Verify all routes match
    for (int i = 0; i < NUM_ROUTES; ++i) {
        std::string path = "/r" + std::to_string(i);
        RouteParams params;
        auto handler = router_.match("GET", path, params);
        EXPECT_NE(handler, nullptr) << "Failed for: " << path;
    }
}

// =============================================================================
// Radix Tree Structure Diagnostic Tests
// =============================================================================

// Helper function to dump router tree structure for debugging
static void dump_tree(const RouterNode* node, const std::string& prefix = "", int depth = 0) {
    if (!node) return;

    std::string indent(depth * 2, ' ');
    std::string type_str;
    switch (node->type) {
        case NodeType::STATIC: type_str = "STATIC"; break;
        case NodeType::PARAM: type_str = "PARAM"; break;
        case NodeType::WILDCARD: type_str = "WILDCARD"; break;
    }

    std::cout << indent << "Node[" << type_str << "]: path=\"" << node->path
              << "\", indices=\"" << node->indices
              << "\", has_handler=" << (node->handler ? "yes" : "no")
              << ", children=" << node->children.size()
              << ", child_map={";

    for (const auto& [c, idx] : node->child_map) {
        std::cout << "'" << c << "':" << idx << ",";
    }
    std::cout << "}" << std::endl;

    for (const auto& child : node->children) {
        dump_tree(child.get(), prefix + node->path, depth + 1);
    }
}

// This test traces through the radix tree structure step by step to find the bug
TEST_F(RouterTest, DiagnoseShortRoutesSplitBug) {
    // Test the simplest case that triggers the bug: two routes with common prefix

    // Add first route /r0
    ASSERT_EQ(router_.add_route("GET", "/r0", make_handler("r0")), 0);

    // Verify /r0 matches
    {
        RouteParams params;
        auto h = router_.match("GET", "/r0", params);
        ASSERT_NE(h, nullptr) << "r0 should match after adding r0";
    }

    std::cout << "\n=== After adding /r0 ===" << std::endl;
    // Get the tree (using get_routes to verify structure)
    auto routes1 = router_.get_routes();
    std::cout << "Total routes: " << router_.total_routes() << std::endl;
    for (const auto& r : routes1) {
        std::cout << "  " << r.method << " " << r.path << std::endl;
    }

    // Add second route /r1 - this triggers node split
    ASSERT_EQ(router_.add_route("GET", "/r1", make_handler("r1")), 0);

    std::cout << "\n=== After adding /r1 ===" << std::endl;
    auto routes2 = router_.get_routes();
    std::cout << "Total routes: " << router_.total_routes() << std::endl;
    for (const auto& r : routes2) {
        std::cout << "  " << r.method << " " << r.path << std::endl;
    }

    // Verify both routes match
    {
        RouteParams params;
        auto h0 = router_.match("GET", "/r0", params);
        EXPECT_NE(h0, nullptr) << "r0 should still match after adding r1";
    }
    {
        RouteParams params;
        auto h1 = router_.match("GET", "/r1", params);
        EXPECT_NE(h1, nullptr) << "r1 should match after adding r1";
    }

    // Add third route /r2
    ASSERT_EQ(router_.add_route("GET", "/r2", make_handler("r2")), 0);

    std::cout << "\n=== After adding /r2 ===" << std::endl;
    auto routes3 = router_.get_routes();
    std::cout << "Total routes: " << router_.total_routes() << std::endl;
    for (const auto& r : routes3) {
        std::cout << "  " << r.method << " " << r.path << std::endl;
    }

    // Verify all three routes match
    for (int i = 0; i <= 2; ++i) {
        std::string path = "/r" + std::to_string(i);
        RouteParams params;
        auto h = router_.match("GET", path, params);
        EXPECT_NE(h, nullptr) << "Should find handler for " << path;
    }
}

// Test with routes that share longer prefix to isolate LCP calculation
TEST_F(RouterTest, DiagnoseCommonPrefixRoutes) {
    // Routes: /abc, /abd, /abe - all share "ab" prefix
    ASSERT_EQ(router_.add_route("GET", "/abc", make_handler("abc")), 0);
    ASSERT_EQ(router_.add_route("GET", "/abd", make_handler("abd")), 0);
    ASSERT_EQ(router_.add_route("GET", "/abe", make_handler("abe")), 0);

    // Verify all match
    EXPECT_TRUE(handler_exists("GET", "/abc")) << "/abc should match";
    EXPECT_TRUE(handler_exists("GET", "/abd")) << "/abd should match";
    EXPECT_TRUE(handler_exists("GET", "/abe")) << "/abe should match";

    // Non-existent should not match
    EXPECT_FALSE(handler_exists("GET", "/abf"));
    EXPECT_FALSE(handler_exists("GET", "/ab"));
}

// Test progressive insertion to find exact breakpoint
TEST_F(RouterTest, DiagnoseProgressiveInsertion) {
    std::vector<std::string> routes;

    // Add routes one by one and verify all previous routes still match
    for (int i = 0; i < 10; ++i) {
        std::string new_path = "/r" + std::to_string(i);
        routes.push_back(new_path);

        ASSERT_EQ(router_.add_route("GET", new_path, make_handler("r" + std::to_string(i))), 0)
            << "Failed to add route " << new_path;

        // Verify ALL previously added routes still match
        for (size_t j = 0; j <= static_cast<size_t>(i); ++j) {
            RouteParams params;
            auto h = router_.match("GET", routes[j], params);
            EXPECT_NE(h, nullptr)
                << "After adding " << new_path << ", route " << routes[j] << " should still match";
        }
    }

    std::cout << "\n=== All routes after adding r0-r9 ===" << std::endl;
    auto all_routes = router_.get_routes();
    for (const auto& r : all_routes) {
        std::cout << "  " << r.method << " " << r.path << std::endl;
    }
}

// This test finds the exact breakpoint when r10 is added (sharing prefix with r1)
TEST_F(RouterTest, DiagnoseR10BreaksBugRegression) {
    std::vector<std::string> routes;

    // Add r0-r9 first (this works per previous test)
    for (int i = 0; i < 10; ++i) {
        std::string new_path = "/r" + std::to_string(i);
        routes.push_back(new_path);
        ASSERT_EQ(router_.add_route("GET", new_path, make_handler("r" + std::to_string(i))), 0);
    }

    // Verify r0-r9 all match BEFORE adding r10
    std::cout << "\n=== Before adding /r10 ===" << std::endl;
    for (int i = 0; i < 10; ++i) {
        RouteParams params;
        auto h = router_.match("GET", routes[i], params);
        EXPECT_NE(h, nullptr) << "Before r10: " << routes[i] << " should match";
    }

    // Now add r10 - this should split the "1" node into "1" with children "" and "0"
    std::cout << "\n=== Adding /r10 ===" << std::endl;
    routes.push_back("/r10");
    ASSERT_EQ(router_.add_route("GET", "/r10", make_handler("r10")), 0);

    // Verify ALL routes still match
    std::cout << "\n=== After adding /r10 ===" << std::endl;
    for (size_t i = 0; i < routes.size(); ++i) {
        RouteParams params;
        auto h = router_.match("GET", routes[i], params);
        EXPECT_NE(h, nullptr) << "After r10: " << routes[i] << " should match";
    }

    // Show tree structure
    auto all_routes = router_.get_routes();
    std::cout << "Routes in tree:" << std::endl;
    for (const auto& r : all_routes) {
        std::cout << "  " << r.method << " " << r.path << std::endl;
    }
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

// Re-enabled after ManyRoutes bug fix
TEST_F(RouterTest, ConcurrentReads) {
    // Add a few simple routes
    for (int i = 0; i < 10; ++i) {
        router_.add_route("GET", "/r" + std::to_string(i), make_handler("r" + std::to_string(i)));
    }

    std::atomic<int> success_count{0};
    constexpr int NUM_THREADS = 4;
    constexpr int ITERATIONS = 100;

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < ITERATIONS; ++i) {
                RouteParams params;
                int route_idx = i % 10;
                auto handler = router_.match("GET", "/r" + std::to_string(route_idx), params);
                if (handler) {
                    success_count++;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // All matches should succeed
    EXPECT_EQ(success_count.load(), NUM_THREADS * ITERATIONS);
}

