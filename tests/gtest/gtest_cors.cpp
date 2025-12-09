/**
 * CORS Middleware Tests
 *
 * Tests for Cross-Origin Resource Sharing handling.
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/cors.h"
#include <random>
#include <chrono>

using namespace fasterapi::http;

// =============================================================================
// Test Fixtures
// =============================================================================

class CorsTest : public ::testing::Test {
protected:
    void SetUp() override {
        gen_.seed(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    std::string random_origin() {
        return "https://random" + std::to_string(gen_()) + ".example.com";
    }

    std::mt19937 gen_;
};

// =============================================================================
// Basic CORS Tests
// =============================================================================

TEST_F(CorsTest, AllowsAllOriginsWithWildcard) {
    CorsConfig config;
    config.allowed_origins = {"*"};
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";

    auto result = cors.check("GET", headers);

    EXPECT_TRUE(result.allowed);
    EXPECT_EQ(result.headers["Access-Control-Allow-Origin"], "*");
}

TEST_F(CorsTest, AllowsSpecificOrigin) {
    CorsConfig config;
    config.allowed_origins = {"https://example.com"};
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";

    auto result = cors.check("GET", headers);

    EXPECT_TRUE(result.allowed);
    EXPECT_EQ(result.headers["Access-Control-Allow-Origin"], "https://example.com");
}

TEST_F(CorsTest, RejectsUnallowedOrigin) {
    CorsConfig config;
    config.allowed_origins = {"https://allowed.com"};
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://notallowed.com";

    auto result = cors.check("GET", headers);

    EXPECT_FALSE(result.allowed);
    EXPECT_EQ(result.headers.count("Access-Control-Allow-Origin"), 0);
}

TEST_F(CorsTest, AllowsMultipleOrigins) {
    CorsConfig config;
    config.allowed_origins = {
        "https://example1.com",
        "https://example2.com",
        "https://example3.com"
    };
    CorsMiddleware cors(config);

    // Test each allowed origin
    for (const auto& origin : config.allowed_origins) {
        std::unordered_map<std::string, std::string> headers;
        headers["Origin"] = origin;

        auto result = cors.check("GET", headers);
        EXPECT_TRUE(result.allowed) << "Origin " << origin << " should be allowed";
        EXPECT_EQ(result.headers["Access-Control-Allow-Origin"], origin);
    }

    // Test disallowed origin
    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://notlisted.com";
    auto result = cors.check("GET", headers);
    EXPECT_FALSE(result.allowed);
}

TEST_F(CorsTest, AllowsWildcardSubdomains) {
    CorsConfig config;
    config.allowed_origins = {"*.example.com"};
    CorsMiddleware cors(config);

    // Should allow subdomains
    std::vector<std::string> allowed = {
        "https://api.example.com",
        "https://www.example.com",
        "https://subdomain.example.com"
    };

    for (const auto& origin : allowed) {
        std::unordered_map<std::string, std::string> headers;
        headers["Origin"] = origin;
        auto result = cors.check("GET", headers);
        EXPECT_TRUE(result.allowed) << "Origin " << origin << " should be allowed";
    }

    // Should NOT allow unrelated domains
    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://notexample.com";
    auto result = cors.check("GET", headers);
    EXPECT_FALSE(result.allowed);
}

TEST_F(CorsTest, NoOriginIsNotCors) {
    CorsConfig config;
    config.allowed_origins = {"https://example.com"};
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    // No Origin header

    auto result = cors.check("GET", headers);

    EXPECT_TRUE(result.allowed);  // Same-origin allowed by default
    EXPECT_EQ(result.request_type, CorsRequestType::NOT_CORS);
}

TEST_F(CorsTest, NoOriginCanBeRejected) {
    CorsConfig config;
    config.allowed_origins = {"https://example.com"};
    config.allow_no_origin = false;
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    // No Origin header

    auto result = cors.check("GET", headers);

    EXPECT_FALSE(result.allowed);
}

// =============================================================================
// Preflight Tests
// =============================================================================

TEST_F(CorsTest, HandlesPreflight) {
    CorsConfig config;
    config.allowed_origins = {"https://example.com"};
    config.allowed_methods = {"GET", "POST", "PUT"};
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";
    headers["Access-Control-Request-Method"] = "PUT";

    auto result = cors.check("OPTIONS", headers);

    EXPECT_TRUE(result.allowed);
    EXPECT_TRUE(result.is_preflight);
    EXPECT_EQ(result.request_type, CorsRequestType::PREFLIGHT);
    EXPECT_TRUE(result.headers.count("Access-Control-Allow-Methods") > 0);
}

TEST_F(CorsTest, PreflightRejectsUnallowedMethod) {
    CorsConfig config;
    config.allowed_origins = {"https://example.com"};
    config.allowed_methods = {"GET", "POST"};  // No DELETE
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";
    headers["Access-Control-Request-Method"] = "DELETE";

    auto result = cors.check("OPTIONS", headers);

    EXPECT_FALSE(result.allowed);
}

TEST_F(CorsTest, PreflightAllowsHeaders) {
    CorsConfig config;
    config.allowed_origins = {"https://example.com"};
    config.allowed_headers = {"Content-Type", "Authorization"};
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";
    headers["Access-Control-Request-Method"] = "POST";
    headers["Access-Control-Request-Headers"] = "Content-Type, Authorization";

    auto result = cors.check("OPTIONS", headers);

    EXPECT_TRUE(result.allowed);
}

TEST_F(CorsTest, PreflightRejectsUnallowedHeader) {
    CorsConfig config;
    config.allowed_origins = {"https://example.com"};
    config.allowed_headers = {"Content-Type"};  // No X-Custom
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";
    headers["Access-Control-Request-Method"] = "POST";
    headers["Access-Control-Request-Headers"] = "X-Custom-Header";

    auto result = cors.check("OPTIONS", headers);

    EXPECT_FALSE(result.allowed);
}

TEST_F(CorsTest, PreflightHeadersCaseInsensitive) {
    CorsConfig config;
    config.allowed_origins = {"https://example.com"};
    config.allowed_headers = {"Content-Type", "Authorization"};
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";
    headers["Access-Control-Request-Method"] = "POST";
    headers["Access-Control-Request-Headers"] = "content-type, AUTHORIZATION";  // Different case

    auto result = cors.check("OPTIONS", headers);

    EXPECT_TRUE(result.allowed);
}

TEST_F(CorsTest, PreflightIncludesMaxAge) {
    CorsConfig config;
    config.allowed_origins = {"https://example.com"};
    config.max_age = 3600;
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";
    headers["Access-Control-Request-Method"] = "GET";

    auto result = cors.check("OPTIONS", headers);

    EXPECT_TRUE(result.allowed);
    EXPECT_EQ(result.headers["Access-Control-Max-Age"], "3600");
}

TEST_F(CorsTest, PreflightOmitsNegativeMaxAge) {
    CorsConfig config;
    config.allowed_origins = {"https://example.com"};
    config.max_age = -1;  // Don't include
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";
    headers["Access-Control-Request-Method"] = "GET";

    auto result = cors.check("OPTIONS", headers);

    EXPECT_EQ(result.headers.count("Access-Control-Max-Age"), 0);
}

// =============================================================================
// Credentials Tests
// =============================================================================

TEST_F(CorsTest, CredentialsIncludesHeader) {
    CorsConfig config;
    config.allowed_origins = {"https://example.com"};
    config.allow_credentials = true;
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";

    auto result = cors.check("GET", headers);

    EXPECT_TRUE(result.allowed);
    EXPECT_EQ(result.headers["Access-Control-Allow-Credentials"], "true");
}

TEST_F(CorsTest, CredentialsNotWithWildcard) {
    CorsConfig config;
    config.allowed_origins = {"*"};
    config.allow_credentials = true;  // This combination should use specific origin
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";

    auto result = cors.check("GET", headers);

    // With credentials, should echo origin instead of *
    EXPECT_EQ(result.headers["Access-Control-Allow-Origin"], "https://example.com");
    EXPECT_EQ(result.headers["Access-Control-Allow-Credentials"], "true");
}

TEST_F(CorsTest, NoCredentialsNoHeader) {
    CorsConfig config;
    config.allowed_origins = {"https://example.com"};
    config.allow_credentials = false;
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";

    auto result = cors.check("GET", headers);

    EXPECT_EQ(result.headers.count("Access-Control-Allow-Credentials"), 0);
}

// =============================================================================
// Expose Headers Tests
// =============================================================================

TEST_F(CorsTest, ExposesCustomHeaders) {
    CorsConfig config;
    config.allowed_origins = {"https://example.com"};
    config.expose_headers = {"X-Custom-Header", "X-Another-Header"};
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";

    auto result = cors.check("GET", headers);

    EXPECT_TRUE(result.headers.count("Access-Control-Expose-Headers") > 0);
    EXPECT_TRUE(result.headers["Access-Control-Expose-Headers"].find("X-Custom-Header") != std::string::npos);
    EXPECT_TRUE(result.headers["Access-Control-Expose-Headers"].find("X-Another-Header") != std::string::npos);
}

TEST_F(CorsTest, NoExposeHeadersIfEmpty) {
    CorsConfig config;
    config.allowed_origins = {"https://example.com"};
    config.expose_headers = {};
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";

    auto result = cors.check("GET", headers);

    EXPECT_EQ(result.headers.count("Access-Control-Expose-Headers"), 0);
}

// =============================================================================
// Custom Origin Validator Tests
// =============================================================================

TEST_F(CorsTest, CustomValidatorAllows) {
    CorsConfig config;
    config.allowed_origins = {};  // No static origins
    config.origin_validator = [](const std::string& origin) {
        // Allow anything from localhost
        return origin.find("localhost") != std::string::npos;
    };
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "http://localhost:3000";

    auto result = cors.check("GET", headers);

    EXPECT_TRUE(result.allowed);
}

TEST_F(CorsTest, CustomValidatorRejects) {
    CorsConfig config;
    config.allowed_origins = {};
    config.origin_validator = [](const std::string& origin) {
        return origin.find("localhost") != std::string::npos;
    };
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://malicious.com";

    auto result = cors.check("GET", headers);

    EXPECT_FALSE(result.allowed);
}

// =============================================================================
// Response Building Tests
// =============================================================================

TEST_F(CorsTest, ApplyHeadersToResponse) {
    CorsConfig config;
    config.allowed_origins = {"https://example.com"};
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Origin"] = "https://example.com";

    auto result = cors.check("GET", req_headers);

    Http1Response response;
    response.status = 200;
    response.body = "OK";

    CorsMiddleware::apply_headers(result, response);

    EXPECT_EQ(response.headers["Access-Control-Allow-Origin"], "https://example.com");
}

TEST_F(CorsTest, BuildPreflightResponse) {
    CorsConfig config;
    config.allowed_origins = {"https://example.com"};
    config.allowed_methods = {"GET", "POST"};
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";
    headers["Access-Control-Request-Method"] = "POST";

    auto result = cors.check("OPTIONS", headers);
    auto response = CorsMiddleware::build_preflight_response(result);

    EXPECT_EQ(response.status, 204);
    EXPECT_TRUE(response.headers.count("Access-Control-Allow-Origin") > 0);
    EXPECT_TRUE(response.headers.count("Access-Control-Allow-Methods") > 0);
}

// =============================================================================
// Request Type Detection Tests
// =============================================================================

TEST_F(CorsTest, DetectsNotCors) {
    CorsMiddleware cors;

    std::unordered_map<std::string, std::string> headers;
    // No Origin

    auto result = cors.check("GET", headers);

    EXPECT_EQ(result.request_type, CorsRequestType::NOT_CORS);
}

TEST_F(CorsTest, DetectsSimplePreflight) {
    CorsMiddleware cors;

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";
    headers["Access-Control-Request-Method"] = "GET";

    auto result = cors.check("OPTIONS", headers);

    EXPECT_EQ(result.request_type, CorsRequestType::PREFLIGHT);
}

TEST_F(CorsTest, DetectsActualRequest) {
    CorsMiddleware cors;

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";

    auto result = cors.check("GET", headers);

    EXPECT_EQ(result.request_type, CorsRequestType::ACTUAL);
}

// =============================================================================
// Preset Tests
// =============================================================================

TEST_F(CorsTest, AllowAllPreset) {
    auto config = cors_presets::allow_all();
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = random_origin();

    auto result = cors.check("GET", headers);

    EXPECT_TRUE(result.allowed);
    EXPECT_EQ(result.headers["Access-Control-Allow-Origin"], "*");
    EXPECT_EQ(result.headers.count("Access-Control-Allow-Credentials"), 0);
}

TEST_F(CorsTest, SingleOriginPreset) {
    auto config = cors_presets::single_origin("https://myapp.com", true);
    CorsMiddleware cors(config);

    // Allowed origin
    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://myapp.com";
    auto result = cors.check("GET", headers);
    EXPECT_TRUE(result.allowed);
    EXPECT_EQ(result.headers["Access-Control-Allow-Origin"], "https://myapp.com");
    EXPECT_EQ(result.headers["Access-Control-Allow-Credentials"], "true");

    // Other origin
    headers["Origin"] = "https://other.com";
    result = cors.check("GET", headers);
    EXPECT_FALSE(result.allowed);
}

TEST_F(CorsTest, MultipleOriginsPreset) {
    auto config = cors_presets::multiple_origins({
        "https://app1.com",
        "https://app2.com"
    }, true);
    CorsMiddleware cors(config);

    for (const auto& origin : {"https://app1.com", "https://app2.com"}) {
        std::unordered_map<std::string, std::string> headers;
        headers["Origin"] = origin;
        auto result = cors.check("GET", headers);
        EXPECT_TRUE(result.allowed);
    }
}

TEST_F(CorsTest, SameOriginOnlyPreset) {
    auto config = cors_presets::same_origin_only();
    CorsMiddleware cors(config);

    // No origin (same-origin)
    std::unordered_map<std::string, std::string> headers;
    auto result = cors.check("GET", headers);
    EXPECT_TRUE(result.allowed);

    // With origin (cross-origin)
    headers["Origin"] = "https://other.com";
    result = cors.check("GET", headers);
    EXPECT_FALSE(result.allowed);
}

// =============================================================================
// Case Sensitivity Tests
// =============================================================================

TEST_F(CorsTest, OriginHeaderCaseInsensitive) {
    CorsConfig config;
    config.allowed_origins = {"https://example.com"};
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["origin"] = "https://example.com";  // lowercase header name

    auto result = cors.check("GET", headers);

    EXPECT_TRUE(result.allowed);
}

TEST_F(CorsTest, MethodCaseSensitive) {
    CorsConfig config;
    config.allowed_origins = {"https://example.com"};
    config.allowed_methods = {"GET", "POST"};  // Uppercase
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";
    headers["Access-Control-Request-Method"] = "get";  // lowercase

    auto result = cors.check("OPTIONS", headers);

    // Methods should be case-sensitive per spec
    EXPECT_FALSE(result.allowed);
}

// =============================================================================
// Performance Tests
// =============================================================================

TEST_F(CorsTest, PerformanceSimpleCheck) {
    CorsConfig config;
    config.allowed_origins = {"https://example.com"};
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";

    auto start = std::chrono::high_resolution_clock::now();
    const int iterations = 100000;

    for (int i = 0; i < iterations; i++) {
        cors.check("GET", headers);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    double ns_per_check = (double(duration_us) * 1000) / iterations;
    double checks_per_second = 1000000000.0 / ns_per_check;

    std::cout << "CORS check: " << ns_per_check << " ns/check, "
              << (checks_per_second / 1000000) << " M checks/sec\n";

    // Should be able to do at least 1M checks/second
    EXPECT_GT(checks_per_second, 1000000);
}

TEST_F(CorsTest, PerformancePreflightCheck) {
    CorsConfig config;
    config.allowed_origins = {"https://example.com"};
    config.allowed_methods = {"GET", "POST", "PUT", "DELETE"};
    config.allowed_headers = {"Content-Type", "Authorization", "X-Custom"};
    CorsMiddleware cors(config);

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";
    headers["Access-Control-Request-Method"] = "PUT";
    headers["Access-Control-Request-Headers"] = "Content-Type, Authorization";

    auto start = std::chrono::high_resolution_clock::now();
    const int iterations = 100000;

    for (int i = 0; i < iterations; i++) {
        cors.check("OPTIONS", headers);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    double ns_per_check = (double(duration_us) * 1000) / iterations;
    double checks_per_second = 1000000000.0 / ns_per_check;

    std::cout << "CORS preflight: " << ns_per_check << " ns/check, "
              << (checks_per_second / 1000000) << " M checks/sec\n";

    // Preflight is more complex but should still be fast
    EXPECT_GT(checks_per_second, 500000);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
