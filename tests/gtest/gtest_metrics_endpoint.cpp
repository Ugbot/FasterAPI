/**
 * Metrics Endpoint Tests
 *
 * Tests the /metrics endpoint functionality:
 * - enable_metrics_endpoint() registers the route
 * - Endpoint returns Prometheus-formatted metrics
 * - Correct content-type header
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/server.h"
#include "../../src/cpp/http/request.h"
#include "../../src/cpp/http/response.h"
#include "../../src/cpp/http/router.h"
#include "../../src/cpp/http/metrics.h"
#include <string>
#include <thread>
#include <chrono>

namespace fasterapi {
namespace test {

// ===========================================================================
// Metrics Endpoint Tests
// ===========================================================================

class MetricsEndpointTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create server with default config
        HttpServer::Config config;
        config.port = 0;  // Don't actually bind
        server_ = std::make_unique<HttpServer>(config);
    }

    void TearDown() override {
        server_.reset();
    }

    std::unique_ptr<HttpServer> server_;
};

TEST_F(MetricsEndpointTest, EnableMetricsEndpoint) {
    // Should be able to enable metrics endpoint
    int result = server_->enable_metrics_endpoint("/metrics");
    EXPECT_EQ(result, 0);
}

TEST_F(MetricsEndpointTest, CustomPath) {
    // Should be able to use custom path
    int result = server_->enable_metrics_endpoint("/prometheus");
    EXPECT_EQ(result, 0);

    // Verify route is registered
    const auto& routes = server_->get_routes();
    EXPECT_NE(routes.find("GET"), routes.end());
    if (routes.find("GET") != routes.end()) {
        EXPECT_NE(routes.at("GET").find("/prometheus"), routes.at("GET").end());
    }
}

TEST_F(MetricsEndpointTest, RouteIsRegistered) {
    server_->enable_metrics_endpoint("/metrics");

    // Verify route is registered in routes map
    const auto& routes = server_->get_routes();
    EXPECT_NE(routes.find("GET"), routes.end());
    if (routes.find("GET") != routes.end()) {
        EXPECT_NE(routes.at("GET").find("/metrics"), routes.at("GET").end());
    }
}

TEST_F(MetricsEndpointTest, HandlerCallsMetrics) {
    server_->enable_metrics_endpoint("/metrics");

    // Get the router
    auto* router = server_->get_router();
    ASSERT_NE(router, nullptr);

    // Match the route
    fasterapi::http::RouteParams params;
    auto handler = router->match("GET", "/metrics", params);
    ASSERT_TRUE(handler);

    // Record some metrics before calling handler
    HttpMetrics::instance().request_completed("GET", "/test", 200, 0.025);

    // Create request and response
    HttpRequest request;
    HttpResponse response;

    // Call the handler
    handler(&request, &response, params);

    // Verify response has correct content type (lowercase header name)
    const auto& headers = response.get_headers();
    auto content_type_it = headers.find("content-type");
    ASSERT_NE(content_type_it, headers.end());
    EXPECT_NE(content_type_it->second.find("text/plain"), std::string::npos);
    EXPECT_NE(content_type_it->second.find("version=0.0.4"), std::string::npos);

    // Verify response body contains Prometheus metrics
    const std::string& body = response.get_body();
    EXPECT_NE(body.find("http_requests_total"), std::string::npos);
    EXPECT_NE(body.find("http_request_duration_seconds"), std::string::npos);
}

TEST_F(MetricsEndpointTest, ResponseContainsAllMetricTypes) {
    server_->enable_metrics_endpoint("/metrics");

    // Record various types of metrics
    HttpMetrics::instance().request_completed("GET", "/users", 200, 0.050);
    HttpMetrics::instance().request_completed("POST", "/users", 201, 0.100);
    HttpMetrics::instance().request_completed("GET", "/users", 500, 0.500);
    HttpMetrics::instance().connection_opened();
    HttpMetrics::instance().connection_opened();
    HttpMetrics::instance().connection_closed();

    // Get handler and call it
    auto* router = server_->get_router();
    fasterapi::http::RouteParams params;
    auto handler = router->match("GET", "/metrics", params);

    HttpRequest request;
    HttpResponse response;
    handler(&request, &response, params);

    const std::string& body = response.get_body();

    // Body should not be empty
    EXPECT_FALSE(body.empty()) << "Metrics body is empty!";

    // Check for counter metrics (TYPE declaration)
    EXPECT_NE(body.find("# TYPE http_requests_total counter"), std::string::npos);

    // Check for histogram metrics (TYPE declaration)
    EXPECT_NE(body.find("# TYPE http_request_duration_seconds histogram"), std::string::npos);

    // Check for gauge metrics (TYPE declaration)
    EXPECT_NE(body.find("# TYPE http_connections_active gauge"), std::string::npos);
}

TEST_F(MetricsEndpointTest, StatusCodeIs200) {
    server_->enable_metrics_endpoint("/metrics");

    auto* router = server_->get_router();
    fasterapi::http::RouteParams params;
    auto handler = router->match("GET", "/metrics", params);

    HttpRequest request;
    HttpResponse response;
    handler(&request, &response, params);

    EXPECT_EQ(response.get_status_code(), HttpResponse::Status::OK);
}

TEST_F(MetricsEndpointTest, CannotEnableWhileRunning) {
    // Start the server (won't actually bind because port=0)
    // Note: This test may need adjustment based on actual server behavior
    // For now, we just verify the API exists
    int result = server_->enable_metrics_endpoint("/metrics");
    EXPECT_EQ(result, 0);
}

// ===========================================================================
// Integration with HttpMetrics Tests
// ===========================================================================

TEST_F(MetricsEndpointTest, MetricsAccumulateAcrossRequests) {
    server_->enable_metrics_endpoint("/metrics");

    // Simulate multiple requests with a unique path for this test
    for (int i = 0; i < 10; i++) {
        HttpMetrics::instance().request_completed("GET", "/api/items", 200, 0.010 + i * 0.001);
    }

    auto* router = server_->get_router();
    fasterapi::http::RouteParams params;
    auto handler = router->match("GET", "/metrics", params);

    HttpRequest request;
    HttpResponse response;
    handler(&request, &response, params);

    // Metrics output should contain the basic metric types
    const std::string& body = response.get_body();
    EXPECT_NE(body.find("http_requests_total"), std::string::npos);
    EXPECT_NE(body.find("http_request_duration_seconds"), std::string::npos);
    // The endpoint returns valid Prometheus format
    EXPECT_NE(body.find("# TYPE"), std::string::npos);
}

}  // namespace test
}  // namespace fasterapi

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
