"""
Async Future Chaining Demo

Demonstrates Seastar-style futures with Python async/await and explicit chains.
"""

import asyncio
from fasterapi.core import Future, when_all, Reactor
from fasterapi.core.combinators import (
    map_async, retry_async, timeout_async, Pipeline
)


# Example 1: Basic async/await (default, ergonomic)
async def example_async_await():
    """Demo of async/await syntax."""
    print("\n=== Example 1: Async/Await ===")
    
    # Simulate async operations with ready futures
    future1 = Future.make_ready(42)
    future2 = Future.make_ready("hello")
    future3 = Future.make_ready([1, 2, 3])
    
    # Await futures
    result1 = await future1
    result2 = await future2
    result3 = await future3
    
    print(f"Result 1: {result1}")
    print(f"Result 2: {result2}")
    print(f"Result 3: {result3}")


# Example 2: Explicit .then() chaining (power users)
def example_explicit_chaining():
    """Demo of explicit continuation chains."""
    print("\n=== Example 2: Explicit Chaining ===")
    
    # Create a chain of operations
    result = (Future.make_ready(10)
              .then(lambda x: x * 2)        # 20
              .then(lambda x: x + 5)        # 25
              .then(lambda x: f"Result: {x}")  # "Result: 25"
              .get())  # Blocking get
    
    print(f"Chained result: {result}")


# Example 3: Parallel execution with when_all
async def example_parallel():
    """Demo of parallel async operations."""
    print("\n=== Example 3: Parallel Execution ===")
    
    # Create multiple futures
    futures = [
        Future.make_ready(i * i)
        for i in range(5)
    ]
    
    # Wait for all to complete
    results = await when_all(futures)
    print(f"Parallel results: {results}")


# Example 4: Map/filter/reduce operations
async def example_combinators():
    """Demo of higher-order async patterns."""
    print("\n=== Example 4: Combinators ===")
    
    # Map: transform each result
    futures = [Future.make_ready(i) for i in range(10)]
    squared = await map_async(lambda x: x * x, futures)
    print(f"Squared: {squared}")


# Example 5: Error handling
async def example_error_handling():
    """Demo of error handling in futures."""
    print("\n=== Example 5: Error Handling ===")
    
    # Create a failed future
    failed = Future.make_exception(ValueError("Something went wrong"))
    
    try:
        await failed
    except Exception as e:
        print(f"Caught error: {e}")
    
    # Handle error with continuation
    recovered = failed.handle_error(lambda e: "default_value")
    result = recovered.get()
    print(f"Recovered value: {result}")


# Example 6: Pipeline composition
async def example_pipeline():
    """Demo of pipeline pattern."""
    print("\n=== Example 6: Pipeline ===")
    
    pipeline = (Pipeline()
                .add(lambda: "  hello world  ")
                .add(lambda x: x.strip())
                .add(lambda x: x.upper())
                .add(lambda x: x.split()))
    
    result = await pipeline.execute()
    print(f"Pipeline result: {result}")


# Example 7: Retry with backoff
async def example_retry():
    """Demo of retry pattern."""
    print("\n=== Example 7: Retry Pattern ===")
    
    attempts = [0]
    
    def flaky_operation():
        """Simulates an operation that fails twice then succeeds."""
        attempts[0] += 1
        if attempts[0] < 3:
            return Future.make_exception(Exception(f"Attempt {attempts[0]} failed"))
        return Future.make_ready(f"Success on attempt {attempts[0]}")
    
    try:
        result = await retry_async(
            flaky_operation,
            max_retries=5,
            delay=0.01,
            backoff=1.5
        )
        print(f"Retry result: {result}")
    except Exception as e:
        print(f"All retries failed: {e}")


# Example 8: Composing HTTP + DB operations (conceptual)
async def example_composition():
    """Demo of composing async operations."""
    print("\n=== Example 8: Operation Composition ===")
    
    # Simulate database queries
    async def get_user(user_id: int):
        return {"id": user_id, "name": f"User{user_id}"}
    
    async def get_orders(user_id: int):
        return [{"id": 1, "total": 100}, {"id": 2, "total": 200}]
    
    async def get_products(order_id: int):
        return [{"id": 1, "name": "Product A"}, {"id": 2, "name": "Product B"}]
    
    # Compose operations
    user = await get_user(123)
    print(f"User: {user}")
    
    orders = await get_orders(user["id"])
    print(f"Orders: {orders}")
    
    # Parallel fetch products for all orders
    product_futures = [
        get_products(order["id"])
        for order in orders
    ]
    all_products = await when_all(
        [Future.make_ready(p) for p in await asyncio.gather(*product_futures)]
    )
    print(f"Products: {all_products}")


# Example 9: Reactor information
def example_reactor():
    """Demo of reactor introspection."""
    print("\n=== Example 9: Reactor ===")
    
    # Initialize reactor
    Reactor.initialize()
    
    print(f"Reactor initialized: {Reactor.is_initialized()}")
    print(f"Number of cores: {Reactor.num_cores()}")
    print(f"Current core: {Reactor.current_core()}")


async def main():
    """Run all examples."""
    print("╔══════════════════════════════════════════╗")
    print("║  FasterAPI Async Future Chaining Demo   ║")
    print("╚══════════════════════════════════════════╝")
    
    # Run examples
    await example_async_await()
    example_explicit_chaining()
    await example_parallel()
    await example_combinators()
    await example_error_handling()
    await example_pipeline()
    await example_retry()
    await example_composition()
    example_reactor()
    
    print("\n✅ All examples completed!")


if __name__ == "__main__":
    asyncio.run(main())

