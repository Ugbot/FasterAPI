"""
Python Future Wrapper

Bridges C++ futures to Python's async/await syntax.
"""

import asyncio
from typing import TypeVar, Generic, Callable, List, Any, Optional
from . import bindings as _bindings

T = TypeVar('T')


class Future(Generic[T]):
    """
    Python wrapper for C++ future.
    
    Supports both async/await (default, ergonomic) and explicit .then() chains
    (for performance-critical paths).
    
    Examples:
        # Async/await (default)
        result = await future
        
        # Explicit chaining (power users)
        future.then(lambda x: process(x)).then(lambda y: respond(y))
    """
    
    def __init__(self, handle: Optional[int] = None, value: Optional[T] = None):
        """
        Create a future.
        
        Args:
            handle: C++ future handle (opaque pointer)
            value: Immediate value for already-resolved futures
        """
        self._handle = handle
        self._value = value
        self._resolved = value is not None
        self._failed = False
        self._exception: Optional[Exception] = None
        
    def __await__(self):
        """
        Make future awaitable.
        
        Integrates with Python's asyncio event loop.
        """
        # Create async generator function
        async def _await_impl():
            if self._resolved:
                # Fast path: already resolved
                return self._value
            
            if self._failed:
                raise self._exception or Exception("Future failed")
            
            # Integrate with asyncio
            loop = asyncio.get_event_loop()
            py_future = loop.create_future()
            
            def callback(handle, success, value_ptr):
                """C++ callback that resolves Python future."""
                if success:
                    py_future.set_result(value_ptr)
                else:
                    py_future.set_exception(Exception("Future failed"))
            
            # Register callback with C++ future
            if self._handle:
                _bindings.future_add_callback(self._handle, callback)
            else:
                # No C++ handle, resolve immediately with value
                py_future.set_result(self._value)
            
            return await py_future
        
        return _await_impl().__await__()
    
    def then(self, func: Callable[[T], Any]) -> 'Future':
        """
        Explicit continuation chaining (power user API).
        
        Args:
            func: Continuation function that receives the value
            
        Returns:
            New future for the result
            
        Example:
            future.then(lambda x: x * 2).then(lambda y: str(y))
        """
        if self._resolved:
            # Fast path: execute immediately
            try:
                result = func(self._value)
                return Future(value=result)
            except Exception as e:
                f = Future()
                f._failed = True
                f._exception = e
                return f
        
        if self._failed:
            # Propagate failure
            f = Future()
            f._failed = True
            f._exception = self._exception
            return f
        
        # Chain via C++
        if self._handle:
            new_handle = _bindings.future_then(self._handle, func)
            return Future(handle=new_handle)
        else:
            # No handle, can't chain
            f = Future()
            f._failed = True
            f._exception = Exception("Cannot chain future without handle")
            return f
    
    def handle_error(self, func: Callable[[Exception], T]) -> 'Future[T]':
        """
        Handle errors in the future chain.
        
        Args:
            func: Error handler that receives the exception
            
        Returns:
            New future with error handling
        """
        if self._failed:
            try:
                result = func(self._exception)
                return Future(value=result)
            except Exception as e:
                f = Future()
                f._failed = True
                f._exception = e
                return f
        
        # TODO: Implement proper error chaining
        return self
    
    def is_ready(self) -> bool:
        """Check if future is ready."""
        if self._resolved or self._failed:
            return True
        
        if self._handle:
            return _bindings.future_is_ready(self._handle)
        
        return False
    
    def failed(self) -> bool:
        """Check if future has failed."""
        return self._failed
    
    def get(self) -> T:
        """
        Get the value (blocking).
        
        Only use in synchronous contexts.
        
        Returns:
            The future's value
            
        Raises:
            Exception if future failed
        """
        if self._failed:
            raise self._exception or Exception("Future failed")
        
        if self._resolved:
            return self._value
        
        if self._handle:
            success, value = _bindings.future_get(self._handle)
            if success:
                return value
            else:
                raise Exception("Future failed")
        
        raise Exception("Future not ready")
    
    @staticmethod
    def make_ready(value: T) -> 'Future[T]':
        """Create a future that's already resolved."""
        return Future(value=value)
    
    @staticmethod
    def make_exception(exception: Exception) -> 'Future[T]':
        """Create a future that's already failed."""
        f = Future()
        f._failed = True
        f._exception = exception
        return f


async def when_all(futures: List[Future[T]]) -> List[T]:
    """
    Wait for all futures to complete.
    
    Args:
        futures: List of futures to wait for
        
    Returns:
        List of results in same order
        
    Example:
        results = await when_all([
            pg.exec_async("SELECT ..."),
            pg.exec_async("SELECT ..."),
        ])
    """
    # Create async tasks for each future
    tasks = [asyncio.create_task(_await_future(f)) for f in futures]
    return await asyncio.gather(*tasks)

async def _await_future(f: Future[T]) -> T:
    """Helper to await a future."""
    return await f


async def when_any(futures: List[Future[T]]) -> T:
    """
    Wait for the first future to complete.
    
    Args:
        futures: List of futures
        
    Returns:
        Result of first completed future
    """
    done, pending = await asyncio.wait(
        [asyncio.create_task(f.__await__()) for f in futures],
        return_when=asyncio.FIRST_COMPLETED
    )
    
    # Cancel pending
    for task in pending:
        task.cancel()
    
    # Return first result
    return done.pop().result()


async def map_async(func: Callable[[T], Any], futures: List[Future[T]]) -> List[Any]:
    """
    Map a function over a list of futures.
    
    Args:
        func: Function to apply to each result
        futures: List of futures
        
    Returns:
        List of mapped results
    """
    results = await when_all(futures)
    return [func(r) for r in results]

