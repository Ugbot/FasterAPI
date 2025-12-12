"""
Benchmark runner for comparing FastAPI vs FasterAPI performance.

Usage:
    python -m benchmarks.runner
    python -m benchmarks.runner --requests 1000 --concurrency 10
"""

import argparse
import asyncio
import json
import os
import random
import statistics
import string
import subprocess
import sys
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional
from uuid import uuid4


@dataclass
class BenchmarkResult:
    """Results from a single benchmark."""

    name: str
    framework: str
    total_requests: int
    successful_requests: int
    failed_requests: int
    total_time: float
    times: List[float] = field(default_factory=list)

    @property
    def requests_per_second(self) -> float:
        return self.total_requests / self.total_time if self.total_time > 0 else 0

    @property
    def avg_latency_ms(self) -> float:
        return statistics.mean(self.times) * 1000 if self.times else 0

    @property
    def min_latency_ms(self) -> float:
        return min(self.times) * 1000 if self.times else 0

    @property
    def max_latency_ms(self) -> float:
        return max(self.times) * 1000 if self.times else 0

    @property
    def p50_latency_ms(self) -> float:
        if not self.times:
            return 0
        sorted_times = sorted(self.times)
        idx = len(sorted_times) // 2
        return sorted_times[idx] * 1000

    @property
    def p95_latency_ms(self) -> float:
        if not self.times:
            return 0
        sorted_times = sorted(self.times)
        idx = int(len(sorted_times) * 0.95)
        return sorted_times[idx] * 1000

    @property
    def p99_latency_ms(self) -> float:
        if not self.times:
            return 0
        sorted_times = sorted(self.times)
        idx = int(len(sorted_times) * 0.99)
        return sorted_times[min(idx, len(sorted_times) - 1)] * 1000

    @property
    def success_rate(self) -> float:
        return (
            self.successful_requests / self.total_requests * 100
            if self.total_requests > 0
            else 0
        )

    def to_dict(self) -> Dict[str, Any]:
        return {
            "name": self.name,
            "framework": self.framework,
            "total_requests": self.total_requests,
            "successful_requests": self.successful_requests,
            "failed_requests": self.failed_requests,
            "success_rate": f"{self.success_rate:.2f}%",
            "total_time_seconds": round(self.total_time, 3),
            "requests_per_second": round(self.requests_per_second, 2),
            "latency_ms": {
                "min": round(self.min_latency_ms, 3),
                "avg": round(self.avg_latency_ms, 3),
                "max": round(self.max_latency_ms, 3),
                "p50": round(self.p50_latency_ms, 3),
                "p95": round(self.p95_latency_ms, 3),
                "p99": round(self.p99_latency_ms, 3),
            },
        }


def random_string(length: int = 10) -> str:
    return "".join(random.choices(string.ascii_letters, k=length))


def random_email() -> str:
    return f"{random_string(8)}@{random_string(5)}.com"


async def run_benchmark_async(
    name: str,
    framework: str,
    num_requests: int,
    concurrency: int,
    request_func: Callable,
) -> BenchmarkResult:
    """Run a benchmark asynchronously with concurrency."""
    import aiohttp

    times: List[float] = []
    successful = 0
    failed = 0
    semaphore = asyncio.Semaphore(concurrency)

    async def make_request(session: aiohttp.ClientSession):
        nonlocal successful, failed
        async with semaphore:
            start = time.perf_counter()
            try:
                success = await request_func(session)
                elapsed = time.perf_counter() - start
                times.append(elapsed)
                if success:
                    successful += 1
                else:
                    failed += 1
            except Exception as e:
                elapsed = time.perf_counter() - start
                times.append(elapsed)
                failed += 1

    start_time = time.perf_counter()

    async with aiohttp.ClientSession() as session:
        tasks = [make_request(session) for _ in range(num_requests)]
        await asyncio.gather(*tasks)

    total_time = time.perf_counter() - start_time

    return BenchmarkResult(
        name=name,
        framework=framework,
        total_requests=num_requests,
        successful_requests=successful,
        failed_requests=failed,
        total_time=total_time,
        times=times,
    )


def run_benchmark_sync(
    name: str,
    framework: str,
    num_requests: int,
    request_func: Callable,
) -> BenchmarkResult:
    """Run a benchmark synchronously."""
    import requests

    times: List[float] = []
    successful = 0
    failed = 0

    start_time = time.perf_counter()

    for _ in range(num_requests):
        start = time.perf_counter()
        try:
            success = request_func(requests)
            elapsed = time.perf_counter() - start
            times.append(elapsed)
            if success:
                successful += 1
            else:
                failed += 1
        except Exception:
            elapsed = time.perf_counter() - start
            times.append(elapsed)
            failed += 1

    total_time = time.perf_counter() - start_time

    return BenchmarkResult(
        name=name,
        framework=framework,
        total_requests=num_requests,
        successful_requests=successful,
        failed_requests=failed,
        total_time=total_time,
        times=times,
    )


class BenchmarkSuite:
    """Suite of benchmarks to run against both frameworks."""

    def __init__(self, base_url: str, num_requests: int = 100, concurrency: int = 10):
        self.base_url = base_url
        self.num_requests = num_requests
        self.concurrency = concurrency
        self.benchmarks: List[Dict[str, Any]] = []

    def add_benchmark(
        self,
        name: str,
        method: str,
        path: str,
        data: Optional[Dict] = None,
        params: Optional[Dict] = None,
        headers: Optional[Dict] = None,
    ):
        """Add a benchmark to the suite."""
        self.benchmarks.append(
            {
                "name": name,
                "method": method.upper(),
                "path": path,
                "data": data,
                "params": params,
                "headers": headers,
            }
        )

    async def run_all_async(self, framework: str) -> List[BenchmarkResult]:
        """Run all benchmarks asynchronously."""
        import aiohttp

        results = []

        for benchmark in self.benchmarks:
            name = benchmark["name"]
            method = benchmark["method"]
            url = f"{self.base_url}{benchmark['path']}"
            data = benchmark.get("data")
            params = benchmark.get("params")
            headers = benchmark.get("headers", {})

            async def make_request(session: aiohttp.ClientSession) -> bool:
                try:
                    if method == "GET":
                        async with session.get(
                            url, params=params, headers=headers
                        ) as resp:
                            return resp.status < 400
                    elif method == "POST":
                        async with session.post(
                            url, json=data, params=params, headers=headers
                        ) as resp:
                            return resp.status < 400
                    elif method == "PUT":
                        async with session.put(
                            url, json=data, params=params, headers=headers
                        ) as resp:
                            return resp.status < 400
                    elif method == "DELETE":
                        async with session.delete(
                            url, params=params, headers=headers
                        ) as resp:
                            return resp.status < 400
                    return False
                except Exception:
                    return False

            result = await run_benchmark_async(
                name=name,
                framework=framework,
                num_requests=self.num_requests,
                concurrency=self.concurrency,
                request_func=make_request,
            )
            results.append(result)

            # Small delay between benchmarks
            await asyncio.sleep(0.5)

        return results


def create_crud_benchmarks(suite: BenchmarkSuite):
    """Add CRUD benchmarks to the suite."""
    # GET requests
    suite.add_benchmark("GET /health", "GET", "/health")
    suite.add_benchmark("GET /items (empty)", "GET", "/items")

    # POST requests with randomized data
    suite.add_benchmark(
        "POST /items",
        "POST",
        "/items",
        data={
            "name": random_string(15),
            "description": random_string(50),
            "price": round(random.uniform(1, 1000), 2),
            "tax": round(random.uniform(0, 50), 2),
            "tags": [random_string(5) for _ in range(3)],
        },
    )

    # GET with query params
    suite.add_benchmark(
        "GET /items?page=1&size=10",
        "GET",
        "/items",
        params={"page": 1, "size": 10},
    )


def create_auth_benchmarks(suite: BenchmarkSuite):
    """Add auth benchmarks to the suite."""
    suite.add_benchmark("GET /public", "GET", "/public")
    suite.add_benchmark(
        "POST /token (login)",
        "POST",
        "/token",
        data={"username": "testuser", "password": "password123"},
    )


def print_results(results: List[BenchmarkResult], framework: str):
    """Print benchmark results in a nice format."""
    print(f"\n{'=' * 60}")
    print(f"Results for {framework.upper()}")
    print(f"{'=' * 60}")

    for result in results:
        print(f"\n{result.name}")
        print(
            f"  Requests: {result.total_requests} ({result.success_rate:.1f}% success)"
        )
        print(f"  RPS: {result.requests_per_second:.2f}")
        print(
            f"  Latency (ms): avg={result.avg_latency_ms:.2f}, p50={result.p50_latency_ms:.2f}, p95={result.p95_latency_ms:.2f}, p99={result.p99_latency_ms:.2f}"
        )


def compare_results(
    fastapi_results: List[BenchmarkResult],
    fasterapi_results: List[BenchmarkResult],
):
    """Compare and print comparison between frameworks."""
    print(f"\n{'=' * 80}")
    print("COMPARISON: FastAPI vs FasterAPI")
    print(f"{'=' * 80}")

    print(
        f"\n{'Benchmark':<35} {'FastAPI RPS':>12} {'FasterAPI RPS':>14} {'Speedup':>10}"
    )
    print("-" * 80)

    for fast_result, faster_result in zip(fastapi_results, fasterapi_results):
        fast_rps = fast_result.requests_per_second
        faster_rps = faster_result.requests_per_second
        speedup = faster_rps / fast_rps if fast_rps > 0 else 0
        speedup_str = (
            f"{speedup:.2f}x" if speedup >= 1 else f"{1 / speedup:.2f}x slower"
        )

        print(
            f"{fast_result.name:<35} {fast_rps:>12.2f} {faster_rps:>14.2f} {speedup_str:>10}"
        )

    print()
    print("Latency Comparison (p95 ms):")
    print(f"{'Benchmark':<35} {'FastAPI':>12} {'FasterAPI':>14} {'Improvement':>12}")
    print("-" * 80)

    for fast_result, faster_result in zip(fastapi_results, fasterapi_results):
        fast_p95 = fast_result.p95_latency_ms
        faster_p95 = faster_result.p95_latency_ms
        improvement = (fast_p95 - faster_p95) / fast_p95 * 100 if fast_p95 > 0 else 0

        print(
            f"{fast_result.name:<35} {fast_p95:>12.2f} {faster_p95:>14.2f} {improvement:>11.1f}%"
        )


async def main_async():
    """Main async entry point."""
    parser = argparse.ArgumentParser(description="Benchmark FastAPI vs FasterAPI")
    parser.add_argument(
        "--requests", type=int, default=100, help="Number of requests per benchmark"
    )
    parser.add_argument(
        "--concurrency", type=int, default=10, help="Concurrent requests"
    )
    parser.add_argument("--port", type=int, default=8000, help="Server port")
    parser.add_argument("--output", type=str, help="Output JSON file")
    args = parser.parse_args()

    base_url = f"http://localhost:{args.port}"

    # Create benchmark suite
    suite = BenchmarkSuite(
        base_url=base_url,
        num_requests=args.requests,
        concurrency=args.concurrency,
    )

    create_crud_benchmarks(suite)

    print(f"Benchmark Configuration:")
    print(f"  Requests per benchmark: {args.requests}")
    print(f"  Concurrency: {args.concurrency}")
    print(f"  Benchmarks: {len(suite.benchmarks)}")

    # Note: In a real scenario, you would:
    # 1. Start FastAPI server, run benchmarks, stop server
    # 2. Start FasterAPI server, run benchmarks, stop server
    # 3. Compare results

    # For now, we'll just run against whatever server is running
    print(f"\nRunning benchmarks against {base_url}...")

    try:
        # Determine which framework is running
        import aiohttp

        async with aiohttp.ClientSession() as session:
            async with session.get(f"{base_url}/health") as resp:
                data = await resp.json()
                framework = data.get("framework", "unknown")
    except Exception as e:
        print(f"Error: Could not connect to server at {base_url}")
        print(f"Please start the server first:")
        print(f"  TEST_FRAMEWORK=fastapi python -m apps.crud_app")
        print(f"  or")
        print(f"  TEST_FRAMEWORK=fasterapi python -m apps.crud_app")
        return

    print(f"Detected framework: {framework}")

    results = await suite.run_all_async(framework)
    print_results(results, framework)

    if args.output:
        output_data = {
            "framework": framework,
            "config": {
                "requests": args.requests,
                "concurrency": args.concurrency,
            },
            "results": [r.to_dict() for r in results],
        }
        with open(args.output, "w") as f:
            json.dump(output_data, f, indent=2)
        print(f"\nResults saved to {args.output}")


def main():
    """Main entry point."""
    asyncio.run(main_async())


if __name__ == "__main__":
    main()
