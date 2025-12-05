#!/usr/bin/env python3.13
"""
Simple benchmark comparing FasterAPI vs FastAPI.

Uses Python's requests library for controlled testing.
"""

import time
import statistics
import requests
import subprocess
import sys
import os
from concurrent.futures import ThreadPoolExecutor, as_completed

def benchmark_endpoint(url, num_requests=1000, concurrency=50):
    """Benchmark a single endpoint."""
    print(f"  Testing {url}")
    print(f"  Requests: {num_requests}, Concurrency: {concurrency}")

    latencies = []
    errors = 0

    def make_request():
        start = time.perf_counter()
        try:
            response = requests.get(url, timeout=5)
            end = time.perf_counter()
            latency_ms = (end - start) * 1000
            if response.status_code == 200:
                return latency_ms, None
            else:
                return None, f"Status {response.status_code}"
        except Exception as e:
            return None, str(e)

    # Warm up
    print("  Warming up...")
    for _ in range(10):
        make_request()

    # Actual benchmark
    print("  Running benchmark...")
    start_time = time.perf_counter()

    with ThreadPoolExecutor(max_workers=concurrency) as executor:
        futures = [executor.submit(make_request) for _ in range(num_requests)]

        for future in as_completed(futures):
            latency, error = future.result()
            if latency is not None:
                latencies.append(latency)
            else:
                errors += 1

    end_time = time.perf_counter()
    total_time = end_time - start_time

    if not latencies:
        return None

    # Calculate statistics
    latencies.sort()
    return {
        'total_requests': num_requests,
        'successful': len(latencies),
        'errors': errors,
        'total_time': total_time,
        'rps': len(latencies) / total_time,
        'mean': statistics.mean(latencies),
        'median': statistics.median(latencies),
        'p95': latencies[int(len(latencies) * 0.95)],
        'p99': latencies[int(len(latencies) * 0.99)],
        'min': min(latencies),
        'max': max(latencies),
    }

def wait_for_server(url, timeout=30):
    """Wait for server to be ready."""
    print(f"  Waiting for server at {url}...")
    start = time.time()
    while time.time() - start < timeout:
        try:
            response = requests.get(url, timeout=1)
            if response.status_code == 200:
                print("  ✓ Server ready")
                return True
        except:
            time.sleep(0.5)
    return False

def print_results(name, results):
    """Print benchmark results."""
    if results is None:
        print(f"\n{name}: FAILED")
        return

    print(f"\n{name} Results:")
    print(f"  Requests/sec:  {results['rps']:.2f}")
    print(f"  Mean latency:  {results['mean']:.2f} ms")
    print(f"  Median:        {results['median']:.2f} ms")
    print(f"  P95:           {results['p95']:.2f} ms")
    print(f"  P99:           {results['p99']:.2f} ms")
    print(f"  Min/Max:       {results['min']:.2f} / {results['max']:.2f} ms")
    print(f"  Success rate:  {results['successful']}/{results['total_requests']}")

def main():
    print("="*80)
    print("FasterAPI vs FastAPI Simple Benchmark")
    print("="*80)

    # Configuration
    num_requests = 5000
    concurrency = 50

    # Kill any existing processes
    os.system("lsof -ti:8000 | xargs kill -9 2>/dev/null")
    os.system("lsof -ti:8001 | xargs kill -9 2>/dev/null")
    time.sleep(1)

    all_results = {}

    # Test FastAPI
    print("\n" + "="*80)
    print("1. Testing FastAPI (Reference)")
    print("="*80)

    fastapi_proc = subprocess.Popen(
        ["python3.13", "benchmarks/fastapi_reference.py"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )

    try:
        if wait_for_server("http://localhost:8001/health"):
            time.sleep(1)

            # Test endpoints
            all_results['fastapi_root'] = benchmark_endpoint(
                "http://localhost:8001/",
                num_requests,
                concurrency
            )
            print_results("FastAPI GET /", all_results['fastapi_root'])

            all_results['fastapi_health'] = benchmark_endpoint(
                "http://localhost:8001/health",
                num_requests,
                concurrency
            )
            print_results("FastAPI GET /health", all_results['fastapi_health'])

            all_results['fastapi_items'] = benchmark_endpoint(
                "http://localhost:8001/items",
                num_requests,
                concurrency
            )
            print_results("FastAPI GET /items", all_results['fastapi_items'])
        else:
            print("  ✗ FastAPI server failed to start")
    finally:
        fastapi_proc.terminate()
        fastapi_proc.wait(timeout=5)

    time.sleep(2)

    # Test FasterAPI
    print("\n" + "="*80)
    print("2. Testing FasterAPI (C++ Optimized)")
    print("="*80)

    env = os.environ.copy()
    env['DYLD_LIBRARY_PATH'] = f"{os.getcwd()}/build/lib:{env.get('DYLD_LIBRARY_PATH', '')}"

    fasterapi_proc = subprocess.Popen(
        ["python3.13", "examples/run_cpp_fastapi_server.py"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=env
    )

    try:
        if wait_for_server("http://localhost:8000/health"):
            time.sleep(1)

            # Test endpoints
            all_results['fasterapi_root'] = benchmark_endpoint(
                "http://localhost:8000/",
                num_requests,
                concurrency
            )
            print_results("FasterAPI GET /", all_results['fasterapi_root'])

            all_results['fasterapi_health'] = benchmark_endpoint(
                "http://localhost:8000/health",
                num_requests,
                concurrency
            )
            print_results("FasterAPI GET /health", all_results['fasterapi_health'])

            all_results['fasterapi_items'] = benchmark_endpoint(
                "http://localhost:8000/items",
                num_requests,
                concurrency
            )
            print_results("FasterAPI GET /items", all_results['fasterapi_items'])
        else:
            print("  ✗ FasterAPI server failed to start")
    finally:
        fasterapi_proc.terminate()
        fasterapi_proc.wait(timeout=5)

    # Comparison
    print("\n" + "="*80)
    print("COMPARISON SUMMARY")
    print("="*80)

    def compare(endpoint_name, fastapi_key, fasterapi_key):
        fastapi = all_results.get(fastapi_key)
        fasterapi = all_results.get(fasterapi_key)

        if fastapi and fasterapi:
            rps_speedup = fasterapi['rps'] / fastapi['rps']
            latency_improvement = (fastapi['mean'] - fasterapi['mean']) / fastapi['mean'] * 100

            print(f"\n{endpoint_name}:")
            print(f"  {'Metric':<20} {'FastAPI':<15} {'FasterAPI':<15} {'Improvement'}")
            print(f"  {'-'*20} {'-'*15} {'-'*15} {'-'*20}")
            print(f"  {'Requests/sec':<20} {fastapi['rps']:>14.0f} {fasterapi['rps']:>14.0f}  {rps_speedup:>6.2f}x faster")
            print(f"  {'Mean latency (ms)':<20} {fastapi['mean']:>14.2f} {fasterapi['mean']:>14.2f}  {latency_improvement:>6.1f}% faster")
            print(f"  {'P95 latency (ms)':<20} {fastapi['p95']:>14.2f} {fasterapi['p95']:>14.2f}")
            print(f"  {'P99 latency (ms)':<20} {fastapi['p99']:>14.2f} {fasterapi['p99']:>14.2f}")

    compare("GET /", 'fastapi_root', 'fasterapi_root')
    compare("GET /health", 'fastapi_health', 'fasterapi_health')
    compare("GET /items", 'fastapi_items', 'fasterapi_items')

    print("\n" + "="*80)
    print("Benchmark Complete!")
    print("="*80)

if __name__ == "__main__":
    main()
