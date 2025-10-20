#!/usr/bin/env python3
"""
Admin MCP Server

Simple MCP server that provides admin tools.
Used as an upstream server for the proxy example.
Requires admin scope for all operations.
"""

import time
from fasterapi.mcp import MCPServer

# Create server
server = MCPServer(
    name="Admin Server",
    version="1.0.0",
    instructions="Provides admin and database tools (requires admin scope)"
)


@server.tool("admin_reset_cache")
def admin_reset_cache() -> dict:
    """
    Reset the application cache (admin only).

    Returns:
        Status of the operation
    """
    return {
        "status": "success",
        "message": "Cache reset successfully",
        "timestamp": time.time()
    }


@server.tool("admin_get_logs")
def admin_get_logs(limit: int = 100) -> dict:
    """
    Get application logs (admin only).

    Args:
        limit: Maximum number of log entries to return

    Returns:
        Log entries
    """
    logs = [
        {"level": "INFO", "message": "Server started", "timestamp": time.time() - 3600},
        {"level": "WARN", "message": "High memory usage", "timestamp": time.time() - 1800},
        {"level": "ERROR", "message": "Database connection failed", "timestamp": time.time() - 900},
        {"level": "INFO", "message": "Database reconnected", "timestamp": time.time() - 600}
    ]

    return {
        "count": min(limit, len(logs)),
        "logs": logs[:limit]
    }


@server.tool("admin_restart_service")
def admin_restart_service(service_name: str) -> dict:
    """
    Restart a service (admin only).

    Args:
        service_name: Name of the service to restart

    Returns:
        Status of the operation
    """
    return {
        "status": "success",
        "service": service_name,
        "message": f"Service '{service_name}' restarted successfully",
        "timestamp": time.time()
    }


@server.tool("database_query")
def database_query(query: str) -> dict:
    """
    Execute a database query (requires database scope).

    Args:
        query: SQL query to execute (SELECT only)

    Returns:
        Query results
    """
    # Validate query is read-only
    query_upper = query.strip().upper()
    if not query_upper.startswith("SELECT"):
        raise ValueError("Only SELECT queries allowed")

    if any(keyword in query_upper for keyword in ["DROP", "DELETE", "UPDATE", "INSERT", "ALTER"]):
        raise ValueError("Destructive operations not allowed")

    # Simulate database query
    return {
        "query": query,
        "rows": [
            {"id": 1, "name": "Alice", "email": "alice@example.com"},
            {"id": 2, "name": "Bob", "email": "bob@example.com"},
            {"id": 3, "name": "Charlie", "email": "charlie@example.com"}
        ],
        "count": 3,
        "execution_time_ms": 12.5
    }


@server.tool("database_backup")
def database_backup() -> dict:
    """
    Create a database backup (admin only).

    Returns:
        Backup information
    """
    return {
        "status": "success",
        "backup_file": f"backup_{int(time.time())}.sql",
        "size_mb": 156.7,
        "timestamp": time.time()
    }


@server.tool("database_optimize")
def database_optimize() -> dict:
    """
    Optimize database tables (admin only).

    Returns:
        Optimization results
    """
    return {
        "status": "success",
        "tables_optimized": 15,
        "space_reclaimed_mb": 42.3,
        "duration_seconds": 8.5,
        "timestamp": time.time()
    }


if __name__ == "__main__":
    server.run(transport="stdio")
