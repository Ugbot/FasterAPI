/**
 * Router Performance Benchmarks
 * 
 * Measures route matching performance for:
 * - Static routes
 * - Parameterized routes
 * - Wildcard routes
 * - Mixed workloads
 */

#include "../src/cpp/http/router.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <random>
#include <algorithm>

using namespace fasterapi::http;
using namespace std::chrono;

// Benchmark runner
template<typename Func>
double benchmark(const char* name, Func&& func, int iterations = 1000000) {
    auto start = high_resolution_clock::now();
    
    for (int i = 0; i < iterations; ++i) {
        func();
    }
    
    auto end = high_resolution_clock::now();
    auto duration = duration_cast<nanoseconds>(end - start).count();
    double ns_per_op = static_cast<double>(duration) / iterations;
    
    std::cout << name << ": " << ns_per_op << " ns/op  ("
              << iterations << " iterations)" << std::endl;
    
    return ns_per_op;
}

// Dummy handler
static RouteHandler dummy_handler = [](HttpRequest*, HttpResponse*, const RouteParams&) {};

int main() {
    std::cout << "╔══════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║          Router Performance Benchmarks                  ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;
    
    // ========================================================================
    // Benchmark 1: Static Route Matching
    // ========================================================================
    
    std::cout << "=== Static Route Matching ===" << std::endl;
    
    Router static_router;
    static_router.add_route("GET", "/", dummy_handler);
    static_router.add_route("GET", "/users", dummy_handler);
    static_router.add_route("GET", "/posts", dummy_handler);
    static_router.add_route("GET", "/api/v1/users", dummy_handler);
    static_router.add_route("GET", "/api/v1/posts", dummy_handler);
    
    RouteParams params;
    
    double t1 = benchmark("  Root path", [&]() {
        static_router.match("GET", "/", params);
        params.clear();
    });
    
    double t2 = benchmark("  Simple path", [&]() {
        static_router.match("GET", "/users", params);
        params.clear();
    });
    
    double t3 = benchmark("  Nested path", [&]() {
        static_router.match("GET", "/api/v1/users", params);
        params.clear();
    });
    
    double t4 = benchmark("  Not found", [&]() {
        static_router.match("GET", "/nonexistent", params);
        params.clear();
    });
    
    std::cout << std::endl;
    
    // ========================================================================
    // Benchmark 2: Parameterized Routes
    // ========================================================================
    
    std::cout << "=== Parameterized Route Matching ===" << std::endl;
    
    Router param_router;
    param_router.add_route("GET", "/users/{id}", dummy_handler);
    param_router.add_route("GET", "/users/{userId}/posts/{postId}", dummy_handler);
    param_router.add_route("GET", "/api/v1/users/{id}", dummy_handler);
    
    double t5 = benchmark("  Single param", [&]() {
        param_router.match("GET", "/users/123", params);
        params.clear();
    });
    
    double t6 = benchmark("  Multiple params", [&]() {
        param_router.match("GET", "/users/42/posts/100", params);
        params.clear();
    });
    
    double t7 = benchmark("  Nested param", [&]() {
        param_router.match("GET", "/api/v1/users/999", params);
        params.clear();
    });
    
    std::cout << std::endl;
    
    // ========================================================================
    // Benchmark 3: Mixed Routes (Realistic API)
    // ========================================================================
    
    std::cout << "=== Mixed Routes (Realistic API) ===" << std::endl;
    
    Router api_router;
    
    // Static routes
    api_router.add_route("GET", "/", dummy_handler);
    api_router.add_route("GET", "/health", dummy_handler);
    api_router.add_route("GET", "/metrics", dummy_handler);
    
    // API routes with params
    api_router.add_route("GET", "/api/v1/users", dummy_handler);
    api_router.add_route("GET", "/api/v1/users/{id}", dummy_handler);
    api_router.add_route("POST", "/api/v1/users", dummy_handler);
    api_router.add_route("PUT", "/api/v1/users/{id}", dummy_handler);
    api_router.add_route("DELETE", "/api/v1/users/{id}", dummy_handler);
    
    api_router.add_route("GET", "/api/v1/users/{userId}/posts", dummy_handler);
    api_router.add_route("GET", "/api/v1/users/{userId}/posts/{postId}", dummy_handler);
    api_router.add_route("POST", "/api/v1/users/{userId}/posts", dummy_handler);
    
    api_router.add_route("GET", "/api/v1/posts", dummy_handler);
    api_router.add_route("GET", "/api/v1/posts/{id}", dummy_handler);
    
    // Wildcard
    api_router.add_route("GET", "/static/*path", dummy_handler);
    
    std::cout << "  Registered routes: " << api_router.total_routes() << std::endl;
    std::cout << std::endl;
    
    double t8 = benchmark("  Static (hot path)", [&]() {
        api_router.match("GET", "/health", params);
        params.clear();
    });
    
    double t9 = benchmark("  Param (common)", [&]() {
        api_router.match("GET", "/api/v1/users/123", params);
        params.clear();
    });
    
    double t10 = benchmark("  Multi-param", [&]() {
        api_router.match("GET", "/api/v1/users/42/posts/100", params);
        params.clear();
    });
    
    double t11 = benchmark("  Wildcard", [&]() {
        api_router.match("GET", "/static/css/main.css", params);
        params.clear();
    });
    
    double t12 = benchmark("  Not found", [&]() {
        api_router.match("GET", "/api/v2/unknown", params);
        params.clear();
    });
    
    std::cout << std::endl;
    
    // ========================================================================
    // Benchmark 4: Large Route Table
    // ========================================================================
    
    std::cout << "=== Large Route Table (1000 routes) ===" << std::endl;
    
    Router large_router;
    
    // Add 1000 routes with various patterns
    for (int i = 0; i < 333; ++i) {
        large_router.add_route("GET", "/api/static/" + std::to_string(i), dummy_handler);
        large_router.add_route("GET", "/api/param/" + std::to_string(i) + "/{id}", dummy_handler);
        large_router.add_route("GET", "/api/multi/" + std::to_string(i) + "/{id1}/{id2}", dummy_handler);
    }
    
    large_router.add_route("GET", "/api/wildcard/*path", dummy_handler);
    
    std::cout << "  Registered routes: " << large_router.total_routes() << std::endl;
    std::cout << std::endl;
    
    double t13 = benchmark("  Match first route", [&]() {
        large_router.match("GET", "/api/static/0", params);
        params.clear();
    }, 100000);
    
    double t14 = benchmark("  Match middle route", [&]() {
        large_router.match("GET", "/api/param/166/xyz", params);
        params.clear();
    }, 100000);
    
    double t15 = benchmark("  Match last route", [&]() {
        large_router.match("GET", "/api/multi/332/a/b", params);
        params.clear();
    }, 100000);
    
    double t16 = benchmark("  Match wildcard", [&]() {
        large_router.match("GET", "/api/wildcard/deep/path/file.txt", params);
        params.clear();
    }, 100000);
    
    std::cout << std::endl;
    
    // ========================================================================
    // Benchmark 5: Route Registration
    // ========================================================================
    
    std::cout << "=== Route Registration ===" << std::endl;
    
    double t17 = benchmark("  Add static route", [&]() {
        Router r;
        r.add_route("GET", "/users", dummy_handler);
    }, 10000);
    
    double t18 = benchmark("  Add param route", [&]() {
        Router r;
        r.add_route("GET", "/users/{id}", dummy_handler);
    }, 10000);
    
    double t19 = benchmark("  Add wildcard route", [&]() {
        Router r;
        r.add_route("GET", "/files/*path", dummy_handler);
    }, 10000);
    
    std::cout << std::endl;
    
    // ========================================================================
    // Summary
    // ========================================================================
    
    std::cout << "=== Performance Summary ===" << std::endl;
    std::cout << std::endl;
    
    std::cout << "Fastest operations:" << std::endl;
    std::cout << "  Static route match:  " << t1 << " ns  (root path)" << std::endl;
    std::cout << "  Simple path match:   " << t2 << " ns  (/users)" << std::endl;
    std::cout << "  Nested path match:   " << t3 << " ns  (/api/v1/users)" << std::endl;
    std::cout << std::endl;
    
    std::cout << "Parameterized routes:" << std::endl;
    std::cout << "  Single param:        " << t5 << " ns" << std::endl;
    std::cout << "  Multiple params:     " << t6 << " ns" << std::endl;
    std::cout << "  Nested param:        " << t7 << " ns" << std::endl;
    std::cout << std::endl;
    
    std::cout << "Large route table (1000 routes):" << std::endl;
    std::cout << "  First route:         " << t13 << " ns" << std::endl;
    std::cout << "  Middle route:        " << t14 << " ns" << std::endl;
    std::cout << "  Last route:          " << t15 << " ns" << std::endl;
    std::cout << "  O(k) scaling:        ✅ " << (t15 / t13 < 3 ? "GOOD" : "NEEDS WORK") << std::endl;
    std::cout << std::endl;
    
    std::cout << "Route registration:" << std::endl;
    std::cout << "  Static:              " << t17 << " ns" << std::endl;
    std::cout << "  Param:               " << t18 << " ns" << std::endl;
    std::cout << "  Wildcard:            " << t19 << " ns" << std::endl;
    std::cout << std::endl;
    
    // Performance targets
    std::cout << "Performance Targets:" << std::endl;
    
    if (t2 < 100) {
        std::cout << "  ✅ Static match: " << t2 << " ns (target: <100ns)" << std::endl;
    } else {
        std::cout << "  ⚠️  Static match: " << t2 << " ns (target: <100ns)" << std::endl;
    }
    
    if (t5 < 200) {
        std::cout << "  ✅ Param match: " << t5 << " ns (target: <200ns)" << std::endl;
    } else {
        std::cout << "  ⚠️  Param match: " << t5 << " ns (target: <200ns)" << std::endl;
    }
    
    if (t15 / t13 < 3) {
        std::cout << "  ✅ O(k) scaling: Good (last/first = " << (t15/t13) << "x)" << std::endl;
    } else {
        std::cout << "  ⚠️  O(k) scaling: Needs work (last/first = " << (t15/t13) << "x)" << std::endl;
    }
    
    std::cout << std::endl;
    std::cout << "🎉 Benchmark complete!" << std::endl;
    
    return 0;
}

