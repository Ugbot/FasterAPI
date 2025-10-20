"""
Future Chaining Benchmarks

Compare performance of:
1. Async/await (ergonomic)
2. Explicit .then() chains (performance)
3. Blocking operations (baseline)
"""

import asyncio
import time
from fasterapi.core import Future
from fasterapi.core.combinators import when_all


def benchmark(name: str, func, iterations: int = 10000):
    """Run a benchmark."""
    start = time.perf_counter()
    
    for _ in range(iterations):
        func()
    
    end = time.perf_counter()
    elapsed = (end - start) * 1000  # ms
    per_op = (elapsed / iterations) * 1000  # µs
    
    print(f"{name:40} {per_op:8.2f} µs/op  ({iterations} iterations)")
    return per_op


async def benchmark_async(name: str, func, iterations: int = 10000):
    """Run an async benchmark."""
    start = time.perf_counter()
    
    for _ in range(iterations):
        await func()
    
    end = time.perf_counter()
    elapsed = (end - start) * 1000  # ms
    per_op = (elapsed / iterations) * 1000  # µs
    
    print(f"{name:40} {per_op:8.2f} µs/op  ({iterations} iterations)")
    return per_op


# Test 1: Future creation
def test_future_creation():
    """Create a ready future."""
    f = Future.make_ready(42)
    return f


# Test 2: Explicit chaining (sync)
def test_explicit_chain():
    """Explicit future chain."""
    result = (Future.make_ready(10)
              .then(lambda x: x * 2)
              .then(lambda x: x + 5)
              .then(lambda x: x - 3)
              .get())
    return result


# Test 3: Async/await style
async def test_async_await():
    """Async/await future."""
    f = Future.make_ready(10)
    result = await f
    return result * 2


# Test 4: Multiple futures (explicit)
def test_multiple_explicit():
    """Multiple futures with explicit chaining."""
    futures = [Future.make_ready(i) for i in range(10)]
    results = [f.then(lambda x: x * 2).get() for f in futures]
    return results


# Test 5: Multiple futures (async)
async def test_multiple_async():
    """Multiple futures with async/await."""
    futures = [Future.make_ready(i) for i in range(10)]
    results = await when_all(futures)
    return results


# Test 6: Complex chain (explicit)
def test_complex_explicit():
    """Complex transformation chain."""
    result = (Future.make_ready([1, 2, 3, 4, 5])
              .then(lambda x: [i * 2 for i in x])
              .then(lambda x: [i + 1 for i in x])
              .then(lambda x: sum(x))
              .get())
    return result


# Test 7: Complex chain (async)
async def test_complex_async():
    """Complex transformation chain with async."""
    f = Future.make_ready([1, 2, 3, 4, 5])
    x = await f
    x = [i * 2 for i in x]
    x = [i + 1 for i in x]
    return sum(x)


# Test 8: Baseline (no futures)
def test_baseline():
    """Baseline without futures."""
    x = 10
    x = x * 2
    x = x + 5
    x = x - 3
    return x


async def main():
    """Run all benchmarks."""
    print("╔════════════════════════════════════════════════════════════╗")
    print("║            FasterAPI Future Benchmarks                     ║")
    print("╚════════════════════════════════════════════════════════════╝")
    print()
    
    iterations = 100000
    
    print(f"Running {iterations} iterations per test...\n")
    
    # Synchronous benchmarks
    print("=== Synchronous Benchmarks ===")
    baseline = benchmark("Baseline (no futures)", test_baseline, iterations)
    creation = benchmark("Future creation", test_future_creation, iterations)
    explicit = benchmark("Explicit chain (3 ops)", test_explicit_chain, iterations)
    multiple_explicit = benchmark("Multiple futures (10x)", test_multiple_explicit, iterations // 10)
    complex_explicit = benchmark("Complex chain (list ops)", test_complex_explicit, iterations)
    
    print()
    
    # Async benchmarks
    print("=== Async Benchmarks ===")
    async_single = await benchmark_async("Async/await (single)", test_async_await, iterations)
    async_multiple = await benchmark_async("Async/await (10x)", test_multiple_async, iterations // 10)
    async_complex = await benchmark_async("Async complex chain", test_complex_async, iterations)
    
    print()
    
    # Analysis
    print("=== Performance Analysis ===")
    print(f"Future creation overhead:        {creation - baseline:8.2f} µs  ({creation / baseline:.2f}x baseline)")
    print(f"Explicit chain overhead:         {explicit - baseline:8.2f} µs  ({explicit / baseline:.2f}x baseline)")
    print(f"Async/await overhead:            {async_single - baseline:8.2f} µs  ({async_single / baseline:.2f}x baseline)")
    print()
    print(f"Explicit vs Async (single):      {async_single / explicit:.2f}x")
    print(f"Explicit vs Async (multiple):    {async_multiple / multiple_explicit:.2f}x")
    print(f"Explicit vs Async (complex):     {async_complex / complex_explicit:.2f}x")
    
    print()
    print("=== Recommendations ===")
    if explicit < async_single:
        speedup = async_single / explicit
        print(f"✅ Explicit chains are {speedup:.1f}x faster than async/await")
        print("   Use explicit chains for performance-critical hot paths")
    else:
        print("✅ Async/await is competitive with explicit chains")
        print("   Use async/await for better code readability")
    
    print()
    print("=== Performance Targets ===")
    print(f"Future allocation:         {creation:8.2f} µs (target: ~0 µs, stack-only)")
    print(f"Continuation overhead:     {(explicit - baseline) / 3:8.2f} µs (target: <0.01 µs per .then())")
    print(f"Async/await overhead:      {async_single - baseline:8.2f} µs (target: <0.1 µs)")
    
    if creation < 0.1:
        print("✅ Future allocation target met (near-zero)")
    else:
        print(f"⚠️  Future allocation: {creation:.2f} µs (target: <0.1 µs)")
    
    if (explicit - baseline) / 3 < 0.01:
        print("✅ Continuation overhead target met")
    else:
        print(f"⚠️  Continuation overhead: {(explicit - baseline) / 3:.2f} µs (target: <0.01 µs)")
    
    if (async_single - baseline) < 0.1:
        print("✅ Async/await overhead target met")
    else:
        print(f"⚠️  Async/await overhead: {async_single - baseline:.2f} µs (target: <0.1 µs)")


if __name__ == "__main__":
    asyncio.run(main())

