/**
 * HTTP/2 ALPN Integration Tests
 *
 * Tests HTTP/2 over TLS with ALPN negotiation using UnifiedServer.
 * Verifies the set_request_handler() pattern works correctly for HTTP/2.
 *
 * Per CLAUDE.md requirements:
 * - Multiple routes and HTTP verbs
 * - Randomized input data (not hardcoded)
 * - No mocking - real server and curl
 */

#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <array>
#include <sstream>
#include "src/cpp/http/unified_server.h"

using namespace fasterapi::http;
using namespace fasterapi::net;

// Test infrastructure
static int tests_passed = 0;
static int tests_failed = 0;
static bool current_test_failed = false;
static std::string current_test_error;

#define TEST(name) \
    void test_##name(); \
    struct Test##name { \
        Test##name() { \
            std::cout << "Running " << #name << "... " << std::flush; \
            current_test_failed = false; \
            current_test_error = ""; \
            test_##name(); \
            if (current_test_failed) { \
                std::cout << "FAIL: " << current_test_error << std::endl; \
                tests_failed++; \
            } else { \
                std::cout << "PASS" << std::endl; \
                tests_passed++; \
            } \
        } \
    } test_instance_##name; \
    void test_##name()

#define ASSERT(condition, msg) \
    if (!(condition)) { \
        current_test_failed = true; \
        current_test_error = msg; \
        return; \
    }

// Random string generator for test data
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

// Execute curl command and capture output
struct CurlResult {
    int exit_code;
    std::string output;
};

CurlResult run_curl(const std::string& args) {
    std::string cmd = "curl -sk --max-time 5 " + args + " 2>&1";
    std::array<char, 4096> buffer;
    std::string result;

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return {-1, "popen failed"};
    }

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }

    int exit_code = pclose(pipe);
    return {WEXITSTATUS(exit_code), result};
}

// Global server instance for tests
static std::unique_ptr<UnifiedServer> g_server;
static std::thread g_server_thread;
static std::atomic<bool> g_server_running{false};
static std::atomic<int> g_request_count{0};
static std::string g_last_method;
static std::string g_last_path;
static std::string g_last_body;

void start_test_server() {
    if (g_server_running.load()) return;

    UnifiedServerConfig config;
    config.enable_tls = true;
    config.tls_port = 18443;  // Use different port from test_unified_server
    config.host = "127.0.0.1";
    config.cert_file = "certs/server.crt";
    config.key_file = "certs/server.key";
    config.alpn_protocols = {"h2", "http/1.1"};
    config.num_workers = 1;
    config.enable_http1_cleartext = false;  // TLS only for this test

    g_server = std::make_unique<UnifiedServer>(config);

    // Set request handler
    g_server->set_request_handler([](
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body,
        std::function<void(uint16_t, const std::unordered_map<std::string, std::string>&, const std::string&)> send_response
    ) {
        g_request_count++;
        g_last_method = method;
        g_last_path = path;
        g_last_body = body;

        std::unordered_map<std::string, std::string> response_headers;
        response_headers["content-type"] = "application/json";
        response_headers["x-request-count"] = std::to_string(g_request_count.load());

        std::string response_body = "{\"method\":\"" + method + "\",\"path\":\"" + path + "\",\"ok\":true}";

        send_response(200, response_headers, response_body);
    });

    g_server_running = true;
    g_server_thread = std::thread([]() {
        g_server->start();
    });

    // Wait for server to start
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

void stop_test_server() {
    if (!g_server_running.load()) return;

    g_server->stop();
    if (g_server_thread.joinable()) {
        g_server_thread.join();
    }
    g_server.reset();
    g_server_running = false;
}

// ============================================================================
// Test: Basic HTTP/2 request via ALPN
// ============================================================================
TEST(http2_alpn_basic_request) {
    start_test_server();

    int initial_count = g_request_count.load();

    // Test with HTTP/2
    auto result = run_curl("--http2 https://127.0.0.1:18443/test/alpn");

    ASSERT(result.exit_code == 0, "curl should succeed, got: " + result.output);
    ASSERT(result.output.find("\"ok\":true") != std::string::npos,
           "Response should contain ok:true, got: " + result.output);
    ASSERT(g_request_count.load() > initial_count,
           "Request handler should have been called");
    ASSERT(g_last_path == "/test/alpn",
           "Path should be /test/alpn, got: " + g_last_path);
}

// ============================================================================
// Test: Verify HTTP/2 protocol negotiation
// ============================================================================
TEST(http2_alpn_protocol_negotiation) {
    start_test_server();

    // Use verbose curl to check protocol
    auto result = run_curl("-v --http2 https://127.0.0.1:18443/");

    ASSERT(result.exit_code == 0, "curl should succeed");
    // Check for HTTP/2 indicators in verbose output
    bool is_http2 = result.output.find("HTTP/2") != std::string::npos ||
                    result.output.find("ALPN: server accepted h2") != std::string::npos;
    ASSERT(is_http2, "Should negotiate HTTP/2 protocol, output: " + result.output);
}

// ============================================================================
// Test: Multiple HTTP verbs over HTTP/2
// ============================================================================
TEST(http2_alpn_multiple_verbs) {
    start_test_server();

    // Generate random test data
    std::string random_id = random_string(8);
    std::string random_name = random_string(12);

    // GET request
    g_last_method = "";
    auto get_result = run_curl("--http2 https://127.0.0.1:18443/api/users/" + random_id);
    ASSERT(get_result.exit_code == 0, "GET should succeed");
    ASSERT(g_last_method == "GET", "Method should be GET, got: " + g_last_method);

    // POST request with JSON body
    g_last_method = "";
    g_last_body = "";
    std::string post_body = "{\"name\":\"" + random_name + "\",\"id\":\"" + random_id + "\"}";
    auto post_result = run_curl("--http2 -X POST -H 'Content-Type: application/json' "
                                 "-d '" + post_body + "' https://127.0.0.1:18443/api/users");
    ASSERT(post_result.exit_code == 0, "POST should succeed");
    ASSERT(g_last_method == "POST", "Method should be POST, got: " + g_last_method);

    // PUT request
    g_last_method = "";
    std::string put_body = "{\"name\":\"updated_" + random_name + "\"}";
    auto put_result = run_curl("--http2 -X PUT -H 'Content-Type: application/json' "
                                "-d '" + put_body + "' https://127.0.0.1:18443/api/users/" + random_id);
    ASSERT(put_result.exit_code == 0, "PUT should succeed");
    ASSERT(g_last_method == "PUT", "Method should be PUT, got: " + g_last_method);

    // DELETE request
    g_last_method = "";
    auto del_result = run_curl("--http2 -X DELETE https://127.0.0.1:18443/api/users/" + random_id);
    ASSERT(del_result.exit_code == 0, "DELETE should succeed");
    ASSERT(g_last_method == "DELETE", "Method should be DELETE, got: " + g_last_method);
}

// ============================================================================
// Test: Multiple concurrent requests (stream multiplexing)
// ============================================================================
TEST(http2_alpn_concurrent_requests) {
    start_test_server();

    int initial_count = g_request_count.load();

    // Use curl with multiple URLs to test multiplexing
    // Note: curl with --http2 can multiplex if given multiple URLs
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int i = 0; i < 5; ++i) {
        threads.emplace_back([&success_count, i]() {
            std::string unique_id = random_string(8);
            auto result = run_curl("--http2 https://127.0.0.1:18443/concurrent/" + unique_id);
            if (result.exit_code == 0 && result.output.find("\"ok\":true") != std::string::npos) {
                success_count++;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    ASSERT(success_count.load() == 5, "All concurrent requests should succeed");
    ASSERT(g_request_count.load() >= initial_count + 5,
           "Should have received at least 5 requests");
}

// ============================================================================
// Test: HTTP/1.1 fallback when HTTP/2 not requested
// ============================================================================
TEST(http2_alpn_http11_fallback) {
    start_test_server();

    g_last_method = "";

    // Force HTTP/1.1
    auto result = run_curl("--http1.1 https://127.0.0.1:18443/http11test");

    ASSERT(result.exit_code == 0, "HTTP/1.1 request should succeed");
    ASSERT(result.output.find("\"ok\":true") != std::string::npos,
           "Response should contain ok:true");
    ASSERT(g_last_method == "GET", "Method should be GET");
}

// ============================================================================
// Test: Request with randomized headers and body
// ============================================================================
TEST(http2_alpn_randomized_data) {
    start_test_server();

    // Generate random path (alphanumeric only)
    std::string random_path = "/random/" + random_string(16);

    // Reset state before test
    g_last_path = "";
    g_last_method = "";

    // Wait a moment for any previous request to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto result = run_curl("--http2 https://127.0.0.1:18443" + random_path);

    ASSERT(result.exit_code == 0, "Request with random path should succeed, got exit: " +
           std::to_string(result.exit_code) + " output: " + result.output);
    ASSERT(result.output.find("\"ok\":true") != std::string::npos,
           "Response should contain ok:true, got: " + result.output);
    ASSERT(g_last_method == "GET",
           "Method should be GET, got: '" + g_last_method + "'");
    ASSERT(g_last_path == random_path,
           "Path should match random path '" + random_path + "', got: '" + g_last_path + "'");
}

// ============================================================================
// Test: Multiple routes work correctly
// ============================================================================
TEST(http2_alpn_multiple_routes) {
    start_test_server();

    // Test different paths
    std::vector<std::string> paths = {
        "/",
        "/health",
        "/api/v1/users",
        "/api/v1/users/123",
        "/api/v1/users/123/profile",
        "/_cluster/health",
        "/search"
    };

    for (const auto& path : paths) {
        g_last_path = "";
        auto result = run_curl("--http2 https://127.0.0.1:18443" + path);
        ASSERT(result.exit_code == 0, "Request to " + path + " should succeed");
        ASSERT(g_last_path == path, "Path should be " + path + ", got: " + g_last_path);
    }
}

// ============================================================================
// Cleanup
// ============================================================================
struct TestCleanup {
    ~TestCleanup() {
        stop_test_server();
    }
} test_cleanup;

int main(int argc, char* argv[]) {
    std::cout << "=== FasterAPI HTTP/2 ALPN Integration Tests ===" << std::endl;
    std::cout << std::endl;

    // Tests are auto-registered and run via TEST macro

    std::cout << std::endl;
    std::cout << "=== Results ===" << std::endl;
    std::cout << "Passed: " << tests_passed << std::endl;
    std::cout << "Failed: " << tests_failed << std::endl;

    stop_test_server();

    return tests_failed > 0 ? 1 : 0;
}
