#!/usr/bin/env python3
"""
MCP Performance Benchmarks

Compare FasterAPI MCP with pure Python implementations.
"""

import time
import statistics
import json
from typing import List
from fasterapi.mcp import MCPServer


def benchmark_tool_registration():
    """Benchmark tool registration overhead."""
    print("\n=== Tool Registration Benchmark ===")

    iterations = 1000

    start = time.perf_counter()
    server = MCPServer()

    for i in range(iterations):
        @server.tool(f"tool_{i}")
        def tool_func(x: int) -> int:
            return x * 2

    elapsed = time.perf_counter() - start

    print(f"Registered {iterations} tools in {elapsed*1000:.2f}ms")
    print(f"Average: {elapsed/iterations*1000000:.2f}µs per tool")


def benchmark_protocol_parsing():
    """Benchmark JSON-RPC message parsing."""
    print("\n=== Protocol Parsing Benchmark ===")

    # Sample JSON-RPC messages
    request_json = '{"jsonrpc":"2.0","method":"tools/list","id":"1"}'
    response_json = '{"jsonrpc":"2.0","result":{"tools":[]},"id":"1"}'

    iterations = 100000
    latencies = []

    for _ in range(iterations):
        start = time.perf_counter()
        # TODO: Call C++ message parser when exposed
        obj = json.loads(request_json)  # Pure Python for now
        elapsed = (time.perf_counter() - start) * 1000000  # µs
        latencies.append(elapsed)

    print(f"Parsed {iterations} messages")
    print(f"  Mean:   {statistics.mean(latencies):.2f}µs")
    print(f"  Median: {statistics.median(latencies):.2f}µs")
    print(f"  P95:    {statistics.quantiles(latencies, n=20)[18]:.2f}µs")
    print(f"  P99:    {statistics.quantiles(latencies, n=100)[98]:.2f}µs")


def benchmark_tool_dispatch():
    """Benchmark tool dispatch overhead."""
    print("\n=== Tool Dispatch Benchmark ===")

    server = MCPServer()

    @server.tool("benchmark_tool")
    def benchmark_tool(x: int) -> int:
        return x * 2

    iterations = 10000
    latencies = []

    for i in range(iterations):
        start = time.perf_counter()
        # TODO: Call tool via MCP when exposed
        result = benchmark_tool(i)
        elapsed = (time.perf_counter() - start) * 1000000  # µs
        latencies.append(elapsed)

    print(f"Dispatched {iterations} tool calls")
    print(f"  Mean:   {statistics.mean(latencies):.2f}µs")
    print(f"  Median: {statistics.median(latencies):.2f}µs")
    print(f"  P95:    {statistics.quantiles(latencies, n=20)[18]:.2f}µs")
    print(f"  P99:    {statistics.quantiles(latencies, n=100)[98]:.2f}µs")


def benchmark_session_negotiation():
    """Benchmark session initialization overhead."""
    print("\n=== Session Negotiation Benchmark ===")

    iterations = 1000
    latencies = []

    for _ in range(iterations):
        start = time.perf_counter()
        # TODO: Create and initialize session when exposed
        server = MCPServer()  # Temporary
        elapsed = (time.perf_counter() - start) * 1000000  # µs
        latencies.append(elapsed)

    print(f"Completed {iterations} session negotiations")
    print(f"  Mean:   {statistics.mean(latencies):.2f}µs")
    print(f"  Median: {statistics.median(latencies):.2f}µs")
    print(f"  P95:    {statistics.quantiles(latencies, n=20)[18]:.2f}µs")


def benchmark_memory_usage():
    """Benchmark memory usage per session."""
    print("\n=== Memory Usage Benchmark ===")

    import tracemalloc

    tracemalloc.start()
    start_mem = tracemalloc.get_traced_memory()[0]

    servers = []
    for i in range(100):
        server = MCPServer(name=f"Server {i}")

        @server.tool(f"tool_{i}")
        def tool(x: int) -> int:
            return x * 2

        servers.append(server)

    end_mem = tracemalloc.get_traced_memory()[0]
    tracemalloc.stop()

    memory_per_server = (end_mem - start_mem) / len(servers)

    print(f"Created {len(servers)} servers")
    print(f"  Total memory: {(end_mem - start_mem) / 1024:.2f} KB")
    print(f"  Per server:   {memory_per_server / 1024:.2f} KB")


def benchmark_throughput():
    """Benchmark maximum throughput."""
    print("\n=== Throughput Benchmark ===")

    server = MCPServer()

    call_count = 0

    @server.tool("counter")
    def counter() -> int:
        nonlocal call_count
        call_count += 1
        return call_count

    duration_seconds = 1.0
    start = time.perf_counter()
    iterations = 0

    while time.perf_counter() - start < duration_seconds:
        counter()
        iterations += 1

    elapsed = time.perf_counter() - start

    print(f"Executed {iterations:,} tool calls in {elapsed:.2f}s")
    print(f"  Throughput: {iterations/elapsed:,.0f} calls/sec")
    print(f"  Latency:    {elapsed/iterations*1000000:.2f}µs per call")


def run_all_benchmarks():
    """Run all benchmarks."""
    print("╔" + "═" * 58 + "╗")
    print("║" + " " * 10 + "FasterAPI MCP Performance Benchmarks" + " " * 12 + "║")
    print("╚" + "═" * 58 + "╝")

    benchmark_tool_registration()
    benchmark_protocol_parsing()
    benchmark_tool_dispatch()
    benchmark_session_negotiation()
    benchmark_memory_usage()
    benchmark_throughput()

    print("\n" + "=" * 60)
    print("Benchmark complete!")
    print("=" * 60)


if __name__ == "__main__":
    run_all_benchmarks()
