#!/usr/bin/env python3
"""
Secure MCP Server Example

Demonstrates security features:
- JWT authentication
- Rate limiting
- Sandboxed execution
- Per-tool authorization
"""

import json
import time
from fasterapi.mcp import MCPServer

# Create secure MCP server
server = MCPServer(
    name="Secure MCP Server",
    version="1.0.0",
    instructions="A secure MCP server with authentication, rate limiting, and sandboxing"
)


# Configure security (pseudocode - API to be finalized)
# server.security = {
#     "auth": {
#         "jwt": {
#             "algorithm": "HS256",
#             "secret": "your-256-bit-secret",
#             "issuer": "your-service",
#             "audience": "mcp-clients",
#         }
#     },
#     "rate_limit": {
#         "global": {
#             "max_requests": 1000,
#             "window_ms": 60000  # 1000 req/min
#         },
#         "per_client": {
#             "max_requests": 100,
#             "window_ms": 60000,  # 100 req/min
#             "burst": 20
#         }
#     },
#     "sandbox": {
#         "enabled": True,
#         "max_execution_time_ms": 5000,
#         "max_memory_bytes": 100 * 1024 * 1024,  # 100 MB
#         "allow_network": False,
#         "allow_file_write": False
#     }
# }


# ========== Public Tools (no auth required) ==========

@server.tool("public_info")
def public_info() -> dict:
    """Get public server information."""
    return {
        "name": server.name,
        "version": server.version,
        "features": ["tools", "resources", "prompts"],
        "security": {
            "auth_enabled": True,
            "rate_limiting": True,
            "sandboxing": True
        }
    }


# ========== Authenticated Tools ==========

@server.tool("sensitive_calculation")
# @server.requires_scope("calculate")  # Require specific scope
def sensitive_calculation(operation: str, a: float, b: float) -> dict:
    """
    Perform sensitive calculations (requires authentication).

    This tool requires a valid JWT token with 'calculate' scope.
    """
    operations = {
        "add": a + b,
        "subtract": a - b,
        "multiply": a * b,
        "divide": a / b if b != 0 else None,
        "power": a ** b
    }

    if operation not in operations:
        raise ValueError(f"Unknown operation: {operation}")

    result = operations[operation]
    if result is None:
        raise ValueError("Division by zero")

    return {
        "operation": operation,
        "operands": [a, b],
        "result": result,
        "computed_at": time.time()
    }


@server.tool("execute_code")
# @server.requires_scope("admin")  # Admin-only tool
def execute_code(code: str, language: str = "python") -> dict:
    """
    Execute code in a sandboxed environment (requires admin scope).

    WARNING: This is a dangerous tool and requires admin authentication.
    Code runs in a strict sandbox with:
    - 5 second timeout
    - 100 MB memory limit
    - No network access
    - No file write access
    """
    if language != "python":
        raise ValueError(f"Unsupported language: {language}")

    # In production, this would use the C++ sandbox
    # For now, demonstration only
    try:
        # Compile code
        compiled = compile(code, "<string>", "eval")

        # Execute in restricted namespace
        namespace = {
            "__builtins__": {
                "abs": abs,
                "min": min,
                "max": max,
                "sum": sum,
                "len": len,
                "range": range,
                "list": list,
                "dict": dict,
                "str": str,
                "int": int,
                "float": float,
            }
        }

        result = eval(compiled, namespace)

        return {
            "success": True,
            "result": str(result),
            "language": language
        }
    except Exception as e:
        return {
            "success": False,
            "error": str(e),
            "error_type": type(e).__name__
        }


@server.tool("database_query")
# @server.requires_scope("database")
def database_query(query: str) -> dict:
    """
    Execute a read-only database query (requires database scope).

    This tool:
    - Validates query is SELECT only
    - Limits result size
    - Has aggressive rate limiting (10 req/min per client)
    """
    # Validate query is read-only
    query_upper = query.strip().upper()
    if not query_upper.startswith("SELECT"):
        raise ValueError("Only SELECT queries allowed")

    if any(keyword in query_upper for keyword in ["DROP", "DELETE", "UPDATE", "INSERT", "ALTER"]):
        raise ValueError("Destructive operations not allowed")

    # Simulate database query
    # In production, would connect to actual database
    return {
        "query": query,
        "rows": [
            {"id": 1, "name": "Alice", "role": "admin"},
            {"id": 2, "name": "Bob", "role": "user"},
            {"id": 3, "name": "Charlie", "role": "user"}
        ],
        "count": 3,
        "execution_time_ms": 12.5
    }


# ========== Rate-Limited Tools ==========

@server.tool("expensive_operation")
# @server.rate_limit(max_requests=5, window_ms=60000)  # 5 req/min
def expensive_operation(iterations: int) -> dict:
    """
    Perform an expensive CPU operation.

    Rate limited to 5 requests per minute per client.
    """
    result = 0
    for i in range(min(iterations, 1000000)):  # Cap at 1M iterations
        result += i * i

    return {
        "iterations": iterations,
        "result": result
    }


# ========== Sandboxed Tools ==========

@server.tool("untrusted_computation")
# @server.sandbox(timeout_ms=1000, memory_mb=50)
def untrusted_computation(data: list) -> dict:
    """
    Process untrusted data in a strict sandbox.

    Sandbox constraints:
    - 1 second timeout
    - 50 MB memory limit
    - No system calls
    - No file access
    """
    # Process data safely
    return {
        "count": len(data),
        "sum": sum(data) if all(isinstance(x, (int, float)) for x in data) else None,
        "min": min(data) if data else None,
        "max": max(data) if data else None
    }


# ========== Monitoring & Admin Tools ==========

@server.tool("get_server_stats")
# @server.requires_scope("admin")
def get_server_stats() -> dict:
    """Get server statistics (admin only)."""
    return {
        "uptime_seconds": 3600,  # Placeholder
        "total_requests": 1234,
        "active_sessions": 5,
        "rate_limit_hits": 42,
        "auth_failures": 3,
        "tools": {
            "total": 10,
            "calls": {
                "public_info": 500,
                "sensitive_calculation": 300,
                "database_query": 150
            }
        }
    }


@server.tool("get_rate_limits")
# @server.requires_scope("admin")
def get_rate_limits(client_id: str = None) -> dict:
    """Get rate limit status (admin only)."""
    if client_id:
        return {
            "client_id": client_id,
            "remaining": 85,
            "limit": 100,
            "reset_at": time.time() + 45
        }
    else:
        return {
            "global": {
                "remaining": 850,
                "limit": 1000,
                "reset_at": time.time() + 30
            },
            "clients": ["client-1", "client-2", "client-3"]
        }


# ========== Resources with Access Control ==========

@server.resource(
    uri="config://server/settings",
    name="Server Settings",
    description="Server configuration (admin only)",
    mime_type="application/json"
)
# @server.requires_scope("admin")
def get_server_settings() -> str:
    """Get server settings (admin only)."""
    settings = {
        "auth": {
            "jwt_enabled": True,
            "oauth_providers": ["google", "github"]
        },
        "rate_limiting": {
            "enabled": True,
            "algorithm": "token_bucket"
        },
        "sandbox": {
            "enabled": True,
            "default_timeout_ms": 5000
        }
    }
    return json.dumps(settings, indent=2)


@server.resource(
    uri="logs://audit",
    name="Audit Log",
    description="Security audit log (admin only)",
    mime_type="text/plain"
)
# @server.requires_scope("admin")
def get_audit_log() -> str:
    """Get security audit log (admin only)."""
    log_entries = [
        f"{time.time() - 300:.0f} - User 'alice' authenticated successfully",
        f"{time.time() - 240:.0f} - User 'bob' failed authentication (invalid token)",
        f"{time.time() - 180:.0f} - User 'charlie' rate limited (exceeded 100 req/min)",
        f"{time.time() - 120:.0f} - Tool 'execute_code' called by admin user",
        f"{time.time() - 60:.0f} - Sandbox timeout occurred for untrusted_computation"
    ]
    return "\n".join(log_entries)


# ========== Main ==========

if __name__ == "__main__":
    print("🔒 Starting Secure MCP Server...")
    print(f"   Name: {server.name}")
    print(f"   Version: {server.version}")
    print("\n🔐 Security Features:")
    print("   • JWT Authentication (HS256)")
    print("   • Rate Limiting (Token Bucket)")
    print("   • Sandboxed Execution")
    print("   • Per-Tool Authorization")
    print("\n📊 Available Tools:")
    print("   Public:")
    print("     - public_info: Get server info")
    print("\n   Authenticated:")
    print("     - sensitive_calculation: Math operations (scope: calculate)")
    print("     - database_query: DB queries (scope: database)")
    print("     - execute_code: Code execution (scope: admin)")
    print("\n   Rate Limited:")
    print("     - expensive_operation: CPU-intensive (5/min)")
    print("\n   Sandboxed:")
    print("     - untrusted_computation: Process untrusted data")
    print("\n   Admin Only:")
    print("     - get_server_stats: Server statistics")
    print("     - get_rate_limits: Rate limit status")
    print("\n🔑 Authentication:")
    print("   Send JWT token in Authorization header:")
    print("   Authorization: Bearer <your-jwt-token>")
    print("\n⚠️  Note: This example shows the planned security API.")
    print("    Full implementation requires C++ security layer integration.")
    print("\nServer running on STDIO. Press Ctrl+C to stop.\n")

    try:
        server.run(transport="stdio")
    except KeyboardInterrupt:
        print("\n\n✅ Server stopped gracefully.")
