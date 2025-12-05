"""
FastAPI-compatible decorator layer for FasterAPI.

This module provides a thin Python decorator layer that mimics FastAPI's API
while leveraging C++ for all performance-critical operations.
"""

import inspect
from typing import Any, Callable, Dict, List, Optional, Type, get_type_hints, get_origin, get_args
from functools import wraps

try:
    from pydantic import BaseModel
    HAS_PYDANTIC = True
except ImportError:
    HAS_PYDANTIC = False
    BaseModel = object

# Import C++ native bindings
try:
    from fasterapi._fastapi_native import (
        register_schema,
        register_route,
        generate_openapi,
        generate_swagger_ui_response,
        generate_redoc_response,
        has_schema,
        get_all_routes
    )
    HAS_NATIVE = True
except ImportError:
    HAS_NATIVE = False
    print("Warning: FasterAPI native bindings not available. Using fallback mode.")


# Type mapping from Python to schema type strings
PYTHON_TYPE_MAP = {
    str: 'string',
    int: 'integer',
    float: 'float',
    bool: 'boolean',
    list: 'array',
    dict: 'object',
    type(None): 'null',
}


def python_type_to_string(type_hint: Any) -> str:
    """Convert Python type hint to schema type string."""
    # Handle Optional types
    origin = get_origin(type_hint)
    if origin is type(None):
        return 'null'

    # Handle Union types (including Optional)
    if origin is type(type_hint) or str(origin) in ('typing.Union', 'types.UnionType'):
        args = get_args(type_hint)
        if len(args) == 2 and type(None) in args:
            # This is Optional[T]
            non_none = args[0] if args[1] is type(None) else args[1]
            return python_type_to_string(non_none)

    # Handle List, Dict
    if origin is list:
        return 'array'
    if origin is dict:
        return 'object'

    # Handle basic types
    if type_hint in PYTHON_TYPE_MAP:
        return PYTHON_TYPE_MAP[type_hint]

    # Handle Pydantic models
    if HAS_PYDANTIC and isinstance(type_hint, type) and issubclass(type_hint, BaseModel):
        return 'object'

    # Default to any
    return 'any'


def extract_pydantic_schema(model: Type[BaseModel], schema_name: Optional[str] = None) -> Dict[str, Any]:
    """
    Extract schema definition from a Pydantic model.

    Args:
        model: Pydantic BaseModel class
        schema_name: Optional name override (defaults to model.__name__)

    Returns:
        Schema dictionary compatible with C++ SchemaValidator
    """
    if not HAS_PYDANTIC or not isinstance(model, type) or not issubclass(model, BaseModel):
        return {'name': schema_name or 'Unknown', 'fields': []}

    name = schema_name or model.__name__
    fields = []

    # Extract fields from Pydantic model
    for field_name, field_info in model.model_fields.items():
        field_type = field_info.annotation
        is_required = field_info.is_required()

        fields.append({
            'name': field_name,
            'type': python_type_to_string(field_type),
            'required': is_required,
            'default': str(field_info.default) if field_info.default is not None else '',
        })

    return {
        'name': name,
        'fields': fields
    }


def extract_function_parameters(
    func: Callable,
    path_pattern: str,
    method: str
) -> List[Dict[str, Any]]:
    """
    Extract parameter definitions from function signature.

    Args:
        func: The handler function
        path_pattern: URL path pattern (e.g., '/users/{user_id}')
        method: HTTP method

    Returns:
        List of parameter definitions
    """
    sig = inspect.signature(func)
    type_hints = get_type_hints(func)
    parameters = []

    # Extract path parameters from pattern
    import re
    path_params = set(re.findall(r'\{(\w+)\}', path_pattern))

    for param_name, param in sig.parameters.items():
        # Get type hint
        param_type = type_hints.get(param_name, Any)
        type_str = python_type_to_string(param_type)

        # Determine parameter location
        if param_name in path_params:
            location = 'path'
            required = True
        elif HAS_PYDANTIC and isinstance(param_type, type) and issubclass(param_type, BaseModel):
            # Pydantic model in body
            location = 'body'
            required = True
        else:
            # Query parameter
            location = 'query'
            required = param.default == inspect.Parameter.empty

        # Get default value
        default_value = '' if param.default == inspect.Parameter.empty else str(param.default)

        parameters.append({
            'name': param_name,
            'type': type_str,
            'location': location,
            'required': required,
            'default': default_value,
            'description': ''
        })

    return parameters


def register_pydantic_schema(model: Type[BaseModel]) -> str:
    """
    Register a Pydantic model schema with C++ SchemaRegistry.

    Args:
        model: Pydantic BaseModel class

    Returns:
        Schema name
    """
    if not HAS_NATIVE or not HAS_PYDANTIC:
        return ''

    schema_name = model.__name__

    # Check if already registered
    if has_schema(schema_name):
        return schema_name

    # Extract and register schema
    schema_def = extract_pydantic_schema(model, schema_name)
    register_schema(schema_name, schema_def)

    return schema_name


def route_decorator(
    method: str,
    path: str,
    response_model: Optional[Type[BaseModel]] = None,
    summary: str = '',
    description: str = '',
    tags: Optional[List[str]] = None,
    **kwargs
):
    """
    Decorator factory for HTTP routes.

    Args:
        method: HTTP method (GET, POST, etc.)
        path: URL path pattern
        response_model: Optional Pydantic model for response validation
        summary: Short description for OpenAPI
        description: Detailed description for OpenAPI
        tags: Tags for OpenAPI grouping
    """
    def decorator(func: Callable):
        if not HAS_NATIVE:
            # Fallback mode - just return the function
            return func

        # Extract function signature
        parameters = extract_function_parameters(func, path, method)

        # Find request body schema (Pydantic model parameter)
        request_body_schema = ''
        sig = inspect.signature(func)
        type_hints = get_type_hints(func)

        for param_name, param_type in type_hints.items():
            if param_name == 'return':
                continue
            if HAS_PYDANTIC and isinstance(param_type, type) and issubclass(param_type, BaseModel):
                request_body_schema = register_pydantic_schema(param_type)
                break

        # Register response schema if provided
        response_schema = ''
        if response_model and HAS_PYDANTIC:
            response_schema = register_pydantic_schema(response_model)

        # Register route with C++ RouteRegistry
        route_id = register_route(
            method=method.upper(),
            path_pattern=path,
            handler=func,
            parameters=parameters,
            request_body_schema=request_body_schema,
            response_schema=response_schema,
            summary=summary,
            description=description,
            tags=tags or []
        )

        if route_id < 0:
            print(f"Warning: Failed to register route {method} {path}")

        # Return the original function (C++ will call it)
        return func

    return decorator


class FastAPIApp:
    """
    FastAPI-compatible application class.

    This provides the same decorator API as FastAPI but uses C++ for
    all performance-critical operations.
    """

    def __init__(
        self,
        title: str = "FasterAPI",
        version: str = "0.1.0",
        description: str = "",
        docs_url: str = "/docs",
        redoc_url: str = "/redoc",
        openapi_url: str = "/openapi.json",
        **kwargs
    ):
        """
        Initialize FastAPI-compatible app.

        Args:
            title: API title for OpenAPI
            version: API version
            description: API description
            docs_url: Swagger UI URL (None to disable)
            redoc_url: ReDoc URL (None to disable)
            openapi_url: OpenAPI spec URL
        """
        self.title = title
        self.version = version
        self.description = description
        self.docs_url = docs_url
        self.redoc_url = redoc_url
        self.openapi_url = openapi_url

        # Register special routes if enabled
        if HAS_NATIVE:
            if openapi_url:
                self._register_openapi_route()
            if docs_url:
                self._register_docs_route()
            if redoc_url:
                self._register_redoc_route()

    def _register_openapi_route(self):
        """Register OpenAPI spec endpoint."""
        @self.get(self.openapi_url)
        def openapi():
            return generate_openapi(self.title, self.version, self.description)

    def _register_docs_route(self):
        """Register Swagger UI endpoint."""
        @self.get(self.docs_url)
        def swagger_ui():
            return generate_swagger_ui_response(self.openapi_url, self.title)

    def _register_redoc_route(self):
        """Register ReDoc endpoint."""
        @self.get(self.redoc_url)
        def redoc():
            return generate_redoc_response(self.openapi_url, self.title)

    def get(
        self,
        path: str,
        response_model: Optional[Type[BaseModel]] = None,
        summary: str = '',
        description: str = '',
        tags: Optional[List[str]] = None,
        **kwargs
    ):
        """GET route decorator."""
        return route_decorator('GET', path, response_model, summary, description, tags, **kwargs)

    def post(
        self,
        path: str,
        response_model: Optional[Type[BaseModel]] = None,
        summary: str = '',
        description: str = '',
        tags: Optional[List[str]] = None,
        **kwargs
    ):
        """POST route decorator."""
        return route_decorator('POST', path, response_model, summary, description, tags, **kwargs)

    def put(
        self,
        path: str,
        response_model: Optional[Type[BaseModel]] = None,
        summary: str = '',
        description: str = '',
        tags: Optional[List[str]] = None,
        **kwargs
    ):
        """PUT route decorator."""
        return route_decorator('PUT', path, response_model, summary, description, tags, **kwargs)

    def delete(
        self,
        path: str,
        response_model: Optional[Type[BaseModel]] = None,
        summary: str = '',
        description: str = '',
        tags: Optional[List[str]] = None,
        **kwargs
    ):
        """DELETE route decorator."""
        return route_decorator('DELETE', path, response_model, summary, description, tags, **kwargs)

    def patch(
        self,
        path: str,
        response_model: Optional[Type[BaseModel]] = None,
        summary: str = '',
        description: str = '',
        tags: Optional[List[str]] = None,
        **kwargs
    ):
        """PATCH route decorator."""
        return route_decorator('PATCH', path, response_model, summary, description, tags, **kwargs)

    def routes(self) -> List[Dict[str, Any]]:
        """Get all registered routes."""
        if HAS_NATIVE:
            return get_all_routes()
        return []


# Alias for FastAPI compatibility
FastAPI = FastAPIApp
