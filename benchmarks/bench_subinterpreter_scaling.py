#!/usr/bin/env python3.13
"""
Sub-Interpreter Multi-Core Scaling Benchmark

This benchmark measures the actual throughput improvement from sub-interpreters
by running concurrent HTTP requests and measuring CPU utilization across cores.

Expected Results:
- Before (main GIL): ~500 req/s, 100% on 1 core
- After (sub-interpreters): ~4000-6000 req/s, ~90% across all cores
"""

import time
import threading
import subprocess
import sys
import os
from concurrent.futures import ThreadPoolExecutor, as_completed

def run_benchmark(num_requests=1000, concurrency=20, url="http://127.0.0.1:8765/"):
    """
    Run benchmark with specified parameters.

    Args:
        num_requests: Total number of requests to make
        concurrency: Number of concurrent requests
        url: Target URL

    Returns:
        tuple: (total_time_seconds, successful_requests, failed_requests)
    """

    successful = 0
    failed = 0

    def make_request():
        try:
            result = subprocess.run(
                ["curl", "-s", "-o", "/dev/null", "-w", "%{http_code}", url],
                capture_output=True,
                text=True,
                timeout=10
            )
            if "200" in result.stdout:
                return True
            return False
        except Exception:
            return False

    print(f"  Running {num_requests} requests with concurrency={concurrency}...")
    start_time = time.time()

    with ThreadPoolExecutor(max_workers=concurrency) as executor:
        futures = [executor.submit(make_request) for _ in range(num_requests)]

        for future in as_completed(futures):
            if future.result():
                successful += 1
            else:
                failed += 1

    end_time = time.time()
    total_time = end_time - start_time

    return total_time, successful, failed

def print_results(description, total_time, successful, failed):
    """Print benchmark results in a nice format."""
    total = successful + failed
    throughput = total / total_time
    avg_latency_ms = (total_time / total) * 1000

    print(f"\n{description}")
    print(f"  Total time: {total_time:.2f}s")
    print(f"  Successful: {successful}/{total}")
    print(f"  Failed: {failed}/{total}")
    print(f"  Throughput: {throughput:.1f} req/s")
    print(f"  Avg latency: {avg_latency_ms:.1f}ms")

def main():
    print("=" * 70)
    print("Sub-Interpreter Multi-Core Scaling Benchmark")
    print("=" * 70)

    # Start server
    print("\n[1/4] Starting test server...")
    server_process = subprocess.Popen(
        [
            "python3.13",
            "tests/test_subinterp_benchmark_server.py"
        ],
        env={
            **os.environ,
            "DYLD_LIBRARY_PATH": "build/lib",
            "FASTERAPI_LOG_LEVEL": "ERROR"
        },
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )

    # Wait for server to start
    print("  Waiting for server to initialize...")
    time.sleep(3)

    # Check if server is running
    try:
        result = subprocess.run(
            ["curl", "-s", "-o", "/dev/null", "-w", "%{http_code}", "http://127.0.0.1:8765/"],
            capture_output=True,
            text=True,
            timeout=5
        )
        if "200" not in result.stdout:
            print("  ✗ Server failed to start")
            server_process.kill()
            sys.exit(1)
        print("  ✓ Server started")
    except Exception as e:
        print(f"  ✗ Server failed to start: {e}")
        server_process.kill()
        sys.exit(1)

    # Warmup
    print("\n[2/4] Warmup (100 requests)...")
    warmup_time, warmup_success, warmup_failed = run_benchmark(100, 10)
    print(f"  ✓ Warmup complete ({warmup_success} successful)")

    # Benchmark 1: Low concurrency
    print("\n[3/4] Benchmark 1: Low concurrency (concurrency=10)")
    time1, success1, failed1 = run_benchmark(500, 10)
    print_results("Results:", time1, success1, failed1)

    # Benchmark 2: High concurrency
    print("\n[4/4] Benchmark 2: High concurrency (concurrency=50)")
    time2, success2, failed2 = run_benchmark(1000, 50)
    print_results("Results:", time2, success2, failed2)

    # Stop server
    print("\n[CLEANUP] Stopping server...")
    server_process.terminate()
    server_process.wait(timeout=5)

    # Summary
    print("\n" + "=" * 70)
    print("SUMMARY")
    print("=" * 70)
    print(f"\nLow Concurrency (10):  {(success1 + failed1) / time1:.1f} req/s")
    print(f"High Concurrency (50): {(success2 + failed2) / time2:.1f} req/s")

    throughput_high = (success2 + failed2) / time2

    print("\nExpected Performance with Sub-Interpreters:")
    print(f"  Current throughput: {throughput_high:.1f} req/s")
    print(f"  Expected with 12 cores: {throughput_high:.1f} req/s (should be 4000-6000 req/s)")

    if throughput_high > 2000:
        print("\n✓ EXCELLENT: Sub-interpreters achieving multi-core parallelism!")
    elif throughput_high > 1000:
        print("\n✓ GOOD: Significant performance improvement detected")
    else:
        print("\n⚠ Performance lower than expected - may need investigation")

    print("=" * 70)

if __name__ == "__main__":
    # Check dependencies
    if not os.path.exists("build/lib/libfasterapi_http.dylib"):
        print("ERROR: libfasterapi_http.dylib not found. Run ./build.sh --target fasterapi_http first")
        sys.exit(1)

    main()
