#!/usr/bin/env python3
"""
Example MCP Client using FasterAPI

This demonstrates how to create an MCP client that can:
- Connect to remote MCP servers
- Call tools
- Read resources
- Get prompts

Usage:
    # Start the server in one terminal:
    python examples/mcp_server_example.py

    # Run this client in another terminal:
    python examples/mcp_client_example.py
"""

import json
from fasterapi.mcp import MCPClient


def main():
    print("🔌 Connecting to MCP Server...")

    # Create client
    client = MCPClient(
        name="FasterAPI Example MCP Client",
        version="1.0.0"
    )

    try:
        # Connect to server via STDIO subprocess
        client.connect_stdio("python3", ["examples/mcp_server_example.py"])
        print("✅ Connected to MCP server\n")

        # ========== Call Tools ==========

        print("📞 Calling tools...")

        # Calculate 42 + 8
        print("\n1. Calculate 42 + 8:")
        result = client.call_tool("calculate", {
            "operation": "add",
            "a": 42,
            "b": 8
        })
        print(f"   Result: {result}")

        # Calculate 12 * 7
        print("\n2. Calculate 12 * 7:")
        result = client.call_tool("calculate", {
            "operation": "multiply",
            "a": 12,
            "b": 7
        })
        print(f"   Result: {result}")

        # Get system info
        print("\n3. Get system info:")
        result = client.call_tool("get_system_info", {})
        print(f"   Platform: {result.get('platform')}")
        print(f"   Python: {result.get('python_implementation')}")

        # Analyze text
        print("\n4. Analyze text:")
        result = client.call_tool("analyze_text", {
            "text": "The quick brown fox jumps over the lazy dog"
        })
        print(f"   Words: {result.get('word_count')}")
        print(f"   Characters: {result.get('character_count')}")

        # ========== Read Resources ==========

        print("\n\n📚 Reading resources...")

        print("\n1. Application settings:")
        settings = client.read_resource("config://app/settings")
        print(f"   {settings[:200]}...")

        print("\n2. README:")
        readme = client.read_resource("data://example/readme")
        print(f"   {readme[:200]}...")

        # ========== Get Prompts ==========

        print("\n\n💬 Getting prompts...")

        print("\n1. Code review prompt:")
        prompt = client.get_prompt("code_review", {
            "code": "def hello(): print('world')",
            "language": "python"
        })
        print(f"   {prompt[:150]}...")

        print("\n2. Explain concept prompt:")
        prompt = client.get_prompt("explain_concept", {
            "concept": "MCP",
            "difficulty": "beginner"
        })
        print(f"   {prompt[:150]}...")

        print("\n\n✅ All operations completed successfully!")

    except Exception as e:
        print(f"\n❌ Error: {e}")
        import traceback
        traceback.print_exc()

    finally:
        # Disconnect
        print("\n🔌 Disconnecting from server...")
        client.disconnect()
        print("✅ Disconnected")


if __name__ == "__main__":
    main()
