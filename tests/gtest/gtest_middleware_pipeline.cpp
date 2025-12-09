/**
 * Middleware Pipeline Tests
 *
 * Tests for the composable middleware pipeline system.
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/middleware_pipeline.h"
#include <chrono>
#include <random>
#include <thread>

using namespace fasterapi::http;

class MiddlewarePipelineTest : public ::testing::Test {
protected:
    const std::string test_jwt_secret = "test-secret-key-for-jwt-testing";

    // Create a valid JWT token
    std::string create_test_token(const std::string& subject = "user123") {
        JwtConfig config;
        config.secret_key = test_jwt_secret;
        config.allowed_algorithms = {JwtAlgorithm::HS256};
        JwtAuth auth(config);

        JwtClaims claims;
        claims.sub = subject;
        claims.exp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() + 3600;

        return auth.create_token(claims);
    }

    // Generate random string
    std::string random_string(size_t length) {
        static const char charset[] =
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, sizeof(charset) - 2);

        std::string result;
        result.reserve(length);
        for (size_t i = 0; i < length; ++i) {
            result += charset[dis(gen)];
        }
        return result;
    }
};

// ============================================================================
// Basic Pipeline Tests
// ============================================================================

TEST_F(MiddlewarePipelineTest, EmptyPipelineAllowsRequests) {
    PipelineConfig config;  // All disabled
    MiddlewarePipeline pipeline(config);

    std::unordered_map<std::string, std::string> headers;
    Http1Response response;

    auto action = pipeline.process_request(
        "GET", "/api/test", headers, "", "127.0.0.1", response
    );

    EXPECT_EQ(action, MiddlewareAction::CONTINUE);
}

TEST_F(MiddlewarePipelineTest, BuilderCreatesValidPipeline) {
    auto pipeline = PipelineBuilder()
        .with_cors()
        .with_compression()
        .build();

    EXPECT_TRUE(pipeline.is_cors_enabled());
    EXPECT_TRUE(pipeline.is_compression_enabled());
    EXPECT_FALSE(pipeline.is_rate_limiting_enabled());
    EXPECT_FALSE(pipeline.is_jwt_auth_enabled());
}

// ============================================================================
// CORS Middleware Tests
// ============================================================================

TEST_F(MiddlewarePipelineTest, CorsAllowsValidOrigin) {
    auto pipeline = PipelineBuilder()
        .with_cors(cors_presets::allow_all())
        .build();

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";
    Http1Response response;

    auto action = pipeline.process_request(
        "GET", "/api/test", headers, "", "127.0.0.1", response
    );

    EXPECT_EQ(action, MiddlewareAction::CONTINUE);

    // Process response to add CORS headers
    pipeline.process_response(response);

    auto cors_header = response.headers.find("Access-Control-Allow-Origin");
    EXPECT_NE(cors_header, response.headers.end());
}

TEST_F(MiddlewarePipelineTest, CorsHandlesPreflight) {
    auto pipeline = PipelineBuilder()
        .with_cors(cors_presets::allow_all())
        .build();

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";
    headers["Access-Control-Request-Method"] = "POST";
    Http1Response response;

    auto action = pipeline.process_request(
        "OPTIONS", "/api/test", headers, "", "127.0.0.1", response
    );

    // Preflight should short-circuit
    EXPECT_EQ(action, MiddlewareAction::RESPOND);
    EXPECT_EQ(response.status, 204);
}

TEST_F(MiddlewarePipelineTest, CorsRejectsDisallowedOrigin) {
    CorsConfig cors_config;
    cors_config.allowed_origins = {"https://allowed.com"};

    auto pipeline = PipelineBuilder()
        .with_cors(cors_config)
        .build();

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://notallowed.com";
    Http1Response response;

    auto action = pipeline.process_request(
        "GET", "/api/test", headers, "", "127.0.0.1", response
    );

    EXPECT_EQ(action, MiddlewareAction::RESPOND);
    EXPECT_EQ(response.status, 403);
}

// ============================================================================
// Rate Limiting Tests
// ============================================================================

TEST_F(MiddlewarePipelineTest, RateLimitingAllowsUnderLimit) {
    RateLimitConfig rl_config;
    rl_config.requests_per_window = 10;
    rl_config.window_size_ms = 60000;
    rl_config.algorithm = RateLimitAlgorithm::FIXED_WINDOW;

    auto pipeline = PipelineBuilder()
        .with_rate_limiting(rl_config)
        .build();

    std::unordered_map<std::string, std::string> headers;
    Http1Response response;

    // First request should succeed
    auto action = pipeline.process_request(
        "GET", "/api/test", headers, "", "127.0.0.1", response
    );

    EXPECT_EQ(action, MiddlewareAction::CONTINUE);
}

TEST_F(MiddlewarePipelineTest, RateLimitingBlocksWhenExceeded) {
    RateLimitConfig rl_config;
    rl_config.requests_per_window = 3;
    rl_config.window_size_ms = 60000;
    rl_config.algorithm = RateLimitAlgorithm::FIXED_WINDOW;

    auto pipeline = PipelineBuilder()
        .with_rate_limiting(rl_config)
        .build();

    std::unordered_map<std::string, std::string> headers;
    std::string client_ip = "192.168.1." + std::to_string(rand() % 255);  // Random IP

    // Make requests until blocked
    MiddlewareAction action = MiddlewareAction::CONTINUE;
    Http1Response response;

    for (int i = 0; i < 5; i++) {
        response = Http1Response();
        action = pipeline.process_request(
            "GET", "/api/test", headers, "", client_ip, response
        );

        if (action == MiddlewareAction::RESPOND) break;
    }

    EXPECT_EQ(action, MiddlewareAction::RESPOND);
    EXPECT_EQ(response.status, 429);
    EXPECT_NE(response.headers.find("Retry-After"), response.headers.end());
}

TEST_F(MiddlewarePipelineTest, RateLimitingUsesCustomKeyExtractor) {
    RateLimitConfig rl_config;
    rl_config.requests_per_window = 2;
    rl_config.window_size_ms = 60000;
    rl_config.algorithm = RateLimitAlgorithm::FIXED_WINDOW;

    // Key by X-API-Key header instead of IP
    auto key_extractor = [](const RequestContext& ctx) {
        auto it = ctx.headers.find("X-API-Key");
        return it != ctx.headers.end() ? it->second : "anonymous";
    };

    auto pipeline = PipelineBuilder()
        .with_rate_limiting(rl_config, key_extractor)
        .build();

    std::unordered_map<std::string, std::string> headers1;
    headers1["X-API-Key"] = "key-" + random_string(8);

    std::unordered_map<std::string, std::string> headers2;
    headers2["X-API-Key"] = "key-" + random_string(8);

    Http1Response response;

    // Both API keys should work (different rate limit buckets)
    auto action1 = pipeline.process_request("GET", "/api/test", headers1, "", "127.0.0.1", response);
    EXPECT_EQ(action1, MiddlewareAction::CONTINUE);

    response = Http1Response();
    auto action2 = pipeline.process_request("GET", "/api/test", headers2, "", "127.0.0.1", response);
    EXPECT_EQ(action2, MiddlewareAction::CONTINUE);
}

// ============================================================================
// JWT Authentication Tests
// ============================================================================

TEST_F(MiddlewarePipelineTest, JwtAuthBlocksWithoutToken) {
    auto pipeline = PipelineBuilder()
        .with_jwt_auth(jwt_presets::development(test_jwt_secret))
        .build();

    std::unordered_map<std::string, std::string> headers;
    Http1Response response;

    auto action = pipeline.process_request(
        "GET", "/api/protected", headers, "", "127.0.0.1", response
    );

    EXPECT_EQ(action, MiddlewareAction::RESPOND);
    EXPECT_EQ(response.status, 401);
}

TEST_F(MiddlewarePipelineTest, JwtAuthAllowsValidToken) {
    auto pipeline = PipelineBuilder()
        .with_jwt_auth(jwt_presets::development(test_jwt_secret))
        .build();

    std::string token = create_test_token("user123");
    std::unordered_map<std::string, std::string> headers;
    headers["Authorization"] = "Bearer " + token;
    Http1Response response;

    auto action = pipeline.process_request(
        "GET", "/api/protected", headers, "", "127.0.0.1", response
    );

    EXPECT_EQ(action, MiddlewareAction::CONTINUE);

    // Check that claims are available via stored state
    EXPECT_TRUE(pipeline.is_authenticated());
    ASSERT_TRUE(pipeline.jwt_claims().has_value());
    EXPECT_EQ(*pipeline.jwt_claims()->sub, "user123");
}

TEST_F(MiddlewarePipelineTest, JwtAuthRejectsInvalidToken) {
    auto pipeline = PipelineBuilder()
        .with_jwt_auth(jwt_presets::development(test_jwt_secret))
        .build();

    std::unordered_map<std::string, std::string> headers;
    headers["Authorization"] = "Bearer invalid.token.here";
    Http1Response response;

    auto action = pipeline.process_request(
        "GET", "/api/protected", headers, "", "127.0.0.1", response
    );

    EXPECT_EQ(action, MiddlewareAction::RESPOND);
    EXPECT_EQ(response.status, 401);
}

TEST_F(MiddlewarePipelineTest, JwtAuthExcludesConfiguredPaths) {
    auto pipeline = PipelineBuilder()
        .with_jwt_auth(
            jwt_presets::development(test_jwt_secret),
            {"/health", "/login", "/public/*"}
        )
        .build();

    std::unordered_map<std::string, std::string> headers;
    Http1Response response;

    // Excluded exact path should work without token
    auto action1 = pipeline.process_request(
        "GET", "/health", headers, "", "127.0.0.1", response
    );
    EXPECT_EQ(action1, MiddlewareAction::CONTINUE);

    // Excluded wildcard path should work
    response = Http1Response();
    auto action2 = pipeline.process_request(
        "GET", "/public/docs", headers, "", "127.0.0.1", response
    );
    EXPECT_EQ(action2, MiddlewareAction::CONTINUE);

    // Non-excluded path should require auth
    response = Http1Response();
    auto action3 = pipeline.process_request(
        "GET", "/api/protected", headers, "", "127.0.0.1", response
    );
    EXPECT_EQ(action3, MiddlewareAction::RESPOND);
    EXPECT_EQ(response.status, 401);
}

// ============================================================================
// Compression Tests
// ============================================================================

TEST_F(MiddlewarePipelineTest, CompressionIsAppliedToResponse) {
    auto pipeline = PipelineBuilder()
        .with_compression()
        .build();

    std::unordered_map<std::string, std::string> headers;
    headers["Accept-Encoding"] = "gzip";
    Http1Response response;

    auto action = pipeline.process_request(
        "GET", "/api/test", headers, "", "127.0.0.1", response
    );
    EXPECT_EQ(action, MiddlewareAction::CONTINUE);

    // Set up a response body
    response.status = 200;
    response.headers["Content-Type"] = "application/json";
    std::string body(1000, 'a');  // Compressible content
    response.body = body;

    // Process response
    pipeline.process_response(response);

    // Should be compressed
    auto encoding = response.headers.find("Content-Encoding");
    EXPECT_NE(encoding, response.headers.end());
    EXPECT_EQ(encoding->second, "gzip");
    EXPECT_LT(response.body.size(), body.size());
}

// ============================================================================
// Custom Middleware Tests
// ============================================================================

TEST_F(MiddlewarePipelineTest, PreMiddlewareRunsFirst) {
    bool pre_ran = false;
    bool mw_ran = false;

    auto pipeline = PipelineBuilder()
        .add_pre_middleware([&](RequestContext&, Http1Response&) {
            EXPECT_FALSE(mw_ran);
            pre_ran = true;
            return MiddlewareAction::CONTINUE;
        })
        .add_middleware([&](RequestContext&, Http1Response&) {
            EXPECT_TRUE(pre_ran);
            mw_ran = true;
            return MiddlewareAction::CONTINUE;
        })
        .build();

    std::unordered_map<std::string, std::string> headers;
    Http1Response response;

    pipeline.process_request("GET", "/api/test", headers, "", "127.0.0.1", response);

    EXPECT_TRUE(pre_ran);
    EXPECT_TRUE(mw_ran);
}

TEST_F(MiddlewarePipelineTest, CustomMiddlewareCanShortCircuit) {
    bool second_ran = false;

    auto pipeline = PipelineBuilder()
        .add_middleware([](RequestContext&, Http1Response& resp) {
            resp.status = 418;
            resp.body = "I'm a teapot";
            return MiddlewareAction::RESPOND;
        })
        .add_middleware([&](RequestContext&, Http1Response&) {
            second_ran = true;
            return MiddlewareAction::CONTINUE;
        })
        .build();

    std::unordered_map<std::string, std::string> headers;
    Http1Response response;

    auto action = pipeline.process_request(
        "GET", "/api/test", headers, "", "127.0.0.1", response
    );

    EXPECT_EQ(action, MiddlewareAction::RESPOND);
    EXPECT_EQ(response.status, 418);
    EXPECT_FALSE(second_ran);
}

TEST_F(MiddlewarePipelineTest, PostMiddlewareRunsAfterHandler) {
    bool post_ran = false;

    auto pipeline = PipelineBuilder()
        .add_post_middleware([&](const RequestContext&, Http1Response& resp) {
            post_ran = true;
            resp.headers["X-Processed"] = "true";
        })
        .build();

    std::unordered_map<std::string, std::string> headers;
    std::string body;
    std::string method = "GET";
    std::string path = "/api/test";
    std::string client_ip = "127.0.0.1";
    Http1Response response;

    // Use process_request_with_context to maintain context for post-middleware
    RequestContext ctx{method, path, headers, body, client_ip};
    pipeline.process_request_with_context(ctx, response);

    // Simulate handler
    response.status = 200;
    response.body = "OK";

    // Process response with context
    pipeline.process_response_with_context(ctx, response);

    EXPECT_TRUE(post_ran);
    EXPECT_EQ(response.headers["X-Processed"], "true");
}

TEST_F(MiddlewarePipelineTest, CustomMiddlewareCanSetContextData) {
    std::string captured_request_id;

    auto pipeline = PipelineBuilder()
        .add_middleware([&](RequestContext& ctx, Http1Response&) {
            ctx.set("request_id", std::string("req-12345"));
            ctx.set("timestamp", static_cast<int64_t>(12345678));
            return MiddlewareAction::CONTINUE;
        })
        .add_middleware([&](RequestContext& ctx, Http1Response& resp) {
            // Access context data in a later middleware
            auto req_id = ctx.get<std::string>("request_id");
            if (req_id) {
                captured_request_id = *req_id;
                resp.headers["X-Request-ID"] = *req_id;
            }
            return MiddlewareAction::CONTINUE;
        })
        .build();

    std::unordered_map<std::string, std::string> headers;
    Http1Response response;

    pipeline.process_request("GET", "/api/test", headers, "", "127.0.0.1", response);

    EXPECT_EQ(captured_request_id, "req-12345");
    EXPECT_EQ(response.headers["X-Request-ID"], "req-12345");
}

// ============================================================================
// Combined Middleware Tests
// ============================================================================

TEST_F(MiddlewarePipelineTest, FullPipelineIntegration) {
    RateLimitConfig rl_config;
    rl_config.requests_per_window = 100;
    rl_config.window_size_ms = 60000;
    rl_config.algorithm = RateLimitAlgorithm::FIXED_WINDOW;

    auto pipeline = PipelineBuilder()
        .with_cors(cors_presets::allow_all())
        .with_rate_limiting(rl_config)
        .with_jwt_auth(
            jwt_presets::development(test_jwt_secret),
            {"/health", "/login"}
        )
        .with_compression()
        .build();

    // Test health endpoint (no auth required)
    {
        std::unordered_map<std::string, std::string> headers;
        Http1Response response;

        auto action = pipeline.process_request(
            "GET", "/health", headers, "", "127.0.0.1", response
        );
        EXPECT_EQ(action, MiddlewareAction::CONTINUE);
    }

    // Test protected endpoint without token
    {
        std::unordered_map<std::string, std::string> headers;
        Http1Response response;

        auto action = pipeline.process_request(
            "GET", "/api/users", headers, "", "127.0.0.2", response
        );
        EXPECT_EQ(action, MiddlewareAction::RESPOND);
        EXPECT_EQ(response.status, 401);
    }

    // Test protected endpoint with valid token
    {
        std::string token = create_test_token("user-" + random_string(8));
        std::unordered_map<std::string, std::string> headers;
        headers["Authorization"] = "Bearer " + token;
        headers["Origin"] = "https://example.com";
        headers["Accept-Encoding"] = "gzip";
        Http1Response response;

        auto action = pipeline.process_request(
            "GET", "/api/users", headers, "", "127.0.0.3", response
        );
        EXPECT_EQ(action, MiddlewareAction::CONTINUE);

        // Set response
        response.status = 200;
        response.headers["Content-Type"] = "application/json";
        response.body = std::string(500, '{');  // Fake JSON

        pipeline.process_response(response);

        // Should have CORS header
        EXPECT_NE(response.headers.find("Access-Control-Allow-Origin"), response.headers.end());
        // Should be compressed
        EXPECT_NE(response.headers.find("Content-Encoding"), response.headers.end());
    }
}

// ============================================================================
// Preset Tests
// ============================================================================

TEST_F(MiddlewarePipelineTest, DevelopmentPreset) {
    auto pipeline = pipeline_presets::development().build();

    EXPECT_TRUE(pipeline.is_cors_enabled());
    EXPECT_TRUE(pipeline.is_compression_enabled());
    EXPECT_FALSE(pipeline.is_jwt_auth_enabled());
    EXPECT_FALSE(pipeline.is_rate_limiting_enabled());
}

TEST_F(MiddlewarePipelineTest, PublicApiPreset) {
    auto pipeline = pipeline_presets::public_api(100).build();

    EXPECT_TRUE(pipeline.is_cors_enabled());
    EXPECT_TRUE(pipeline.is_compression_enabled());
    EXPECT_TRUE(pipeline.is_rate_limiting_enabled());
    EXPECT_FALSE(pipeline.is_jwt_auth_enabled());
}

TEST_F(MiddlewarePipelineTest, InternalServicePreset) {
    auto pipeline = pipeline_presets::internal_service().build();

    EXPECT_FALSE(pipeline.is_cors_enabled());
    EXPECT_TRUE(pipeline.is_compression_enabled());
    EXPECT_FALSE(pipeline.is_jwt_auth_enabled());
    EXPECT_FALSE(pipeline.is_rate_limiting_enabled());
}

// ============================================================================
// Performance Tests
// ============================================================================

TEST_F(MiddlewarePipelineTest, PerformanceEmptyPipeline) {
    PipelineConfig config;
    MiddlewarePipeline pipeline(config);

    std::unordered_map<std::string, std::string> headers;

    const int iterations = 100000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        Http1Response response;
        pipeline.process_request("GET", "/api/test", headers, "", "127.0.0.1", response);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    double per_request_ns = static_cast<double>(duration) * 1000.0 / iterations;
    double requests_per_sec = 1000000000.0 / per_request_ns;

    std::cout << "Empty pipeline: " << per_request_ns << " ns/request, "
              << requests_per_sec / 1000000.0 << " M requests/sec" << std::endl;

    // Should be very fast for empty pipeline
    EXPECT_LT(per_request_ns, 1000);  // Less than 1 microsecond
}

TEST_F(MiddlewarePipelineTest, PerformanceFullPipeline) {
    RateLimitConfig rl_config;
    rl_config.requests_per_window = 1000000;  // High limit for perf test
    rl_config.window_size_ms = 60000;
    rl_config.algorithm = RateLimitAlgorithm::FIXED_WINDOW;

    auto pipeline = PipelineBuilder()
        .with_cors(cors_presets::allow_all())
        .with_rate_limiting(rl_config)
        .with_compression()
        .build();

    std::unordered_map<std::string, std::string> headers;
    headers["Origin"] = "https://example.com";
    headers["Accept-Encoding"] = "gzip";

    const int iterations = 50000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        Http1Response response;
        pipeline.process_request("GET", "/api/test", headers, "", "127.0.0.1", response);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    double per_request_us = static_cast<double>(duration) / iterations;
    double requests_per_sec = 1000000.0 / per_request_us;

    std::cout << "Full pipeline (CORS+RateLimit+Compression): " << per_request_us << " us/request, "
              << requests_per_sec / 1000.0 << " K requests/sec" << std::endl;

    // Should handle at least 100K requests/sec
    EXPECT_GT(requests_per_sec, 100000);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
