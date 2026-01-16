/**
 * Unit tests for async coroutine route handlers.
 *
 * Tests:
 * - Async route registration (put_async, post_async, get_async, etc.)
 * - Async route matching with path parameters
 * - Async route dispatch and coroutine execution
 * - Mixed sync/async route handling
 *
 * Note: FasterAPI has exceptions disabled, so we use assert() instead.
 */

#include "../src/cpp/http/app.h"
#include "../src/cpp/core/coro_task.h"
#include <iostream>
#include <cassert>
#include <string>
#include <unordered_map>

using namespace fasterapi;
using namespace fasterapi::http;
using namespace fasterapi::core;

// Test runner
void run_tests() {
    std::cout << "=== Async Handler Tests ===\n\n";

    // Test 1: GET async route registration
    {
        std::cout << "Test 1: GET async route registration... ";
        App::Config config;
        config.pure_cpp_mode = true;
        App app(config);

        app.get_async("/test", [](Request& req, Response& res) -> coro_task<void> {
            res.status(200).json(R"({"result":"ok"})");
            co_return;
        });

        assert(app.has_async_route("GET", "/test"));
        std::cout << "PASSED\n";
    }

    // Test 2: POST async route registration
    {
        std::cout << "Test 2: POST async route registration... ";
        App::Config config;
        config.pure_cpp_mode = true;
        App app(config);

        app.post_async("/submit", [](Request& req, Response& res) -> coro_task<void> {
            res.status(201).json(R"({"created":true})");
            co_return;
        });

        assert(app.has_async_route("POST", "/submit"));
        std::cout << "PASSED\n";
    }

    // Test 3: PUT async route registration
    {
        std::cout << "Test 3: PUT async route registration... ";
        App::Config config;
        config.pure_cpp_mode = true;
        App app(config);

        app.put_async("/update", [](Request& req, Response& res) -> coro_task<void> {
            res.status(200).json(R"({"updated":true})");
            co_return;
        });

        assert(app.has_async_route("PUT", "/update"));
        std::cout << "PASSED\n";
    }

    // Test 4: DELETE async route registration
    {
        std::cout << "Test 4: DELETE async route registration... ";
        App::Config config;
        config.pure_cpp_mode = true;
        App app(config);

        app.del_async("/remove", [](Request& req, Response& res) -> coro_task<void> {
            res.status(200).json(R"({"deleted":true})");
            co_return;
        });

        assert(app.has_async_route("DELETE", "/remove"));
        std::cout << "PASSED\n";
    }

    // Test 5: PATCH async route registration
    {
        std::cout << "Test 5: PATCH async route registration... ";
        App::Config config;
        config.pure_cpp_mode = true;
        App app(config);

        app.patch_async("/partial", [](Request& req, Response& res) -> coro_task<void> {
            res.status(200).json(R"({"patched":true})");
            co_return;
        });

        assert(app.has_async_route("PATCH", "/partial"));
        std::cout << "PASSED\n";
    }

    // Test 6: Async route with path parameter
    {
        std::cout << "Test 6: Async route with path parameter... ";
        App::Config config;
        config.pure_cpp_mode = true;
        App app(config);

        app.get_async("/users/{id}", [](Request& req, Response& res) -> coro_task<void> {
            auto id = req.path_param("id");
            res.status(200).json("{\"user_id\":\"" + id + "\"}");
            co_return;
        });

        assert(app.has_async_route("GET", "/users/123"));
        assert(app.has_async_route("GET", "/users/abc"));
        std::cout << "PASSED\n";
    }

    // Test 7: Async route with multiple params
    {
        std::cout << "Test 7: Async route with multiple params... ";
        App::Config config;
        config.pure_cpp_mode = true;
        App app(config);

        app.get_async("/{index}/_doc/{id}", [](Request& req, Response& res) -> coro_task<void> {
            auto index = req.path_param("index");
            auto id = req.path_param("id");
            res.status(200).json("{\"index\":\"" + index + "\",\"id\":\"" + id + "\"}");
            co_return;
        });

        assert(app.has_async_route("GET", "/test-index/_doc/doc-123"));
        std::cout << "PASSED\n";
    }

    // Test 8: Async route dispatch
    {
        std::cout << "Test 8: Async route dispatch... ";
        App::Config config;
        config.pure_cpp_mode = true;
        App app(config);

        bool handler_called = false;

        app.get_async("/dispatch-test", [&handler_called](Request& req, Response& res) -> coro_task<void> {
            handler_called = true;
            res.status(200).json(R"({"dispatched":true})");
            co_return;
        });

        // Create mock request and response
        std::unordered_map<std::string, std::string> headers;
        HttpRequest http_req = HttpRequest::from_parsed_data("GET", "/dispatch-test", headers, "");
        HttpResponse http_res;
        RouteParams params;
        Request req(&http_req, params);
        Response res(&http_res);

        // Dispatch async route
        auto coro = app.dispatch_async(req, res);

        // Run coroutine to completion
        while (coro.resume()) {
            // Keep resuming until coroutine completes
        }

        assert(coro.done());
        assert(handler_called);
        std::cout << "PASSED\n";
    }

    // Test 9: Async route dispatch with params
    {
        std::cout << "Test 9: Async route dispatch with params... ";
        App::Config config;
        config.pure_cpp_mode = true;
        App app(config);

        std::string captured_id;

        app.get_async("/items/{id}", [&captured_id](Request& req, Response& res) -> coro_task<void> {
            captured_id = req.path_param("id");
            res.status(200).json("{\"id\":\"" + captured_id + "\"}");
            co_return;
        });

        // Create mock request and response
        std::unordered_map<std::string, std::string> headers;
        HttpRequest http_req = HttpRequest::from_parsed_data("GET", "/items/item-456", headers, "");
        HttpResponse http_res;
        RouteParams params;
        Request req(&http_req, params);
        Response res(&http_res);

        // Dispatch async route
        auto coro = app.dispatch_async(req, res);

        while (coro.resume()) {
            // Keep resuming
        }

        assert(coro.done());
        assert(captured_id == "item-456");
        std::cout << "PASSED\n";
    }

    // Test 10: Mixed sync and async routes
    {
        std::cout << "Test 10: Mixed sync and async routes... ";
        App::Config config;
        config.pure_cpp_mode = true;
        App app(config);

        // Register sync route
        app.get("/sync", [](Request& req, Response& res) {
            res.status(200).json(R"({"type":"sync"})");
        });

        // Register async route
        app.get_async("/async", [](Request& req, Response& res) -> coro_task<void> {
            res.status(200).json(R"({"type":"async"})");
            co_return;
        });

        // Sync route should not be found in async routes
        assert(!app.has_async_route("GET", "/sync"));

        // Async route should be found
        assert(app.has_async_route("GET", "/async"));
        std::cout << "PASSED\n";
    }

    // Test 11: Same path, different methods
    {
        std::cout << "Test 11: Same path, different methods... ";
        App::Config config;
        config.pure_cpp_mode = true;
        App app(config);

        app.get_async("/resource", [](Request& req, Response& res) -> coro_task<void> {
            res.status(200).json(R"({"method":"GET"})");
            co_return;
        });

        app.post_async("/resource", [](Request& req, Response& res) -> coro_task<void> {
            res.status(201).json(R"({"method":"POST"})");
            co_return;
        });

        app.put_async("/resource", [](Request& req, Response& res) -> coro_task<void> {
            res.status(200).json(R"({"method":"PUT"})");
            co_return;
        });

        app.del_async("/resource", [](Request& req, Response& res) -> coro_task<void> {
            res.status(200).json(R"({"method":"DELETE"})");
            co_return;
        });

        assert(app.has_async_route("GET", "/resource"));
        assert(app.has_async_route("POST", "/resource"));
        assert(app.has_async_route("PUT", "/resource"));
        assert(app.has_async_route("DELETE", "/resource"));
        assert(!app.has_async_route("PATCH", "/resource"));
        std::cout << "PASSED\n";
    }

    // Test 12: Async route not found
    {
        std::cout << "Test 12: Async route not found... ";
        App::Config config;
        config.pure_cpp_mode = true;
        App app(config);

        app.get_async("/exists", [](Request& req, Response& res) -> coro_task<void> {
            res.status(200).json(R"({})");
            co_return;
        });

        assert(!app.has_async_route("GET", "/not-exists"));
        assert(!app.has_async_route("POST", "/exists"));  // Wrong method
        std::cout << "PASSED\n";
    }

    // Test 13: ES-compatible route patterns
    {
        std::cout << "Test 13: ES-compatible route patterns... ";
        App::Config config;
        config.pure_cpp_mode = true;
        App app(config);

        // ES document routes
        app.put_async("/{index}/_doc/{id}", [](Request& req, Response& res) -> coro_task<void> {
            res.status(201).json(R"({"result":"created"})");
            co_return;
        });

        app.get_async("/{index}/_doc/{id}", [](Request& req, Response& res) -> coro_task<void> {
            res.status(200).json(R"({"found":true})");
            co_return;
        });

        app.del_async("/{index}/_doc/{id}", [](Request& req, Response& res) -> coro_task<void> {
            res.status(200).json(R"({"result":"deleted"})");
            co_return;
        });

        app.post_async("/{index}/_doc", [](Request& req, Response& res) -> coro_task<void> {
            res.status(201).json(R"({"result":"created"})");
            co_return;
        });

        app.post_async("/{index}/_search", [](Request& req, Response& res) -> coro_task<void> {
            res.status(200).json(R"({"hits":[]})");
            co_return;
        });

        app.post_async("/{index}/_bulk", [](Request& req, Response& res) -> coro_task<void> {
            res.status(200).json(R"({"items":[]})");
            co_return;
        });

        // Test matching
        assert(app.has_async_route("PUT", "/my-index/_doc/123"));
        assert(app.has_async_route("GET", "/my-index/_doc/123"));
        assert(app.has_async_route("DELETE", "/my-index/_doc/123"));
        assert(app.has_async_route("POST", "/my-index/_doc"));
        assert(app.has_async_route("POST", "/my-index/_search"));
        assert(app.has_async_route("POST", "/my-index/_bulk"));
        std::cout << "PASSED\n";
    }

    // Test 14: Fluent API chaining
    {
        std::cout << "Test 14: Fluent API chaining... ";
        App::Config config;
        config.pure_cpp_mode = true;
        App app(config);

        // Test that async methods return App& for chaining
        app.get_async("/one", [](Request& req, Response& res) -> coro_task<void> {
            co_return;
        }).post_async("/two", [](Request& req, Response& res) -> coro_task<void> {
            co_return;
        }).put_async("/three", [](Request& req, Response& res) -> coro_task<void> {
            co_return;
        });

        assert(app.has_async_route("GET", "/one"));
        assert(app.has_async_route("POST", "/two"));
        assert(app.has_async_route("PUT", "/three"));
        std::cout << "PASSED\n";
    }

    std::cout << "\n=== All tests passed! ===\n";
}

int main() {
    run_tests();
    return 0;
}
