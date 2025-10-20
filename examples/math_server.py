#!/usr/bin/env python3
"""
Math MCP Server

Simple MCP server that provides math-related tools.
Used as an upstream server for the proxy example.
"""

from fasterapi.mcp import MCPServer

# Create server
server = MCPServer(
    name="Math Server",
    version="1.0.0",
    instructions="Provides math calculation tools"
)


@server.tool("calculate")
def calculate(operation: str, a: float, b: float) -> dict:
    """
    Perform basic math calculations.

    Args:
        operation: One of "add", "subtract", "multiply", "divide"
        a: First operand
        b: Second operand

    Returns:
        Result of the operation
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

    return {
        "operation": operation,
        "operands": [a, b],
        "result": result
    }


@server.tool("math_add")
def math_add(a: float, b: float) -> float:
    """Add two numbers."""
    return a + b


@server.tool("math_subtract")
def math_subtract(a: float, b: float) -> float:
    """Subtract two numbers."""
    return a - b


@server.tool("math_multiply")
def math_multiply(a: float, b: float) -> float:
    """Multiply two numbers."""
    return a * b


@server.tool("math_divide")
def math_divide(a: float, b: float) -> float:
    """Divide two numbers."""
    if b == 0:
        raise ValueError("Division by zero")
    return a / b


@server.tool("math_power")
def math_power(base: float, exponent: float) -> float:
    """Raise base to exponent."""
    return base ** exponent


@server.tool("expensive_calculation")
def expensive_calculation(iterations: int) -> dict:
    """Perform an expensive calculation (for rate limiting demo)."""
    result = 0
    for i in range(min(iterations, 1000000)):
        result += i * i

    return {
        "iterations": iterations,
        "result": result
    }


if __name__ == "__main__":
    server.run(transport="stdio")
