"""
Enhanced FastAPI-compatible dependency injection system for FasterAPI.

Provides full dependency injection with:
- Request-scoped caching
- Nested dependencies
- Yield dependencies (cleanup after response)
- Async dependency support
- Class-based dependencies

Usage:
    from fasterapi import Depends

    def get_db():
        db = Database()
        try:
            yield db
        finally:
            db.close()

    @app.get("/items")
    def list_items(db = Depends(get_db)):
        return db.query("SELECT * FROM items")
"""

import asyncio
import contextvars
import inspect
from typing import (
    Any,
    AsyncGenerator,
    Callable,
    Dict,
    Generator,
    List,
    Optional,
    Tuple,
    Type,
    TypeVar,
    Union,
    get_type_hints,
)

# Re-export Depends from params for convenience
from fasterapi.params import Depends, Security

T = TypeVar("T")


# Context variables for request-scoped state
_dependency_cache: contextvars.ContextVar[Optional[Dict[int, Any]]] = (
    contextvars.ContextVar("dependency_cache", default=None)
)
_cleanup_stack: contextvars.ContextVar[Optional[List[Tuple[str, Any]]]] = (
    contextvars.ContextVar("cleanup_stack", default=None)
)
_current_request: contextvars.ContextVar[Optional[Any]] = contextvars.ContextVar(
    "current_request", default=None
)


class DependencyError(Exception):
    """Raised when dependency resolution fails."""

    pass


async def resolve_dependency(
    depends: Depends,
    request: Any,
    path_params: Dict[str, Any],
    query_params: Dict[str, Any],
    dependency_overrides: Optional[Dict[Callable, Callable]] = None,
) -> Any:
    """
    Resolve a single dependency.

    Handles caching, nested dependencies, and yield dependencies.

    Args:
        depends: The Depends marker
        request: The HTTP request object
        path_params: Extracted path parameters
        query_params: Extracted query parameters
        dependency_overrides: Optional dict mapping original deps to overrides (for testing)

    Returns:
        The resolved dependency value

    Raises:
        DependencyError: If resolution fails
    """
    cache = _dependency_cache.get()
    if cache is None:
        cache = {}
        _dependency_cache.set(cache)

    cleanup_stack = _cleanup_stack.get()
    if cleanup_stack is None:
        cleanup_stack = []
        _cleanup_stack.set(cleanup_stack)

    dep_func = depends.dependency
    if dep_func is None:
        raise DependencyError("Dependency callable is None")

    # Check for dependency override (FastAPI-compatible testing pattern)
    original_dep_func = dep_func
    if dependency_overrides and dep_func in dependency_overrides:
        dep_func = dependency_overrides[dep_func]

    # Check cache (use original func as key for consistency)
    cache_key = id(original_dep_func)
    if depends.use_cache and cache_key in cache:
        return cache[cache_key]

    # Get the dependency function's signature
    try:
        dep_sig = inspect.signature(dep_func)
    except (ValueError, TypeError):
        # Can't inspect - call with no args
        dep_sig = None

    # Resolve nested dependencies
    dep_kwargs = {}
    if dep_sig:
        type_hints = {}
        try:
            type_hints = get_type_hints(dep_func)
        except Exception:
            pass

        for dep_param_name, dep_param in dep_sig.parameters.items():
            # Check for Request parameter
            if dep_param_name == "request":
                dep_kwargs[dep_param_name] = request
            # Check for nested Depends
            elif isinstance(dep_param.default, Depends):
                dep_kwargs[dep_param_name] = await resolve_dependency(
                    dep_param.default,
                    request,
                    path_params,
                    query_params,
                    dependency_overrides,
                )
            # Check path params
            elif dep_param_name in path_params:
                dep_kwargs[dep_param_name] = path_params[dep_param_name]
            # Check query params
            elif dep_param_name in query_params:
                dep_kwargs[dep_param_name] = query_params[dep_param_name]
            # Use default if available
            elif dep_param.default is not inspect.Parameter.empty:
                pass  # Will use default

    # Call the dependency
    try:
        if inspect.isasyncgenfunction(dep_func):
            # Async generator (async yield dependency)
            gen = dep_func(**dep_kwargs)
            value = await gen.__anext__()
            cleanup_stack.append(("async_gen", gen))
        elif inspect.isgeneratorfunction(dep_func):
            # Sync generator (yield dependency)
            gen = dep_func(**dep_kwargs)
            value = next(gen)
            cleanup_stack.append(("sync_gen", gen))
        elif inspect.iscoroutinefunction(dep_func):
            # Async function
            value = await dep_func(**dep_kwargs)
        elif inspect.isclass(dep_func):
            # Class-based dependency
            value = dep_func(**dep_kwargs)
            if hasattr(value, "__call__"):
                # Callable class instance
                if inspect.iscoroutinefunction(value.__call__):
                    value = await value()
                else:
                    value = value()
        else:
            # Sync function
            value = dep_func(**dep_kwargs)
    except Exception as e:
        raise DependencyError(
            f"Failed to resolve dependency {dep_func.__name__}: {e}"
        ) from e

    # Cache if enabled
    if depends.use_cache:
        cache[cache_key] = value

    return value


async def resolve_dependencies(
    handler: Callable,
    request: Any,
    path_params: Dict[str, Any],
    query_params: Dict[str, Any],
    body_params: Optional[Dict[str, Any]] = None,
) -> Dict[str, Any]:
    """
    Resolve all dependencies for a handler function.

    Args:
        handler: The route handler function
        request: The HTTP request object
        path_params: Extracted path parameters
        query_params: Extracted query parameters
        body_params: Extracted body parameters

    Returns:
        Dictionary of resolved kwargs for the handler
    """
    sig = inspect.signature(handler)
    resolved = {}

    type_hints = {}
    try:
        type_hints = get_type_hints(handler)
    except Exception:
        pass

    body_params = body_params or {}

    for param_name, param in sig.parameters.items():
        # Skip *args and **kwargs
        if param.kind in (
            inspect.Parameter.VAR_POSITIONAL,
            inspect.Parameter.VAR_KEYWORD,
        ):
            continue

        # Check for Request parameter
        if param_name == "request":
            resolved[param_name] = request
            continue

        # Check for Depends
        if isinstance(param.default, Depends):
            resolved[param_name] = await resolve_dependency(
                param.default,
                request,
                path_params,
                query_params,
            )
            continue

        # Check path params
        if param_name in path_params:
            resolved[param_name] = path_params[param_name]
            continue

        # Check query params
        if param_name in query_params:
            resolved[param_name] = query_params[param_name]
            continue

        # Check body params
        if param_name in body_params:
            resolved[param_name] = body_params[param_name]
            continue

        # Use default if available
        if param.default is not inspect.Parameter.empty:
            resolved[param_name] = param.default

    return resolved


async def cleanup_dependencies() -> None:
    """
    Cleanup all yield dependencies after request completes.

    This should be called in a finally block after the handler returns.
    Processes cleanup in reverse order (LIFO).
    """
    cleanup_stack = _cleanup_stack.get()
    if cleanup_stack is None:
        return

    errors = []

    # Process in reverse order (LIFO)
    while cleanup_stack:
        cleanup_type, gen = cleanup_stack.pop()
        try:
            if cleanup_type == "async_gen":
                try:
                    await gen.__anext__()
                except StopAsyncIteration:
                    pass
            else:  # sync_gen
                try:
                    next(gen)
                except StopIteration:
                    pass
        except Exception as e:
            # Collect errors but continue cleanup
            errors.append(e)

    # Clear context vars
    _dependency_cache.set(None)
    _cleanup_stack.set(None)
    _current_request.set(None)

    # Raise first error if any occurred
    if errors:
        raise errors[0]


class DependencyScope:
    """
    Context manager for dependency scope.

    Creates a new scope for dependency caching and cleanup.

    Usage:
        async with DependencyScope(request):
            kwargs = await resolve_dependencies(handler, request, ...)
            result = await handler(**kwargs)
    """

    def __init__(self, request: Any = None):
        """
        Initialize dependency scope.

        Args:
            request: The HTTP request object
        """
        self.request = request

    async def __aenter__(self) -> "DependencyScope":
        """Enter the scope."""
        _dependency_cache.set({})
        _cleanup_stack.set([])
        _current_request.set(self.request)
        return self

    async def __aexit__(
        self,
        exc_type: Optional[Type[BaseException]],
        exc_val: Optional[BaseException],
        exc_tb: Any,
    ) -> bool:
        """Exit the scope and cleanup dependencies."""
        try:
            await cleanup_dependencies()
        except Exception as cleanup_error:
            # If we're already handling an exception, don't mask it
            if exc_val is None:
                raise cleanup_error
        return False


def dependency_scope(request: Any = None) -> DependencyScope:
    """
    Create a dependency scope context manager.

    Args:
        request: The HTTP request object

    Returns:
        DependencyScope context manager
    """
    return DependencyScope(request)


def get_current_request() -> Optional[Any]:
    """
    Get the current request from dependency context.

    Returns:
        The current request object, or None if not in a request context
    """
    return _current_request.get()


class DependencyCache:
    """
    Manual dependency cache for advanced use cases.

    Allows manually caching and retrieving dependency values.
    """

    @staticmethod
    def get(key: Any) -> Optional[Any]:
        """Get a cached dependency value."""
        cache = _dependency_cache.get()
        if cache is None:
            return None
        return cache.get(id(key))

    @staticmethod
    def set(key: Any, value: Any) -> None:
        """Set a cached dependency value."""
        cache = _dependency_cache.get()
        if cache is None:
            cache = {}
            _dependency_cache.set(cache)
        cache[id(key)] = value

    @staticmethod
    def clear() -> None:
        """Clear the dependency cache."""
        _dependency_cache.set({})
