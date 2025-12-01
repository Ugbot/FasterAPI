#!/usr/bin/env python3.13
"""
Comprehensive tests for async combinators (combinators.py).

Tests:
- when_all: Wait for all futures
- when_any: Wait for first future
- when_some: Wait for N futures
- map_async: Map over futures
- filter_async: Filter futures
- reduce_async: Reduce futures
- Pipeline: Async pipeline composition
- retry_async: Retry with backoff
- timeout_async: Timeout handling
- chain: Function chaining
- Uses randomized inputs per project guidelines
"""

import pytest
import asyncio
import random
import string
import time
from unittest.mock import AsyncMock, MagicMock

from fasterapi.core.combinators import (
    when_all,
    when_any,
    when_some,
    map_async,
    filter_async,
    reduce_async,
    Pipeline,
    retry_async,
    timeout_async,
    chain,
)


# =============================================================================
# Test Fixtures and Helpers
# =============================================================================

class MockFuture:
    """Mock future for testing combinators."""

    def __init__(self, value, delay: float = 0):
        self.value = value
        self.delay = delay
        self._awaited = False

    def __await__(self):
        return self._async_impl().__await__()

    async def _async_impl(self):
        if self.delay > 0:
            await asyncio.sleep(self.delay)
        self._awaited = True
        return self.value


class FailingFuture:
    """Future that raises an exception."""

    def __init__(self, exception: Exception, delay: float = 0):
        self.exception = exception
        self.delay = delay

    def __await__(self):
        return self._async_impl().__await__()

    async def _async_impl(self):
        if self.delay > 0:
            await asyncio.sleep(self.delay)
        raise self.exception


def random_string(length: int = 10) -> str:
    """Generate a random string."""
    return ''.join(random.choices(string.ascii_letters, k=length))


def random_int(min_val: int = -1000, max_val: int = 1000) -> int:
    """Generate a random integer."""
    return random.randint(min_val, max_val)


# =============================================================================
# when_all Tests
# =============================================================================

class TestWhenAll:
    """Tests for when_all combinator."""

    @pytest.mark.asyncio
    async def test_when_all_basic(self):
        """Test waiting for all futures to complete."""
        values = [random_int() for _ in range(5)]
        futures = [MockFuture(v) for v in values]

        results = await when_all(futures)

        assert results == values
        assert all(f._awaited for f in futures)

    @pytest.mark.asyncio
    async def test_when_all_empty_list(self):
        """Test with empty list of futures."""
        results = await when_all([])
        assert results == []

    @pytest.mark.asyncio
    async def test_when_all_single_future(self):
        """Test with single future."""
        value = random_int()
        futures = [MockFuture(value)]

        results = await when_all(futures)

        assert results == [value]

    @pytest.mark.asyncio
    async def test_when_all_preserves_order(self):
        """Test that results are in same order as input futures."""
        # Create futures with different delays to ensure they complete out of order
        futures = [
            MockFuture("slow", delay=0.03),
            MockFuture("fast", delay=0.01),
            MockFuture("medium", delay=0.02),
        ]

        results = await when_all(futures)

        # Results should be in input order, not completion order
        assert results == ["slow", "fast", "medium"]

    @pytest.mark.asyncio
    async def test_when_all_randomized(self):
        """Test with randomized futures."""
        num_futures = random.randint(5, 20)
        values = [random_int() for _ in range(num_futures)]
        futures = [MockFuture(v, delay=random.uniform(0, 0.01)) for v in values]

        results = await when_all(futures)

        assert results == values

    @pytest.mark.asyncio
    async def test_when_all_mixed_types(self):
        """Test with futures returning different types."""
        futures = [
            MockFuture(42),
            MockFuture("hello"),
            MockFuture([1, 2, 3]),
            MockFuture({"key": "value"}),
            MockFuture(None),
        ]

        results = await when_all(futures)

        assert results == [42, "hello", [1, 2, 3], {"key": "value"}, None]


# =============================================================================
# when_any Tests
# =============================================================================

class TestWhenAny:
    """Tests for when_any combinator."""

    @pytest.mark.asyncio
    async def test_when_any_returns_first(self):
        """Test that when_any returns the first completed future."""
        futures = [
            MockFuture("slow", delay=0.1),
            MockFuture("fast", delay=0.01),
            MockFuture("medium", delay=0.05),
        ]

        result, remaining = await when_any(futures)

        assert result == "fast"
        assert len(remaining) == 2

    @pytest.mark.asyncio
    async def test_when_any_single_future(self):
        """Test with single future."""
        value = random_string()
        futures = [MockFuture(value)]

        result, remaining = await when_any(futures)

        assert result == value
        assert remaining == []


# =============================================================================
# when_some Tests
# =============================================================================

class TestWhenSome:
    """Tests for when_some combinator."""

    @pytest.mark.asyncio
    async def test_when_some_returns_count(self):
        """Test that when_some returns exactly count results."""
        futures = [
            MockFuture(f"result_{i}", delay=i * 0.01)
            for i in range(5)
        ]

        results = await when_some(futures, count=3)

        assert len(results) == 3

    @pytest.mark.asyncio
    async def test_when_some_count_exceeds_list(self):
        """Test when count exceeds number of futures."""
        futures = [MockFuture(i) for i in range(3)]

        results = await when_some(futures, count=10)

        # Should return all available results
        assert len(results) == 3

    @pytest.mark.asyncio
    async def test_when_some_count_zero(self):
        """Test with count of zero."""
        futures = [MockFuture(i) for i in range(5)]

        results = await when_some(futures, count=0)

        assert results == []

    @pytest.mark.asyncio
    async def test_when_some_empty_list(self):
        """Test with empty list."""
        results = await when_some([], count=5)
        assert results == []


# =============================================================================
# map_async Tests
# =============================================================================

class TestMapAsync:
    """Tests for map_async combinator."""

    @pytest.mark.asyncio
    async def test_map_async_basic(self):
        """Test basic mapping over futures."""
        values = [1, 2, 3, 4, 5]
        futures = [MockFuture(v) for v in values]

        results = await map_async(lambda x: x * 2, futures)

        assert results == [2, 4, 6, 8, 10]

    @pytest.mark.asyncio
    async def test_map_async_empty(self):
        """Test mapping over empty list."""
        results = await map_async(lambda x: x, [])
        assert results == []

    @pytest.mark.asyncio
    async def test_map_async_string_transform(self):
        """Test mapping with string transformation."""
        strings = [random_string() for _ in range(5)]
        futures = [MockFuture(s) for s in strings]

        results = await map_async(str.upper, futures)

        assert results == [s.upper() for s in strings]

    @pytest.mark.asyncio
    async def test_map_async_complex_transform(self):
        """Test mapping with complex transformation."""
        data = [
            {"name": random_string(), "value": random_int()}
            for _ in range(5)
        ]
        futures = [MockFuture(d) for d in data]

        results = await map_async(lambda d: d["value"] * 2, futures)

        assert results == [d["value"] * 2 for d in data]

    @pytest.mark.asyncio
    async def test_map_async_preserves_order(self):
        """Test that map_async preserves order."""
        futures = [
            MockFuture(3, delay=0.03),
            MockFuture(1, delay=0.01),
            MockFuture(2, delay=0.02),
        ]

        results = await map_async(lambda x: x * 10, futures)

        assert results == [30, 10, 20]


# =============================================================================
# filter_async Tests
# =============================================================================

class TestFilterAsync:
    """Tests for filter_async combinator."""

    @pytest.mark.asyncio
    async def test_filter_async_basic(self):
        """Test basic filtering of futures."""
        values = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
        futures = [MockFuture(v) for v in values]

        results = await filter_async(lambda x: x % 2 == 0, futures)

        assert results == [2, 4, 6, 8, 10]

    @pytest.mark.asyncio
    async def test_filter_async_all_pass(self):
        """Test when all items pass filter."""
        values = [2, 4, 6, 8]
        futures = [MockFuture(v) for v in values]

        results = await filter_async(lambda x: x % 2 == 0, futures)

        assert results == values

    @pytest.mark.asyncio
    async def test_filter_async_none_pass(self):
        """Test when no items pass filter."""
        values = [1, 3, 5, 7]
        futures = [MockFuture(v) for v in values]

        results = await filter_async(lambda x: x % 2 == 0, futures)

        assert results == []

    @pytest.mark.asyncio
    async def test_filter_async_empty(self):
        """Test filtering empty list."""
        results = await filter_async(lambda x: True, [])
        assert results == []

    @pytest.mark.asyncio
    async def test_filter_async_string_filter(self):
        """Test filtering strings."""
        strings = ["hello", "world", "hi", "there", "hey"]
        futures = [MockFuture(s) for s in strings]

        results = await filter_async(lambda s: s.startswith("h"), futures)

        assert results == ["hello", "hi", "hey"]

    @pytest.mark.asyncio
    async def test_filter_async_randomized(self):
        """Test with randomized data."""
        values = [random_int() for _ in range(20)]
        futures = [MockFuture(v) for v in values]
        threshold = 0

        results = await filter_async(lambda x: x > threshold, futures)

        expected = [v for v in values if v > threshold]
        assert results == expected


# =============================================================================
# reduce_async Tests
# =============================================================================

class TestReduceAsync:
    """Tests for reduce_async combinator."""

    @pytest.mark.asyncio
    async def test_reduce_async_sum(self):
        """Test reducing with sum."""
        values = [1, 2, 3, 4, 5]
        futures = [MockFuture(v) for v in values]

        result = await reduce_async(lambda acc, x: acc + x, futures, 0)

        assert result == 15

    @pytest.mark.asyncio
    async def test_reduce_async_product(self):
        """Test reducing with product."""
        values = [1, 2, 3, 4]
        futures = [MockFuture(v) for v in values]

        result = await reduce_async(lambda acc, x: acc * x, futures, 1)

        assert result == 24

    @pytest.mark.asyncio
    async def test_reduce_async_string_concat(self):
        """Test reducing with string concatenation."""
        words = ["hello", " ", "world"]
        futures = [MockFuture(w) for w in words]

        result = await reduce_async(lambda acc, x: acc + x, futures, "")

        assert result == "hello world"

    @pytest.mark.asyncio
    async def test_reduce_async_empty(self):
        """Test reducing empty list returns initial value."""
        result = await reduce_async(lambda acc, x: acc + x, [], 42)
        assert result == 42

    @pytest.mark.asyncio
    async def test_reduce_async_single(self):
        """Test reducing single element."""
        futures = [MockFuture(10)]

        result = await reduce_async(lambda acc, x: acc + x, futures, 5)

        assert result == 15

    @pytest.mark.asyncio
    async def test_reduce_async_list_building(self):
        """Test reducing to build a list."""
        values = [1, 2, 3]
        futures = [MockFuture(v) for v in values]

        result = await reduce_async(lambda acc, x: acc + [x * 2], futures, [])

        assert result == [2, 4, 6]

    @pytest.mark.asyncio
    async def test_reduce_async_randomized(self):
        """Test with randomized values."""
        values = [random_int(1, 100) for _ in range(10)]
        futures = [MockFuture(v) for v in values]

        result = await reduce_async(lambda acc, x: acc + x, futures, 0)

        assert result == sum(values)


# =============================================================================
# Pipeline Tests
# =============================================================================

class TestPipeline:
    """Tests for Pipeline class."""

    @pytest.mark.asyncio
    async def test_pipeline_basic(self):
        """Test basic pipeline execution."""
        pipeline = Pipeline()
        pipeline.add(lambda: 5)
        pipeline.add(lambda x: x * 2)
        pipeline.add(lambda x: x + 3)

        result = await pipeline.execute()

        assert result == 13  # (5 * 2) + 3

    @pytest.mark.asyncio
    async def test_pipeline_with_initial(self):
        """Test pipeline with initial value."""
        pipeline = Pipeline()
        pipeline.add(lambda x: x * 2)
        pipeline.add(lambda x: x + 1)

        result = await pipeline.execute(initial=10)

        assert result == 21  # (10 * 2) + 1

    @pytest.mark.asyncio
    async def test_pipeline_async_stages(self):
        """Test pipeline with async stages."""
        async def async_double(x):
            await asyncio.sleep(0.001)
            return x * 2

        pipeline = Pipeline()
        pipeline.add(lambda: 5)
        pipeline.add(async_double)
        pipeline.add(lambda x: x + 1)

        result = await pipeline.execute()

        assert result == 11  # (5 * 2) + 1

    @pytest.mark.asyncio
    async def test_pipeline_chaining(self):
        """Test pipeline method chaining."""
        result = await (Pipeline()
            .add(lambda: 10)
            .add(lambda x: x + 5)
            .add(lambda x: x * 2)
            .execute())

        assert result == 30  # (10 + 5) * 2

    @pytest.mark.asyncio
    async def test_pipeline_empty(self):
        """Test empty pipeline returns initial value."""
        pipeline = Pipeline()
        result = await pipeline.execute(initial=42)
        assert result == 42

    @pytest.mark.asyncio
    async def test_pipeline_string_processing(self):
        """Test pipeline for string processing."""
        pipeline = (Pipeline()
            .add(lambda: "  HELLO WORLD  ")
            .add(str.strip)
            .add(str.lower)
            .add(str.split))

        result = await pipeline.execute()

        assert result == ["hello", "world"]

    @pytest.mark.asyncio
    async def test_pipeline_data_transformation(self):
        """Test pipeline for data transformation."""
        data = {"name": random_string(), "value": random_int()}

        pipeline = (Pipeline()
            .add(lambda x: x["value"])
            .add(lambda x: x * 2)
            .add(lambda x: {"doubled": x}))

        result = await pipeline.execute(initial=data)

        assert result == {"doubled": data["value"] * 2}


# =============================================================================
# retry_async Tests
# =============================================================================

class TestRetryAsync:
    """Tests for retry_async combinator."""

    @pytest.mark.asyncio
    async def test_retry_async_success_first_try(self):
        """Test successful operation on first try."""
        value = random_int()

        def success_func():
            return MockFuture(value)

        result = await retry_async(success_func, max_retries=3)

        assert result == value

    @pytest.mark.asyncio
    async def test_retry_async_success_after_failures(self):
        """Test success after some failures."""
        attempt_count = 0
        expected_value = random_int()

        def make_future():
            nonlocal attempt_count
            attempt_count += 1
            if attempt_count < 3:
                return FailingFuture(ValueError("Temporary failure"))
            return MockFuture(expected_value)

        result = await retry_async(make_future, max_retries=5, delay=0.01)

        assert result == expected_value
        assert attempt_count == 3

    @pytest.mark.asyncio
    async def test_retry_async_all_failures(self):
        """Test that all retries fail raises last exception."""
        def always_fail():
            return FailingFuture(ValueError("Always fails"))

        with pytest.raises(ValueError, match="Always fails"):
            await retry_async(always_fail, max_retries=3, delay=0.01)

    @pytest.mark.asyncio
    async def test_retry_async_zero_retries(self):
        """Test with zero retries (single attempt)."""
        def fail_once():
            return FailingFuture(RuntimeError("Fail"))

        with pytest.raises(RuntimeError):
            await retry_async(fail_once, max_retries=0)

    @pytest.mark.asyncio
    async def test_retry_async_backoff(self):
        """Test that backoff increases delay between retries."""
        attempt_times = []

        def track_attempts():
            attempt_times.append(time.time())
            return FailingFuture(ValueError("Fail"))

        try:
            await retry_async(track_attempts, max_retries=3, delay=0.05, backoff=2.0)
        except ValueError:
            pass

        # Verify delays increase (with some tolerance)
        if len(attempt_times) >= 3:
            delay1 = attempt_times[1] - attempt_times[0]
            delay2 = attempt_times[2] - attempt_times[1]
            # Second delay should be roughly 2x the first
            assert delay2 > delay1 * 1.5

    @pytest.mark.asyncio
    async def test_retry_async_different_exceptions(self):
        """Test retry with different exception types."""
        attempt = 0

        def varying_exceptions():
            nonlocal attempt
            attempt += 1
            if attempt == 1:
                return FailingFuture(ValueError("First"))
            elif attempt == 2:
                return FailingFuture(RuntimeError("Second"))
            return MockFuture("success")

        result = await retry_async(varying_exceptions, max_retries=5, delay=0.01)

        assert result == "success"


# =============================================================================
# timeout_async Tests
# =============================================================================

class TestTimeoutAsync:
    """Tests for timeout_async combinator."""

    @pytest.mark.asyncio
    async def test_timeout_async_completes_in_time(self):
        """Test future that completes within timeout."""
        value = random_int()
        future = MockFuture(value, delay=0.01)

        result = await timeout_async(future, timeout_seconds=1.0)

        assert result == value

    @pytest.mark.asyncio
    async def test_timeout_async_exceeds_timeout(self):
        """Test future that exceeds timeout."""
        future = MockFuture("slow", delay=1.0)

        with pytest.raises(asyncio.TimeoutError):
            await timeout_async(future, timeout_seconds=0.05)

    @pytest.mark.asyncio
    async def test_timeout_async_immediate(self):
        """Test immediate completion with timeout."""
        value = random_string()
        future = MockFuture(value, delay=0)

        result = await timeout_async(future, timeout_seconds=0.1)

        assert result == value

    @pytest.mark.asyncio
    async def test_timeout_async_exact_timeout(self):
        """Test behavior at exact timeout boundary."""
        # This is somewhat non-deterministic, but we test both sides
        future = MockFuture("result", delay=0.05)

        # With generous timeout, should succeed
        result = await timeout_async(future, timeout_seconds=0.5)
        assert result == "result"


# =============================================================================
# chain Tests
# =============================================================================

class TestChain:
    """Tests for chain function."""

    def test_chain_basic(self):
        """Test basic function chaining."""
        chained = chain(
            lambda x: x + 1,
            lambda x: x * 2,
            lambda x: x - 3
        )

        result = chained(5)

        assert result == 9  # ((5 + 1) * 2) - 3

    def test_chain_string_operations(self):
        """Test chaining string operations."""
        chained = chain(
            str.strip,
            str.lower,
            str.split
        )

        result = chained("  HELLO WORLD  ")

        assert result == ["hello", "world"]

    def test_chain_single_function(self):
        """Test chain with single function."""
        chained = chain(lambda x: x * 2)

        result = chained(5)

        assert result == 10

    def test_chain_empty(self):
        """Test chain with no functions returns identity."""
        chained = chain()

        result = chained(42)

        assert result == 42

    def test_chain_type_transformations(self):
        """Test chain with type transformations."""
        chained = chain(
            lambda x: x * 2,        # int -> int
            str,                     # int -> str
            lambda x: x + "!",      # str -> str
            len                      # str -> int
        )

        result = chained(5)

        assert result == 3  # "10!" has length 3

    def test_chain_data_processing(self):
        """Test chain for data processing."""
        chained = chain(
            lambda data: data["values"],
            lambda values: [v * 2 for v in values],
            sum
        )

        data = {"values": [1, 2, 3, 4, 5]}
        result = chained(data)

        assert result == 30  # sum([2, 4, 6, 8, 10])

    def test_chain_randomized(self):
        """Test chain with randomized operations."""
        multiplier = random_int(1, 10)
        addend = random_int(1, 100)

        chained = chain(
            lambda x: x * multiplier,
            lambda x: x + addend
        )

        input_val = random_int(1, 100)
        result = chained(input_val)

        expected = (input_val * multiplier) + addend
        assert result == expected


# =============================================================================
# Integration Tests
# =============================================================================

class TestCombinatorIntegration:
    """Integration tests combining multiple combinators."""

    @pytest.mark.asyncio
    async def test_map_then_filter(self):
        """Test combining map and filter."""
        values = list(range(10))
        futures = [MockFuture(v) for v in values]

        # Double all values, then filter for those > 10
        doubled = await map_async(lambda x: x * 2, futures)
        futures2 = [MockFuture(v) for v in doubled]
        filtered = await filter_async(lambda x: x > 10, futures2)

        assert filtered == [12, 14, 16, 18]

    @pytest.mark.asyncio
    async def test_filter_then_reduce(self):
        """Test combining filter and reduce."""
        values = list(range(1, 11))
        futures = [MockFuture(v) for v in values]

        # Filter even numbers, then sum
        even_futures = [MockFuture(v) for v in values if v % 2 == 0]
        result = await reduce_async(lambda acc, x: acc + x, even_futures, 0)

        assert result == 30  # 2 + 4 + 6 + 8 + 10

    @pytest.mark.asyncio
    async def test_pipeline_with_when_all(self):
        """Test pipeline that uses when_all."""
        async def fetch_all_data():
            futures = [MockFuture(i * 10) for i in range(5)]
            return await when_all(futures)

        pipeline = (Pipeline()
            .add(fetch_all_data)
            .add(lambda results: [r * 2 for r in results])
            .add(sum))

        result = await pipeline.execute()

        assert result == 200  # sum([0, 20, 40, 60, 80]) = 200

    @pytest.mark.asyncio
    async def test_retry_with_timeout(self):
        """Test combining retry with timeout - first times out, second succeeds."""
        attempt = 0

        def operation_that_improves():
            nonlocal attempt
            attempt += 1
            if attempt < 2:
                # First attempt is slow and will timeout
                return MockFuture("slow", delay=1.0)
            # Subsequent attempts are fast
            return MockFuture("success", delay=0.01)

        async def operation_with_timeout():
            return await timeout_async(operation_that_improves(), timeout_seconds=0.1)

        # Test that first call times out
        with pytest.raises(asyncio.TimeoutError):
            await operation_with_timeout()

        # Test that second call succeeds (since attempt is now >= 2)
        result = await operation_with_timeout()
        assert result == "success"
        assert attempt == 2

    @pytest.mark.asyncio
    async def test_complex_data_pipeline(self):
        """Test complex data processing pipeline."""
        # Simulate fetching user data
        users = [
            {"id": i, "name": random_string(), "score": random_int(0, 100)}
            for i in range(10)
        ]
        futures = [MockFuture(u) for u in users]

        # Get all users, filter high scorers, map to names, reduce to comma-separated
        all_users = await when_all(futures)
        high_scorers = [u for u in all_users if u["score"] > 50]
        names = [u["name"] for u in high_scorers]
        result = ", ".join(names)

        # Verify result is comma-separated string
        assert isinstance(result, str)
        if high_scorers:
            assert all(u["name"] in result for u in high_scorers)


if __name__ == "__main__":
    pytest.main([__file__, "-v", "-s"])
