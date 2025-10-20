"""
MCP Client implementation.
"""

import json
from typing import List, Optional, Any, Dict

from .bindings import bindings
from .types import Tool, Resource, Prompt, ToolResult


class MCPClient:
    """
    MCP Client for calling remote MCP servers.

    Example:
        ```python
        from fasterapi.mcp import MCPClient

        client = MCPClient()
        client.connect_stdio("python", ["my_mcp_server.py"])

        result = client.call_tool("calculate", {"operation": "add", "a": 5, "b": 3})
        print(result)  # {"result": 8}

        client.disconnect()
        ```
    """

    def __init__(
        self,
        name: str = "FasterAPI MCP Client",
        version: str = "0.1.0"
    ):
        """
        Create an MCP client.

        Args:
            name: Client name
            version: Client version
        """
        if bindings is None:
            raise RuntimeError("MCP library not loaded. Run 'make build' to compile.")

        self.name = name
        self.version = version
        self._handle = bindings.client_create(name, version)
        self._connected = False

    def __del__(self):
        if hasattr(self, '_handle') and self._handle:
            if self._connected:
                self.disconnect()
            bindings.client_destroy(self._handle)

    def connect_stdio(self, command: str, args: Optional[List[str]] = None):
        """
        Connect to MCP server via STDIO subprocess.

        Args:
            command: Command to execute
            args: Command arguments

        Example:
            ```python
            client.connect_stdio("python", ["server.py"])
            ```
        """
        args = args or []
        result = bindings.client_connect_stdio(self._handle, command, args)
        if result != 0:
            raise RuntimeError(f"Failed to connect: error code {result}")
        self._connected = True

    def disconnect(self):
        """Disconnect from the server."""
        if self._connected:
            bindings.client_disconnect(self._handle)
            self._connected = False

    def call_tool(self, name: str, params: Optional[Dict[str, Any]] = None) -> Any:
        """
        Call a tool on the server.

        Args:
            name: Tool name
            params: Tool parameters

        Returns:
            Tool result

        Example:
            ```python
            result = client.call_tool("add", {"a": 5, "b": 3})
            ```
        """
        if not self._connected:
            raise RuntimeError("Client not connected")

        params_json = json.dumps(params or {})
        result_json = bindings.client_call_tool(self._handle, name, params_json)

        return json.loads(result_json)

    def list_tools(self) -> List[Tool]:
        """
        List available tools from the server.

        Returns:
            List of Tool objects
        """
        # TODO: Implement
        raise NotImplementedError()

    def list_resources(self) -> List[Resource]:
        """
        List available resources from the server.

        Returns:
            List of Resource objects
        """
        # TODO: Implement
        raise NotImplementedError()

    def read_resource(self, uri: str) -> str:
        """
        Read a resource from the server.

        Args:
            uri: Resource URI

        Returns:
            Resource content
        """
        # TODO: Implement
        raise NotImplementedError()

    def list_prompts(self) -> List[Prompt]:
        """
        List available prompts from the server.

        Returns:
            List of Prompt objects
        """
        # TODO: Implement
        raise NotImplementedError()

    def get_prompt(self, name: str, args: Optional[Dict[str, Any]] = None) -> str:
        """
        Get a prompt from the server.

        Args:
            name: Prompt name
            args: Prompt arguments

        Returns:
            Prompt content
        """
        # TODO: Implement
        raise NotImplementedError()
