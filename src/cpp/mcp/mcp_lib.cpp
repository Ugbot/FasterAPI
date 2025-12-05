/**
 * C API for MCP (Model Context Protocol) Python bindings.
 *
 * This file provides a C interface to the C++ MCP implementation
 * for use via ctypes from Python.
 */

#include "server/mcp_server.h"
#include "client/mcp_client.h"
#include "transports/transport.h"
#include "proxy/proxy_core.h"
#include "proxy/upstream_connection.h"
#include <cstring>
#include <memory>
#include <sstream>

using namespace fasterapi::mcp;
using namespace fasterapi::mcp::proxy;

// Opaque handles
using MCPServerHandle = void*;
using MCPClientHandle = void*;
using TransportHandle = void*;
using MCPProxyHandle = void*;

extern "C" {

// ========== Server API ==========

/**
 * Create an MCP server.
 *
 * @param name Server name
 * @param version Server version
 * @return Server handle
 */
MCPServerHandle mcp_server_create(const char* name, const char* version) {
    MCPServer::Config config;
    config.name = name ? name : "FasterAPI MCP Server";
    config.version = version ? version : "0.1.0";

    auto* server = new MCPServer(config);
    return static_cast<MCPServerHandle>(server);
}

/**
 * Destroy an MCP server.
 */
void mcp_server_destroy(MCPServerHandle handle) {
    auto* server = static_cast<MCPServer*>(handle);
    delete server;
}

/**
 * Start MCP server with STDIO transport.
 *
 * @param handle Server handle
 * @return 0 on success, negative on error
 */
int mcp_server_start_stdio(MCPServerHandle handle) {
    auto* server = static_cast<MCPServer*>(handle);
    auto transport = TransportFactory::create_stdio();
    return server->start(std::move(transport));
}

/**
 * Stop MCP server.
 */
void mcp_server_stop(MCPServerHandle handle) {
    auto* server = static_cast<MCPServer*>(handle);
    server->stop();
}

/**
 * Register a tool with the server.
 *
 * @param handle Server handle
 * @param name Tool name
 * @param description Tool description
 * @param input_schema JSON schema for input (optional)
 * @param handler_id Numeric ID to identify the handler in Python
 * @return 0 on success, negative on error
 */
int mcp_server_register_tool(
    MCPServerHandle handle,
    const char* name,
    const char* description,
    const char* input_schema,
    uint64_t handler_id
) {
    auto* server = static_cast<MCPServer*>(handle);

    Tool tool;
    tool.name = name;
    tool.description = description;
    if (input_schema && strlen(input_schema) > 0) {
        tool.input_schema = input_schema;
    }

    // Create a handler that will call back to Python
    // For now, just return a placeholder
    ToolHandler handler = [handler_id](const std::string& params) -> std::string {
        // TODO: Call Python callback via handler_id
        return "{\"result\": \"Handler " + std::to_string(handler_id) + " called\"}";
    };

    bool registered = server->tools().register_tool(tool, handler);
    return registered ? 0 : -1;
}

/**
 * Register a resource with the server.
 *
 * @param handle Server handle
 * @param uri Resource URI
 * @param name Resource name
 * @param description Resource description (optional)
 * @param mime_type MIME type (optional)
 * @param provider_id Numeric ID to identify the provider in Python
 * @return 0 on success, negative on error
 */
int mcp_server_register_resource(
    MCPServerHandle handle,
    const char* uri,
    const char* name,
    const char* description,
    const char* mime_type,
    uint64_t provider_id
) {
    auto* server = static_cast<MCPServer*>(handle);

    Resource resource;
    resource.uri = uri;
    resource.name = name;
    if (description && strlen(description) > 0) {
        resource.description = description;
    }
    if (mime_type && strlen(mime_type) > 0) {
        resource.mime_type = mime_type;
    }

    ResourceProvider provider = [provider_id](const std::string& uri) -> ResourceContent {
        // TODO: Call Python callback via provider_id
        ResourceContent content;
        content.uri = uri;
        content.mime_type = "text/plain";
        content.content = "Provider " + std::to_string(provider_id) + " content";
        return content;
    };

    bool registered = server->resources().register_resource(resource, provider);
    return registered ? 0 : -1;
}

// ========== Client API ==========

/**
 * Create an MCP client.
 *
 * @param name Client name
 * @param version Client version
 * @return Client handle
 */
MCPClientHandle mcp_client_create(const char* name, const char* version) {
    MCPClient::Config config;
    config.client_name = name ? name : "FasterAPI MCP Client";
    config.client_version = version ? version : "0.1.0";

    auto* client = new MCPClient(config);
    return static_cast<MCPClientHandle>(client);
}

/**
 * Destroy an MCP client.
 */
void mcp_client_destroy(MCPClientHandle handle) {
    auto* client = static_cast<MCPClient*>(handle);
    delete client;
}

/**
 * Connect client to MCP server via STDIO subprocess.
 *
 * @param handle Client handle
 * @param command Command to execute
 * @param argc Number of arguments
 * @param argv Arguments
 * @return 0 on success, negative on error
 */
int mcp_client_connect_stdio(
    MCPClientHandle handle,
    const char* command,
    int argc,
    const char** argv
) {
    auto* client = static_cast<MCPClient*>(handle);

    std::vector<std::string> args;
    for (int i = 0; i < argc; i++) {
        args.push_back(argv[i]);
    }

    auto transport = TransportFactory::create_stdio(command, args);
    return client->connect(std::move(transport));
}

/**
 * Disconnect client from server.
 */
void mcp_client_disconnect(MCPClientHandle handle) {
    auto* client = static_cast<MCPClient*>(handle);
    client->disconnect();
}

/**
 * Call a tool on the server.
 *
 * @param handle Client handle
 * @param name Tool name
 * @param params Parameters as JSON string
 * @param result_buffer Buffer to store result
 * @param buffer_size Size of buffer
 * @return 0 on success, negative on error
 */
int mcp_client_call_tool(
    MCPClientHandle handle,
    const char* name,
    const char* params,
    char* result_buffer,
    size_t buffer_size
) {
    auto* client = static_cast<MCPClient*>(handle);

    try {
        auto result = client->call_tool(name, params);
        if (result.is_error) {
            return -1;
        }

        size_t len = std::min(result.content.length(), buffer_size - 1);
        memcpy(result_buffer, result.content.c_str(), len);
        result_buffer[len] = '\0';

        return 0;
    } catch (...) {
        return -1;
    }
}

// ========== Transport API ==========

/**
 * Create STDIO transport.
 *
 * @param command Command to execute (empty for server mode)
 * @param argc Number of arguments
 * @param argv Arguments
 * @return Transport handle
 */
TransportHandle mcp_transport_create_stdio(const char* command, int argc, const char** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; i++) {
        args.push_back(argv[i]);
    }

    std::string cmd = command ? command : "";
    auto transport = TransportFactory::create_stdio(cmd, args);
    return static_cast<TransportHandle>(transport.release());
}

/**
 * Destroy transport.
 */
void mcp_transport_destroy(TransportHandle handle) {
    auto* transport = static_cast<Transport*>(handle);
    delete transport;
}

// ========== Proxy API ==========

/**
 * Create an MCP proxy.
 *
 * @param name Proxy name
 * @param version Proxy version
 * @param enable_auth Enable authentication
 * @param enable_rate_limiting Enable rate limiting
 * @param enable_authorization Enable authorization
 * @param enable_caching Enable caching
 * @param cache_ttl_ms Cache TTL in milliseconds
 * @param enable_request_logging Enable request logging
 * @param enable_metrics Enable metrics
 * @param failover_enabled Enable failover
 * @param circuit_breaker_enabled Enable circuit breaker
 * @param circuit_breaker_threshold Circuit breaker threshold
 * @return Proxy handle
 */
MCPProxyHandle mcp_proxy_create(
    const char* name,
    const char* version,
    bool enable_auth,
    bool enable_rate_limiting,
    bool enable_authorization,
    bool enable_caching,
    uint32_t cache_ttl_ms,
    bool enable_request_logging,
    bool enable_metrics,
    bool failover_enabled,
    bool circuit_breaker_enabled,
    uint32_t circuit_breaker_threshold
) {
    MCPProxy::Config config;
    config.name = name ? name : "FasterAPI MCP Proxy";
    config.version = version ? version : "1.0.0";
    config.enable_auth = enable_auth;
    config.enable_rate_limiting = enable_rate_limiting;
    config.enable_authorization = enable_authorization;
    config.enable_caching = enable_caching;
    config.cache_ttl_ms = cache_ttl_ms;
    config.enable_request_logging = enable_request_logging;
    config.enable_metrics = enable_metrics;
    config.failover_enabled = failover_enabled;
    config.circuit_breaker_enabled = circuit_breaker_enabled;
    config.circuit_breaker_threshold = circuit_breaker_threshold;

    auto* proxy = new MCPProxy(config);
    return static_cast<MCPProxyHandle>(proxy);
}

/**
 * Destroy an MCP proxy.
 */
void mcp_proxy_destroy(MCPProxyHandle handle) {
    auto* proxy = static_cast<MCPProxy*>(handle);
    delete proxy;
}

/**
 * Add upstream server to proxy.
 *
 * @param handle Proxy handle
 * @param name Upstream name
 * @param transport_type Transport type ("stdio", "http", "websocket")
 * @param command Command for STDIO
 * @param argc Number of arguments
 * @param argv Arguments for STDIO
 * @param url URL for HTTP/WebSocket
 * @param auth_token Auth token for HTTP/WebSocket
 * @param max_connections Maximum connections
 * @param connect_timeout_ms Connect timeout
 * @param request_timeout_ms Request timeout
 * @param enable_health_check Enable health check
 * @param health_check_interval_ms Health check interval
 * @param max_retries Maximum retries
 * @param retry_delay_ms Retry delay
 * @return 0 on success, negative on error
 */
int mcp_proxy_add_upstream(
    MCPProxyHandle handle,
    const char* name,
    const char* transport_type,
    const char* command,
    int argc,
    const char** argv,
    const char* url,
    const char* auth_token,
    uint32_t max_connections,
    uint32_t connect_timeout_ms,
    uint32_t request_timeout_ms,
    bool enable_health_check,
    uint32_t health_check_interval_ms,
    uint32_t max_retries,
    uint32_t retry_delay_ms
) {
    auto* proxy = static_cast<MCPProxy*>(handle);

    UpstreamConfig config;
    config.name = name;
    config.transport_type = transport_type;
    config.command = command ? command : "";

    for (int i = 0; i < argc; i++) {
        config.args.push_back(argv[i]);
    }

    config.url = url ? url : "";
    config.auth_token = auth_token ? auth_token : "";
    config.max_connections = max_connections;
    config.connect_timeout_ms = connect_timeout_ms;
    config.request_timeout_ms = request_timeout_ms;
    config.enable_health_check = enable_health_check;
    config.health_check_interval_ms = health_check_interval_ms;
    config.max_retries = max_retries;
    config.retry_delay_ms = retry_delay_ms;

    proxy->add_upstream(config);
    return 0;
}

/**
 * Add route to proxy.
 *
 * @param handle Proxy handle
 * @param upstream_name Upstream name
 * @param tool_pattern Tool pattern (wildcard supported)
 * @param resource_pattern Resource pattern (wildcard supported)
 * @param prompt_pattern Prompt pattern (wildcard supported)
 * @param enable_request_transform Enable request transformation
 * @param enable_response_transform Enable response transformation
 * @param required_scope Required scope (NULL if none)
 * @param rate_limit_override Rate limit override (0 if none)
 * @return 0 on success, negative on error
 */
int mcp_proxy_add_route(
    MCPProxyHandle handle,
    const char* upstream_name,
    const char* tool_pattern,
    const char* resource_pattern,
    const char* prompt_pattern,
    bool enable_request_transform,
    bool enable_response_transform,
    const char* required_scope,
    uint32_t rate_limit_override
) {
    auto* proxy = static_cast<MCPProxy*>(handle);

    ProxyRoute route;
    route.upstream_name = upstream_name;
    route.tool_pattern = tool_pattern ? tool_pattern : "";
    route.resource_pattern = resource_pattern ? resource_pattern : "";
    route.prompt_pattern = prompt_pattern ? prompt_pattern : "";
    route.enable_request_transform = enable_request_transform;
    route.enable_response_transform = enable_response_transform;

    if (required_scope && strlen(required_scope) > 0) {
        route.required_scope = required_scope;
    }

    if (rate_limit_override > 0) {
        route.rate_limit_override = rate_limit_override;
    }

    proxy->add_route(route);
    return 0;
}

/**
 * Handle an MCP request through the proxy.
 *
 * @param handle Proxy handle
 * @param request_json JSON-RPC request as JSON string
 * @param auth_header Authorization header value
 * @param response_buffer Buffer to store response
 * @param buffer_size Size of buffer
 * @return 0 on success, negative on error
 */
int mcp_proxy_handle_request(
    MCPProxyHandle handle,
    const char* request_json,
    const char* auth_header,
    char* response_buffer,
    size_t buffer_size
) {
    auto* proxy = static_cast<MCPProxy*>(handle);

    try {
        // Parse request JSON
        JsonRpcRequest request = JsonRpcMessage::parse_request(request_json);

        // Handle request through proxy
        std::string auth = auth_header ? auth_header : "";
        JsonRpcResponse response = proxy->handle_request(request, auth);

        // Serialize response
        std::string response_str = JsonRpcMessage::serialize_response(response);

        size_t len = std::min(response_str.length(), buffer_size - 1);
        memcpy(response_buffer, response_str.c_str(), len);
        response_buffer[len] = '\0';

        return 0;
    } catch (...) {
        return -1;
    }
}

/**
 * Get proxy statistics.
 *
 * @param handle Proxy handle
 * @param stats_json Buffer to store stats as JSON
 * @param buffer_size Size of buffer
 * @return 0 on success, negative on error
 */
int mcp_proxy_get_stats(
    MCPProxyHandle handle,
    char* stats_json,
    size_t buffer_size
) {
    auto* proxy = static_cast<MCPProxy*>(handle);

    try {
        ProxyStats stats = proxy->get_stats();

        // Serialize stats to JSON
        std::ostringstream oss;
        oss << "{";
        oss << "\"total_requests\":" << stats.total_requests << ",";
        oss << "\"successful_requests\":" << stats.successful_requests << ",";
        oss << "\"failed_requests\":" << stats.failed_requests << ",";
        oss << "\"retried_requests\":" << stats.retried_requests << ",";
        oss << "\"cached_responses\":" << stats.cached_responses << ",";
        oss << "\"total_latency_ms\":" << stats.total_latency_ms << ",";
        oss << "\"min_latency_ms\":" << stats.min_latency_ms << ",";
        oss << "\"max_latency_ms\":" << stats.max_latency_ms;

        // Add upstream requests
        oss << ",\"upstream_requests\":{";
        bool first = true;
        for (const auto& [name, count] : stats.upstream_requests) {
            if (!first) oss << ",";
            oss << "\"" << name << "\":" << count;
            first = false;
        }
        oss << "}";

        // Add tool requests
        oss << ",\"tool_requests\":{";
        first = true;
        for (const auto& [name, count] : stats.tool_requests) {
            if (!first) oss << ",";
            oss << "\"" << name << "\":" << count;
            first = false;
        }
        oss << "}";

        oss << "}";

        std::string stats_str = oss.str();
        size_t len = std::min(stats_str.length(), buffer_size - 1);
        memcpy(stats_json, stats_str.c_str(), len);
        stats_json[len] = '\0';

        return 0;
    } catch (...) {
        return -1;
    }
}

/**
 * Get upstream health status.
 *
 * @param handle Proxy handle
 * @param health_json Buffer to store health as JSON
 * @param buffer_size Size of buffer
 * @return 0 on success, negative on error
 */
int mcp_proxy_get_upstream_health(
    MCPProxyHandle handle,
    char* health_json,
    size_t buffer_size
) {
    auto* proxy = static_cast<MCPProxy*>(handle);

    try {
        auto health = proxy->get_upstream_health();

        // Serialize health to JSON
        std::ostringstream oss;
        oss << "{";
        bool first = true;
        for (const auto& [name, is_healthy] : health) {
            if (!first) oss << ",";
            oss << "\"" << name << "\":" << (is_healthy ? "true" : "false");
            first = false;
        }
        oss << "}";

        std::string health_str = oss.str();
        size_t len = std::min(health_str.length(), buffer_size - 1);
        memcpy(health_json, health_str.c_str(), len);
        health_json[len] = '\0';

        return 0;
    } catch (...) {
        return -1;
    }
}

} // extern "C"
