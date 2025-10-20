#!/usr/bin/env python3
"""
Example MCP Server using FasterAPI

This demonstrates how to create an MCP server that exposes:
- Tools (functions that can be called)
- Resources (data that can be read)
- Prompts (templates for LLM interactions)

Run this server and connect to it using the Claude Desktop app or any MCP client.
"""

import json
from fasterapi.mcp import MCPServer

# Create MCP server
server = MCPServer(
    name="FasterAPI Example MCP Server",
    version="1.0.0",
    instructions="A high-performance MCP server demonstrating FasterAPI capabilities"
)


# ========== Tools ==========

@server.tool("calculate")
def calculate(operation: str, a: float, b: float) -> dict:
    """
    Perform a mathematical calculation.

    Args:
        operation: The operation to perform (add, subtract, multiply, divide)
        a: First number
        b: Second number

    Returns:
        The result of the calculation
    """
    operations = {
        "add": a + b,
        "subtract": a - b,
        "multiply": a * b,
        "divide": a / b if b != 0 else None
    }

    if operation not in operations:
        raise ValueError(f"Unknown operation: {operation}")

    result = operations[operation]
    if result is None:
        raise ValueError("Division by zero")

    return {"result": result, "operation": operation}


@server.tool("get_system_info")
def get_system_info() -> dict:
    """
    Get system information.

    Returns:
        System information including platform, Python version, etc.
    """
    import platform
    import sys

    return {
        "platform": platform.system(),
        "platform_release": platform.release(),
        "platform_version": platform.version(),
        "architecture": platform.machine(),
        "processor": platform.processor(),
        "python_version": sys.version,
        "python_implementation": platform.python_implementation()
    }


@server.tool("analyze_text")
def analyze_text(text: str) -> dict:
    """
    Analyze a text string.

    Args:
        text: The text to analyze

    Returns:
        Statistics about the text
    """
    words = text.split()
    return {
        "character_count": len(text),
        "word_count": len(words),
        "line_count": text.count('\n') + 1,
        "average_word_length": sum(len(word) for word in words) / len(words) if words else 0
    }


# ========== Resources ==========

@server.resource(
    uri="config://app/settings",
    name="Application Settings",
    description="Current application configuration",
    mime_type="application/json"
)
def get_app_settings() -> str:
    """Return application settings as JSON."""
    settings = {
        "server_name": server.name,
        "server_version": server.version,
        "features": {
            "tools_enabled": True,
            "resources_enabled": True,
            "prompts_enabled": True
        },
        "performance": {
            "language": "C++",
            "expected_speedup": "10-100x vs pure Python"
        }
    }
    return json.dumps(settings, indent=2)


@server.resource(
    uri="data://example/readme",
    name="README",
    description="Project README",
    mime_type="text/markdown"
)
def get_readme() -> str:
    """Return project README."""
    return """# FasterAPI MCP Server Example

This is a high-performance MCP server built with FasterAPI.

## Features

- **Fast**: C++ core for maximum performance
- **Simple**: Python API that's easy to use
- **Complete**: Supports tools, resources, and prompts
- **Secure**: Built-in authentication and sandboxing

## Available Tools

- `calculate`: Perform mathematical operations
- `get_system_info`: Get system information
- `analyze_text`: Analyze text statistics

## Available Resources

- `config://app/settings`: Application configuration
- `data://example/readme`: This README

## Usage

Connect to this server using any MCP client, such as:
- Claude Desktop
- VS Code with MCP extension
- Custom MCP clients

"""


# ========== Prompts ==========

@server.prompt(
    name="code_review",
    description="Generate a code review prompt",
    arguments=["code", "language"]
)
def code_review_prompt(code: str, language: str = "python") -> str:
    """Generate a code review prompt."""
    return f"""Please review the following {language} code for:
- Code quality and style
- Potential bugs or issues
- Performance improvements
- Security concerns
- Best practices

Code:
```{language}
{code}
```

Provide specific, actionable feedback.
"""


@server.prompt(
    name="explain_concept",
    description="Generate a prompt to explain a concept",
    arguments=["concept", "difficulty"]
)
def explain_concept_prompt(concept: str, difficulty: str = "beginner") -> str:
    """Generate a concept explanation prompt."""
    difficulty_map = {
        "beginner": "simple terms, assuming no prior knowledge",
        "intermediate": "moderate detail, assuming basic familiarity",
        "advanced": "technical depth, assuming strong background"
    }

    explanation_style = difficulty_map.get(difficulty, difficulty_map["beginner"])

    return f"""Please explain the concept of "{concept}" in {explanation_style}.

Include:
1. A clear definition
2. Why it's important
3. Real-world examples
4. Common misconceptions

Keep it concise but thorough.
"""


# ========== Main ==========

if __name__ == "__main__":
    print("🚀 Starting FasterAPI MCP Server...")
    print(f"   Name: {server.name}")
    print(f"   Version: {server.version}")
    print("\nAvailable tools:")
    print("  - calculate: Perform math operations")
    print("  - get_system_info: Get system information")
    print("  - analyze_text: Analyze text statistics")
    print("\nAvailable resources:")
    print("  - config://app/settings: App configuration")
    print("  - data://example/readme: Project README")
    print("\nServer running on STDIO. Connect with an MCP client.")
    print("Press Ctrl+C to stop.\n")

    try:
        server.run(transport="stdio")
    except KeyboardInterrupt:
        print("\n\n✅ Server stopped gracefully.")
