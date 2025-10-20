"""
MCP type definitions.
"""

from dataclasses import dataclass
from typing import Optional, List
from enum import Enum


@dataclass
class Tool:
    """MCP tool definition."""
    name: str
    description: str
    input_schema: Optional[dict] = None


@dataclass
class Resource:
    """MCP resource definition."""
    uri: str
    name: str
    description: Optional[str] = None
    mime_type: Optional[str] = None


@dataclass
class Prompt:
    """MCP prompt definition."""
    name: str
    description: str
    arguments: Optional[List[str]] = None


@dataclass
class ToolResult:
    """Result from tool execution."""
    is_error: bool
    content: str
    error_message: Optional[str] = None


@dataclass
class ResourceContent:
    """Content of a resource."""
    uri: str
    mime_type: str
    content: str


class TransportType(Enum):
    """Transport types."""
    STDIO = "stdio"
    SSE = "sse"
    STREAMABLE = "streamable"
    WEBSOCKET = "websocket"
