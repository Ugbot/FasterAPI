"""
FasterAPI MCP (Model Context Protocol) Support

High-performance MCP server and client implementation in C++,
exposed via Python bindings.

Features:
- MCP server for exposing tools/resources/prompts
- MCP client for calling remote servers
- Multiple transport options (STDIO, HTTP+SSE, WebSocket)
- Security features (JWT, OAuth 2.0, sandboxing)
- 10-100x faster than pure Python implementations
"""

from .server import MCPServer, tool, resource, prompt
from .client import MCPClient
from .proxy import (
    MCPProxy,
    UpstreamConfig,
    ProxyRoute,
    ProxyStats,
    create_metadata_transformer,
    create_sanitizing_transformer,
    create_caching_transformer
)
from .types import Tool, Resource, Prompt, ToolResult, ResourceContent

__all__ = [
    # Server
    'MCPServer',
    'tool',
    'resource',
    'prompt',

    # Client
    'MCPClient',

    # Proxy
    'MCPProxy',
    'UpstreamConfig',
    'ProxyRoute',
    'ProxyStats',
    'create_metadata_transformer',
    'create_sanitizing_transformer',
    'create_caching_transformer',

    # Types
    'Tool',
    'Resource',
    'Prompt',
    'ToolResult',
    'ResourceContent',
]
