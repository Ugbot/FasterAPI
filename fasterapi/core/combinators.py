"""
Higher-Order Async Patterns

Utilities for composing and combining async operations.
"""

from typing import TypeVar, List, Callable, Any, Optional
from .future import Future, when_all as _when_all
import asyncio

T = TypeVar('T')
U = TypeVar('U')


async def when_all(futures: List[Future[T]]) -> List[T]:
    """
    Wait for all futures to complete.
    
    Parallel execution of multiple async operations.
    
    Args:
        futures: List of futures to wait for
        
    Returns:
        List of results in same order
        
    Example:
        results = await when_all([
            pg.exec_async("SELECT * FROM users"),
            pg.exec_async("SELECT * FROM products"),
        ])
        users, products = results
    """
    return await _when_all(futures)


async def when_any(futures: List[Future[T]]) -> tuple[T, List[Future[T]]]:
    """
    Wait for first future to complete.
    
    Args:
        futures: List of futures
        
    Returns:
        (result, remaining_futures)
        
    Example:
        result, pending = await when_any([
            cache.get_async(key),
            db.get_async(key),
        ])
    """
    tasks = [asyncio.create_task(f.__await__()) for f in futures]
    done, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
    
    # Get first result
    first_result = done.pop().result()
    
    # Return result and remaining futures
    remaining = [futures[i] for i, t in enumerate(tasks) if t in pending]
    
    return first_result, remaining


async def when_some(futures: List[Future[T]], count: int) -> List[T]:
    """
    Wait for at least 'count' futures to complete.
    
    Args:
        futures: List of futures
        count: Minimum number of futures to wait for
        
    Returns:
        List of results from first 'count' completed futures
    """
    if count > len(futures):
        count = len(futures)
    
    # Helper to await a future
    async def _await(f):
        return await f
    
    tasks = [asyncio.create_task(_await(f)) for f in futures]
    done, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
    
    results = []
    while len(results) < count and (done or pending):
        if done:
            results.append(done.pop().result())
        if len(results) < count and pending:
            new_done, pending = await asyncio.wait(pending, return_when=asyncio.FIRST_COMPLETED)
            done.update(new_done)
    
    # Cancel remaining
    for task in pending:
        task.cancel()
    
    return results[:count]


async def map_async(func: Callable[[T], U], futures: List[Future[T]]) -> List[U]:
    """
    Map a function over futures.
    
    Args:
        func: Function to apply to each result
        futures: List of futures
        
    Returns:
        List of mapped results
        
    Example:
        prices = await map_async(
            lambda item: item.price,
            [get_item_async(id) for id in ids]
        )
    """
    results = await when_all(futures)
    return [func(r) for r in results]


async def filter_async(
    predicate: Callable[[T], bool], 
    futures: List[Future[T]]
) -> List[T]:
    """
    Filter future results by predicate.
    
    Args:
        predicate: Filter function
        futures: List of futures
        
    Returns:
        Filtered list of results
    """
    results = await when_all(futures)
    return [r for r in results if predicate(r)]


async def reduce_async(
    func: Callable[[U, T], U],
    futures: List[Future[T]],
    initial: U
) -> U:
    """
    Reduce future results.
    
    Args:
        func: Reduction function
        futures: List of futures
        initial: Initial accumulator value
        
    Returns:
        Reduced value
    """
    results = await when_all(futures)
    accumulator = initial
    for result in results:
        accumulator = func(accumulator, result)
    return accumulator


class Pipeline:
    """
    Async pipeline for composing operations.
    
    Example:
        result = await (Pipeline()
            .add(lambda: fetch_data())
            .add(lambda data: transform(data))
            .add(lambda data: validate(data))
            .execute())
    """
    
    def __init__(self):
        self.stages: List[Callable] = []
    
    def add(self, func: Callable) -> 'Pipeline':
        """Add a stage to the pipeline."""
        self.stages.append(func)
        return self
    
    async def execute(self, initial: Any = None) -> Any:
        """Execute the pipeline."""
        result = initial
        for stage in self.stages:
            if asyncio.iscoroutinefunction(stage):
                result = await stage(result) if result is not None else await stage()
            else:
                result = stage(result) if result is not None else stage()
        return result


async def retry_async(
    func: Callable[[], Future[T]],
    max_retries: int = 3,
    delay: float = 0.1,
    backoff: float = 2.0
) -> T:
    """
    Retry an async operation with exponential backoff.
    
    Args:
        func: Function that returns a future
        max_retries: Maximum number of retries
        delay: Initial delay between retries (seconds)
        backoff: Backoff multiplier
        
    Returns:
        Result of successful operation
        
    Raises:
        Last exception if all retries fail
        
    Example:
        result = await retry_async(
            lambda: db.query_async("SELECT ..."),
            max_retries=3
        )
    """
    last_exception = None
    current_delay = delay
    
    for attempt in range(max_retries + 1):
        try:
            future = func()
            return await future
        except Exception as e:
            last_exception = e
            if attempt < max_retries:
                await asyncio.sleep(current_delay)
                current_delay *= backoff
    
    raise last_exception or Exception("All retries failed")


async def timeout_async(future: Future[T], timeout_seconds: float) -> T:
    """
    Add timeout to a future.
    
    Args:
        future: Future to timeout
        timeout_seconds: Timeout in seconds
        
    Returns:
        Result if completed within timeout
        
    Raises:
        asyncio.TimeoutError if timeout exceeded
        
    Example:
        result = await timeout_async(
            slow_operation_async(),
            timeout_seconds=5.0
        )
    """
    return await asyncio.wait_for(future.__await__(), timeout=timeout_seconds)


def chain(*funcs: Callable) -> Callable:
    """
    Create a function chain.
    
    Args:
        *funcs: Functions to chain
        
    Returns:
        Chained function
        
    Example:
        process = chain(
            lambda x: x.strip(),
            lambda x: x.lower(),
            lambda x: x.split()
        )
        result = process("  HELLO WORLD  ")
    """
    def chained(value):
        result = value
        for func in funcs:
            result = func(result)
        return result
    return chained

