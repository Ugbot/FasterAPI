/**
 * Composable Middleware Pipeline
 *
 * A flexible, high-performance middleware system that allows chaining
 * multiple middleware components in a configurable order.
 *
 * Features:
 * - Optional middleware - enable/disable individual components
 * - Composable - chain middleware in any order
 * - Short-circuit support - early termination (auth, rate limit)
 * - Context passing - share data between middleware
 * - Type-safe configuration
 * - Zero-cost abstractions when middleware is disabled
 */

#pragma once

#include "http1_connection.h"
#include "cors.h"
#include "rate_limiter.h"
#include "jwt_auth.h"
#include "compression_middleware.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <functional>
#include <optional>
#include <memory>
#include <any>

namespace fasterapi {
namespace http {

/**
 * Middleware execution result
 */
enum class MiddlewareAction {
    CONTINUE,       // Continue to next middleware
    RESPOND,        // Send response and stop (short-circuit)
    SKIP_HANDLER    // Skip route handler but continue response processing
};

/**
 * Request context passed through middleware chain
 */
struct RequestContext {
    // Request info (read-only)
    const std::string& method;
    const std::string& path;
    const std::unordered_map<std::string, std::string>& headers;
    const std::string& body;
    std::string client_ip;

    // Middleware can add context data
    std::unordered_map<std::string, std::any> data;

    // JWT claims (populated by JWT middleware if enabled)
    std::optional<JwtClaims> jwt_claims;
    bool authenticated{false};

    // Rate limit info
    std::optional<RateLimitResult> rate_limit_result;

    // CORS result
    std::optional<CorsResult> cors_result;

    /**
     * Get typed context data
     */
    template<typename T>
    std::optional<T> get(const std::string& key) const {
        auto it = data.find(key);
        if (it != data.end()) {
            try {
                return std::any_cast<T>(it->second);
            } catch (...) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    /**
     * Set context data
     */
    template<typename T>
    void set(const std::string& key, T value) {
        data[key] = std::move(value);
    }
};

/**
 * Middleware function type
 */
using MiddlewareFunc = std::function<MiddlewareAction(
    RequestContext& ctx,
    Http1Response& response
)>;

/**
 * Response middleware function type (runs after handler)
 */
using ResponseMiddlewareFunc = std::function<void(
    const RequestContext& ctx,
    Http1Response& response
)>;

/**
 * Pipeline configuration
 */
struct PipelineConfig {
    // CORS configuration (optional)
    bool enable_cors{false};
    CorsConfig cors_config;

    // Rate limiting (optional)
    bool enable_rate_limiting{false};
    RateLimitConfig rate_limit_config;
    std::function<std::string(const RequestContext&)> rate_limit_key_extractor;

    // JWT authentication (optional)
    bool enable_jwt_auth{false};
    JwtConfig jwt_config;
    std::vector<std::string> jwt_excluded_paths;  // Paths that don't require auth

    // Response compression (optional)
    bool enable_compression{false};
    CompressionConfig compression_config;

    // Custom middleware (runs in order specified)
    std::vector<MiddlewareFunc> pre_middleware;     // Before built-in middleware
    std::vector<MiddlewareFunc> middleware;          // After built-in, before handler
    std::vector<ResponseMiddlewareFunc> post_middleware;  // After handler

    PipelineConfig() = default;
};

/**
 * Middleware Pipeline
 *
 * Processes requests through a chain of middleware, with support for
 * short-circuiting and response processing.
 *
 * Usage:
 *   PipelineConfig config;
 *   config.enable_cors = true;
 *   config.cors_config = cors_presets::allow_all();
 *   config.enable_compression = true;
 *
 *   MiddlewarePipeline pipeline(config);
 *
 *   // Process request
 *   Http1Response response;
 *   auto action = pipeline.process_request(method, path, headers, body, ip, response);
 *   if (action == MiddlewareAction::RESPOND) {
 *       // Send response immediately (auth failed, rate limited, etc.)
 *       return response;
 *   }
 *
 *   // Call route handler...
 *   handler(request, response);
 *
 *   // Process response (add headers, compress, etc.)
 *   pipeline.process_response(ctx, response);
 */
class MiddlewarePipeline {
public:
    /**
     * Create pipeline with configuration
     */
    explicit MiddlewarePipeline(const PipelineConfig& config)
        : config_(config) {
        initialize();
    }

    /**
     * Process incoming request through middleware chain
     *
     * @param method HTTP method
     * @param path Request path
     * @param headers Request headers
     * @param body Request body
     * @param client_ip Client IP address
     * @param response Response to populate (if short-circuiting)
     * @return Action indicating whether to continue, respond immediately, etc.
     */
    MiddlewareAction process_request(
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body,
        const std::string& client_ip,
        Http1Response& response
    ) {
        // Store headers for response processing (compression needs Accept-Encoding)
        last_request_headers_ = &headers;

        // Reset stored state
        stored_cors_result_.reset();
        stored_jwt_claims_.reset();
        stored_authenticated_ = false;

        // Create context (references are valid for duration of call)
        RequestContext ctx{method, path, headers, body, client_ip};

        auto result = process_request_with_context(ctx, response);

        // Store results for later use in process_response
        stored_cors_result_ = ctx.cors_result;
        stored_jwt_claims_ = ctx.jwt_claims;
        stored_authenticated_ = ctx.authenticated;

        // Store context pointer only if user needs it (for last_context())
        // Note: This pointer becomes invalid after process_request returns
        // Use stored_* fields instead in process_response
        last_context_ = nullptr;

        return result;
    }

    /**
     * Process request with existing context
     *
     * Note: When using this method, you must also use process_response_with_context()
     * to ensure the context is properly used for response processing.
     */
    MiddlewareAction process_request_with_context(
        RequestContext& ctx,
        Http1Response& response
    ) {
        // Store context for response processing (caller must ensure lifetime)
        last_context_ = &ctx;
        last_request_headers_ = &ctx.headers;

        // Run pre-middleware
        for (const auto& mw : config_.pre_middleware) {
            auto action = mw(ctx, response);
            if (action != MiddlewareAction::CONTINUE) {
                return action;
            }
        }

        // CORS (must run early)
        if (config_.enable_cors) {
            auto action = process_cors(ctx, response);
            if (action != MiddlewareAction::CONTINUE) {
                return action;
            }
        }

        // Rate limiting
        if (config_.enable_rate_limiting) {
            auto action = process_rate_limit(ctx, response);
            if (action != MiddlewareAction::CONTINUE) {
                return action;
            }
        }

        // JWT authentication
        if (config_.enable_jwt_auth) {
            auto action = process_jwt_auth(ctx, response);
            if (action != MiddlewareAction::CONTINUE) {
                return action;
            }
        }

        // Custom middleware
        for (const auto& mw : config_.middleware) {
            auto action = mw(ctx, response);
            if (action != MiddlewareAction::CONTINUE) {
                return action;
            }
        }

        return MiddlewareAction::CONTINUE;
    }

    /**
     * Process response after handler
     *
     * This adds CORS headers, compresses response, etc.
     * Uses stored state from the last process_request() call.
     *
     * @param response Response from handler
     */
    void process_response(Http1Response& response) {
        // Apply CORS headers from stored result
        if (config_.enable_cors && stored_cors_result_) {
            CorsMiddleware::apply_headers(*stored_cors_result_, response);
        }

        // Compression (runs last to compress final response)
        if (config_.enable_compression && compression_middleware_ && last_request_headers_) {
            compression_middleware_->apply(*last_request_headers_, response);
        }
    }

    /**
     * Process response with context
     *
     * Use this when you called process_request_with_context() and have
     * maintained the context's lifetime.
     */
    void process_response_with_context(
        const RequestContext& ctx,
        Http1Response& response
    ) {
        // Apply CORS headers
        if (config_.enable_cors && ctx.cors_result) {
            CorsMiddleware::apply_headers(*ctx.cors_result, response);
        }

        // Run post-middleware (e.g., logging)
        for (const auto& mw : config_.post_middleware) {
            mw(ctx, response);
        }

        // Compression (runs last to compress final response)
        if (config_.enable_compression && compression_middleware_) {
            compression_middleware_->apply(ctx.headers, response);
        }
    }

    /**
     * Get stored JWT claims from last request
     */
    const std::optional<JwtClaims>& jwt_claims() const {
        return stored_jwt_claims_;
    }

    /**
     * Check if last request was authenticated
     */
    bool is_authenticated() const {
        return stored_authenticated_;
    }

    /**
     * Get the full context after processing
     *
     * WARNING: Only valid when using process_request_with_context() and
     * the original context is still in scope.
     */
    const RequestContext* last_context() const {
        return last_context_;
    }

    /**
     * Update configuration
     */
    void set_config(const PipelineConfig& config) {
        config_ = config;
        initialize();
    }

    /**
     * Get current configuration
     */
    const PipelineConfig& config() const {
        return config_;
    }

    /**
     * Check if a specific middleware is enabled
     */
    bool is_cors_enabled() const { return config_.enable_cors; }
    bool is_rate_limiting_enabled() const { return config_.enable_rate_limiting; }
    bool is_jwt_auth_enabled() const { return config_.enable_jwt_auth; }
    bool is_compression_enabled() const { return config_.enable_compression; }

private:
    PipelineConfig config_;
    RequestContext* last_context_{nullptr};
    const std::unordered_map<std::string, std::string>* last_request_headers_{nullptr};

    // Stored state from last request (for process_response)
    std::optional<CorsResult> stored_cors_result_;
    std::optional<JwtClaims> stored_jwt_claims_;
    bool stored_authenticated_{false};

    // Middleware instances (created lazily based on config)
    std::unique_ptr<CorsMiddleware> cors_middleware_;
    std::unique_ptr<RateLimiter> rate_limiter_;
    std::unique_ptr<JwtAuth> jwt_auth_;
    std::unique_ptr<CompressionMiddleware> compression_middleware_;

    /**
     * Initialize middleware instances based on config
     */
    void initialize() {
        if (config_.enable_cors) {
            cors_middleware_ = std::make_unique<CorsMiddleware>(config_.cors_config);
        } else {
            cors_middleware_.reset();
        }

        if (config_.enable_rate_limiting) {
            rate_limiter_ = std::make_unique<RateLimiter>(config_.rate_limit_config);
        } else {
            rate_limiter_.reset();
        }

        if (config_.enable_jwt_auth) {
            jwt_auth_ = std::make_unique<JwtAuth>(config_.jwt_config);
        } else {
            jwt_auth_.reset();
        }

        if (config_.enable_compression) {
            compression_middleware_ = std::make_unique<CompressionMiddleware>(config_.compression_config);
        } else {
            compression_middleware_.reset();
        }
    }

    /**
     * Process CORS
     */
    MiddlewareAction process_cors(RequestContext& ctx, Http1Response& response) {
        if (!cors_middleware_) return MiddlewareAction::CONTINUE;

        auto result = cors_middleware_->check(ctx.method, ctx.headers);
        ctx.cors_result = result;

        // Handle preflight
        if (result.is_preflight) {
            if (result.allowed) {
                response = CorsMiddleware::build_preflight_response(result);
            } else {
                response.status = 403;
                response.status_message = "Forbidden";
                response.body = "CORS preflight check failed";
            }
            return MiddlewareAction::RESPOND;
        }

        // Reject disallowed origins
        if (!result.allowed) {
            response.status = 403;
            response.status_message = "Forbidden";
            response.body = "Origin not allowed";
            return MiddlewareAction::RESPOND;
        }

        return MiddlewareAction::CONTINUE;
    }

    /**
     * Process rate limiting
     */
    MiddlewareAction process_rate_limit(RequestContext& ctx, Http1Response& response) {
        if (!rate_limiter_) return MiddlewareAction::CONTINUE;

        // Get rate limit key
        std::string key;
        if (config_.rate_limit_key_extractor) {
            key = config_.rate_limit_key_extractor(ctx);
        } else {
            key = ctx.client_ip;  // Default to IP-based
        }

        auto result = rate_limiter_->check(key);
        ctx.rate_limit_result = result;

        if (!result.allowed) {
            response.status = 429;
            response.status_message = "Too Many Requests";
            response.headers["Retry-After"] = std::to_string(result.retry_after_ms / 1000);
            response.headers["X-RateLimit-Limit"] = std::to_string(result.limit);
            response.headers["X-RateLimit-Remaining"] = "0";
            response.body = "{\"error\":\"rate_limit_exceeded\",\"retry_after_ms\":" +
                           std::to_string(result.retry_after_ms) + "}";
            response.headers["Content-Type"] = "application/json";
            return MiddlewareAction::RESPOND;
        }

        // Add rate limit headers
        response.headers["X-RateLimit-Limit"] = std::to_string(result.limit);
        response.headers["X-RateLimit-Remaining"] = std::to_string(result.remaining);

        return MiddlewareAction::CONTINUE;
    }

    /**
     * Process JWT authentication
     */
    MiddlewareAction process_jwt_auth(RequestContext& ctx, Http1Response& response) {
        if (!jwt_auth_) return MiddlewareAction::CONTINUE;

        // Check if path is excluded from auth
        for (const auto& excluded : config_.jwt_excluded_paths) {
            if (ctx.path == excluded ||
                (excluded.back() == '*' &&
                 ctx.path.compare(0, excluded.size() - 1, excluded, 0, excluded.size() - 1) == 0)) {
                return MiddlewareAction::CONTINUE;
            }
        }

        auto result = jwt_auth_->check_request(ctx.headers);

        if (!result.valid) {
            response = jwt_auth_->build_unauthorized_response(result);
            return MiddlewareAction::RESPOND;
        }

        ctx.jwt_claims = result.claims;
        ctx.authenticated = true;

        return MiddlewareAction::CONTINUE;
    }
};

/**
 * Pipeline builder for fluent configuration
 */
class PipelineBuilder {
public:
    PipelineBuilder() = default;

    /**
     * Enable CORS with configuration
     */
    PipelineBuilder& with_cors(const CorsConfig& config = CorsConfig()) {
        config_.enable_cors = true;
        config_.cors_config = config;
        return *this;
    }

    /**
     * Enable rate limiting
     */
    PipelineBuilder& with_rate_limiting(
        const RateLimitConfig& config,
        std::function<std::string(const RequestContext&)> key_extractor = nullptr
    ) {
        config_.enable_rate_limiting = true;
        config_.rate_limit_config = config;
        config_.rate_limit_key_extractor = std::move(key_extractor);
        return *this;
    }

    /**
     * Enable JWT authentication
     */
    PipelineBuilder& with_jwt_auth(
        const JwtConfig& config,
        const std::vector<std::string>& excluded_paths = {}
    ) {
        config_.enable_jwt_auth = true;
        config_.jwt_config = config;
        config_.jwt_excluded_paths = excluded_paths;
        return *this;
    }

    /**
     * Enable response compression
     */
    PipelineBuilder& with_compression(const CompressionConfig& config = CompressionConfig()) {
        config_.enable_compression = true;
        config_.compression_config = config;
        return *this;
    }

    /**
     * Add custom pre-middleware (runs before built-in middleware)
     */
    PipelineBuilder& add_pre_middleware(MiddlewareFunc mw) {
        config_.pre_middleware.push_back(std::move(mw));
        return *this;
    }

    /**
     * Add custom middleware (runs after built-in, before handler)
     */
    PipelineBuilder& add_middleware(MiddlewareFunc mw) {
        config_.middleware.push_back(std::move(mw));
        return *this;
    }

    /**
     * Add custom post-middleware (runs after handler)
     */
    PipelineBuilder& add_post_middleware(ResponseMiddlewareFunc mw) {
        config_.post_middleware.push_back(std::move(mw));
        return *this;
    }

    /**
     * Build the pipeline
     */
    MiddlewarePipeline build() {
        return MiddlewarePipeline(config_);
    }

    /**
     * Build as unique_ptr
     */
    std::unique_ptr<MiddlewarePipeline> build_ptr() {
        return std::make_unique<MiddlewarePipeline>(config_);
    }

    /**
     * Get configuration
     */
    const PipelineConfig& config() const {
        return config_;
    }

private:
    PipelineConfig config_;
};

/**
 * Common pipeline presets
 */
namespace pipeline_presets {

/**
 * Development preset - permissive CORS, no auth
 */
inline PipelineBuilder development() {
    return PipelineBuilder()
        .with_cors(cors_presets::allow_all())
        .with_compression();
}

/**
 * Production API preset - strict security
 */
inline PipelineBuilder production_api(
    const std::string& jwt_secret,
    const std::string& jwt_issuer,
    const std::vector<std::string>& allowed_origins,
    uint32_t rate_limit_per_minute = 60
) {
    RateLimitConfig rl_config;
    rl_config.requests_per_window = rate_limit_per_minute;
    rl_config.window_size_ms = 60000;
    rl_config.algorithm = RateLimitAlgorithm::SLIDING_WINDOW;

    return PipelineBuilder()
        .with_cors(cors_presets::multiple_origins(allowed_origins, true))
        .with_rate_limiting(rl_config)
        .with_jwt_auth(
            jwt_presets::production_hs256(jwt_secret, jwt_issuer, {}),
            {"/health", "/login", "/register"}
        )
        .with_compression();
}

/**
 * Public API preset - rate limiting and compression, no auth
 */
inline PipelineBuilder public_api(
    uint32_t rate_limit_per_minute = 100
) {
    RateLimitConfig rl_config;
    rl_config.requests_per_window = rate_limit_per_minute;
    rl_config.window_size_ms = 60000;
    rl_config.algorithm = RateLimitAlgorithm::SLIDING_WINDOW;

    return PipelineBuilder()
        .with_cors(cors_presets::allow_all())
        .with_rate_limiting(rl_config)
        .with_compression();
}

/**
 * Internal microservice preset - no CORS, no public auth
 */
inline PipelineBuilder internal_service() {
    return PipelineBuilder()
        .with_compression();
}

} // namespace pipeline_presets

} // namespace http
} // namespace fasterapi
