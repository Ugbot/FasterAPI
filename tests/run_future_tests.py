"""
Simple test runner for future tests (no pytest required).
"""

import asyncio
import sys
sys.path.insert(0, '/Users/bengamble/FasterAPI')

from fasterapi.core import Future, when_all, when_any, Reactor
from fasterapi.core.combinators import (
    map_async, filter_async, reduce_async,
    retry_async, timeout_async, Pipeline, when_some
)


def test(name):
    """Test decorator."""
    def decorator(func):
        func.test_name = name
        return func
    return decorator


class TestRunner:
    """Simple test runner."""
    
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.errors = []
    
    def run_test(self, test_func):
        """Run a single test."""
        test_name = getattr(test_func, 'test_name', test_func.__name__)
        try:
            if asyncio.iscoroutinefunction(test_func):
                asyncio.run(test_func())
            else:
                test_func()
            print(f"✅ {test_name}")
            self.passed += 1
        except AssertionError as e:
            print(f"❌ {test_name}: {e}")
            self.failed += 1
            self.errors.append((test_name, str(e)))
        except Exception as e:
            print(f"💥 {test_name}: {e}")
            self.failed += 1
            self.errors.append((test_name, str(e)))
    
    def report(self):
        """Print test report."""
        print("\n" + "="*60)
        print(f"Tests: {self.passed + self.failed}")
        print(f"Passed: {self.passed}")
        print(f"Failed: {self.failed}")
        
        if self.errors:
            print("\nFailures:")
            for name, error in self.errors:
                print(f"  - {name}: {error}")
        
        return self.failed == 0


# Test Basic Futures
@test("Create ready future")
def test_make_ready():
    f = Future.make_ready(42)
    assert f.is_ready(), "Future should be ready"
    assert not f.failed(), "Future should not be failed"
    assert f.get() == 42, "Future value should be 42"


@test("Create failed future")
def test_make_exception():
    f = Future.make_exception(ValueError("test error"))
    # A failed future is NOT ready (ready means successfully resolved)
    # But it IS failed
    assert f.failed(), "Future should be failed"


@test("Await ready future")
async def test_await_ready():
    f = Future.make_ready(123)
    result = await f
    assert result == 123, f"Expected 123, got {result}"


@test("Await multiple futures")
async def test_await_multiple():
    f1 = Future.make_ready(1)
    f2 = Future.make_ready(2)
    f3 = Future.make_ready(3)
    
    results = await asyncio.gather(f1, f2, f3)
    assert results == [1, 2, 3], f"Expected [1,2,3], got {results}"


# Test Chaining
@test("Simple chain")
def test_simple_chain():
    f = Future.make_ready(10)
    result = f.then(lambda x: x * 2).get()
    assert result == 20, f"Expected 20, got {result}"


@test("Multi-step chain")
def test_multi_chain():
    result = (Future.make_ready(5)
              .then(lambda x: x * 2)
              .then(lambda x: x + 5)
              .then(lambda x: x * 3)
              .get())
    assert result == 45, f"Expected 45, got {result}"


@test("Chain with type changes")
def test_chain_types():
    result = (Future.make_ready(42)
              .then(lambda x: str(x))
              .then(lambda x: x + " is the answer")
              .get())
    assert result == "42 is the answer", f"Unexpected result: {result}"


# Test Parallel Execution
@test("when_all combinator")
async def test_when_all():
    futures = [Future.make_ready(i * i) for i in range(5)]
    results = await when_all(futures)
    assert results == [0, 1, 4, 9, 16], f"Expected [0,1,4,9,16], got {results}"


@test("when_all empty list")
async def test_when_all_empty():
    results = await when_all([])
    assert results == [], f"Expected [], got {results}"


@test("when_some combinator")
async def test_when_some_op():
    futures = [Future.make_ready(i) for i in range(10)]
    results = await when_some(futures, 3)
    assert len(results) == 3, f"Expected 3 results, got {len(results)}"


# Test Transformations
@test("map_async")
async def test_map():
    futures = [Future.make_ready(i) for i in range(5)]
    results = await map_async(lambda x: x * 2, futures)
    assert results == [0, 2, 4, 6, 8], f"Expected [0,2,4,6,8], got {results}"


@test("filter_async")
async def test_filter():
    futures = [Future.make_ready(i) for i in range(10)]
    results = await filter_async(lambda x: x % 2 == 0, futures)
    assert results == [0, 2, 4, 6, 8], f"Expected evens, got {results}"


@test("reduce_async")
async def test_reduce():
    futures = [Future.make_ready(i) for i in range(1, 6)]
    result = await reduce_async(lambda acc, x: acc + x, futures, 0)
    assert result == 15, f"Expected 15, got {result}"


# Test Error Handling
@test("handle_error")
def test_handle_error():
    f = Future.make_exception(ValueError("error"))
    result = f.handle_error(lambda e: "default")
    assert result.get() == "default", "Should return default value"


@test("retry success")
async def test_retry():
    call_count = [0]
    
    def operation():
        call_count[0] += 1
        if call_count[0] < 3:
            return Future.make_exception(Exception("fail"))
        return Future.make_ready("success")
    
    result = await retry_async(operation, max_retries=5, delay=0.01)
    assert result == "success", f"Expected success, got {result}"
    assert call_count[0] == 3, f"Expected 3 calls, got {call_count[0]}"


# Test Pipeline
@test("simple pipeline")
async def test_pipeline():
    pipeline = (Pipeline()
                .add(lambda: 10)
                .add(lambda x: x * 2)
                .add(lambda x: x + 5))
    
    result = await pipeline.execute()
    assert result == 25, f"Expected 25, got {result}"


@test("pipeline with strings")
async def test_pipeline_strings():
    pipeline = (Pipeline()
                .add(lambda: "  hello world  ")
                .add(lambda x: x.strip())
                .add(lambda x: x.upper())
                .add(lambda x: x.split()))
    
    result = await pipeline.execute()
    assert result == ['HELLO', 'WORLD'], f"Unexpected result: {result}"


# Test Reactor
@test("reactor initialize")
def test_reactor_init():
    Reactor.initialize()
    assert Reactor.is_initialized(), "Reactor should be initialized"
    assert Reactor.num_cores() > 0, "Should have at least 1 core"


@test("reactor current core")
def test_reactor_core():
    Reactor.initialize()
    core = Reactor.current_core()
    assert isinstance(core, int), "Core should be an integer"
    assert core >= 0, "Core should be non-negative"


# Test Complex Scenarios
@test("mixed sync/async")
async def test_mixed():
    sync_result = (Future.make_ready(10)
                   .then(lambda x: x * 2)
                   .get())
    
    async_result = await Future.make_ready(20)
    
    assert sync_result + async_result == 40, "Combined result should be 40"


@test("many chains")
def test_many_chains():
    f = Future.make_ready(0)
    for i in range(100):
        f = f.then(lambda x: x + 1)
    
    result = f.get()
    assert result == 100, f"Expected 100, got {result}"


@test("many parallel")
async def test_many_parallel():
    futures = [Future.make_ready(i) for i in range(100)]
    results = await when_all(futures)
    assert len(results) == 100, f"Expected 100 results, got {len(results)}"
    assert results == list(range(100)), "Results should match range(100)"


def main():
    """Run all tests."""
    print("╔══════════════════════════════════════════════════════════╗")
    print("║          FasterAPI Future Test Suite                    ║")
    print("╚══════════════════════════════════════════════════════════╝")
    print()
    
    runner = TestRunner()
    
    # Collect all test functions
    tests = [
        test_make_ready,
        test_make_exception,
        test_await_ready,
        test_await_multiple,
        test_simple_chain,
        test_multi_chain,
        test_chain_types,
        test_when_all,
        test_when_all_empty,
        test_when_some_op,
        test_map,
        test_filter,
        test_reduce,
        test_handle_error,
        test_retry,
        test_pipeline,
        test_pipeline_strings,
        test_reactor_init,
        test_reactor_core,
        test_mixed,
        test_many_chains,
        test_many_parallel,
    ]
    
    # Run all tests
    for test_func in tests:
        runner.run_test(test_func)
    
    # Print report
    success = runner.report()
    
    if success:
        print("\n🎉 All tests passed!")
        return 0
    else:
        print("\n❌ Some tests failed")
        return 1


if __name__ == "__main__":
    sys.exit(main())

