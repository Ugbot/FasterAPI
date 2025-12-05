# distutils: language = c++
# cython: language_level=3

"""
Cython implementation layer for FasterAPI C++ components.
This provides Python-accessible wrappers for all C++ FastAPI functionality.
"""

from libcpp.string cimport string
from libcpp.vector cimport vector
from libcpp.unordered_map cimport unordered_map
from libcpp cimport bool as bool_t
from libcpp.memory cimport shared_ptr, make_shared
from libcpp.utility cimport move
from cpython.ref cimport PyObject, Py_INCREF, Py_DECREF
from libc.stdint cimport uintptr_t

from fasterapi._fastapi_native cimport (
    SchemaType, Schema, SchemaRegistry,
    schema_type_from_int, schema_type_to_int,
    ParameterLocation, ParameterInfo, RouteMetadata, RouteRegistry,
    param_location_from_int, param_location_to_int,
    OpenAPIGenerator, StaticDocs, ValidationErrorFormatter,
    ValidationResult, ValidationError
)

import sys
from typing import Dict, List, Any, Callable, Optional


# ============================================================================
# Schema Type Conversion
# ============================================================================

cdef SchemaType python_type_to_schema_type(str type_name):
    """Convert Python type string to C++ SchemaType enum."""
    type_map = {
        'string': SchemaType.STRING,
        'str': SchemaType.STRING,
        'integer': SchemaType.INTEGER,
        'int': SchemaType.INTEGER,
        'float': SchemaType.FLOAT,
        'number': SchemaType.FLOAT,
        'boolean': SchemaType.BOOLEAN,
        'bool': SchemaType.BOOLEAN,
        'array': SchemaType.ARRAY,
        'list': SchemaType.ARRAY,
        'object': SchemaType.OBJECT,
        'dict': SchemaType.OBJECT,
        'null': SchemaType.NULL_TYPE,
        'any': SchemaType.ANY,
    }
    return type_map.get(type_name.lower(), SchemaType.ANY)


cdef ParameterLocation python_loc_to_param_location(str location):
    """Convert Python location string to C++ ParameterLocation enum."""
    loc_map = {
        'path': ParameterLocation.PATH,
        'query': ParameterLocation.QUERY,
        'body': ParameterLocation.BODY,
        'header': ParameterLocation.HEADER,
        'cookie': ParameterLocation.COOKIE,
    }
    return loc_map.get(location.lower(), ParameterLocation.QUERY)


# ============================================================================
# Schema Management
# ============================================================================

def register_schema(str schema_name, dict schema_def):
    """
    Register a schema with the C++ SchemaRegistry.

    Args:
        schema_name: Name of the schema (e.g., 'User')
        schema_def: Dictionary defining the schema fields
                   {'fields': [{'name': 'id', 'type': 'integer', 'required': True}, ...]}
    """
    cdef shared_ptr[Schema] schema = make_shared[Schema](schema_name.encode('utf-8'))
    cdef dict field
    cdef str field_name, field_type
    cdef bool_t required

    # Extract fields from schema definition
    fields = schema_def.get('fields', [])
    for field in fields:
        field_name = field.get('name', '')
        field_type = field.get('type', 'any')
        required = field.get('required', True)

        schema.get().add_field(
            field_name.encode('utf-8'),
            python_type_to_schema_type(field_type),
            required
        )

    # Register with the global registry
    SchemaRegistry.instance().register_schema(
        schema_name.encode('utf-8'),
        schema
    )


def has_schema(str schema_name) -> bool:
    """Check if a schema is registered."""
    return SchemaRegistry.instance().has_schema(schema_name.encode('utf-8'))


# ============================================================================
# Route Registration
# ============================================================================

# Global route registry instance
cdef RouteRegistry* _route_registry = NULL

def _init_route_registry():
    """Initialize the global route registry."""
    global _route_registry
    if _route_registry == NULL:
        _route_registry = new RouteRegistry()

def _cleanup_route_registry():
    """Cleanup the global route registry."""
    global _route_registry
    if _route_registry != NULL:
        del _route_registry
        _route_registry = NULL

# Initialize on module load
_init_route_registry()

# Register cleanup on module unload
import atexit
atexit.register(_cleanup_route_registry)


def register_route(
    str method,
    str path_pattern,
    object handler,
    list parameters = None,
    str request_body_schema = '',
    str response_schema = '',
    str summary = '',
    str description = '',
    list tags = None
):
    """
    Register a route with the C++ RouteRegistry.

    Args:
        method: HTTP method (GET, POST, etc.)
        path_pattern: URL path pattern (e.g., '/users/{user_id}')
        handler: Python callable that handles the request
        parameters: List of parameter definitions
        request_body_schema: Name of request body schema (if any)
        response_schema: Name of response schema (if any)
        summary: Short description for OpenAPI
        description: Detailed description for OpenAPI
        tags: List of tags for OpenAPI

    Returns:
        Route ID (0 on success, -1 on error)
    """
    global _route_registry
    # Declare C variables at the beginning
    cdef PyObject* handler_ptr
    cdef ParameterInfo param_info
    cdef RouteMetadata* metadata_ptr
    cdef int result

    if _route_registry == NULL:
        return -1

    # Heap-allocate RouteMetadata using new (Cython syntax)
    metadata_ptr = new RouteMetadata(
        method.encode('utf-8'),
        path_pattern.encode('utf-8')
    )

    try:
        # Set handler (increment refcount to keep Python object alive)
        handler_ptr = <PyObject*>handler
        Py_INCREF(handler)
        metadata_ptr.handler = handler_ptr

        # Set schemas
        if request_body_schema:
            metadata_ptr.request_body_schema = request_body_schema.encode('utf-8')
        if response_schema:
            metadata_ptr.response_schema = response_schema.encode('utf-8')

        # Set OpenAPI metadata
        if summary:
            metadata_ptr.summary = summary.encode('utf-8')
        if description:
            metadata_ptr.description = description.encode('utf-8')

        # Add tags
        if tags:
            for tag in tags:
                metadata_ptr.tags.push_back((<str>tag).encode('utf-8'))

        # Add parameters
        if parameters:
            for param_dict in parameters:
                param_info.name = param_dict.get('name', '').encode('utf-8')
                param_info.type = python_type_to_schema_type(param_dict.get('type', 'any'))
                param_info.location = python_loc_to_param_location(param_dict.get('location', 'query'))
                param_info.required = param_dict.get('required', True)
                param_info.default_value = str(param_dict.get('default', '')).encode('utf-8')
                param_info.description = param_dict.get('description', '').encode('utf-8')

                metadata_ptr.parameters.push_back(param_info)

        # Register the route (use move semantics to transfer ownership)
        result = _route_registry.register_route(move(metadata_ptr[0]))

        return result
    finally:
        # Clean up heap allocation
        del metadata_ptr


def get_all_routes() -> List[Dict[str, Any]]:
    """
    Get all registered routes as Python dictionaries.

    Returns:
        List of route dictionaries with metadata
    """
    global _route_registry
    cdef list result = []
    cdef size_t i, j
    cdef size_t num_routes
    cdef dict route_dict

    if _route_registry == NULL:
        return []

    # Get reference to routes vector and cache size
    num_routes = _route_registry.get_all_routes().size()

    for i in range(num_routes):
        # Access route directly from the vector each time (avoiding reference storage)
        route_dict = {
            'method': _route_registry.get_all_routes()[i].method.decode('utf-8'),
            'path_pattern': _route_registry.get_all_routes()[i].path_pattern.decode('utf-8'),
            'request_body_schema': _route_registry.get_all_routes()[i].request_body_schema.decode('utf-8'),
            'response_schema': _route_registry.get_all_routes()[i].response_schema.decode('utf-8'),
            'summary': _route_registry.get_all_routes()[i].summary.decode('utf-8'),
            'description': _route_registry.get_all_routes()[i].description.decode('utf-8'),
            'tags': [tag.decode('utf-8') for tag in _route_registry.get_all_routes()[i].tags],
            'parameters': []
        }

        # Convert parameters
        for j in range(_route_registry.get_all_routes()[i].parameters.size()):
            route_dict['parameters'].append({
                'name': _route_registry.get_all_routes()[i].parameters[j].name.decode('utf-8'),
                'location': _param_location_to_string(_route_registry.get_all_routes()[i].parameters[j].location),
                'required': _route_registry.get_all_routes()[i].parameters[j].required,
                'default_value': _route_registry.get_all_routes()[i].parameters[j].default_value.decode('utf-8'),
                'description': _route_registry.get_all_routes()[i].parameters[j].description.decode('utf-8')
            })

        result.append(route_dict)

    return result


cdef str _param_location_to_string(ParameterLocation loc):
    """Convert C++ ParameterLocation to Python string."""
    if loc == ParameterLocation.PATH:
        return 'path'
    elif loc == ParameterLocation.QUERY:
        return 'query'
    elif loc == ParameterLocation.BODY:
        return 'body'
    elif loc == ParameterLocation.HEADER:
        return 'header'
    elif loc == ParameterLocation.COOKIE:
        return 'cookie'
    else:
        return 'unknown'


# ============================================================================
# OpenAPI Generation
# ============================================================================

def generate_openapi(
    str title = 'FasterAPI',
    str version = '0.1.0',
    str description = ''
) -> str:
    """
    Generate OpenAPI 3.0 specification from registered routes.

    Args:
        title: API title
        version: API version
        description: API description

    Returns:
        OpenAPI JSON string
    """
    global _route_registry
    cdef string openapi_json

    if _route_registry == NULL:
        return '{}'

    # Pass the reference directly to generate without storing it
    openapi_json = OpenAPIGenerator.generate(
        _route_registry.get_all_routes(),
        title.encode('utf-8'),
        version.encode('utf-8'),
        description.encode('utf-8')
    )

    return openapi_json.decode('utf-8')


# ============================================================================
# Static Documentation Pages
# ============================================================================

def generate_swagger_ui_response(
    str openapi_url = '/openapi.json',
    str title = 'API Documentation'
) -> str:
    """
    Generate complete HTTP response with Swagger UI HTML.

    Args:
        openapi_url: URL to fetch OpenAPI spec from
        title: Page title

    Returns:
        Complete HTTP response string (headers + body)
    """
    cdef string response = StaticDocs.generate_swagger_ui_response(
        openapi_url.encode('utf-8'),
        title.encode('utf-8')
    )
    return response.decode('utf-8')


def generate_redoc_response(
    str openapi_url = '/openapi.json',
    str title = 'API Documentation'
) -> str:
    """
    Generate complete HTTP response with ReDoc HTML.

    Args:
        openapi_url: URL to fetch OpenAPI spec from
        title: Page title

    Returns:
        Complete HTTP response string (headers + body)
    """
    cdef string response = StaticDocs.generate_redoc_response(
        openapi_url.encode('utf-8'),
        title.encode('utf-8')
    )
    return response.decode('utf-8')


# ============================================================================
# Validation Error Formatting
# ============================================================================

def format_validation_errors_as_json(list errors) -> str:
    """
    Format validation errors as FastAPI-compatible JSON.

    Args:
        errors: List of error dicts with 'loc', 'msg', 'type' fields

    Returns:
        JSON string in FastAPI 422 format
    """
    cdef ValidationResult result
    result.valid = False

    cdef ValidationError err
    for error_dict in errors:
        err.loc.clear()
        for loc_part in error_dict.get('loc', []):
            err.loc.push_back(str(loc_part).encode('utf-8'))

        err.msg = error_dict.get('msg', '').encode('utf-8')
        err.type = error_dict.get('type', '').encode('utf-8')

        result.errors.push_back(err)

    cdef string json_str = ValidationErrorFormatter.format_as_json(result)
    return json_str.decode('utf-8')


def format_validation_errors_as_http_response(list errors) -> str:
    """
    Format validation errors as complete HTTP 422 response.

    Args:
        errors: List of error dicts with 'loc', 'msg', 'type' fields

    Returns:
        Complete HTTP response string (headers + body)
    """
    cdef ValidationResult result
    result.valid = False

    cdef ValidationError err
    for error_dict in errors:
        err.loc.clear()
        for loc_part in error_dict.get('loc', []):
            err.loc.push_back(str(loc_part).encode('utf-8'))

        err.msg = error_dict.get('msg', '').encode('utf-8')
        err.type = error_dict.get('type', '').encode('utf-8')

        result.errors.push_back(err)

    cdef string response = ValidationErrorFormatter.format_as_http_response(result)
    return response.decode('utf-8')


# ============================================================================
# Module Info
# ============================================================================

def get_version() -> str:
    """Get FasterAPI native module version."""
    return "0.1.0"


# ============================================================================
# RouteRegistry Access for C API
# ============================================================================

cdef public void* get_route_registry_ptr():
    """
    Get the global RouteRegistry pointer for C API access.

    This allows the C HTTP server to access the RouteRegistry
    for metadata-aware parameter extraction.

    Returns:
        Pointer to global RouteRegistry instance (as void*)
    """
    global _route_registry
    return <void*>_route_registry


def get_registry_ptr():
    """
    Get the RouteRegistry pointer as an integer for Python ctypes access.

    Returns:
        int: RouteRegistry pointer as integer, or 0 if not initialized
    """
    global _route_registry
    if _route_registry == NULL:
        return 0
    return <uintptr_t><void*>_route_registry


# Forward declare C function from http_server_c_api
cdef extern from "http/http_server_c_api.h":
    int http_connect_route_registry(void* registry_ptr)


def connect_route_registry_to_server():
    """
    Connect the global RouteRegistry to the HTTP server's PythonCallbackBridge.

    This enables metadata-aware parameter extraction for FastAPI routes.
    Must be called after routes are registered and before starting the server.

    Returns:
        0 on success, non-zero on error
    """
    global _route_registry
    if _route_registry == NULL:
        print("Warning: RouteRegistry not initialized")
        return -1

    cdef void* registry_ptr = <void*>_route_registry
    result = http_connect_route_registry(registry_ptr)
    return result


# ============================================================================
# Parameter Extraction Utilities
# ============================================================================

def extract_path_params(str pattern):
    """
    Extract path parameter names from a route pattern.

    Args:
        pattern: Route pattern like "/users/{user_id}/posts/{post_id}"

    Returns:
        List of parameter names: ["user_id", "post_id"]

    Example:
        >>> extract_path_params("/users/{user_id}")
        ["user_id"]
        >>> extract_path_params("/api/{version}/items/{item_id}")
        ["version", "item_id"]
    """
    cdef vector[string] params = ParameterExtractor.extract_path_params(pattern.encode('utf-8'))
    return [p.decode('utf-8') for p in params]


def parse_query_params(str url):
    """
    Parse query parameters from a URL.

    Args:
        url: URL with query string like "/search?q=test&limit=10"

    Returns:
        Dictionary of query parameters: {"q": "test", "limit": "10"}

    Example:
        >>> parse_query_params("/search?q=hello&limit=50")
        {"q": "hello", "limit": "50"}
        >>> parse_query_params("/api/users?active=true")
        {"active": "true"}
    """
    cdef unordered_map[string, string] params = ParameterExtractor.get_query_params(url.encode('utf-8'))
    return {k.decode('utf-8'): v.decode('utf-8') for k, v in params}


def url_decode(str encoded):
    """
    URL-decode a string.

    Args:
        encoded: URL-encoded string like "hello%20world"

    Returns:
        Decoded string: "hello world"

    Example:
        >>> url_decode("hello%20world")
        "hello world"
        >>> url_decode("user%40example.com")
        "user@example.com"
    """
    cdef string decoded = ParameterExtractor.url_decode(encoded.encode('utf-8'))
    return decoded.decode('utf-8')
