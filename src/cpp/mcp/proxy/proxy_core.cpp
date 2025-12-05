#include "proxy_core.h"
#include "upstream_connection.h"
#include <algorithm>
#include <chrono>
#include <sstream>
#include <cstring>
#include <fnmatch.h>
#include <thread>

namespace fasterapi {
namespace mcp {
namespace proxy {

// ========== ConnectionPool Implementation ==========

ConnectionPool::ConnectionPool(const UpstreamConfig& config)
    : config_(config) {
}

std::shared_ptr<UpstreamConnection> ConnectionPool::acquire() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Try to get an idle connection
    if (!idle_connections_.empty()) {
        auto conn = idle_connections_.back();
        idle_connections_.pop_back();

        // Check if connection is still healthy
        if (conn->is_healthy()) {
            active_connections_.push_back(conn);
            return conn;
        }
    }

    // Create new connection if we haven't hit the limit
    if (active_connections_.size() + idle_connections_.size() < config_.max_connections) {
        auto conn = create_connection();
        if (conn && conn->connect()) {
            active_connections_.push_back(conn);
            return conn;
        }
    }

    return nullptr;
}

void ConnectionPool::release(std::shared_ptr<UpstreamConnection> conn) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Remove from active connections
    auto it = std::find(active_connections_.begin(), active_connections_.end(), conn);
    if (it != active_connections_.end()) {
        active_connections_.erase(it);

        // If connection is still healthy, return to idle pool
        if (conn->is_healthy()) {
            idle_connections_.push_back(conn);
        }
    }
}

ConnectionPool::Stats ConnectionPool::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    Stats stats;
    stats.total_connections = static_cast<uint32_t>(active_connections_.size() + idle_connections_.size());
    stats.active_connections = static_cast<uint32_t>(active_connections_.size());
    stats.idle_connections = static_cast<uint32_t>(idle_connections_.size());
    stats.total_acquires = 0;  // TODO: Track this
    stats.total_releases = 0;  // TODO: Track this

    return stats;
}

std::shared_ptr<UpstreamConnection> ConnectionPool::create_connection() {
    return UpstreamConnectionFactory::create(config_);
}

// ========== MCPProxy Implementation ==========

MCPProxy::MCPProxy(const Config& config)
    : config_(config) {
}

MCPProxy::~MCPProxy() = default;

void MCPProxy::add_upstream(const UpstreamConfig& upstream) {
    auto pool = std::make_shared<ConnectionPool>(upstream);
    upstreams_[upstream.name] = pool;
}

void MCPProxy::add_route(const ProxyRoute& route) {
    routes_.push_back(route);
}

void MCPProxy::add_transformer(const std::string& name, std::shared_ptr<Transformer> transformer) {
    transformers_[name] = transformer;
}

void MCPProxy::set_auth(std::shared_ptr<security::Authenticator> auth) {
    authenticator_ = auth;
}

void MCPProxy::set_rate_limiter(std::shared_ptr<security::RateLimitMiddleware> rate_limiter) {
    rate_limiter_ = rate_limiter;
}

JsonRpcResponse MCPProxy::handle_request(const JsonRpcRequest& request, const std::string& auth_header) {
    ProxyContext ctx;
    ctx.request_id = request.id.value_or("unknown");
    ctx.original_method = request.method;
    ctx.original_params = request.params.value_or("{}");
    ctx.start_time_ms = now_ms();

    // 1. Authentication
    if (config_.enable_auth && !check_auth(auth_header, ctx)) {
        JsonRpcResponse response;
        response.error = JsonRpcError{
            -32001,
            "Authentication failed",
            std::nullopt
        };
        response.id = request.id;
        record_request(ctx, false, now_ms() - ctx.start_time_ms);
        return response;
    }

    // 2. Rate limiting
    if (config_.enable_rate_limiting && !check_rate_limit(ctx)) {
        JsonRpcResponse response;
        response.error = JsonRpcError{
            -32002,
            "Rate limit exceeded",
            std::nullopt
        };
        response.id = request.id;
        record_request(ctx, false, now_ms() - ctx.start_time_ms);
        return response;
    }

    // 3. Find route
    auto route = find_route(request.method, ctx.original_params);
    if (!route.has_value()) {
        JsonRpcResponse response;
        response.error = JsonRpcError{
            -32601,
            "No route found for request",
            std::nullopt
        };
        response.id = request.id;
        record_request(ctx, false, now_ms() - ctx.start_time_ms);
        return response;
    }

    ctx.upstream_name = route->upstream_name;

    // 4. Authorization
    if (config_.enable_authorization && !check_authorization(ctx, *route)) {
        JsonRpcResponse response;
        response.error = JsonRpcError{
            -32003,
            "Authorization failed",
            std::nullopt
        };
        response.id = request.id;
        record_request(ctx, false, now_ms() - ctx.start_time_ms);
        return response;
    }

    // 5. Proxy request
    auto proxy_response = proxy_request(ctx, request);

    // 6. Record stats
    bool success = !proxy_response.error.has_value();
    uint64_t latency_ms = now_ms() - ctx.start_time_ms;
    record_request(ctx, success, latency_ms);

    return proxy_response;
}

ProxyStats MCPProxy::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

std::unordered_map<std::string, bool> MCPProxy::get_upstream_health() const {
    std::unordered_map<std::string, bool> health;

    for (const auto& [name, pool] : upstreams_) {
        auto conn = const_cast<ConnectionPool*>(pool.get())->acquire();
        if (conn) {
            health[name] = conn->is_healthy();
            const_cast<ConnectionPool*>(pool.get())->release(conn);
        } else {
            health[name] = false;
        }
    }

    return health;
}

std::optional<ProxyRoute> MCPProxy::find_route(const std::string& method, const std::string& params) {
    // Try to extract tool/resource/prompt name from params
    std::string target_name;

    if (method == "tools/call" || method == "tools/list") {
        // Extract tool name from params
        size_t name_pos = params.find("\"name\"");
        if (name_pos != std::string::npos) {
            size_t start = params.find("\"", name_pos + 6);
            if (start != std::string::npos) {
                size_t end = params.find("\"", start + 1);
                if (end != std::string::npos) {
                    target_name = params.substr(start + 1, end - start - 1);
                }
            }
        }

        // Match against tool patterns
        for (const auto& route : routes_) {
            if (!route.tool_pattern.empty() && match_pattern(route.tool_pattern, target_name)) {
                return route;
            }
        }
    } else if (method == "resources/read" || method == "resources/list") {
        // Extract resource URI from params
        size_t uri_pos = params.find("\"uri\"");
        if (uri_pos != std::string::npos) {
            size_t start = params.find("\"", uri_pos + 5);
            if (start != std::string::npos) {
                size_t end = params.find("\"", start + 1);
                if (end != std::string::npos) {
                    target_name = params.substr(start + 1, end - start - 1);
                }
            }
        }

        // Match against resource patterns
        for (const auto& route : routes_) {
            if (!route.resource_pattern.empty() && match_pattern(route.resource_pattern, target_name)) {
                return route;
            }
        }
    } else if (method == "prompts/get" || method == "prompts/list") {
        // Extract prompt name from params
        size_t name_pos = params.find("\"name\"");
        if (name_pos != std::string::npos) {
            size_t start = params.find("\"", name_pos + 6);
            if (start != std::string::npos) {
                size_t end = params.find("\"", start + 1);
                if (end != std::string::npos) {
                    target_name = params.substr(start + 1, end - start - 1);
                }
            }
        }

        // Match against prompt patterns
        for (const auto& route : routes_) {
            if (!route.prompt_pattern.empty() && match_pattern(route.prompt_pattern, target_name)) {
                return route;
            }
        }
    }

    // Default route (first route with empty pattern)
    for (const auto& route : routes_) {
        if (route.tool_pattern.empty() && route.resource_pattern.empty() && route.prompt_pattern.empty()) {
            return route;
        }
    }

    return std::nullopt;
}

JsonRpcResponse MCPProxy::proxy_request(const ProxyContext& ctx, const JsonRpcRequest& request) {
    // Serialize request
    std::ostringstream oss;
    oss << "{\"jsonrpc\":\"" << request.jsonrpc << "\","
        << "\"method\":\"" << request.method << "\"";

    if (request.params.has_value()) {
        oss << ",\"params\":" << request.params.value();
    }

    if (request.id.has_value()) {
        oss << ",\"id\":\"" << request.id.value() << "\"";
    }

    oss << "}";
    std::string request_str = oss.str();

    // Apply request transformation if enabled
    auto route = find_route(request.method, ctx.original_params);
    if (route.has_value() && route->enable_request_transform) {
        // TODO: Apply transformer
    }

    // Send to upstream with retry logic
    uint32_t retries = 0;
    const uint32_t max_retries = 3;  // TODO: Get from upstream config

    while (retries <= max_retries) {
        // Check circuit breaker
        if (config_.circuit_breaker_enabled && is_circuit_open(ctx.upstream_name)) {
            // Try failover if enabled
            if (config_.failover_enabled) {
                // TODO: Implement failover logic
            }

            JsonRpcResponse response;
            response.error = JsonRpcError{
                -32004,
                "Circuit breaker open for upstream: " + ctx.upstream_name,
                std::nullopt
            };
            response.id = request.id;
            return response;
        }

        // Send request
        auto response_str = send_to_upstream(ctx.upstream_name, request_str, 30000);

        if (response_str.has_value()) {
            // Parse response
            JsonRpcResponse response;
            // TODO: Parse JSON response properly
            // For now, just return a placeholder
            response.result = response_str.value();
            response.id = request.id;

            // Apply response transformation if enabled
            if (route.has_value() && route->enable_response_transform) {
                // TODO: Apply transformer
            }

            // Record success
            record_success(ctx.upstream_name);

            return response;
        }

        // Record failure
        record_failure(ctx.upstream_name);

        // Retry if not at max retries
        if (retries < max_retries) {
            retries++;
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));  // TODO: Exponential backoff
        } else {
            break;
        }
    }

    // All retries failed
    JsonRpcResponse response;
    response.error = JsonRpcError{
        -32005,
        "Failed to proxy request to upstream after " + std::to_string(max_retries) + " retries",
        std::nullopt
    };
    response.id = request.id;
    return response;
}

std::optional<std::string> MCPProxy::send_to_upstream(
    const std::string& upstream_name,
    const std::string& request,
    uint32_t timeout_ms
) {
    // Get upstream pool
    auto it = upstreams_.find(upstream_name);
    if (it == upstreams_.end()) {
        return std::nullopt;
    }

    // Acquire connection
    auto conn = it->second->acquire();
    if (!conn) {
        return std::nullopt;
    }

    // Send request
    auto response = conn->send_request(request, timeout_ms);

    // Release connection
    it->second->release(conn);

    return response;
}

bool MCPProxy::check_auth(const std::string& auth_header, ProxyContext& ctx) {
    if (!authenticator_) {
        return true;  // No auth configured
    }

    auto result = authenticator_->authenticate(auth_header);
    if (!result.success) {
        return false;
    }

    ctx.client_id = result.user_id;
    ctx.client_scopes = result.scopes;

    return true;
}

bool MCPProxy::check_rate_limit(const ProxyContext& ctx) {
    if (!rate_limiter_) {
        return true;  // No rate limiter configured
    }

    auto result = rate_limiter_->check(ctx.client_id);
    return result.allowed;
}

bool MCPProxy::check_authorization(const ProxyContext& ctx, const ProxyRoute& route) {
    if (!route.required_scope.has_value()) {
        return true;  // No scope required
    }

    const std::string& required_scope = route.required_scope.value();

    // Check if client has required scope
    for (const auto& scope : ctx.client_scopes) {
        if (scope == required_scope || scope == "*") {
            return true;
        }
    }

    return false;
}

bool MCPProxy::is_circuit_open(const std::string& upstream_name) {
    std::lock_guard<std::mutex> lock(circuit_breaker_mutex_);

    auto it = circuit_breakers_.find(upstream_name);
    if (it == circuit_breakers_.end()) {
        return false;
    }

    auto& state = it->second;

    // If circuit is open, check if we should try again
    if (state.is_open) {
        uint64_t now = now_ms();
        uint64_t time_since_failure = now - state.last_failure_time_ms;

        // Try again after 60 seconds
        if (time_since_failure > 60000) {
            state.is_open = false;
            state.failure_count = 0;
        }
    }

    return state.is_open;
}

void MCPProxy::record_success(const std::string& upstream_name) {
    std::lock_guard<std::mutex> lock(circuit_breaker_mutex_);

    auto& state = circuit_breakers_[upstream_name];
    state.failure_count = 0;
    state.is_open = false;
}

void MCPProxy::record_failure(const std::string& upstream_name) {
    std::lock_guard<std::mutex> lock(circuit_breaker_mutex_);

    auto& state = circuit_breakers_[upstream_name];
    state.failure_count++;
    state.last_failure_time_ms = now_ms();

    // Open circuit if failure threshold exceeded
    if (state.failure_count >= config_.circuit_breaker_threshold) {
        state.is_open = true;
    }
}

void MCPProxy::record_request(const ProxyContext& ctx, bool success, uint64_t latency_ms) {
    std::lock_guard<std::mutex> lock(stats_mutex_);

    stats_.total_requests++;

    if (success) {
        stats_.successful_requests++;
    } else {
        stats_.failed_requests++;
    }

    if (ctx.retry_count > 0) {
        stats_.retried_requests++;
    }

    stats_.total_latency_ms += latency_ms;

    if (stats_.min_latency_ms == 0 || latency_ms < stats_.min_latency_ms) {
        stats_.min_latency_ms = latency_ms;
    }

    if (latency_ms > stats_.max_latency_ms) {
        stats_.max_latency_ms = latency_ms;
    }

    stats_.upstream_requests[ctx.upstream_name]++;

    // Extract tool name from method
    if (ctx.original_method == "tools/call") {
        // TODO: Extract tool name from params
        stats_.tool_requests["unknown"]++;
    }
}

bool MCPProxy::match_pattern(const std::string& pattern, const std::string& value) {
    // Use fnmatch for wildcard matching
    return fnmatch(pattern.c_str(), value.c_str(), 0) == 0;
}

uint64_t MCPProxy::now_ms() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

// ========== Default Transformers Implementation ==========

MetadataTransformer::MetadataTransformer(const std::string& proxy_name)
    : proxy_name_(proxy_name) {
}

std::string MetadataTransformer::transform_request(const ProxyContext& ctx, const std::string& request) {
    // Add proxy metadata to request
    std::ostringstream oss;
    oss << request.substr(0, request.length() - 1);  // Remove closing brace
    oss << ",\"_proxy\":{";
    oss << "\"name\":\"" << proxy_name_ << "\",";
    oss << "\"request_id\":\"" << ctx.request_id << "\",";
    oss << "\"client_id\":\"" << ctx.client_id << "\"";
    oss << "}}";

    return oss.str();
}

std::string MetadataTransformer::transform_response(const ProxyContext& ctx, const std::string& response) {
    // Add proxy metadata to response
    std::ostringstream oss;
    oss << response.substr(0, response.length() - 1);  // Remove closing brace
    oss << ",\"_proxy\":{";
    oss << "\"name\":\"" << proxy_name_ << "\",";
    oss << "\"request_id\":\"" << ctx.request_id << "\",";
    oss << "\"latency_ms\":" << (now_ms() - ctx.start_time_ms);
    oss << "}}";

    return oss.str();
}

uint64_t MetadataTransformer::now_ms() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

SanitizingTransformer::SanitizingTransformer(const std::vector<std::string>& sensitive_fields)
    : sensitive_fields_(sensitive_fields) {
}

std::string SanitizingTransformer::transform_request(const ProxyContext& ctx, const std::string& request) {
    return redact_fields(request);
}

std::string SanitizingTransformer::transform_response(const ProxyContext& ctx, const std::string& response) {
    return redact_fields(response);
}

std::string SanitizingTransformer::redact_fields(const std::string& json) {
    std::string result = json;

    for (const auto& field : sensitive_fields_) {
        // Find field in JSON
        std::string field_pattern = "\"" + field + "\":";
        size_t pos = 0;

        while ((pos = result.find(field_pattern, pos)) != std::string::npos) {
            // Find value start
            size_t value_start = result.find("\"", pos + field_pattern.length());
            if (value_start == std::string::npos) {
                break;
            }

            // Find value end
            size_t value_end = result.find("\"", value_start + 1);
            if (value_end == std::string::npos) {
                break;
            }

            // Replace with [REDACTED]
            result.replace(value_start + 1, value_end - value_start - 1, "[REDACTED]");

            pos = value_end + 1;
        }
    }

    return result;
}

CachingTransformer::CachingTransformer(uint32_t ttl_ms)
    : ttl_ms_(ttl_ms) {
}

std::string CachingTransformer::transform_request(const ProxyContext& ctx, const std::string& request) {
    // Add cache control headers
    std::ostringstream oss;
    oss << request.substr(0, request.length() - 1);  // Remove closing brace
    oss << ",\"_cache\":{";
    oss << "\"ttl_ms\":" << ttl_ms_;
    oss << "}}";

    return oss.str();
}

std::string CachingTransformer::transform_response(const ProxyContext& ctx, const std::string& response) {
    // Add cache metadata
    std::ostringstream oss;
    oss << response.substr(0, response.length() - 1);  // Remove closing brace
    oss << ",\"_cache\":{";
    oss << "\"ttl_ms\":" << ttl_ms_ << ",";
    oss << "\"cached_at\":" << MCPProxy::now_ms();
    oss << "}}";

    return oss.str();
}

} // namespace proxy
} // namespace mcp
} // namespace fasterapi
