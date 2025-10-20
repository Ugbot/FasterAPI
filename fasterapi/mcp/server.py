"""
MCP Server implementation.
"""

import json
from typing import Callable, Optional, Dict, Any
from functools import wraps

from .bindings import bindings
from .types import Tool, Resource, Prompt


class MCPServer:
    """
    MCP Server for exposing tools, resources, and prompts.

    Example:
        ```python
        from fasterapi.mcp import MCPServer, tool

        server = MCPServer(name="My MCP Server")

        @server.tool("calculate")
        def calculate(operation: str, a: float, b: float) -> float:
            '''Perform a calculation'''
            if operation == "add":
                return a + b
            elif operation == "multiply":
                return a * b
            else:
                raise ValueError(f"Unknown operation: {operation}")

        server.run()  # Start server on STDIO
        ```
    """

    def __init__(
        self,
        name: str = "FasterAPI MCP Server",
        version: str = "0.1.0",
        instructions: Optional[str] = None
    ):
        """
        Create an MCP server.

        Args:
            name: Server name
            version: Server version
            instructions: Optional instructions for clients
        """
        if bindings is None:
            raise RuntimeError("MCP library not loaded. Run 'make build' to compile.")

        self.name = name
        self.version = version
        self.instructions = instructions

        self._handle = bindings.server_create(name, version)
        self._tool_handlers: Dict[int, Callable] = {}
        self._resource_providers: Dict[int, Callable] = {}
        self._prompt_generators: Dict[int, Callable] = {}
        self._next_id = 1

    def __del__(self):
        if hasattr(self, '_handle') and self._handle:
            bindings.server_destroy(self._handle)

    def tool(
        self,
        name: Optional[str] = None,
        description: Optional[str] = None,
        schema: Optional[dict] = None
    ):
        """
        Decorator to register a tool.

        Args:
            name: Tool name (defaults to function name)
            description: Tool description (defaults to function docstring)
            schema: JSON Schema for parameters

        Example:
            ```python
            @server.tool("greet", "Greet a person")
            def greet(name: str) -> str:
                return f"Hello, {name}!"
            ```
        """
        def decorator(func: Callable) -> Callable:
            tool_name = name or func.__name__
            tool_desc = description or func.__doc__ or ""
            tool_schema = json.dumps(schema) if schema else ""

            handler_id = self._next_id
            self._next_id += 1

            # Register with C++ layer
            result = bindings.server_register_tool(
                self._handle,
                tool_name,
                tool_desc,
                tool_schema,
                handler_id
            )

            if result != 0:
                raise RuntimeError(f"Failed to register tool: {tool_name}")

            # Store handler
            self._tool_handlers[handler_id] = func

            return func

        return decorator

    def resource(
        self,
        uri: Optional[str] = None,
        name: Optional[str] = None,
        description: Optional[str] = None,
        mime_type: str = "text/plain"
    ):
        """
        Decorator to register a resource.

        Args:
            uri: Resource URI (defaults to function name)
            name: Resource name (defaults to function name)
            description: Resource description (defaults to function docstring)
            mime_type: MIME type of the resource

        Example:
            ```python
            @server.resource("file:///config.json", "Configuration", mime_type="application/json")
            def get_config() -> str:
                return json.dumps({"setting": "value"})
            ```
        """
        def decorator(func: Callable) -> Callable:
            resource_uri = uri or f"resource://{func.__name__}"
            resource_name = name or func.__name__
            resource_desc = description or func.__doc__ or ""

            provider_id = self._next_id
            self._next_id += 1

            # Register with C++ layer
            result = bindings.server_register_resource(
                self._handle,
                resource_uri,
                resource_name,
                resource_desc,
                mime_type,
                provider_id
            )

            if result != 0:
                raise RuntimeError(f"Failed to register resource: {resource_uri}")

            # Store provider
            self._resource_providers[provider_id] = func

            return func

        return decorator

    def prompt(
        self,
        name: Optional[str] = None,
        description: Optional[str] = None,
        arguments: Optional[list] = None
    ):
        """
        Decorator to register a prompt.

        Args:
            name: Prompt name (defaults to function name)
            description: Prompt description (defaults to function docstring)
            arguments: List of argument names

        Example:
            ```python
            @server.prompt("code_review", "Review code", arguments=["code"])
            def code_review(code: str) -> str:
                return f"Please review this code:\n\n{code}"
            ```
        """
        def decorator(func: Callable) -> Callable:
            # TODO: Implement prompt registration
            return func

        return decorator

    def run(self, transport: str = "stdio"):
        """
        Start the server.

        Args:
            transport: Transport type ("stdio", "sse", "websocket")
        """
        if transport == "stdio":
            result = bindings.server_start_stdio(self._handle)
            if result != 0:
                raise RuntimeError(f"Failed to start server: error code {result}")
        else:
            raise NotImplementedError(f"Transport '{transport}' not yet implemented")

        try:
            # Keep server running
            import time
            while True:
                time.sleep(0.1)
        except KeyboardInterrupt:
            print("\nShutting down MCP server...")
            bindings.server_stop(self._handle)


# Standalone decorators for convenience
def tool(name: Optional[str] = None, description: Optional[str] = None, schema: Optional[dict] = None):
    """Standalone tool decorator (use with MCPServer instance)."""
    raise RuntimeError("Use @server.tool() instead of @tool()")


def resource(uri: Optional[str] = None, name: Optional[str] = None, description: Optional[str] = None):
    """Standalone resource decorator (use with MCPServer instance)."""
    raise RuntimeError("Use @server.resource() instead of @resource()")


def prompt(name: Optional[str] = None, description: Optional[str] = None):
    """Standalone prompt decorator (use with MCPServer instance)."""
    raise RuntimeError("Use @server.prompt() instead of @prompt()")
