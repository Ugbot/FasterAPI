#!/usr/bin/env python3
"""
Data MCP Server

Simple MCP server that provides data resources.
Used as an upstream server for the proxy example.
"""

import json
from fasterapi.mcp import MCPServer

# Create server
server = MCPServer(
    name="Data Server",
    version="1.0.0",
    instructions="Provides data resources and tools"
)


@server.resource(
    uri="data://users/list",
    name="User List",
    description="List of all users",
    mime_type="application/json"
)
def get_users() -> str:
    """Get list of users."""
    users = [
        {"id": 1, "name": "Alice", "role": "admin"},
        {"id": 2, "name": "Bob", "role": "user"},
        {"id": 3, "name": "Charlie", "role": "user"}
    ]
    return json.dumps(users, indent=2)


@server.resource(
    uri="data://products/list",
    name="Product List",
    description="List of all products",
    mime_type="application/json"
)
def get_products() -> str:
    """Get list of products."""
    products = [
        {"id": 1, "name": "Widget", "price": 19.99},
        {"id": 2, "name": "Gadget", "price": 29.99},
        {"id": 3, "name": "Doohickey", "price": 39.99}
    ]
    return json.dumps(products, indent=2)


@server.resource(
    uri="config://app/settings",
    name="App Settings",
    description="Application configuration",
    mime_type="application/json"
)
def get_settings() -> str:
    """Get application settings."""
    settings = {
        "app_name": "Data Server",
        "version": "1.0.0",
        "features": {
            "users": True,
            "products": True,
            "reports": True
        },
        "limits": {
            "max_users": 1000,
            "max_products": 5000
        }
    }
    return json.dumps(settings, indent=2)


@server.tool("search_users")
def search_users(query: str) -> list:
    """
    Search for users by name.

    Args:
        query: Search query

    Returns:
        List of matching users
    """
    all_users = [
        {"id": 1, "name": "Alice", "role": "admin"},
        {"id": 2, "name": "Bob", "role": "user"},
        {"id": 3, "name": "Charlie", "role": "user"},
        {"id": 4, "name": "David", "role": "user"},
        {"id": 5, "name": "Eve", "role": "moderator"}
    ]

    query_lower = query.lower()
    matches = [
        user for user in all_users
        if query_lower in user["name"].lower()
    ]

    return matches


@server.tool("get_user_stats")
def get_user_stats() -> dict:
    """Get user statistics."""
    return {
        "total_users": 100,
        "active_users": 85,
        "inactive_users": 15,
        "new_users_today": 5,
        "roles": {
            "admin": 10,
            "moderator": 20,
            "user": 70
        }
    }


if __name__ == "__main__":
    server.run(transport="stdio")
