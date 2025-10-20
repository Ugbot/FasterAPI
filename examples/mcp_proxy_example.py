#!/usr/bin/env python3
"""
MCP Proxy Example

Demonstrates how to use the FasterAPI MCP Proxy to route requests
between multiple upstream MCP servers with:
- Pattern-based routing
- Security enforcement
- Connection pooling
- Health checking
- Circuit breaker
"""

from fasterapi.mcp import (
    MCPProxy,
    UpstreamConfig,
    ProxyRoute,
    create_metadata_transformer,
    create_sanitizing_transformer
)


def main():
    # Create MCP proxy with full security
    proxy = MCPProxy(
        name="Multi-Server MCP Proxy",
        version="1.0.0",
        enable_auth=True,
        enable_rate_limiting=True,
        enable_authorization=True,
        circuit_breaker_enabled=True,
        circuit_breaker_threshold=5,
        failover_enabled=True
    )

    # ========== Configure Upstream Servers ==========

    # Math server - handles all math-related tools
    proxy.add_upstream(UpstreamConfig(
        name="math-server",
        transport_type="stdio",
        command="python",
        args=["examples/math_server.py"],
        max_connections=10,
        connect_timeout_ms=5000,
        request_timeout_ms=30000,
        enable_health_check=True,
        health_check_interval_ms=30000,
        max_retries=3,
        retry_delay_ms=1000
    ))

    # Data server - handles data resources
    proxy.add_upstream(UpstreamConfig(
        name="data-server",
        transport_type="stdio",
        command="python",
        args=["examples/data_server.py"],
        max_connections=10,
        connect_timeout_ms=5000,
        request_timeout_ms=30000,
        enable_health_check=True,
        health_check_interval_ms=30000
    ))

    # Admin server - handles admin operations
    proxy.add_upstream(UpstreamConfig(
        name="admin-server",
        transport_type="stdio",
        command="python",
        args=["examples/admin_server.py"],
        max_connections=5,
        connect_timeout_ms=5000,
        request_timeout_ms=30000,
        enable_health_check=True,
        health_check_interval_ms=30000
    ))

    # ========== Configure Routing Rules ==========

    # Route all math_* tools to math-server
    proxy.add_route(ProxyRoute(
        upstream_name="math-server",
        tool_pattern="math_*",  # Wildcard: math_add, math_subtract, etc.
        enable_request_transform=True,
        enable_response_transform=True,
        required_scope="calculate"  # Requires 'calculate' scope
    ))

    # Route calculate tool to math-server
    proxy.add_route(ProxyRoute(
        upstream_name="math-server",
        tool_pattern="calculate",
        required_scope="calculate"
    ))

    # Route all data:// resources to data-server
    proxy.add_route(ProxyRoute(
        upstream_name="data-server",
        resource_pattern="data://*",
        required_scope="read_data"
    ))

    # Route all config:// resources to data-server
    proxy.add_route(ProxyRoute(
        upstream_name="data-server",
        resource_pattern="config://*",
        required_scope="read_config"
    ))

    # Route admin tools to admin-server (strict rate limit)
    proxy.add_route(ProxyRoute(
        upstream_name="admin-server",
        tool_pattern="admin_*",
        required_scope="admin",
        rate_limit_override=10  # 10 req/min for admin tools
    ))

    # Route database operations to admin-server
    proxy.add_route(ProxyRoute(
        upstream_name="admin-server",
        tool_pattern="database_*",
        required_scope="database",
        rate_limit_override=20  # 20 req/min for database operations
    ))

    # Default route - catch-all for unmatched requests
    # Routes to math-server as a default
    proxy.add_route(ProxyRoute(
        upstream_name="math-server"
    ))

    # ========== Configure Security ==========

    # JWT Authentication (production)
    proxy.set_jwt_auth(
        secret="your-256-bit-secret-key-change-this-in-production",
        algorithm="HS256",
        issuer="your-service",
        audience="mcp-clients",
        verify_expiry=True,
        clock_skew_seconds=60
    )

    # Or Bearer Token Authentication (development/testing)
    # proxy.set_bearer_auth(token="your-secret-token")

    # Global rate limiting
    proxy.set_rate_limit(
        max_requests=1000,  # 1000 requests per minute globally
        window_ms=60000,
        burst=100,  # Allow burst of 100
        algorithm="token_bucket"
    )

    # Per-tool rate limits
    proxy.set_tool_rate_limit(
        tool_name="expensive_calculation",
        max_requests=5,
        window_ms=60000  # 5 requests per minute
    )

    proxy.set_tool_rate_limit(
        tool_name="database_query",
        max_requests=20,
        window_ms=60000  # 20 requests per minute
    )

    # ========== Configure Transformers ==========

    # Add metadata to all requests/responses
    proxy.add_request_transformer(
        "metadata",
        create_metadata_transformer("Multi-Server MCP Proxy")
    )

    proxy.add_response_transformer(
        "metadata",
        create_metadata_transformer("Multi-Server MCP Proxy")
    )

    # Redact sensitive fields
    proxy.add_request_transformer(
        "sanitize",
        create_sanitizing_transformer([
            "password",
            "secret",
            "token",
            "api_key",
            "private_key"
        ])
    )

    proxy.add_response_transformer(
        "sanitize",
        create_sanitizing_transformer([
            "password",
            "secret",
            "token",
            "api_key",
            "private_key"
        ])
    )

    # ========== Run Proxy ==========

    print("=" * 70)
    print("🔀 FasterAPI MCP Proxy")
    print("=" * 70)
    print(f"\n📋 Configuration:")
    print(f"   Name: {proxy.name}")
    print(f"   Version: {proxy.version}")
    print(f"\n🖥️  Upstream Servers: {len(proxy.upstreams)}")
    for upstream in proxy.upstreams:
        print(f"   • {upstream.name} ({upstream.transport_type})")
        if upstream.transport_type == "stdio":
            print(f"     Command: {upstream.command} {' '.join(upstream.args)}")
        else:
            print(f"     URL: {upstream.url}")

    print(f"\n🛣️  Routing Rules: {len(proxy.routes)}")
    for i, route in enumerate(proxy.routes, 1):
        print(f"   {i}. → {route.upstream_name}")
        if route.tool_pattern:
            print(f"      Tools: {route.tool_pattern}")
        if route.resource_pattern:
            print(f"      Resources: {route.resource_pattern}")
        if route.prompt_pattern:
            print(f"      Prompts: {route.prompt_pattern}")
        if route.required_scope:
            print(f"      Scope: {route.required_scope}")

    print(f"\n🔐 Security:")
    print(f"   • Authentication: JWT (HS256)")
    print(f"   • Rate Limiting: 1000 req/min (token bucket)")
    print(f"   • Authorization: Scope-based")
    print(f"   • Circuit Breaker: Enabled (threshold: 5)")

    print(f"\n🔧 Features:")
    print(f"   • Connection Pooling: 10 connections/upstream")
    print(f"   • Health Checking: Every 30 seconds")
    print(f"   • Failover: Enabled")
    print(f"   • Request/Response Transformation: Enabled")
    print(f"   • Metrics: Enabled")

    print("\n" + "=" * 70)
    print("🚀 Starting proxy on STDIO...")
    print("=" * 70)
    print("\n📌 Example requests:")
    print("""
    # Call math tool (routed to math-server)
    {
      "jsonrpc": "2.0",
      "method": "tools/call",
      "params": {
        "name": "calculate",
        "arguments": {"operation": "add", "a": 5, "b": 3}
      },
      "id": "req-1"
    }

    # Read data resource (routed to data-server)
    {
      "jsonrpc": "2.0",
      "method": "resources/read",
      "params": {"uri": "data://users/list"},
      "id": "req-2"
    }

    # Call admin tool (routed to admin-server, requires admin scope)
    {
      "jsonrpc": "2.0",
      "method": "tools/call",
      "params": {
        "name": "admin_reset_cache",
        "arguments": {}
      },
      "id": "req-3"
    }
    """)

    print("\n🔑 Authentication:")
    print("   Include JWT token in Authorization header")
    print("   (Proxy will validate and extract scopes)")

    print("\n" + "=" * 70)

    try:
        # Run proxy on STDIO
        proxy.run(transport="stdio")
    except KeyboardInterrupt:
        print("\n\n✅ Proxy stopped gracefully.")
        print("\n📊 Final Statistics:")
        stats = proxy.get_stats()
        print(f"   Total Requests: {stats.total_requests}")
        print(f"   Successful: {stats.successful_requests}")
        print(f"   Failed: {stats.failed_requests}")
        print(f"   Retried: {stats.retried_requests}")
        print(f"   Success Rate: {stats.success_rate:.1%}")
        print(f"   Avg Latency: {stats.avg_latency_ms:.2f} ms")

        if stats.upstream_requests:
            print(f"\n   Requests per Upstream:")
            for upstream, count in stats.upstream_requests.items():
                print(f"     • {upstream}: {count}")

        print(f"\n🏥 Upstream Health:")
        health = proxy.get_upstream_health()
        for upstream, is_healthy in health.items():
            status = "✅ Healthy" if is_healthy else "❌ Unhealthy"
            print(f"   • {upstream}: {status}")


if __name__ == "__main__":
    main()
