"""
Parameter resolver for __main__ handlers executed directly in C++.

This module provides the same parameter resolution logic as ZMQ workers,
but in a standalone form that can be easily called from C++.
"""
import inspect
import os
import json
from typing import Callable, get_type_hints, get_origin, get_args, Any

# Try to import Request class
try:
    from fasterapi.http.request import Request
    _HAS_REQUEST_CLASS = True
except ImportError:
    _HAS_REQUEST_CLASS = False
    Request = None


class NamedRoutesProxy:
    """Lightweight proxy that provides url_path_for() for url_for support.

    This is used in __main__ handlers where the full app object isn't available.
    """

    def __init__(self, named_routes: dict):
        self._named_routes = named_routes or {}

    def url_path_for(self, name: str, **path_params) -> str:
        """Generate URL path for a named route."""
        if name not in self._named_routes:
            raise KeyError(f"No route with name '{name}'")

        path_template = self._named_routes[name]
        # Replace path parameters
        result = path_template
        for key, value in path_params.items():
            result = result.replace(f"{{{key}}}", str(value))
        return result


# Global proxy instance - initialized once from environment
_named_routes_proxy = None
_proxy_initialized = False


def _init_named_routes_proxy():
    """Initialize the named routes proxy from environment variable."""
    global _named_routes_proxy, _proxy_initialized

    if _proxy_initialized:
        return

    _proxy_initialized = True

    # Try to get named routes from environment (set by C++ server)
    env_routes = os.environ.get('FASTERAPI_NAMED_ROUTES')
    if env_routes:
        try:
            named_routes = json.loads(env_routes)
            _named_routes_proxy = NamedRoutesProxy(named_routes)
        except (json.JSONDecodeError, Exception):
            pass


def get_named_routes_proxy():
    """Get the named routes proxy, initializing if needed."""
    if not _proxy_initialized:
        _init_named_routes_proxy()
    return _named_routes_proxy


def coerce_value(value: str, target_type: type, param_name: str) -> tuple:
    """
    Coerce a string value to the target type.

    Returns:
        tuple: (coerced_value, error_dict or None)
    """
    # Handle None/null
    if value is None:
        return None, None

    # Handle Optional types
    origin = get_origin(target_type)
    if origin is not None:
        if str(origin) in ("typing.Union", "types.UnionType"):
            args = get_args(target_type)
            # Get non-None types
            non_none_types = [t for t in args if t is not type(None)]
            if non_none_types:
                target_type = non_none_types[0]
            else:
                return value, None

    # Already correct type
    if isinstance(value, target_type):
        return value, None

    # String coercion
    if not isinstance(value, str):
        return value, None

    try:
        if target_type is int:
            return int(value), None
        elif target_type is float:
            return float(value), None
        elif target_type is bool:
            if value.lower() in ("true", "1", "yes", "on"):
                return True, None
            elif value.lower() in ("false", "0", "no", "off"):
                return False, None
            else:
                return None, {
                    "loc": ("query", param_name),
                    "msg": f"value is not a valid boolean",
                    "type": "type_error.bool",
                    "input": value
                }
        elif target_type is str:
            return value, None
        else:
            return value, None
    except (ValueError, TypeError) as e:
        return None, {
            "loc": ("query", param_name),
            "msg": str(e),
            "type": f"type_error.{target_type.__name__.lower()}",
            "input": value
        }


def resolve_params(handler: Callable, kwargs: dict) -> dict:
    """
    Resolve handler parameters with type coercion and Request injection.

    This is called from C++ for __main__ handlers that can't use ZMQ workers.

    Args:
        handler: The handler function
        kwargs: Raw kwargs from C++ (may contain __request_data__)

    Returns:
        Resolved kwargs with proper types and Request injection

    Raises:
        ValueError: If type coercion fails (with JSON-serializable error)
    """
    # Always filter out internal keys that start with __ (like __request_data__)
    filtered_kwargs = {k: v for k, v in kwargs.items() if not k.startswith("__")}

    try:
        hints = get_type_hints(handler)
    except Exception:
        # If we can't get type hints, return filtered kwargs
        return filtered_kwargs

    if not hints:
        return filtered_kwargs

    sig = inspect.signature(handler)
    resolved_kwargs = {}
    validation_errors = []

    for param_name, param in sig.parameters.items():
        # Get the type hint
        param_type = hints.get(param_name)
        if param_type is None:
            # No type hint - pass through as-is
            if param_name in kwargs:
                resolved_kwargs[param_name] = kwargs[param_name]
            continue

        # Check if this is a Request type
        if _HAS_REQUEST_CLASS and param_type is Request:
            # Create a Request object from the available data
            request_data = kwargs.get("__request_data__", {})

            # Build scope with app reference for url_for support
            scope = request_data.get("scope", {})
            if "app" not in scope or scope.get("app") is None:
                # Add named routes proxy for url_for support
                proxy = get_named_routes_proxy()
                if proxy:
                    scope = dict(scope)  # Don't modify original
                    scope["app"] = proxy

            request = Request(
                method=request_data.get("method", "GET"),
                path=request_data.get("path", "/"),
                query=request_data.get("query", ""),
                headers=request_data.get("headers", {}),
                query_params=request_data.get("query_params", {}),
                path_params=request_data.get("path_params", {}),
                body=request_data.get("body", ""),
                client_ip=request_data.get("client_ip", "127.0.0.1"),
                client_port=request_data.get("client_port", 0),
                scope=scope,
            )
            resolved_kwargs[param_name] = request
            continue

        # Get value from kwargs
        value = kwargs.get(param_name)

        # Check if param has a default
        has_default = param.default is not inspect.Parameter.empty
        default_value = param.default if has_default else None

        if value is None:
            # No value provided - use default if available
            if has_default:
                resolved_kwargs[param_name] = default_value
            else:
                # Check if Optional type
                origin = get_origin(param_type)
                if str(origin) in ("typing.Union", "types.UnionType"):
                    args = get_args(param_type)
                    if type(None) in args:
                        resolved_kwargs[param_name] = None
                        continue
                # Required param missing
                resolved_kwargs[param_name] = None
            continue

        # Coerce value to target type
        coerced_value, error = coerce_value(value, param_type, param_name)
        if error:
            validation_errors.append(error)
        else:
            resolved_kwargs[param_name] = coerced_value

    # If there were validation errors, raise
    if validation_errors:
        import json
        raise ValueError(json.dumps({"detail": validation_errors}))

    return resolved_kwargs
