/**
 * CoroUnifiedServer Unit Tests
 *
 * Tests the coroutine-based unified server implementation:
 * - Server lifecycle (start, stop)
 * - Request handling through coroutine handler
 * - Statistics tracking
 * - Configuration options
 * - I/O thread and worker pool sizing
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/coro_unified_server.h"
#include <thread>
#include <chrono>
#include <atomic>
#include <random>
#include <vector>
#include <string>
#include <cstring>
#include <unordered_map>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace fasterapi {
namespace http {
namespace test {

class CoroUnifiedServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create minimal config for unit testing
        config_.enable_tls = false;
        config_.enable_http1_cleartext = false;  // Don't bind to ports initially
        config_.num_io_threads = 1;
        config_.num_workers = 2;
        config_.host = "127.0.0.1";
    }

    void TearDown() override {
        if (server_ && server_->is_running()) {
            server_->stop();
        }
        server_.reset();
    }

    // Generate random number for test variations
    uint32_t random_count(uint32_t min, uint32_t max) {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        std::uniform_int_distribution<uint32_t> dis(min, max);
        return dis(gen);
    }

    CoroUnifiedServerConfig config_;
    std::unique_ptr<CoroUnifiedServer> server_;
};

// Test initial server state
TEST_F(CoroUnifiedServerTest, InitialState) {
    server_ = std::make_unique<CoroUnifiedServer>(config_);
    EXPECT_FALSE(server_->is_running());
    
    auto stats = server_->get_stats();
    EXPECT_EQ(stats.connections_accepted, 0u);
    EXPECT_EQ(stats.connections_active, 0u);
    EXPECT_EQ(stats.requests_total, 0u);
}

// Test configuration options
TEST_F(CoroUnifiedServerTest, ConfigurationOptions) {
    // Verify config values are applied
    CoroUnifiedServerConfig custom_config;
    custom_config.enable_tls = true;
    custom_config.tls_port = 8443;
    custom_config.enable_http1_cleartext = true;
    custom_config.http1_port = 8080;
    custom_config.num_io_threads = 2;
    custom_config.num_workers = 8;
    custom_config.host = "127.0.0.1";
    custom_config.backlog = 512;

    // Server should accept the config without error
    EXPECT_NO_THROW({
        auto server = std::make_unique<CoroUnifiedServer>(custom_config);
        EXPECT_FALSE(server->is_running());
    });
}

// Test handler setting
TEST_F(CoroUnifiedServerTest, SetHandler) {
    server_ = std::make_unique<CoroUnifiedServer>(config_);
    
    std::atomic<bool> handler_set{false};
    
    // Set a coroutine handler
    server_->set_handler([&handler_set](const CoroHttpRequest& req) 
        -> core::coro_task<CoroHttpResponse> {
        handler_set.store(true);
        CoroHttpResponse res;
        res.status = 200;
        res.body = "OK";
        co_return res;
    });
    
    // Handler is just set, not invoked yet
    EXPECT_FALSE(handler_set.load());
}

// Test default I/O thread sizing
TEST_F(CoroUnifiedServerTest, DefaultIOThreadCount) {
    // num_io_threads = 1 is the default
    CoroUnifiedServerConfig default_config;
    EXPECT_EQ(default_config.num_io_threads, 1u);
}

// Test auto-detection of worker count
TEST_F(CoroUnifiedServerTest, AutoWorkerCount) {
    // num_workers = 0 means auto-detect
    CoroUnifiedServerConfig auto_config;
    EXPECT_EQ(auto_config.num_workers, 0u);
}

// Test ALPN protocol defaults
TEST_F(CoroUnifiedServerTest, ALPNProtocolDefaults) {
    CoroUnifiedServerConfig default_config;
    ASSERT_EQ(default_config.alpn_protocols.size(), 2u);
    EXPECT_EQ(default_config.alpn_protocols[0], "h2");
    EXPECT_EQ(default_config.alpn_protocols[1], "http/1.1");
}

// Test server stats structure
TEST_F(CoroUnifiedServerTest, StatsStructure) {
    server_ = std::make_unique<CoroUnifiedServer>(config_);
    
    auto stats = server_->get_stats();
    
    // Initial stats should all be zero
    EXPECT_EQ(stats.connections_accepted, 0u);
    EXPECT_EQ(stats.connections_active, 0u);
    EXPECT_EQ(stats.requests_total, 0u);
    EXPECT_EQ(stats.requests_http1, 0u);
    EXPECT_EQ(stats.requests_http2, 0u);
}

// Test multiple server instances
TEST_F(CoroUnifiedServerTest, MultipleInstances) {
    // Create multiple server instances with different configs
    std::vector<std::unique_ptr<CoroUnifiedServer>> servers;
    
    for (int i = 0; i < 3; i++) {
        CoroUnifiedServerConfig cfg;
        cfg.enable_tls = false;
        cfg.enable_http1_cleartext = false;
        cfg.num_io_threads = 1;
        cfg.num_workers = 2;
        
        EXPECT_NO_THROW({
            servers.push_back(std::make_unique<CoroUnifiedServer>(cfg));
        });
    }
    
    EXPECT_EQ(servers.size(), 3u);
    
    for (const auto& server : servers) {
        EXPECT_FALSE(server->is_running());
    }
}

// Test handler with various response codes
TEST_F(CoroUnifiedServerTest, HandlerResponseCodes) {
    server_ = std::make_unique<CoroUnifiedServer>(config_);
    
    // Set a handler that returns different status codes
    server_->set_handler([](const CoroHttpRequest& req) 
        -> core::coro_task<CoroHttpResponse> {
        CoroHttpResponse res;
        
        if (req.path == "/ok") {
            res.status = 200;
            res.body = "OK";
        } else if (req.path == "/not-found") {
            res.status = 404;
            res.body = "Not Found";
        } else if (req.path == "/server-error") {
            res.status = 500;
            res.body = "Internal Server Error";
        } else {
            res.status = 400;
            res.body = "Bad Request";
        }
        
        co_return res;
    });
    
    // Handler is set, server not started
    EXPECT_FALSE(server_->is_running());
}

// Test configuration with empty cert paths
TEST_F(CoroUnifiedServerTest, EmptyCertPaths) {
    CoroUnifiedServerConfig tls_config;
    tls_config.enable_tls = true;
    tls_config.cert_file = "";
    tls_config.key_file = "";
    // Server should handle empty cert paths gracefully
    
    EXPECT_NO_THROW({
        auto server = std::make_unique<CoroUnifiedServer>(tls_config);
    });
}

// Test configuration with in-memory certs
TEST_F(CoroUnifiedServerTest, InMemoryCerts) {
    CoroUnifiedServerConfig tls_config;
    tls_config.enable_tls = true;
    tls_config.cert_data = "-----BEGIN CERTIFICATE-----\ntest\n-----END CERTIFICATE-----";
    tls_config.key_data = "-----BEGIN PRIVATE KEY-----\ntest\n-----END PRIVATE KEY-----";
    
    EXPECT_NO_THROW({
        auto server = std::make_unique<CoroUnifiedServer>(tls_config);
    });
}

// Test request structure
TEST_F(CoroUnifiedServerTest, RequestStructure) {
    CoroHttpRequest req;
    req.method = "GET";
    req.path = "/api/test";
    req.headers["Content-Type"] = "application/json";
    req.body = "{\"test\": true}";
    
    EXPECT_EQ(req.method, "GET");
    EXPECT_EQ(req.path, "/api/test");
    EXPECT_EQ(req.headers.size(), 1u);
    EXPECT_EQ(req.headers.at("Content-Type"), "application/json");
    EXPECT_FALSE(req.body.empty());
}

// Test response structure
TEST_F(CoroUnifiedServerTest, ResponseStructure) {
    CoroHttpResponse res;
    res.status = 201;
    res.status_message = "Created";
    res.headers["Content-Type"] = "application/json";
    res.headers["Location"] = "/api/resource/123";
    res.body = "{\"id\": 123}";
    
    EXPECT_EQ(res.status, 201);
    EXPECT_EQ(res.status_message, "Created");
    EXPECT_EQ(res.headers.size(), 2u);
    EXPECT_FALSE(res.body.empty());
}

// Test port configuration
TEST_F(CoroUnifiedServerTest, PortConfiguration) {
    // Test various port configurations
    CoroUnifiedServerConfig port_config;
    
    // Standard ports
    port_config.tls_port = 443;
    port_config.http1_port = 80;
    EXPECT_EQ(port_config.tls_port, 443u);
    EXPECT_EQ(port_config.http1_port, 80u);
    
    // Development ports
    port_config.tls_port = 8443;
    port_config.http1_port = 8080;
    EXPECT_EQ(port_config.tls_port, 8443u);
    EXPECT_EQ(port_config.http1_port, 8080u);
}

// Test backlog configuration
TEST_F(CoroUnifiedServerTest, BacklogConfiguration) {
    CoroUnifiedServerConfig backlog_config;
    
    // Default backlog
    EXPECT_EQ(backlog_config.backlog, 1024);
    
    // Custom backlog
    backlog_config.backlog = 4096;
    EXPECT_EQ(backlog_config.backlog, 4096);
}

// Test thread count recommendations
TEST_F(CoroUnifiedServerTest, IOThreadRecommendations) {
    // Verify that 1-2 I/O threads is the recommended range
    CoroUnifiedServerConfig one_io;
    one_io.num_io_threads = 1;
    
    CoroUnifiedServerConfig two_io;
    two_io.num_io_threads = 2;
    
    // Both should be valid configurations
    EXPECT_NO_THROW({
        auto server1 = std::make_unique<CoroUnifiedServer>(one_io);
        auto server2 = std::make_unique<CoroUnifiedServer>(two_io);
    });
}

// =============================================================================
// Integration Tests - HTTP/1.1 and HTTP/2 with actual network I/O
// =============================================================================

// Helper to find an available port
static uint16_t find_available_port() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;  // Let OS choose

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return 0;
    }

    socklen_t len = sizeof(addr);
    if (getsockname(sock, (struct sockaddr*)&addr, &len) < 0) {
        close(sock);
        return 0;
    }

    uint16_t port = ntohs(addr.sin_port);
    close(sock);
    return port;
}

class CoroUnifiedServerIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        port_ = find_available_port();
        if (port_ == 0) {
            // Fallback to random port in high range
            port_ = 30000 + (random_count(0, 10000));
        }
    }

    void TearDown() override {
        if (server_ && server_->is_running()) {
            server_->stop();
        }
        server_.reset();
        
        // Give the server time to release the port
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    uint32_t random_count(uint32_t min, uint32_t max) {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        std::uniform_int_distribution<uint32_t> dis(min, max);
        return dis(gen);
    }

    // Create a TCP connection to the server
    int connect_to_server(uint16_t port) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return -1;

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");

        // Set timeout
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock);
            return -1;
        }

        return sock;
    }

    // Send HTTP/1.1 request and receive response
    std::pair<int, std::string> send_http1_request(
        int sock,
        const std::string& method,
        const std::string& path,
        const std::string& body = "",
        const std::vector<std::pair<std::string, std::string>>& headers = {}
    ) {
        // Build request
        std::string request = method + " " + path + " HTTP/1.1\r\n";
        request += "Host: localhost\r\n";
        
        for (const auto& [name, value] : headers) {
            request += name + ": " + value + "\r\n";
        }
        
        if (!body.empty()) {
            request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        
        request += "\r\n";
        request += body;

        // Send request
        ssize_t sent = send(sock, request.data(), request.size(), 0);
        if (sent < 0) {
            return {-1, ""};
        }

        // Receive response
        std::string response;
        char buffer[4096];
        ssize_t received;
        
        while ((received = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
            response.append(buffer, received);
            
            // Check if we've received the full response
            // Simple check: look for Content-Length or double CRLF for headers-only
            size_t header_end = response.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                // Check for Content-Length
                size_t cl_pos = response.find("Content-Length: ");
                if (cl_pos != std::string::npos && cl_pos < header_end) {
                    size_t cl_end = response.find("\r\n", cl_pos);
                    std::string cl_str = response.substr(cl_pos + 16, cl_end - cl_pos - 16);
                    size_t content_length = std::stoul(cl_str);
                    size_t body_start = header_end + 4;
                    if (response.size() >= body_start + content_length) {
                        break;
                    }
                } else {
                    // No Content-Length, assume headers-only or chunked
                    break;
                }
            }
        }

        // Parse status code
        int status_code = -1;
        size_t status_pos = response.find("HTTP/1.1 ");
        if (status_pos != std::string::npos) {
            status_code = std::stoi(response.substr(status_pos + 9, 3));
        }

        return {status_code, response};
    }

    // Extract body from HTTP/1.1 response
    std::string extract_body(const std::string& response) {
        size_t body_start = response.find("\r\n\r\n");
        if (body_start == std::string::npos) return "";
        return response.substr(body_start + 4);
    }

    uint16_t port_;
    std::unique_ptr<CoroUnifiedServer> server_;
};

// Test 1: HTTP/1.1 async handler is called
TEST_F(CoroUnifiedServerIntegrationTest, HTTP1AsyncHandlerCalled) {
    std::atomic<bool> handler_called{false};
    std::atomic<int> call_count{0};
    std::string received_path;
    std::string received_method;

    CoroUnifiedServerConfig config;
    config.enable_tls = false;
    config.enable_http1_cleartext = true;
    config.http1_port = port_;
    config.num_io_threads = 1;
    config.num_workers = 2;
    config.host = "127.0.0.1";

    server_ = std::make_unique<CoroUnifiedServer>(config);
    
    server_->set_handler([&](const CoroHttpRequest& req) 
        -> core::coro_task<CoroHttpResponse> {
        handler_called.store(true);
        call_count.fetch_add(1);
        received_path = req.path;
        received_method = req.method;
        
        CoroHttpResponse res;
        res.status = 200;
        res.body = "Handler was called!";
        res.headers["Content-Type"] = "text/plain";
        co_return res;
    });

    ASSERT_EQ(server_->start_background(), 0);
    EXPECT_TRUE(server_->is_running());
    
    // Wait for server to be ready
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Send HTTP/1.1 request
    int sock = connect_to_server(port_);
    ASSERT_GT(sock, 0) << "Failed to connect to server";

    auto [status, response] = send_http1_request(sock, "GET", "/test/path");
    close(sock);

    EXPECT_EQ(status, 200);
    EXPECT_TRUE(handler_called.load()) << "Handler was not called for HTTP/1.1 request";
    EXPECT_EQ(call_count.load(), 1);
    EXPECT_EQ(received_path, "/test/path");
    EXPECT_EQ(received_method, "GET");
    
    std::string body = extract_body(response);
    EXPECT_EQ(body, "Handler was called!");
}

// Test 2: HTTP/1.1 POST with body
TEST_F(CoroUnifiedServerIntegrationTest, HTTP1PostWithBody) {
    std::string received_body;

    CoroUnifiedServerConfig config;
    config.enable_tls = false;
    config.enable_http1_cleartext = true;
    config.http1_port = port_;
    config.num_io_threads = 1;
    config.num_workers = 2;
    config.host = "127.0.0.1";

    server_ = std::make_unique<CoroUnifiedServer>(config);
    
    server_->set_handler([&](const CoroHttpRequest& req) 
        -> core::coro_task<CoroHttpResponse> {
        received_body = req.body;
        
        CoroHttpResponse res;
        res.status = 200;
        res.body = "Received: " + req.body;
        res.headers["Content-Type"] = "text/plain";
        co_return res;
    });

    ASSERT_EQ(server_->start_background(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int sock = connect_to_server(port_);
    ASSERT_GT(sock, 0);

    std::string post_body = R"({"key": "value", "number": 42})";
    auto [status, response] = send_http1_request(sock, "POST", "/api/data", post_body, 
        {{"Content-Type", "application/json"}});
    close(sock);

    EXPECT_EQ(status, 200);
    EXPECT_EQ(received_body, post_body);
    
    std::string body = extract_body(response);
    EXPECT_EQ(body, "Received: " + post_body);
}

// Test 3: HTTP/1.1 concurrent requests
TEST_F(CoroUnifiedServerIntegrationTest, HTTP1ConcurrentRequests) {
    std::atomic<int> call_count{0};

    CoroUnifiedServerConfig config;
    config.enable_tls = false;
    config.enable_http1_cleartext = true;
    config.http1_port = port_;
    config.num_io_threads = 1;
    config.num_workers = 4;
    config.host = "127.0.0.1";

    server_ = std::make_unique<CoroUnifiedServer>(config);
    
    server_->set_handler([&](const CoroHttpRequest& req) 
        -> core::coro_task<CoroHttpResponse> {
        int count = call_count.fetch_add(1) + 1;
        
        CoroHttpResponse res;
        res.status = 200;
        res.body = "Request " + std::to_string(count);
        co_return res;
    });

    ASSERT_EQ(server_->start_background(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Send concurrent requests
    constexpr int NUM_REQUESTS = 10;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    for (int i = 0; i < NUM_REQUESTS; i++) {
        threads.emplace_back([&, i]() {
            int sock = connect_to_server(port_);
            if (sock < 0) return;

            auto [status, response] = send_http1_request(sock, "GET", "/request/" + std::to_string(i));
            close(sock);

            if (status == 200) {
                success_count.fetch_add(1);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(call_count.load(), NUM_REQUESTS);
    EXPECT_EQ(success_count.load(), NUM_REQUESTS);
}

// Test 4: HTTP/1.1 handler receives correct headers
TEST_F(CoroUnifiedServerIntegrationTest, HTTP1HandlerReceivesHeaders) {
    std::unordered_map<std::string, std::string> received_headers;

    CoroUnifiedServerConfig config;
    config.enable_tls = false;
    config.enable_http1_cleartext = true;
    config.http1_port = port_;
    config.num_io_threads = 1;
    config.num_workers = 2;
    config.host = "127.0.0.1";

    server_ = std::make_unique<CoroUnifiedServer>(config);
    
    server_->set_handler([&](const CoroHttpRequest& req) 
        -> core::coro_task<CoroHttpResponse> {
        received_headers = req.headers;
        
        CoroHttpResponse res;
        res.status = 200;
        res.body = "OK";
        co_return res;
    });

    ASSERT_EQ(server_->start_background(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int sock = connect_to_server(port_);
    ASSERT_GT(sock, 0);

    auto [status, response] = send_http1_request(sock, "GET", "/headers", "", 
        {{"X-Custom-Header", "custom-value"}, {"Accept", "application/json"}});
    close(sock);

    EXPECT_EQ(status, 200);
    EXPECT_EQ(received_headers["X-Custom-Header"], "custom-value");
    EXPECT_EQ(received_headers["Accept"], "application/json");
}

// Test 5: HTTP/1.1 handler can return different status codes
TEST_F(CoroUnifiedServerIntegrationTest, HTTP1DifferentStatusCodes) {
    CoroUnifiedServerConfig config;
    config.enable_tls = false;
    config.enable_http1_cleartext = true;
    config.http1_port = port_;
    config.num_io_threads = 1;
    config.num_workers = 2;
    config.host = "127.0.0.1";

    server_ = std::make_unique<CoroUnifiedServer>(config);
    
    server_->set_handler([](const CoroHttpRequest& req) 
        -> core::coro_task<CoroHttpResponse> {
        CoroHttpResponse res;
        
        if (req.path == "/ok") {
            res.status = 200;
            res.body = "OK";
        } else if (req.path == "/created") {
            res.status = 201;
            res.body = "Created";
        } else if (req.path == "/not-found") {
            res.status = 404;
            res.body = "Not Found";
        } else if (req.path == "/error") {
            res.status = 500;
            res.body = "Internal Server Error";
        } else {
            res.status = 400;
            res.body = "Bad Request";
        }
        
        co_return res;
    });

    ASSERT_EQ(server_->start_background(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Test 200
    {
        int sock = connect_to_server(port_);
        ASSERT_GT(sock, 0);
        auto [status, response] = send_http1_request(sock, "GET", "/ok");
        close(sock);
        EXPECT_EQ(status, 200);
    }

    // Test 201
    {
        int sock = connect_to_server(port_);
        ASSERT_GT(sock, 0);
        auto [status, response] = send_http1_request(sock, "GET", "/created");
        close(sock);
        EXPECT_EQ(status, 201);
    }

    // Test 404
    {
        int sock = connect_to_server(port_);
        ASSERT_GT(sock, 0);
        auto [status, response] = send_http1_request(sock, "GET", "/not-found");
        close(sock);
        EXPECT_EQ(status, 404);
    }

    // Test 500
    {
        int sock = connect_to_server(port_);
        ASSERT_GT(sock, 0);
        auto [status, response] = send_http1_request(sock, "GET", "/error");
        close(sock);
        EXPECT_EQ(status, 500);
    }
}

// Test 6: Server statistics are tracked
TEST_F(CoroUnifiedServerIntegrationTest, HTTP1StatisticsTracked) {
    CoroUnifiedServerConfig config;
    config.enable_tls = false;
    config.enable_http1_cleartext = true;
    config.http1_port = port_;
    config.num_io_threads = 1;
    config.num_workers = 2;
    config.host = "127.0.0.1";

    server_ = std::make_unique<CoroUnifiedServer>(config);
    
    server_->set_handler([](const CoroHttpRequest& req) 
        -> core::coro_task<CoroHttpResponse> {
        CoroHttpResponse res;
        res.status = 200;
        res.body = "OK";
        co_return res;
    });

    auto stats_before = server_->get_stats();
    EXPECT_EQ(stats_before.connections_accepted, 0u);
    EXPECT_EQ(stats_before.requests_total, 0u);

    ASSERT_EQ(server_->start_background(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Send 3 requests
    for (int i = 0; i < 3; i++) {
        int sock = connect_to_server(port_);
        ASSERT_GT(sock, 0);
        auto [status, response] = send_http1_request(sock, "GET", "/stats");
        close(sock);
        EXPECT_EQ(status, 200);
    }

    // Wait for stats to update
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto stats_after = server_->get_stats();
    EXPECT_GE(stats_after.connections_accepted, 3u);
    EXPECT_GE(stats_after.requests_total, 3u);
    EXPECT_GE(stats_after.requests_http1, 3u);
}

// Test 7: HTTP/1.1 keep-alive works
TEST_F(CoroUnifiedServerIntegrationTest, HTTP1KeepAlive) {
    std::atomic<int> call_count{0};

    CoroUnifiedServerConfig config;
    config.enable_tls = false;
    config.enable_http1_cleartext = true;
    config.http1_port = port_;
    config.num_io_threads = 1;
    config.num_workers = 2;
    config.host = "127.0.0.1";

    server_ = std::make_unique<CoroUnifiedServer>(config);
    
    server_->set_handler([&](const CoroHttpRequest& req) 
        -> core::coro_task<CoroHttpResponse> {
        call_count.fetch_add(1);
        
        CoroHttpResponse res;
        res.status = 200;
        res.body = "OK";
        co_return res;
    });

    ASSERT_EQ(server_->start_background(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Send multiple requests on same connection
    int sock = connect_to_server(port_);
    ASSERT_GT(sock, 0);

    for (int i = 0; i < 3; i++) {
        auto [status, response] = send_http1_request(sock, "GET", "/keepalive/" + std::to_string(i));
        EXPECT_EQ(status, 200);
    }

    close(sock);
    
    // Wait for handler to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(call_count.load(), 3);
}

// Test 8: Server start/stop lifecycle
TEST_F(CoroUnifiedServerIntegrationTest, StartStopLifecycle) {
    CoroUnifiedServerConfig config;
    config.enable_tls = false;
    config.enable_http1_cleartext = true;
    config.http1_port = port_;
    config.num_io_threads = 1;
    config.num_workers = 2;
    config.host = "127.0.0.1";

    server_ = std::make_unique<CoroUnifiedServer>(config);
    
    server_->set_handler([](const CoroHttpRequest& req) 
        -> core::coro_task<CoroHttpResponse> {
        CoroHttpResponse res;
        res.status = 200;
        res.body = "OK";
        co_return res;
    });

    EXPECT_FALSE(server_->is_running());
    
    // Start
    ASSERT_EQ(server_->start_background(), 0);
    EXPECT_TRUE(server_->is_running());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Verify working
    int sock = connect_to_server(port_);
    ASSERT_GT(sock, 0);
    auto [status1, response1] = send_http1_request(sock, "GET", "/test");
    close(sock);
    EXPECT_EQ(status1, 200);

    // Stop
    server_->stop();
    EXPECT_FALSE(server_->is_running());

    // Verify stopped (connection should fail)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sock = connect_to_server(port_);
    EXPECT_LT(sock, 0) << "Should not be able to connect to stopped server";
}

// =============================================================================
// SSE (Server-Sent Events) Tests
// =============================================================================

// Test SSE handler registration
TEST_F(CoroUnifiedServerIntegrationTest, SSEHandlerRegistration) {
    std::atomic<bool> sse_handler_called{false};

    CoroUnifiedServerConfig config;
    config.enable_tls = false;
    config.enable_http1_cleartext = true;
    config.http1_port = port_;
    config.num_io_threads = 1;
    config.num_workers = 2;
    config.host = "127.0.0.1";

    server_ = std::make_unique<CoroUnifiedServer>(config);
    
    // Register SSE handler
    server_->add_sse_handler("/events", 
        [&sse_handler_called](net::IODispatcher& io, int fd, const CoroHttpRequest& req) 
            -> core::coro_task<void> {
        sse_handler_called.store(true);
        
        // Send a few SSE events
        const char* event1 = "data: event 1\n\n";
        co_await io.async_write(fd, event1, strlen(event1));
        
        const char* event2 = "data: event 2\n\n";
        co_await io.async_write(fd, event2, strlen(event2));
        
        const char* event3 = "data: done\n\n";
        co_await io.async_write(fd, event3, strlen(event3));
    });

    // Set default handler for non-SSE requests
    server_->set_handler([](const CoroHttpRequest& req) 
        -> core::coro_task<CoroHttpResponse> {
        CoroHttpResponse res;
        res.status = 200;
        res.body = "Not SSE";
        co_return res;
    });

    ASSERT_EQ(server_->start_background(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Send SSE request with Accept: text/event-stream
    int sock = connect_to_server(port_);
    ASSERT_GT(sock, 0);

    // Build SSE request manually
    std::string request = 
        "GET /events HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Accept: text/event-stream\r\n"
        "\r\n";

    ssize_t sent = send(sock, request.data(), request.size(), 0);
    ASSERT_GT(sent, 0);

    // Receive response
    std::string response;
    char buffer[4096];
    ssize_t received;
    
    // Set longer timeout for SSE
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while ((received = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
        response.append(buffer, received);
        // Check if we got the "done" event
        if (response.find("data: done") != std::string::npos) {
            break;
        }
    }

    close(sock);

    EXPECT_TRUE(sse_handler_called.load()) << "SSE handler was not called";
    EXPECT_NE(response.find("text/event-stream"), std::string::npos) << "Response should have SSE content type";
    EXPECT_NE(response.find("data: event 1"), std::string::npos) << "Should contain first event";
    EXPECT_NE(response.find("data: event 2"), std::string::npos) << "Should contain second event";
    EXPECT_NE(response.find("data: done"), std::string::npos) << "Should contain done event";
}

// =============================================================================
// HTTP Pipelining Tests
// =============================================================================

// Test HTTP pipelining - multiple requests on same connection
TEST_F(CoroUnifiedServerIntegrationTest, HTTPPipelining) {
    std::atomic<int> request_count{0};
    std::vector<std::string> received_paths;
    std::mutex paths_mutex;

    CoroUnifiedServerConfig config;
    config.enable_tls = false;
    config.enable_http1_cleartext = true;
    config.http1_port = port_;
    config.num_io_threads = 1;
    config.num_workers = 2;
    config.host = "127.0.0.1";
    config.idle_timeout_ms = 60000;  // Long timeout for this test

    server_ = std::make_unique<CoroUnifiedServer>(config);
    
    server_->set_handler([&](const CoroHttpRequest& req) 
        -> core::coro_task<CoroHttpResponse> {
        request_count.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(paths_mutex);
            received_paths.push_back(req.path);
        }
        
        CoroHttpResponse res;
        res.status = 200;
        res.body = "OK: " + req.path;
        res.headers["Content-Type"] = "text/plain";
        co_return res;
    });

    ASSERT_EQ(server_->start_background(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int sock = connect_to_server(port_);
    ASSERT_GT(sock, 0);

    // Send multiple pipelined requests at once
    std::string pipelined_requests;
    for (int i = 0; i < 3; i++) {
        pipelined_requests += 
            "GET /request/" + std::to_string(i) + " HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "\r\n";
    }

    ssize_t sent = send(sock, pipelined_requests.data(), pipelined_requests.size(), 0);
    ASSERT_GT(sent, 0);

    // Receive all responses
    std::string response;
    char buffer[4096];
    int response_count = 0;
    
    while (response_count < 3) {
        ssize_t received = recv(sock, buffer, sizeof(buffer), 0);
        if (received <= 0) break;
        response.append(buffer, received);
        
        // Count HTTP/1.1 200 OK occurrences
        response_count = 0;
        size_t pos = 0;
        while ((pos = response.find("HTTP/1.1 200", pos)) != std::string::npos) {
            response_count++;
            pos++;
        }
    }

    close(sock);

    // Wait for all handlers to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(request_count.load(), 3) << "Should have received 3 pipelined requests";
    EXPECT_EQ(response_count, 3) << "Should have received 3 responses";
    
    {
        std::lock_guard<std::mutex> lock(paths_mutex);
        EXPECT_EQ(received_paths.size(), 3u);
        EXPECT_EQ(received_paths[0], "/request/0");
        EXPECT_EQ(received_paths[1], "/request/1");
        EXPECT_EQ(received_paths[2], "/request/2");
    }
}

// =============================================================================
// Body Size Limit Tests (413 Payload Too Large)
// =============================================================================

// Test body size limit enforcement
TEST_F(CoroUnifiedServerIntegrationTest, BodySizeLimitReturns413) {
    CoroUnifiedServerConfig config;
    config.enable_tls = false;
    config.enable_http1_cleartext = true;
    config.http1_port = port_;
    config.num_io_threads = 1;
    config.num_workers = 2;
    config.host = "127.0.0.1";
    config.max_body_size = 100;  // Very small limit for testing

    server_ = std::make_unique<CoroUnifiedServer>(config);
    
    server_->set_handler([](const CoroHttpRequest& req) 
        -> core::coro_task<CoroHttpResponse> {
        CoroHttpResponse res;
        res.status = 200;
        res.body = "OK";
        co_return res;
    });

    ASSERT_EQ(server_->start_background(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int sock = connect_to_server(port_);
    ASSERT_GT(sock, 0);

    // Send request with body larger than limit
    std::string large_body(200, 'X');  // 200 bytes, exceeds 100 byte limit
    
    std::string request = 
        "POST /upload HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: " + std::to_string(large_body.size()) + "\r\n"
        "\r\n" + large_body;

    ssize_t sent = send(sock, request.data(), request.size(), 0);
    ASSERT_GT(sent, 0);

    // Receive response
    std::string response;
    char buffer[4096];
    ssize_t received = recv(sock, buffer, sizeof(buffer), 0);
    if (received > 0) {
        response.append(buffer, received);
    }

    close(sock);

    // Should get 413 Payload Too Large
    EXPECT_NE(response.find("HTTP/1.1 413"), std::string::npos) 
        << "Expected 413 Payload Too Large, got: " << response.substr(0, 100);
}

// Test body within limit is accepted
TEST_F(CoroUnifiedServerIntegrationTest, BodyWithinLimitAccepted) {
    CoroUnifiedServerConfig config;
    config.enable_tls = false;
    config.enable_http1_cleartext = true;
    config.http1_port = port_;
    config.num_io_threads = 1;
    config.num_workers = 2;
    config.host = "127.0.0.1";
    config.max_body_size = 1000;  // 1KB limit

    server_ = std::make_unique<CoroUnifiedServer>(config);
    
    std::string received_body;
    server_->set_handler([&](const CoroHttpRequest& req) 
        -> core::coro_task<CoroHttpResponse> {
        received_body = req.body;
        CoroHttpResponse res;
        res.status = 200;
        res.body = "Received " + std::to_string(req.body.size()) + " bytes";
        co_return res;
    });

    ASSERT_EQ(server_->start_background(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int sock = connect_to_server(port_);
    ASSERT_GT(sock, 0);

    // Send request with body within limit
    std::string body(500, 'Y');  // 500 bytes, within 1000 byte limit
    auto [status, response] = send_http1_request(sock, "POST", "/upload", body,
        {{"Content-Type", "application/octet-stream"}});
    close(sock);

    EXPECT_EQ(status, 200) << "Expected 200 OK for body within limit";
    EXPECT_EQ(received_body, body);
}

// =============================================================================
// Header Size Limit Tests
// =============================================================================

// Test that max_header_size config is accessible
TEST_F(CoroUnifiedServerIntegrationTest, HeaderSizeConfigAccessor) {
    CoroUnifiedServerConfig config;
    config.enable_tls = false;
    config.enable_http1_cleartext = false;  // No network
    config.max_header_size = 4096;

    auto server = std::make_unique<CoroUnifiedServer>(config);
    EXPECT_EQ(server->get_max_header_size(), 4096u);
}

// =============================================================================
// Request Timeout Tests
// =============================================================================

// Test that timeout config values are accessible
TEST_F(CoroUnifiedServerIntegrationTest, TimeoutConfigAccessor) {
    CoroUnifiedServerConfig config;
    config.enable_tls = false;
    config.enable_http1_cleartext = false;  // No network
    config.request_timeout_ms = 5000;
    config.idle_timeout_ms = 10000;

    auto server = std::make_unique<CoroUnifiedServer>(config);
    EXPECT_EQ(server->get_request_timeout_ms(), 5000u);
    EXPECT_EQ(server->get_idle_timeout_ms(), 10000u);
}

// =============================================================================
// Graceful Shutdown Tests
// =============================================================================

// Test graceful shutdown
TEST_F(CoroUnifiedServerIntegrationTest, GracefulShutdown) {
    CoroUnifiedServerConfig config;
    config.enable_tls = false;
    config.enable_http1_cleartext = true;
    config.http1_port = port_;
    config.num_io_threads = 1;
    config.num_workers = 2;
    config.host = "127.0.0.1";
    config.shutdown_timeout_ms = 1000;

    server_ = std::make_unique<CoroUnifiedServer>(config);
    
    server_->set_handler([](const CoroHttpRequest& req) 
        -> core::coro_task<CoroHttpResponse> {
        CoroHttpResponse res;
        res.status = 200;
        res.body = "OK";
        co_return res;
    });

    ASSERT_EQ(server_->start_background(), 0);
    EXPECT_TRUE(server_->is_running());
    EXPECT_TRUE(server_->is_accepting());
    EXPECT_FALSE(server_->is_draining());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Verify working
    int sock = connect_to_server(port_);
    ASSERT_GT(sock, 0);
    auto [status, response] = send_http1_request(sock, "GET", "/test");
    close(sock);
    EXPECT_EQ(status, 200);

    // Initiate graceful shutdown
    bool shutdown_complete = server_->shutdown_gracefully();
    
    EXPECT_TRUE(shutdown_complete) << "Graceful shutdown should complete within timeout";
    EXPECT_FALSE(server_->is_accepting()) << "Should not be accepting after shutdown";
}

// Test shutdown state accessor
TEST_F(CoroUnifiedServerIntegrationTest, ShutdownStateAccessor) {
    CoroUnifiedServerConfig config;
    config.enable_tls = false;
    config.enable_http1_cleartext = true;
    config.http1_port = port_;
    config.num_io_threads = 1;
    config.num_workers = 2;
    config.host = "127.0.0.1";

    server_ = std::make_unique<CoroUnifiedServer>(config);
    
    server_->set_handler([](const CoroHttpRequest& req) 
        -> core::coro_task<CoroHttpResponse> {
        CoroHttpResponse res;
        res.status = 200;
        res.body = "OK";
        co_return res;
    });

    // Before start
    EXPECT_EQ(server_->get_shutdown_state(), CoroShutdownState::STOPPED);
    
    ASSERT_EQ(server_->start_background(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // After start
    EXPECT_EQ(server_->get_shutdown_state(), CoroShutdownState::RUNNING);
    
    server_->stop();
    
    // After stop
    EXPECT_EQ(server_->get_shutdown_state(), CoroShutdownState::STOPPED);
}

// =============================================================================
// Ultra-Fast Callback Tests
// =============================================================================

// Static counter for ultra-fast callback test (must be static because callback is a raw function pointer)
static std::atomic<int> s_ultra_fast_call_count{0};

// Static ultra-fast callback function
static size_t ultra_fast_test_callback(const Http1RequestView& req, FastResponseWriter& writer) {
    s_ultra_fast_call_count.fetch_add(1);
    
    writer.write("HTTP/1.1 200 OK\r\n");
    writer.write("Content-Type: text/plain\r\n");
    writer.write("Content-Length: 10\r\n");
    writer.write("\r\n");
    writer.write("Ultra Fast");
    
    return writer.size;
}

// Test ultra-fast callback is invoked
TEST_F(CoroUnifiedServerIntegrationTest, UltraFastCallbackInvoked) {
    s_ultra_fast_call_count.store(0);  // Reset counter
    std::atomic<bool> regular_handler_called{false};

    CoroUnifiedServerConfig config;
    config.enable_tls = false;
    config.enable_http1_cleartext = true;
    config.http1_port = port_;
    config.num_io_threads = 1;
    config.num_workers = 2;
    config.host = "127.0.0.1";

    server_ = std::make_unique<CoroUnifiedServer>(config);
    
    // Set ultra-fast callback (static function pointer)
    server_->set_ultra_fast_callback(ultra_fast_test_callback);
    
    // Set regular handler (should not be called if ultra-fast works)
    server_->set_handler([&](const CoroHttpRequest& req) 
        -> core::coro_task<CoroHttpResponse> {
        regular_handler_called.store(true);
        CoroHttpResponse res;
        res.status = 200;
        res.body = "Regular";
        co_return res;
    });

    ASSERT_EQ(server_->start_background(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int sock = connect_to_server(port_);
    ASSERT_GT(sock, 0);
    
    auto [status, response] = send_http1_request(sock, "GET", "/fast");
    close(sock);

    EXPECT_EQ(status, 200);
    EXPECT_GT(s_ultra_fast_call_count.load(), 0) << "Ultra-fast callback should have been called";
    EXPECT_FALSE(regular_handler_called.load()) << "Regular handler should not be called when ultra-fast succeeds";
    
    std::string body = extract_body(response);
    EXPECT_EQ(body, "Ultra Fast");
}

// =============================================================================
// Active Connections Counter Tests
// =============================================================================

// Test active connections tracking
TEST_F(CoroUnifiedServerIntegrationTest, ActiveConnectionsTracking) {
    CoroUnifiedServerConfig config;
    config.enable_tls = false;
    config.enable_http1_cleartext = true;
    config.http1_port = port_;
    config.num_io_threads = 1;
    config.num_workers = 2;
    config.host = "127.0.0.1";

    server_ = std::make_unique<CoroUnifiedServer>(config);
    
    server_->set_handler([](const CoroHttpRequest& req) 
        -> core::coro_task<CoroHttpResponse> {
        CoroHttpResponse res;
        res.status = 200;
        res.body = "OK";
        co_return res;
    });

    EXPECT_EQ(server_->get_active_connections(), 0u) << "No active connections before start";

    ASSERT_EQ(server_->start_background(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Make a connection
    int sock = connect_to_server(port_);
    ASSERT_GT(sock, 0);
    
    // Connection should be tracked after request
    auto [status, response] = send_http1_request(sock, "GET", "/test");
    EXPECT_EQ(status, 200);
    
    close(sock);
    
    // Give time for connection to close
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    EXPECT_EQ(server_->get_active_connections(), 0u) << "No active connections after close";
}

} // namespace test
} // namespace http
} // namespace fasterapi
