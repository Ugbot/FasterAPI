#!/usr/bin/env python3
"""
Compare FastAPI vs FasterAPI performance.

This script:
1. Starts a FastAPI server
2. Runs benchmarks
3. Stops FastAPI server
4. Starts FasterAPI server
5. Runs benchmarks
6. Compares results

Usage:
    python compare_frameworks.py --requests 1000 --concurrency 50
"""

import argparse
import asyncio
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional

# Add parent directory to path
sys.path.insert(0, str(Path(__file__).parent.parent))

from benchmarks.runner import (
    BenchmarkResult,
    BenchmarkSuite,
    compare_results,
    create_crud_benchmarks,
    print_results,
)


def start_server(framework: str, app_module: str, port: int) -> subprocess.Popen:
    """Start a server for the given framework."""
    env = os.environ.copy()
    env["TEST_FRAMEWORK"] = framework

    # Use uvicorn to run the app
    cmd = [
        sys.executable,
        "-m",
        "uvicorn",
        f"apps.{app_module}:app",
        "--host",
        "0.0.0.0",
        "--port",
        str(port),
        "--log-level",
        "warning",
    ]

    process = subprocess.Popen(
        cmd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=str(Path(__file__).parent.parent),
    )

    return process


def stop_server(process: subprocess.Popen):
    """Stop a server process."""
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


async def wait_for_server(url: str, timeout: float = 30.0) -> bool:
    """Wait for server to be ready."""
    import aiohttp

    start = time.time()
    while time.time() - start < timeout:
        try:
            async with aiohttp.ClientSession() as session:
                async with session.get(
                    f"{url}/health", timeout=aiohttp.ClientTimeout(total=1)
                ) as resp:
                    if resp.status == 200:
                        return True
        except Exception:
            pass
        await asyncio.sleep(0.5)
    return False


async def run_benchmarks_for_framework(
    framework: str,
    base_url: str,
    num_requests: int,
    concurrency: int,
) -> List[BenchmarkResult]:
    """Run all benchmarks for a framework."""
    suite = BenchmarkSuite(
        base_url=base_url,
        num_requests=num_requests,
        concurrency=concurrency,
    )

    create_crud_benchmarks(suite)

    return await suite.run_all_async(framework)


async def main_async():
    """Main async entry point."""
    parser = argparse.ArgumentParser(description="Compare FastAPI vs FasterAPI")
    parser.add_argument(
        "--requests", type=int, default=500, help="Requests per benchmark"
    )
    parser.add_argument(
        "--concurrency", type=int, default=20, help="Concurrent requests"
    )
    parser.add_argument("--port", type=int, default=8765, help="Server port")
    parser.add_argument(
        "--app", type=str, default="crud_app", help="App module to test"
    )
    parser.add_argument("--output", type=str, help="Output JSON file for results")
    parser.add_argument(
        "--skip-fastapi", action="store_true", help="Skip FastAPI benchmarks"
    )
    parser.add_argument(
        "--skip-fasterapi", action="store_true", help="Skip FasterAPI benchmarks"
    )
    args = parser.parse_args()

    base_url = f"http://localhost:{args.port}"
    results: Dict[str, List[BenchmarkResult]] = {}

    print("=" * 70)
    print("FastAPI vs FasterAPI Performance Comparison")
    print("=" * 70)
    print(f"Configuration:")
    print(f"  Requests per benchmark: {args.requests}")
    print(f"  Concurrency: {args.concurrency}")
    print(f"  Application: {args.app}")
    print()

    # Benchmark FastAPI
    if not args.skip_fastapi:
        print("Starting FastAPI server...")
        fastapi_process = start_server("fastapi", args.app, args.port)

        try:
            if await wait_for_server(base_url):
                print("FastAPI server ready. Running benchmarks...")
                results["fastapi"] = await run_benchmarks_for_framework(
                    "fastapi", base_url, args.requests, args.concurrency
                )
                print_results(results["fastapi"], "FastAPI")
            else:
                print("ERROR: FastAPI server failed to start")
        finally:
            print("Stopping FastAPI server...")
            stop_server(fastapi_process)

        # Wait for port to be released
        await asyncio.sleep(2)

    # Benchmark FasterAPI
    if not args.skip_fasterapi:
        print("\nStarting FasterAPI server...")
        fasterapi_process = start_server("fasterapi", args.app, args.port)

        try:
            if await wait_for_server(base_url):
                print("FasterAPI server ready. Running benchmarks...")
                results["fasterapi"] = await run_benchmarks_for_framework(
                    "fasterapi", base_url, args.requests, args.concurrency
                )
                print_results(results["fasterapi"], "FasterAPI")
            else:
                print("ERROR: FasterAPI server failed to start")
        finally:
            print("Stopping FasterAPI server...")
            stop_server(fasterapi_process)

    # Compare results
    if "fastapi" in results and "fasterapi" in results:
        compare_results(results["fastapi"], results["fasterapi"])

    # Save results
    if args.output:
        output_data = {
            "config": {
                "requests": args.requests,
                "concurrency": args.concurrency,
                "app": args.app,
            },
            "results": {
                framework: [r.to_dict() for r in res]
                for framework, res in results.items()
            },
        }

        # Calculate summary
        if "fastapi" in results and "fasterapi" in results:
            summary = []
            for fast, faster in zip(results["fastapi"], results["fasterapi"]):
                fast_rps = fast.requests_per_second
                faster_rps = faster.requests_per_second
                speedup = faster_rps / fast_rps if fast_rps > 0 else 0
                summary.append(
                    {
                        "benchmark": fast.name,
                        "fastapi_rps": round(fast_rps, 2),
                        "fasterapi_rps": round(faster_rps, 2),
                        "speedup": round(speedup, 2),
                    }
                )
            output_data["summary"] = summary

        with open(args.output, "w") as f:
            json.dump(output_data, f, indent=2)
        print(f"\nResults saved to {args.output}")


def main():
    """Main entry point."""
    try:
        asyncio.run(main_async())
    except KeyboardInterrupt:
        print("\nBenchmark interrupted")
        sys.exit(1)


if __name__ == "__main__":
    main()
