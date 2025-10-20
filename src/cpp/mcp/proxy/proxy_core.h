#pragma once

#include "../protocol/message.h"
#include "../protocol/session.h"
#include "../security/auth.h"
#include "../security/rate_limit.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <optional>

namespace fasterapi {
namespace mcp {
namespace proxy {

/**
 * Upstream server configuration
 */
struct UpstreamConfig {
    std::string name;
    std::string transport_type;  // "stdio", "http", "websocket"

    // For STDIO
    std::string command;
    std::vector<std::string> args;

    // For HTTP/WebSocket
    std::string url;
    std::string auth_token;

    // Connection settings
    uint32_t max_connections = 10;
    uint32_t connect_timeout_ms = 5000;
    uint32_t request_timeout_ms = 30000;

    // Health check
    bool enable_health_check = true;
    uint32_t health_check_interval_ms = 30000;

    // Retry policy
    uint32_t max_retries = 3;
    uint32_t retry_delay_ms = 1000;
};

/**
 * Proxy route configuration
 */
struct ProxyRoute {
    // Route matching
    std::string tool_pattern;      // Tool name pattern (supports wildcards)
    std::string resource_pattern;  // Resource URI pattern
    std::string prompt_pattern;    // Prompt name pattern

    // Target upstream
    std::string upstream_name;

    // Transform rules
    bool enable_request_transform = false;
    bool enable_response_transform = false;

    // Security override
    std::optional<std::string> required_scope;
    std::optional<uint32_t> rate_limit_override;
};

/**
 * Proxy statistics
 */
struct ProxyStats {
    uint64_t total_requests = 0;
    uint64_t successful_requests = 0;
    uint64_t failed_requests = 0;
    uint64_t retried_requests = 0;
    uint64_t cached_responses = 0;

    uint64_t total_latency_ms = 0;
    uint64_t min_latency_ms = 0;
    uint64_t max_latency_ms = 0;

    std::unordered_map<std::string, uint64_t> upstream_requests;
    std::unordered_map<std::string, uint64_t> tool_requests;
};

/**
 * Request context for proxying
 */
struct ProxyContext {
    std::string request_id;
    std::string client_id;
    std::vector<std::string> client_scopes;

    std::string upstream_name;
    std::string original_method;
    std::string original_params;

    uint64_t start_time_ms;
    uint32_t retry_count = 0;

    // For response transformation
    std::string response_data;
    bool is_error = false;
};

/**
 * Request/Response transformer interface
 */
class Transformer {
public:
    virtual ~Transformer() = default;

    /**
     * Transform request before sending to upstream.
     */
    virtual std::string transform_request(const ProxyContext& ctx, const std::string& request) = 0;

    /**
     * Transform response before sending to client.
     */
    virtual std::string transform_response(const ProxyContext& ctx, const std::string& response) = 0;
};

/**
 * Upstream server connection
 */
class UpstreamConnection {
public:
    virtual ~UpstreamConnection() = default;

    /**
     * Connect to upstream server.
     */
    virtual bool connect() = 0;

    /**
     * Disconnect from upstream server.
     */
    virtual void disconnect() = 0;

    /**
     * Send request and get response.
     */
    virtual std::optional<std::string> send_request(const std::string& request, uint32_t timeout_ms) = 0;

    /**
     * Check if connection is healthy.
     */
    virtual bool is_healthy() const = 0;

    /**
     * Get upstream server name.
     */
    virtual std::string get_name() const = 0;
};

/**
 * Connection pool for upstream servers
 */
class ConnectionPool {
public:
    explicit ConnectionPool(const UpstreamConfig& config);

    /**
     * Get a connection from the pool.
     */
    std::shared_ptr<UpstreamConnection> acquire();

    /**
     * Release a connection back to the pool.
     */
    void release(std::shared_ptr<UpstreamConnection> conn);

    /**
     * Get pool statistics.
     */
    struct Stats {
        uint32_t total_connections;
        uint32_t active_connections;
        uint32_t idle_connections;
        uint64_t total_acquires;
        uint64_t total_releases;
    };

    Stats get_stats() const;

private:
    UpstreamConfig config_;
    std::vector<std::shared_ptr<UpstreamConnection>> idle_connections_;
    std::vector<std::shared_ptr<UpstreamConnection>> active_connections_;
    mutable std::mutex mutex_;

    std::shared_ptr<UpstreamConnection> create_connection();
};

/**
 * MCP Proxy Server
 *
 * Features:
 * - Route requests to multiple upstream MCP servers
 * - Security enforcement (auth, rate limiting, authorization)
 * - Request/response transformation
 * - Connection pooling
 * - Health checking
 * - Metrics and monitoring
 * - Caching (optional)
 */
class MCPProxy {
public:
    struct Config {
        std::string name = "FasterAPI MCP Proxy";
        std::string version = "1.0.0";

        // Security
        bool enable_auth = true;
        bool enable_rate_limiting = true;
        bool enable_authorization = true;

        // Features
        bool enable_caching = false;
        uint32_t cache_ttl_ms = 60000;
        bool enable_request_logging = true;
        bool enable_metrics = true;

        // Proxy behavior
        bool failover_enabled = true;
        bool circuit_breaker_enabled = true;
        uint32_t circuit_breaker_threshold = 5;  // Failures before opening
    };

    explicit MCPProxy(const Config& config);
    ~MCPProxy();

    /**
     * Add upstream server.
     */
    void add_upstream(const UpstreamConfig& upstream);

    /**
     * Add routing rule.
     */
    void add_route(const ProxyRoute& route);

    /**
     * Add request transformer.
     */
    void add_transformer(const std::string& name, std::shared_ptr<Transformer> transformer);

    /**
     * Set authentication middleware.
     */
    void set_auth(std::shared_ptr<security::Authenticator> auth);

    /**
     * Set rate limiting middleware.
     */
    void set_rate_limiter(std::shared_ptr<security::RateLimitMiddleware> rate_limiter);

    /**
     * Handle incoming MCP request.
     */
    JsonRpcResponse handle_request(const JsonRpcRequest& request, const std::string& auth_header);

    /**
     * Get proxy statistics.
     */
    ProxyStats get_stats() const;

    /**
     * Get upstream health status.
     */
    std::unordered_map<std::string, bool> get_upstream_health() const;

private:
    Config config_;

    // Upstreams and routing
    std::unordered_map<std::string, std::shared_ptr<ConnectionPool>> upstreams_;
    std::vector<ProxyRoute> routes_;

    // Security
    std::shared_ptr<security::Authenticator> authenticator_;
    std::shared_ptr<security::RateLimitMiddleware> rate_limiter_;

    // Transformers
    std::unordered_map<std::string, std::shared_ptr<Transformer>> transformers_;

    // Stats
    ProxyStats stats_;
    mutable std::mutex stats_mutex_;

    // Circuit breaker state
    struct CircuitBreakerState {
        uint32_t failure_count = 0;
        bool is_open = false;
        uint64_t last_failure_time_ms = 0;
    };
    std::unordered_map<std::string, CircuitBreakerState> circuit_breakers_;
    mutable std::mutex circuit_breaker_mutex_;

    // Route matching
    std::optional<ProxyRoute> find_route(const std::string& method, const std::string& params);

    // Request handling
    JsonRpcResponse proxy_request(const ProxyContext& ctx, const JsonRpcRequest& request);
    std::optional<std::string> send_to_upstream(
        const std::string& upstream_name,
        const std::string& request,
        uint32_t timeout_ms
    );

    // Security checks
    bool check_auth(const std::string& auth_header, ProxyContext& ctx);
    bool check_rate_limit(const ProxyContext& ctx);
    bool check_authorization(const ProxyContext& ctx, const ProxyRoute& route);

    // Circuit breaker
    bool is_circuit_open(const std::string& upstream_name);
    void record_success(const std::string& upstream_name);
    void record_failure(const std::string& upstream_name);

    // Stats
    void record_request(const ProxyContext& ctx, bool success, uint64_t latency_ms);

    // Pattern matching
    static bool match_pattern(const std::string& pattern, const std::string& value);

    // Time helpers
    static uint64_t now_ms();
};

/**
 * Default transformers
 */

/**
 * Add metadata to requests/responses
 */
class MetadataTransformer : public Transformer {
public:
    MetadataTransformer(const std::string& proxy_name);

    std::string transform_request(const ProxyContext& ctx, const std::string& request) override;
    std::string transform_response(const ProxyContext& ctx, const std::string& response) override;

private:
    std::string proxy_name_;
};

/**
 * Filter/redact sensitive data
 */
class SanitizingTransformer : public Transformer {
public:
    explicit SanitizingTransformer(const std::vector<std::string>& sensitive_fields);

    std::string transform_request(const ProxyContext& ctx, const std::string& request) override;
    std::string transform_response(const ProxyContext& ctx, const std::string& response) override;

private:
    std::vector<std::string> sensitive_fields_;

    std::string redact_fields(const std::string& json);
};

/**
 * Add caching headers
 */
class CachingTransformer : public Transformer {
public:
    explicit CachingTransformer(uint32_t ttl_ms);

    std::string transform_request(const ProxyContext& ctx, const std::string& request) override;
    std::string transform_response(const ProxyContext& ctx, const std::string& response) override;

private:
    uint32_t ttl_ms_;
};

} // namespace proxy
} // namespace mcp
} // namespace fasterapi
