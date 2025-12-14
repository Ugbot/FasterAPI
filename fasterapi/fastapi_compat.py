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
    all performance-critical operations. Also supports ASGI interface
    for running with uvicorn in fallback mode.
    """

    def __init__(
        self,
        title: str = "FasterAPI",
        version: str = "0.1.0",
        description: str = "",
        docs_url: str = "/docs",
        redoc_url: str = "/redoc",
        openapi_url: str = "/openapi.json",
        lifespan: Optional[Callable] = None,
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
            lifespan: Async context manager for startup/shutdown

        Example lifespan usage:
            from contextlib import asynccontextmanager

            @asynccontextmanager
            async def lifespan(app):
                # Startup
                app.state.db = await create_db_pool()
                yield
                # Shutdown
                await app.state.db.close()

            app = FastAPI(lifespan=lifespan)
        """
        self.title = title
        self.version = version
        self.description = description
        self.docs_url = docs_url
        self.redoc_url = redoc_url
        self.openapi_url = openapi_url

        # Lifespan context manager (new API)
        self._lifespan = lifespan
        self._lifespan_context = None  # Will hold the context manager instance

        # App state for storing shared data
        from fasterapi.datastructures import State

        self.state = State()

        # Exception handlers registry
        self._exception_handlers: Dict[Type[Exception], Callable] = {}

        # Lifecycle event handlers (old API, still supported)
        self._on_startup: List[Callable] = []
        self._on_shutdown: List[Callable] = []

        # Middleware stack
        self._middleware: List[tuple] = []
        self._middleware_app: Optional[Any] = None  # Cached middleware-wrapped app

        # Route storage for ASGI fallback mode
        self._routes: Dict[str, Dict[str, Callable]] = {}  # {path: {method: handler}}
        self._websocket_routes: Dict[str, Callable] = {}

        # Mounted sub-applications {path_prefix: app}
        self._mounted_apps: Dict[str, Any] = {}

        # Register special routes if enabled
        if HAS_NATIVE:
            if openapi_url:
                self._register_openapi_route()
            if docs_url:
                self._register_docs_route()
            if redoc_url:
                self._register_redoc_route()

    def _build_middleware_stack(self) -> Any:
        """Build the middleware-wrapped application."""
        app = self._handle_request

        # Apply middleware in reverse order (last added = outermost)
        for middleware_class, options in reversed(self._middleware):
            app = middleware_class(app, **options)

        return app

    async def _handle_request(self, scope, receive, send):
        """Handle HTTP/WebSocket requests (inner app for middleware)."""
        if scope["type"] == "http":
            await self._handle_http(scope, receive, send)
        elif scope["type"] == "websocket":
            await self._handle_websocket(scope, receive, send)

    async def _handle_http(self, scope, receive, send):
        """Handle HTTP request (called by middleware or directly)."""
        import json as json_module
        import re

        path = scope["path"]
        method = scope["method"]

        # Check mounted sub-applications first
        for mount_path, mounted_app in self._mounted_apps.items():
            if path == mount_path or path.startswith(mount_path + "/"):
                # Adjust path for the sub-app (strip mount prefix)
                sub_path = path[len(mount_path) :] or "/"
                # Create new scope with adjusted path
                sub_scope = dict(scope)
                sub_scope["path"] = sub_path
                sub_scope["root_path"] = scope.get("root_path", "") + mount_path
                # Delegate to mounted app
                await mounted_app(sub_scope, receive, send)
                return

        # Find matching route
        handler = None
        response_model = None
        path_params = {}

        for route_path, methods in self._routes.items():
            if method in methods:
                # Check for exact match
                if route_path == path:
                    route_info = methods[method]
                    if isinstance(route_info, tuple):
                        handler, response_model = route_info
                    else:
                        handler = route_info
                    break

                # Check for path parameter match
                pattern = re.sub(r"\{(\w+)\}", r"(?P<\1>[^/]+)", route_path)
                pattern = f"^{pattern}$"
                match = re.match(pattern, path)
                if match:
                    route_info = methods[method]
                    if isinstance(route_info, tuple):
                        handler, response_model = route_info
                    else:
                        handler = route_info
                    path_params = match.groupdict()
                    break

        if handler is None:
            # 404 Not Found
            await self._send_json_response(send, {"detail": "Not Found"}, 404)
            return

        # Parse request body
        body = b""
        while True:
            message = await receive()
            body += message.get("body", b"")
            if not message.get("more_body", False):
                break

        # Parse query params
        from urllib.parse import unquote

        query_string = scope.get("query_string", b"").decode()
        query_params = {}
        if query_string:
            for param in query_string.split("&"):
                if "=" in param:
                    key, value = param.split("=", 1)
                    query_params[key] = unquote(value)

        # Parse body based on content type
        body_data = None
        form_data = None
        content_type = ""
        for key, value in scope.get("headers", []):
            if key == b"content-type":
                content_type = value.decode()
                break

        content_type_lower = content_type.lower()

        if body:
            if "application/json" in content_type_lower:
                try:
                    body_data = json_module.loads(body.decode())
                except (json_module.JSONDecodeError, UnicodeDecodeError):
                    pass
            elif (
                "multipart/form-data" in content_type_lower
                or "application/x-www-form-urlencoded" in content_type_lower
            ):
                # Parse form data (keep original content_type for boundary parsing)
                from fasterapi.http.request import Request

                temp_request = Request(
                    "POST",
                    path,
                    body_bytes=body,
                    headers={"content-type": content_type},
                )
                form_data = await temp_request.form()

        # Build kwargs for handler based on signature
        kwargs = {}
        sig = inspect.signature(handler)
        try:
            type_hints = get_type_hints(handler)
        except Exception:
            type_hints = {}

        # Import param classes and UploadFile to check instance
        try:
            from fasterapi.datastructures import UploadFile
            from fasterapi.params import Body as BodyParam
            from fasterapi.params import Depends as DependsParam
            from fasterapi.params import File as FileParam
            from fasterapi.params import Form as FormParam
            from fasterapi.params import Path as PathParam
            from fasterapi.params import Query as QueryParam

            has_param_classes = True
        except ImportError:
            has_param_classes = False
            UploadFile = None
            DependsParam = None

        # Create a request object for dependencies
        from fasterapi.http.request import Request

        request_headers = {
            k.decode() if isinstance(k, bytes) else k: v.decode()
            if isinstance(v, bytes)
            else v
            for k, v in scope.get("headers", [])
        }
        request_obj = Request(
            method=method,
            path=path,
            query_params=query_params,
            headers=request_headers,
            body_bytes=body,
        )

        # Process each parameter based on its type and default
        for param_name, param in sig.parameters.items():
            param_type = type_hints.get(param_name, str)
            default = param.default

            # Check if it's a path parameter
            if param_name in path_params:
                value = path_params[param_name]
                # Convert to appropriate type
                if param_type == int:
                    value = int(value)
                elif param_type == float:
                    value = float(value)
                kwargs[param_name] = value
                continue

            # Check if it's a Depends parameter (dependency injection)
            if has_param_classes and DependsParam is not None:
                if isinstance(default, DependsParam):
                    dep_func = default.dependency
                    if dep_func is not None:
                        try:
                            # Call the dependency with the request object
                            if inspect.iscoroutinefunction(dep_func):
                                kwargs[param_name] = await dep_func(request=request_obj)
                            elif hasattr(dep_func, "__call__"):
                                # Check if __call__ is async (for callable classes like JWTBearer)
                                call_method = getattr(dep_func, "__call__", None)
                                if call_method and inspect.iscoroutinefunction(
                                    call_method
                                ):
                                    kwargs[param_name] = await dep_func(
                                        request=request_obj
                                    )
                                else:
                                    kwargs[param_name] = dep_func(request=request_obj)
                            else:
                                kwargs[param_name] = dep_func(request=request_obj)
                        except HTTPException as e:
                            # Dependency raised HTTPException (e.g., auth failure)
                            response = (
                                {"detail": e.detail} if e.detail is not None else {}
                            )
                            await self._send_json_response(
                                send, response, e.status_code, e.headers or {}
                            )
                            return
                    continue

            # Check if it's an UploadFile parameter (by type annotation)
            if has_param_classes and UploadFile is not None:
                is_upload_file_type = param_type is UploadFile or (
                    isinstance(param_type, type) and param_type.__name__ == "UploadFile"
                )
                if is_upload_file_type:
                    if form_data and param_name in form_data:
                        kwargs[param_name] = form_data[param_name]
                    elif isinstance(default, FileParam):
                        # Use the default from File(...) descriptor
                        kwargs[param_name] = getattr(default, "default", None)
                    elif default is not inspect.Parameter.empty and default is not ...:
                        kwargs[param_name] = default
                    else:
                        kwargs[param_name] = None
                    continue

            # Check if it's a Pydantic model (body parameter)
            if (
                HAS_PYDANTIC
                and isinstance(param_type, type)
                and issubclass(param_type, BaseModel)
            ):
                if body_data is not None:
                    try:
                        # Convert dict to Pydantic model instance
                        kwargs[param_name] = param_type.model_validate(body_data)
                    except Exception as e:
                        await self._send_json_response(
                            send,
                            {
                                "detail": [
                                    {
                                        "loc": ["body"],
                                        "msg": str(e),
                                        "type": "value_error",
                                    }
                                ]
                            },
                            422,
                        )
                        return
                continue

            # Handle Query/Path/Body/Form/File descriptors or regular defaults
            is_descriptor = False
            actual_default = inspect.Parameter.empty

            if has_param_classes and default is not inspect.Parameter.empty:
                if isinstance(default, (QueryParam, PathParam)):
                    is_descriptor = True
                    actual_default = getattr(default, "default", None)
                elif isinstance(default, BodyParam):
                    is_descriptor = True
                    if body_data is not None:
                        kwargs[param_name] = body_data
                    continue
                elif isinstance(default, FormParam):
                    # Form field parameter
                    is_descriptor = True
                    if form_data and param_name in form_data:
                        kwargs[param_name] = form_data[param_name]
                    else:
                        actual_default = getattr(default, "default", None)
                        if actual_default is not ...:
                            kwargs[param_name] = actual_default
                        else:
                            kwargs[param_name] = ""
                    continue
                elif isinstance(default, FileParam):
                    # File upload parameter
                    is_descriptor = True
                    if form_data and param_name in form_data:
                        kwargs[param_name] = form_data[param_name]
                    else:
                        actual_default = getattr(default, "default", None)
                        if actual_default is not ...:
                            kwargs[param_name] = actual_default
                        else:
                            kwargs[param_name] = None
                    continue

            if is_descriptor:
                # Extract value from query params or use default
                if param_name in query_params:
                    value = query_params[param_name]
                    # Convert to appropriate type
                    if param_type == int:
                        value = int(value)
                    elif param_type == float:
                        value = float(value)
                    elif param_type == bool:
                        value = value.lower() in ("true", "1", "yes")
                    kwargs[param_name] = value
                elif actual_default is not ...:
                    # Use the actual default (can be None for Optional params)
                    kwargs[param_name] = actual_default
            elif param_name in query_params:
                # Regular query parameter
                value = query_params[param_name]
                if param_type == int:
                    value = int(value)
                elif param_type == float:
                    value = float(value)
                elif param_type == bool:
                    value = value.lower() in ("true", "1", "yes")
                kwargs[param_name] = value
            elif default is not inspect.Parameter.empty:
                # Use default value if no query param provided
                kwargs[param_name] = default

        # Call handler
        try:
            if inspect.iscoroutinefunction(handler):
                result = await handler(**kwargs)
            else:
                result = handler(**kwargs)

            # Handle tuple responses (body, status) or (body, status, headers)
            status_code = 200
            headers = {}
            if isinstance(result, tuple):
                if len(result) == 2:
                    result, status_code = result
                elif len(result) >= 3:
                    result, status_code, headers = result[0], result[1], result[2]

            # Apply response_model validation/filtering if specified
            if response_model is not None and HAS_PYDANTIC:
                try:
                    # Check if response_model is a generic like List[Model]
                    origin = get_origin(response_model)
                    inner_model = None

                    if origin is list:
                        args = get_args(response_model)
                        if (
                            args
                            and isinstance(args[0], type)
                            and issubclass(args[0], BaseModel)
                        ):
                            inner_model = args[0]

                    if isinstance(result, list) and inner_model is not None:
                        # Handle List[ResponseModel]
                        validated = []
                        for item in result:
                            if isinstance(item, BaseModel):
                                validated.append(item.model_dump())
                            elif isinstance(item, dict):
                                validated.append(
                                    inner_model.model_validate(item).model_dump()
                                )
                            else:
                                validated.append(item)
                        result = validated
                    elif isinstance(result, BaseModel):
                        # Already a model, convert to dict
                        result = result.model_dump()
                    elif (
                        isinstance(result, dict)
                        and isinstance(response_model, type)
                        and issubclass(response_model, BaseModel)
                    ):
                        # Validate and filter through response_model
                        result = response_model.model_validate(result).model_dump()
                except PydanticValidationError as e:
                    # Response validation failed
                    errors = convert_pydantic_validation_error(
                        e, loc_prefix=("response",)
                    )
                    await self._send_json_response(
                        send, format_validation_error_response(errors), 500
                    )
                    return
            elif HAS_PYDANTIC and isinstance(result, BaseModel):
                # Convert Pydantic models to dict even without response_model
                result = result.model_dump()

            # Handle Response objects (StreamingResponse, FileResponse, etc.)
            from fasterapi.responses import FileResponse, StreamingResponse
            from fasterapi.responses import Response as BaseResponse

            if isinstance(result, StreamingResponse):
                # Send streaming response
                response_headers = [
                    (
                        k.encode() if isinstance(k, str) else k,
                        v.encode() if isinstance(v, str) else v,
                    )
                    for k, v in result.headers.items()
                ]
                await send(
                    {
                        "type": "http.response.start",
                        "status": result.status_code,
                        "headers": response_headers,
                    }
                )
                async for chunk in result.stream_response():
                    await send(
                        {
                            "type": "http.response.body",
                            "body": chunk,
                            "more_body": True,
                        }
                    )
                await send(
                    {
                        "type": "http.response.body",
                        "body": b"",
                        "more_body": False,
                    }
                )
                return

            if isinstance(result, FileResponse):
                # Send file response
                response_headers = [
                    (
                        k.encode() if isinstance(k, str) else k,
                        v.encode() if isinstance(v, str) else v,
                    )
                    for k, v in result.headers.items()
                ]
                await send(
                    {
                        "type": "http.response.start",
                        "status": result.status_code,
                        "headers": response_headers,
                    }
                )
                async for chunk in result.stream_response():
                    await send(
                        {
                            "type": "http.response.body",
                            "body": chunk,
                            "more_body": True,
                        }
                    )
                await send(
                    {
                        "type": "http.response.body",
                        "body": b"",
                        "more_body": False,
                    }
                )
                return

            if isinstance(result, BaseResponse):
                # Send regular Response object
                response_headers = [
                    (
                        k.encode() if isinstance(k, str) else k,
                        v.encode() if isinstance(v, str) else v,
                    )
                    for k, v in result.headers.items()
                ]
                await send(
                    {
                        "type": "http.response.start",
                        "status": result.status_code,
                        "headers": response_headers,
                    }
                )
                await send(
                    {
                        "type": "http.response.body",
                        "body": result.body or b"",
                    }
                )
                return

            await self._send_json_response(send, result, status_code, headers)

        except HTTPException as e:
            response = {"detail": e.detail} if e.detail is not None else {}
            await self._send_json_response(
                send, response, e.status_code, e.headers or {}
            )
        except Exception as e:
            await self._send_json_response(send, {"detail": str(e)}, 500)

    async def _handle_websocket(self, scope, receive, send):
        """Handle WebSocket connection."""
        import re

        path = scope["path"]

        # Check mounted sub-applications first
        for mount_path, mounted_app in self._mounted_apps.items():
            if path == mount_path or path.startswith(mount_path + "/"):
                # Adjust path for the sub-app (strip mount prefix)
                sub_path = path[len(mount_path) :] or "/"
                # Create new scope with adjusted path
                sub_scope = dict(scope)
                sub_scope["path"] = sub_path
                sub_scope["root_path"] = scope.get("root_path", "") + mount_path
                # Delegate to mounted app
                await mounted_app(sub_scope, receive, send)
                return

        # Find matching WebSocket route
        handler = None
        path_params = {}

        for route_path, ws_handler in self._websocket_routes.items():
            if route_path == path:
                handler = ws_handler
                break

            # Check for path parameter match
            pattern = re.sub(r"\{(\w+)\}", r"(?P<\1>[^/]+)", route_path)
            pattern = f"^{pattern}$"
            match = re.match(pattern, path)
            if match:
                handler = ws_handler
                path_params = match.groupdict()
                break

        if handler is None:
            await send({"type": "websocket.close", "code": 4004})
            return

        # Create WebSocket wrapper
        from fasterapi.websockets import WebSocket as FasterAPIWebSocket

        websocket = FasterAPIWebSocket(scope, receive, send)

        # Call handler with path params
        try:
            # Parse query params for WebSocket
            query_string = scope.get("query_string", b"").decode()
            query_params = {}
            if query_string:
                for param in query_string.split("&"):
                    if "=" in param:
                        key, value = param.split("=", 1)
                        query_params[key] = value

            kwargs = {"websocket": websocket}
            kwargs.update(path_params)
            kwargs.update(query_params)

            if inspect.iscoroutinefunction(handler):
                await handler(**kwargs)
            else:
                handler(**kwargs)
        except Exception:
            try:
                await send({"type": "websocket.close", "code": 1011})
            except Exception:
                pass

    async def __call__(self, scope, receive, send):
        """
        ASGI interface for running with uvicorn/other ASGI servers.

        This is the fallback mode when native C++ bindings aren't available.
        """
        import json as json_module
        import re

        if scope["type"] == "lifespan":
            # Handle lifespan events
            while True:
                message = await receive()
                if message["type"] == "lifespan.startup":
                    try:
                        # If lifespan context manager is provided, use it
                        if self._lifespan is not None:
                            self._lifespan_context = self._lifespan(self)
                            await self._lifespan_context.__aenter__()

                        # Also run old-style startup handlers for compatibility
                        for handler in self._on_startup:
                            if inspect.iscoroutinefunction(handler):
                                await handler()
                            else:
                                handler()
                        await send({"type": "lifespan.startup.complete"})
                    except Exception as e:
                        await send(
                            {"type": "lifespan.startup.failed", "message": str(e)}
                        )
                        return

                elif message["type"] == "lifespan.shutdown":
                    try:
                        # Run old-style shutdown handlers first
                        for handler in self._on_shutdown:
                            if inspect.iscoroutinefunction(handler):
                                await handler()
                            else:
                                handler()

                        # If lifespan context manager was used, exit it
                        if self._lifespan_context is not None:
                            await self._lifespan_context.__aexit__(None, None, None)
                            self._lifespan_context = None

                        await send({"type": "lifespan.shutdown.complete"})
                    except Exception:
                        await send({"type": "lifespan.shutdown.complete"})
                    return

        elif scope["type"] == "http":
            # Build middleware stack if needed
            if self._middleware:
                if self._middleware_app is None:
                    self._middleware_app = self._build_middleware_stack()
                await self._middleware_app(scope, receive, send)
                return

            # No middleware - handle directly
            await self._handle_http(scope, receive, send)

        elif scope["type"] == "websocket":
            await self._handle_websocket(scope, receive, send)

    async def _send_json_response(self, send, body, status_code=200, headers=None):
        """Send a JSON response."""
        import json as json_module
        from datetime import date, datetime

        if headers is None:
            headers = {}

        def json_serializer(obj):
            if isinstance(obj, datetime):
                return obj.isoformat()
            elif isinstance(obj, date):
                return obj.isoformat()
            raise TypeError(
                f"Object of type {type(obj).__name__} is not JSON serializable"
            )

        body_bytes = (
            json_module.dumps(body, default=json_serializer).encode()
            if body is not None
            else b""
        )

        response_headers = [
            (b"content-type", b"application/json"),
            (b"content-length", str(len(body_bytes)).encode()),
        ]
        for key, value in headers.items():
            response_headers.append((key.lower().encode(), str(value).encode()))

        await send(
            {
                "type": "http.response.start",
                "status": status_code,
                "headers": response_headers,
            }
        )
        await send(
            {
                "type": "http.response.body",
                "body": body_bytes,
            }
        )

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

    def add_middleware(
        self,
        middleware_class: Type,
        **options: Any,
    ) -> None:
        """
        Add middleware to the application.

        Usage:
            from fasterapi.middleware import CORSMiddleware

            app.add_middleware(
                CORSMiddleware,
                allow_origins=["*"],
                allow_methods=["*"],
                allow_headers=["*"],
            )

        Args:
            middleware_class: Middleware class to add
            **options: Options to pass to the middleware constructor
        """
        self._middleware.append((middleware_class, options))

    def middleware(self, middleware_type: str) -> Callable[[Callable], Callable]:
        """
        Decorator to add middleware using the dispatch pattern.

        Usage:
            @app.middleware("http")
            async def add_process_time_header(request, call_next):
                import time
                start_time = time.time()
                response = await call_next(request)
                process_time = time.time() - start_time
                response.headers["X-Process-Time"] = str(process_time)
                return response

        Args:
            middleware_type: Type of middleware ("http")

        Returns:
            Decorator function
        """

        def decorator(func: Callable) -> Callable:
            # Import BaseHTTPMiddleware to create wrapper
            from fasterapi.middleware import BaseHTTPMiddleware

            # Create a middleware class from the function
            class FunctionMiddleware(BaseHTTPMiddleware):
                async def dispatch(self, request, call_next):
                    return await func(request, call_next)

            self._middleware.append((FunctionMiddleware, {}))
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

    def _register_route(
        self,
        method: str,
        path: str,
        response_model: Optional[Type[BaseModel]] = None,
        summary: str = "",
        description: str = "",
        tags: Optional[List[str]] = None,
        **kwargs,
    ):
        """Internal method to register a route and return decorator."""
        app = self  # Capture self for the decorator

        def decorator(func: Callable) -> Callable:
            # Store route for ASGI fallback mode (handler, response_model)
            if path not in app._routes:
                app._routes[path] = {}
            app._routes[path][method.upper()] = (func, response_model)

            # If native bindings available, also register with C++
            if HAS_NATIVE:
                # Use the existing route_decorator logic
                native_decorator = route_decorator(
                    method, path, response_model, summary, description, tags, **kwargs
                )
                return native_decorator(func)

            return func

        return decorator

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
        return self._register_route(
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
        return self._register_route(
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
        return self._register_route(
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
        return self._register_route(
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
        return self._register_route(
            "PATCH", path, response_model, summary, description, tags, **kwargs
        )

    def options(
        self,
        path: str,
        response_model: Optional[Type[BaseModel]] = None,
        summary: str = "",
        description: str = "",
        tags: Optional[List[str]] = None,
        **kwargs,
    ):
        """OPTIONS route decorator."""
        return self._register_route(
            "OPTIONS", path, response_model, summary, description, tags, **kwargs
        )

    def head(
        self,
        path: str,
        response_model: Optional[Type[BaseModel]] = None,
        summary: str = "",
        description: str = "",
        tags: Optional[List[str]] = None,
        **kwargs,
    ):
        """HEAD route decorator."""
        return self._register_route(
            "HEAD", path, response_model, summary, description, tags, **kwargs
        )

    def websocket(
        self,
        path: str,
        **kwargs,
    ) -> Callable[[Callable], Callable]:
        """
        WebSocket route decorator.

        Usage:
            @app.websocket("/ws")
            async def websocket_endpoint(websocket: WebSocket):
                await websocket.accept()
                while True:
                    data = await websocket.receive_text()
                    await websocket.send_text(f"Echo: {data}")

        Args:
            path: WebSocket endpoint path
            **kwargs: Additional options

        Returns:
            Decorator function
        """

        def decorator(func: Callable) -> Callable:
            # Store the websocket handler
            if not hasattr(self, "_websocket_routes"):
                self._websocket_routes: Dict[str, Callable] = {}
            self._websocket_routes[path] = func

            # Register with native if available
            if HAS_NATIVE:
                register_route("WEBSOCKET", path, func.__name__, "", "", [])

            return func

        return decorator

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

    def mount(self, path: str, app: Any, name: str = ""):
        """
        Mount a sub-application at a path.

        Supports:
        - StaticFiles for serving static files
        - ASGI applications (FastAPI, FasterAPI, Starlette, etc.)

        Args:
            path: URL path prefix (e.g., "/static" or "/api/v1")
            app: Application to mount (StaticFiles or ASGI app)
            name: Optional name for the mount

        Examples:
            # Mount static files
            app.mount("/static", StaticFiles(directory="static"))

            # Mount sub-application
            api_v1 = FastAPI()
            app.mount("/api/v1", api_v1)
        """
        # Normalize path
        if not path.startswith("/"):
            path = "/" + path
        if path.endswith("/") and path != "/":
            path = path[:-1]

        # Check if it's an ASGI app (has __call__ method that's async)
        if hasattr(app, "__call__") and not isinstance(app, StaticFiles):
            # Mount as sub-application
            self._mounted_apps[path] = app
            return

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
