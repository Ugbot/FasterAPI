"""
Unit Tests for Future Chaining

Comprehensive test suite for FasterAPI's Seastar-style futures.
"""

import pytest
import asyncio
from fasterapi.core import Future, when_all, when_any, Reactor
from fasterapi.core.combinators import (
    map_async, filter_async, reduce_async,
    retry_async, timeout_async, Pipeline, when_some
)


class TestBasicFutures:
    """Test basic future operations."""
    
    def test_make_ready(self):
        """Test creating a ready future."""
        f = Future.make_ready(42)
        assert f.is_ready()
        assert not f.failed()
        assert f.get() == 42
    
    def test_make_exception(self):
        """Test creating a failed future."""
        f = Future.make_exception(ValueError("test error"))
        assert not f.is_ready()
        assert f.failed()
    
    @pytest.mark.asyncio
    async def test_await_ready_future(self):
        """Test awaiting an already-ready future."""
        f = Future.make_ready(123)
        result = await f
        assert result == 123
    
    @pytest.mark.asyncio
    async def test_await_multiple_futures(self):
        """Test awaiting multiple futures."""
        f1 = Future.make_ready(1)
        f2 = Future.make_ready(2)
        f3 = Future.make_ready(3)
        
        results = await asyncio.gather(f1, f2, f3)
        assert results == [1, 2, 3]


class TestChaining:
    """Test future chaining operations."""
    
    def test_simple_chain(self):
        """Test simple .then() chain."""
        f = Future.make_ready(10)
        result = f.then(lambda x: x * 2).get()
        assert result == 20
    
    def test_multi_step_chain(self):
        """Test multi-step chain."""
        result = (Future.make_ready(5)
                  .then(lambda x: x * 2)      # 10
                  .then(lambda x: x + 5)      # 15
                  .then(lambda x: x * 3)      # 45
                  .get())
        assert result == 45
    
    def test_chain_with_types(self):
        """Test chain that changes types."""
        result = (Future.make_ready(42)
                  .then(lambda x: str(x))
                  .then(lambda x: x + " is the answer")
                  .get())
        assert result == "42 is the answer"
    
    def test_chain_with_error(self):
        """Test error propagation through chain."""
        f = Future.make_exception(ValueError("error"))
        result = f.then(lambda x: x * 2)
        assert result.failed()


class TestParallelExecution:
    """Test parallel execution patterns."""
    
    @pytest.mark.asyncio
    async def test_when_all(self):
        """Test when_all combinator."""
        futures = [Future.make_ready(i * i) for i in range(5)]
        results = await when_all(futures)
        assert results == [0, 1, 4, 9, 16]
    
    @pytest.mark.asyncio
    async def test_when_all_empty(self):
        """Test when_all with empty list."""
        results = await when_all([])
        assert results == []
    
    @pytest.mark.asyncio
    async def test_when_any(self):
        """Test when_any combinator."""
        futures = [
            Future.make_ready(1),
            Future.make_ready(2),
            Future.make_ready(3)
        ]
        result, remaining = await when_any(futures)
        assert result in [1, 2, 3]
        assert len(remaining) >= 0
    
    @pytest.mark.asyncio
    async def test_when_some(self):
        """Test when_some combinator."""
        futures = [Future.make_ready(i) for i in range(10)]
        results = await when_some(futures, 3)
        assert len(results) == 3
        assert all(r in range(10) for r in results)


class TestTransformations:
    """Test transformation combinators."""
    
    @pytest.mark.asyncio
    async def test_map_async(self):
        """Test map_async combinator."""
        futures = [Future.make_ready(i) for i in range(5)]
        results = await map_async(lambda x: x * 2, futures)
        assert results == [0, 2, 4, 6, 8]
    
    @pytest.mark.asyncio
    async def test_filter_async(self):
        """Test filter_async combinator."""
        futures = [Future.make_ready(i) for i in range(10)]
        results = await filter_async(lambda x: x % 2 == 0, futures)
        assert results == [0, 2, 4, 6, 8]
    
    @pytest.mark.asyncio
    async def test_reduce_async(self):
        """Test reduce_async combinator."""
        futures = [Future.make_ready(i) for i in range(1, 6)]
        result = await reduce_async(lambda acc, x: acc + x, futures, 0)
        assert result == 15  # 1+2+3+4+5


class TestErrorHandling:
    """Test error handling patterns."""
    
    def test_handle_error(self):
        """Test error handling with handle_error."""
        f = Future.make_exception(ValueError("error"))
        result = f.handle_error(lambda e: "default")
        assert result.get() == "default"
    
    def test_handle_error_no_error(self):
        """Test handle_error when no error."""
        f = Future.make_ready(42)
        result = f.handle_error(lambda e: 0)
        assert result.get() == 42
    
    @pytest.mark.asyncio
    async def test_retry_success_first_try(self):
        """Test retry that succeeds on first try."""
        call_count = [0]
        
        def operation():
            call_count[0] += 1
            return Future.make_ready("success")
        
        result = await retry_async(operation, max_retries=3, delay=0.01)
        assert result == "success"
        assert call_count[0] == 1
    
    @pytest.mark.asyncio
    async def test_retry_success_after_failures(self):
        """Test retry that succeeds after failures."""
        call_count = [0]
        
        def operation():
            call_count[0] += 1
            if call_count[0] < 3:
                return Future.make_exception(Exception("fail"))
            return Future.make_ready("success")
        
        result = await retry_async(operation, max_retries=5, delay=0.01)
        assert result == "success"
        assert call_count[0] == 3
    
    @pytest.mark.asyncio
    async def test_timeout_success(self):
        """Test timeout with successful completion."""
        f = Future.make_ready(42)
        result = await timeout_async(f, timeout_seconds=1.0)
        assert result == 42
    
    @pytest.mark.asyncio
    async def test_timeout_failure(self):
        """Test timeout with timeout exceeded."""
        # Create a future that never completes
        async def slow_operation():
            await asyncio.sleep(10)
            return 42
        
        with pytest.raises(asyncio.TimeoutError):
            f = Future(value=None)
            f._resolved = False  # Force it to wait
            await timeout_async(f, timeout_seconds=0.01)


class TestPipeline:
    """Test pipeline pattern."""
    
    @pytest.mark.asyncio
    async def test_simple_pipeline(self):
        """Test simple pipeline."""
        pipeline = (Pipeline()
                    .add(lambda: 10)
                    .add(lambda x: x * 2)
                    .add(lambda x: x + 5))
        
        result = await pipeline.execute()
        assert result == 25
    
    @pytest.mark.asyncio
    async def test_pipeline_with_strings(self):
        """Test pipeline with string operations."""
        pipeline = (Pipeline()
                    .add(lambda: "  hello world  ")
                    .add(lambda x: x.strip())
                    .add(lambda x: x.upper())
                    .add(lambda x: x.split()))
        
        result = await pipeline.execute()
        assert result == ['HELLO', 'WORLD']
    
    @pytest.mark.asyncio
    async def test_pipeline_with_initial(self):
        """Test pipeline with initial value."""
        pipeline = (Pipeline()
                    .add(lambda x: x * 2)
                    .add(lambda x: x + 10))
        
        result = await pipeline.execute(initial=5)
        assert result == 20


class TestReactor:
    """Test reactor functionality."""
    
    def test_reactor_initialize(self):
        """Test reactor initialization."""
        Reactor.initialize()
        assert Reactor.is_initialized()
        assert Reactor.num_cores() > 0
    
    def test_reactor_current_core(self):
        """Test getting current core."""
        Reactor.initialize()
        core = Reactor.current_core()
        assert isinstance(core, int)
        assert core >= 0
    
    def test_reactor_num_cores(self):
        """Test getting number of cores."""
        Reactor.initialize()
        num_cores = Reactor.num_cores()
        assert num_cores > 0
        assert num_cores <= 128  # Reasonable upper bound


class TestComplexScenarios:
    """Test complex real-world scenarios."""
    
    @pytest.mark.asyncio
    async def test_mixed_sync_async(self):
        """Test mixing synchronous and asynchronous operations."""
        # Sync chain
        sync_result = (Future.make_ready(10)
                       .then(lambda x: x * 2)
                       .get())
        
        # Async await
        async_result = await Future.make_ready(20)
        
        assert sync_result + async_result == 40
    
    @pytest.mark.asyncio
    async def test_nested_parallel(self):
        """Test nested parallel operations."""
        # Create groups of futures
        group1 = [Future.make_ready(i) for i in range(3)]
        group2 = [Future.make_ready(i * 10) for i in range(3)]
        
        # Execute both groups in parallel
        results = await when_all([
            asyncio.create_task(when_all(group1)),
            asyncio.create_task(when_all(group2))
        ])
        
        assert results[0] == [0, 1, 2]
        assert results[1] == [0, 10, 20]
    
    @pytest.mark.asyncio
    async def test_chain_then_parallel(self):
        """Test chaining followed by parallel execution."""
        # First chain some operations
        base = Future.make_ready(5).then(lambda x: x * 2).get()
        
        # Then use result in parallel operations
        futures = [Future.make_ready(base + i) for i in range(3)]
        results = await when_all(futures)
        
        assert results == [10, 11, 12]
    
    @pytest.mark.asyncio
    async def test_error_recovery_in_pipeline(self):
        """Test error recovery in a pipeline."""
        call_count = [0]
        
        def maybe_fail():
            call_count[0] += 1
            if call_count[0] < 2:
                return Future.make_exception(Exception("fail"))
            return Future.make_ready(42)
        
        result = await retry_async(maybe_fail, max_retries=3, delay=0.01)
        assert result == 42


class TestPerformance:
    """Performance-oriented tests."""
    
    def test_many_chains(self):
        """Test performance with many chained operations."""
        f = Future.make_ready(0)
        for i in range(100):
            f = f.then(lambda x: x + 1)
        
        result = f.get()
        assert result == 100
    
    @pytest.mark.asyncio
    async def test_many_parallel(self):
        """Test performance with many parallel operations."""
        futures = [Future.make_ready(i) for i in range(100)]
        results = await when_all(futures)
        assert len(results) == 100
        assert results == list(range(100))


if __name__ == "__main__":
    pytest.main([__file__, "-v"])

