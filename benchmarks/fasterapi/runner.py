"""Benchmark runner - orchestrates all performance tests."""

import sys
import time
import subprocess
from pathlib import Path
from datetime import datetime


def run_benchmark(script_name: str) -> dict:
    """Run a benchmark script and capture results.
    
    Args:
        script_name: Name of benchmark module (bench_pool.py, etc.)
        
    Returns:
        Results dict from benchmark.
    """
    print(f"\n{'='*60}")
    print(f"Running: {script_name}")
    print(f"{'='*60}")
    
    try:
        result = subprocess.run(
            [sys.executable, f"benchmarks/{script_name}"],
            timeout=300,
            capture_output=True,
            text=True
        )
        
        if result.returncode != 0:
            print(f"FAILED: {script_name}")
            print(f"stdout: {result.stdout}")
            print(f"stderr: {result.stderr}")
            return {"status": "failed", "error": result.stderr}
        
        print(result.stdout)
        return {"status": "passed", "script": script_name}
    except subprocess.TimeoutExpired:
        print(f"TIMEOUT: {script_name} exceeded 5 minutes")
        return {"status": "timeout"}
    except Exception as e:
        print(f"ERROR: {script_name}: {e}")
        return {"status": "error", "error": str(e)}


def main():
    """Run all benchmarks and generate report."""
    print("FasterAPI PostgreSQL Benchmarks")
    print(f"Started: {datetime.now()}")
    
    benchmarks = [
        "bench_pool.py",
        "bench_codecs.py",
    ]
    
    results = {}
    for benchmark in benchmarks:
        results[benchmark] = run_benchmark(benchmark)
        time.sleep(1)  # Brief pause between benchmarks
    
    # Generate summary
    print(f"\n{'='*60}")
    print("BENCHMARK SUMMARY")
    print(f"{'='*60}")
    
    passed = sum(1 for r in results.values() if r.get("status") == "passed")
    failed = sum(1 for r in results.values() if r.get("status") == "failed")
    
    print(f"Passed: {passed}/{len(benchmarks)}")
    print(f"Failed: {failed}/{len(benchmarks)}")
    print(f"Completed: {datetime.now()}")
    
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
