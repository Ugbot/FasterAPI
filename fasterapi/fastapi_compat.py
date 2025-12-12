"""
FastAPI-compatible decorator layer for FasterAPI.

This module provides a thin Python decorator layer that mimics FastAPI's API
while leveraging C++ for all performance-critical operations.
"""

import inspect
from functools import wraps
from typing import (
    Any,
    Callable,
    Dict,
    List,
    Optional,
    Type,
    Union,
    get_args,
    get_origin,
    get_type_hints,
)

try:
    from pydantic import BaseModel
    from pydantic import ValidationError as PydanticValidationError

    HAS_PYDANTIC = True
except ImportError:
    HAS_PYDANTIC = False
    BaseModel = object
    PydanticValidationError = Exception

# Import our exception classes
from fasterapi.exceptions import (
    HTTPException,
    RequestValidationError,
    ValidationError,
    convert_pydantic_validation_error,
    format_validation_error_response,
)

# Import C++ native bindings
try:
    from fasterapi._fastapi_native import (
        generate_openapi,
        generate_redoc_response,
        generate_swagger_ui_response,
        get_all_routes,
        has_schema,
        register_route,
        register_schema,
    )

    HAS_NATIVE = True
except ImportError:
    HAS_NATIVE = False
    print("Warning: FasterAPI native bindings not available. Using fallback mode.")


# Type mapping from Python to schema type strings
PYTHON_TYPE_MAP = {
    str: "string",
    int: "integer",
    float: "float",
    bool: "boolean",
    list: "array",
    dict: "object",
    type(None): "null",
}


def python_type_to_string(type_hint: Any) -> str:
    """Convert Python type hint to schema type string."""
    # Handle Optional types
    origin = get_origin(type_hint)
    if origin is type(None):
        return "null"

    # Handle Union types (including Optional)
    if origin is type(type_hint) or str(origin) in ("typing.Union", "types.UnionType"):
        args = get_args(type_hint)
        if len(args) == 2 and type(None) in args:
            # This is Optional[T]
            non_none = args[0] if args[1] is type(None) else args[1]
            return python_type_to_string(non_none)

    # Handle List, Dict
    if origin is list:
        return "array"
    if origin is dict:
        return "object"

    # Handle basic types
    if type_hint in PYTHON_TYPE_MAP:
        return PYTHON_TYPE_MAP[type_hint]

    # Handle Pydantic models
    if (
        HAS_PYDANTIC
        and isinstance(type_hint, type)
        and issubclass(type_hint, BaseModel)
    ):
        return "object"

    # Default to any
    return "any"


def extract_pydantic_schema(
    model: Type[BaseModel], schema_name: Optional[str] = None
) -> Dict[str, Any]:
    """
    Extract schema definition from a Pydantic model.

    Args:
        model: Pydantic BaseModel class
        schema_name: Optional name override (defaults to model.__name__)

    Returns:
        Schema dictionary compatible with C++ SchemaValidator
    """
    if (
        not HAS_PYDANTIC
        or not isinstance(model, type)
        or not issubclass(model, BaseModel)
    ):
        return {"name": schema_name or "Unknown", "fields": []}

    name = schema_name or model.__name__
    fields = []

    # Extract fields from Pydantic model
    for field_name, field_info in model.model_fields.items():
        field_type = field_info.annotation
        is_required = field_info.is_required()

        fields.append(
            {
                "name": field_name,
                "type": python_type_to_string(field_type),
                "required": is_required,
                "default": str(field_info.default)
                if field_info.default is not None
                else "",
            }
        )

    return {"name": name, "fields": fields}


def extract_function_parameters(
    func: Callable, path_pattern: str, method: str
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

    path_params = set(re.findall(r"\{(\w+)\}", path_pattern))

    for param_name, param in sig.parameters.items():
        # Get type hint
        param_type = type_hints.get(param_name, Any)
        type_str = python_type_to_string(param_type)

        # Determine parameter location
        if param_name in path_params:
            location = "path"
            required = True
        elif (
            HAS_PYDANTIC
            and isinstance(param_type, type)
            and issubclass(param_type, BaseModel)
        ):
            # Pydantic model in body
            location = "body"
            required = True
        else:
            # Query parameter
            location = "query"
            required = param.default == inspect.Parameter.empty

        # Get default value
        default_value = (
            "" if param.default == inspect.Parameter.empty else str(param.default)
        )

        parameters.append(
            {
                "name": param_name,
                "type": type_str,
                "location": location,
                "required": required,
                "default": default_value,
                "description": "",
            }
        )

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
        return ""

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
    summary: str = "",
    description: str = "",
    tags: Optional[List[str]] = None,
    **kwargs,
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
        request_body_schema = ""
        type_hints = get_type_hints(func)

        # Build map of Pydantic model parameters for wrapper
        pydantic_params: Dict[str, Type[BaseModel]] = {}
        for param_name, param_type in type_hints.items():
            if param_name == "return":
                continue
            if (
                HAS_PYDANTIC
                and isinstance(param_type, type)
                and issubclass(param_type, BaseModel)
            ):
                pydantic_params[param_name] = param_type
                if not request_body_schema:
                    request_body_schema = register_pydantic_schema(param_type)

        # Register response schema if provided
        response_schema = ""
        if response_model and HAS_PYDANTIC:
            response_schema = register_pydantic_schema(response_model)

        # Create wrapper that validates Pydantic models and handles exceptions
        @wraps(func)
        def wrapper(*args, **kw):
            try:
                # Convert dict kwargs to Pydantic model instances
                if pydantic_params:
                    for param_name, model_class in pydantic_params.items():
                        if param_name in kw and isinstance(kw[param_name], dict):
                            try:
                                kw[param_name] = model_class.model_validate(
                                    kw[param_name]
                                )
                            except PydanticValidationError as e:
                                # Return 422 validation error in FastAPI format
                                errors = convert_pydantic_validation_error(
                                    e, loc_prefix=("body",)
                                )
                                return (format_validation_error_response(errors), 422)
                            except Exception as e:
                                # Generic validation error
                                return (
                                    {
                                        "detail": [
                                            {
                                                "type": "value_error",
                                                "loc": ["body", param_name],
                                                "msg": str(e),
                                                "input": kw.get(param_name),
                                            }
                                        ]
                                    },
                                    422,
                                )

                # Call the actual handler
                return func(*args, **kw)

            except HTTPException as e:
                # Return HTTPException as response with status code
                response = {"detail": e.detail} if e.detail is not None else {}
                # Return tuple with response, status code, and headers
                if e.headers:
                    return (response, e.status_code, e.headers)
                return (response, e.status_code)
            except RequestValidationError as e:
                # Return validation error in FastAPI format
                return (format_validation_error_response(e), 422)
            except Exception as e:
                # Unexpected error - return 500
                return ({"detail": f"Internal server error: {str(e)}"}, 500)

        handler_to_register = wrapper

        # Register route with C++ RouteRegistry
        route_id = register_route(
            method=method.upper(),
            path_pattern=path,
            handler=handler_to_register,
            parameters=parameters,
            request_body_schema=request_body_schema,
            response_schema=response_schema,
            summary=summary,
            description=description,
            tags=tags or [],
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
        **kwargs,
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

        # Exception handlers registry
        self._exception_handlers: Dict[Type[Exception], Callable] = {}

        # Lifecycle event handlers
        self._on_startup: List[Callable] = []
        self._on_shutdown: List[Callable] = []

        # Register special routes if enabled
        if HAS_NATIVE:
            if openapi_url:
                self._register_openapi_route()
            if docs_url:
                self._register_docs_route()
            if redoc_url:
                self._register_redoc_route()

    def exception_handler(
        self, exc_class: Type[Exception]
    ) -> Callable[[Callable], Callable]:
        """
        Register an exception handler.

        Usage:
            @app.exception_handler(HTTPException)
            async def http_exception_handler(request, exc):
                return JSONResponse(
                    status_code=exc.status_code,
                    content={"detail": exc.detail}
                )

        Args:
            exc_class: Exception class to handle

        Returns:
            Decorator function
        """

        def decorator(func: Callable) -> Callable:
            self._exception_handlers[exc_class] = func
            return func

        return decorator

    def add_exception_handler(
        self, exc_class: Type[Exception], handler: Callable
    ) -> None:
        """
        Add an exception handler programmatically.

        Args:
            exc_class: Exception class to handle
            handler: Handler function
        """
        self._exception_handlers[exc_class] = handler

    def on_event(self, event_type: str) -> Callable[[Callable], Callable]:
        """
        Register a lifecycle event handler.

        Usage:
            @app.on_event("startup")
            async def startup():
                print("Starting up...")

            @app.on_event("shutdown")
            async def shutdown():
                print("Shutting down...")

        Args:
            event_type: Either "startup" or "shutdown"

        Returns:
            Decorator function
        """

        def decorator(func: Callable) -> Callable:
            if event_type == "startup":
                self._on_startup.append(func)
            elif event_type == "shutdown":
                self._on_shutdown.append(func)
            else:
                raise ValueError(
                    f"Invalid event type: {event_type}. Must be 'startup' or 'shutdown'"
                )
            return func

        return decorator

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
        summary: str = "",
        description: str = "",
        tags: Optional[List[str]] = None,
        **kwargs,
    ):
        """GET route decorator."""
        return route_decorator(
            "GET", path, response_model, summary, description, tags, **kwargs
        )

    def post(
        self,
        path: str,
        response_model: Optional[Type[BaseModel]] = None,
        summary: str = "",
        description: str = "",
        tags: Optional[List[str]] = None,
        **kwargs,
    ):
        """POST route decorator."""
        return route_decorator(
            "POST", path, response_model, summary, description, tags, **kwargs
        )

    def put(
        self,
        path: str,
        response_model: Optional[Type[BaseModel]] = None,
        summary: str = "",
        description: str = "",
        tags: Optional[List[str]] = None,
        **kwargs,
    ):
        """PUT route decorator."""
        return route_decorator(
            "PUT", path, response_model, summary, description, tags, **kwargs
        )

    def delete(
        self,
        path: str,
        response_model: Optional[Type[BaseModel]] = None,
        summary: str = "",
        description: str = "",
        tags: Optional[List[str]] = None,
        **kwargs,
    ):
        """DELETE route decorator."""
        return route_decorator(
            "DELETE", path, response_model, summary, description, tags, **kwargs
        )

    def patch(
        self,
        path: str,
        response_model: Optional[Type[BaseModel]] = None,
        summary: str = "",
        description: str = "",
        tags: Optional[List[str]] = None,
        **kwargs,
    ):
        """PATCH route decorator."""
        return route_decorator(
            "PATCH", path, response_model, summary, description, tags, **kwargs
        )

    def routes(self) -> List[Dict[str, Any]]:
        """Get all registered routes."""
        if HAS_NATIVE:
            return get_all_routes()
        return []

    def include_router(
        self,
        router: "APIRouter",
        *,
        prefix: str = "",
        tags: Optional[List[str]] = None,
        dependencies: Optional[List[Any]] = None,
        responses: Optional[Dict[int, Dict[str, Any]]] = None,
    ) -> None:
        """
        Include routes from an APIRouter.

        This allows organizing routes into separate modules and then
        including them in the main application.

        Usage:
            from fasterapi import FastAPI, APIRouter

            router = APIRouter(prefix="/users", tags=["users"])

            @router.get("/")
            def list_users():
                return []

            app = FastAPI()
            app.include_router(router)
            # Or with additional prefix:
            app.include_router(router, prefix="/api/v1")

        Args:
            router: The APIRouter to include
            prefix: Additional prefix to prepend to all routes
            tags: Additional tags to add to all routes
            dependencies: Additional dependencies for all routes
            responses: Additional responses for OpenAPI
        """
        # Import here to avoid circular imports
        from fasterapi.routing import APIRouter as RouterClass

        if not isinstance(router, RouterClass):
            raise TypeError(f"Expected APIRouter, got {type(router)}")

        # Get all routes from the router with merged settings
        routes = router.get_routes(
            prefix=prefix,
            tags=tags,
            dependencies=dependencies,
            responses=responses,
        )

        # Register each route with this app
        for route in routes:
            # Use the appropriate method decorator
            decorator = route_decorator(
                method=route.method,
                path=route.path,
                response_model=route.response_model,
                summary=route.summary,
                description=route.description,
                tags=route.tags,
            )
            # Apply the decorator to register the route
            decorator(route.handler)

        # Merge lifecycle hooks
        self._on_startup.extend(router._on_startup)
        self._on_shutdown.extend(router._on_shutdown)

    def mount(self, path: str, app: "StaticFiles", name: str = ""):
        """
        Mount a sub-application (typically StaticFiles) at a path.

        Args:
            path: URL path prefix (e.g., "/static")
            app: Application to mount (e.g., StaticFiles instance)
            name: Optional name for the mount
        """
        if isinstance(app, StaticFiles):
            # Register a wildcard route for static files
            import mimetypes
            import os

            # Initialize mimetypes
            mimetypes.init()

            # Add common MIME types that might be missing
            mimetypes.add_type("text/javascript", ".js")
            mimetypes.add_type("text/javascript", ".mjs")
            mimetypes.add_type("application/json", ".json")
            mimetypes.add_type("image/svg+xml", ".svg")
            mimetypes.add_type("font/woff", ".woff")
            mimetypes.add_type("font/woff2", ".woff2")
            mimetypes.add_type("application/wasm", ".wasm")

            directory = app.directory

            # Normalize path
            if not path.startswith("/"):
                path = "/" + path
            if path.endswith("/"):
                path = path[:-1]

            # Create handler for static files
            def static_handler(file_path: str = ""):
                # Build full file path
                full_path = os.path.join(directory, file_path)

                # Security: prevent directory traversal
                real_path = os.path.realpath(full_path)
                real_dir = os.path.realpath(directory)
                if not real_path.startswith(real_dir):
                    return {"error": "Access denied"}, 403

                # Check if file exists
                if not os.path.isfile(real_path):
                    # Try index.html for directories
                    if os.path.isdir(real_path):
                        index_path = os.path.join(real_path, "index.html")
                        if os.path.isfile(index_path):
                            real_path = index_path
                        else:
                            return {"error": "Not found"}, 404
                    else:
                        return {"error": "Not found"}, 404

                # Read file
                try:
                    with open(real_path, "rb") as f:
                        content = f.read()
                except IOError:
                    return {"error": "Failed to read file"}, 500

                # Determine MIME type
                mime_type, _ = mimetypes.guess_type(real_path)
                if mime_type is None:
                    mime_type = "application/octet-stream"

                # Return as FileResponse
                return FileResponse(content, mime_type, os.path.basename(real_path))

            # Register the route with wildcard
            pattern = path + "/{file_path:path}"

            # Register route with path parameter
            if HAS_NATIVE:
                parameters = [
                    {
                        "name": "file_path",
                        "type": "string",
                        "location": "path",
                        "required": False,  # Allow empty for root
                        "default": "",
                        "description": "Path to static file",
                    }
                ]

                route_id = register_route(
                    method="GET",
                    path_pattern=pattern,
                    handler=static_handler,
                    parameters=parameters,
                    request_body_schema="",
                    response_schema="",
                    summary=f"Static files from {directory}",
                    description="",
                    tags=["static"],
                )


class StaticFiles:
    """
    Starlette-compatible StaticFiles class for serving static files.

    Usage:
        app.mount("/static", StaticFiles(directory="static"), name="static")
    """

    def __init__(self, directory: str, html: bool = False, check_dir: bool = True):
        """
        Initialize static files handler.

        Args:
            directory: Directory path to serve files from
            html: If True, serve index.html for directories
            check_dir: If True, verify directory exists at init
        """
        import os

        self.directory = os.path.abspath(directory)
        self.html = html

        if check_dir and not os.path.isdir(self.directory):
            raise RuntimeError(f"Static files directory '{directory}' does not exist")


class FileResponse:
    """
    Special response type for file content.

    When a handler returns a FileResponse, the server will send
    the raw bytes with the appropriate Content-Type header.
    """

    def __init__(self, content: bytes, media_type: str, filename: str = ""):
        self.content = content
        self.media_type = media_type
        self.filename = filename


# Alias for FastAPI compatibility
FastAPI = FastAPIApp
