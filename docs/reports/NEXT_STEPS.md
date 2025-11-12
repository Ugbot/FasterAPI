# Next Steps: Python Integration & Testing

## Overview

Phase 1-2 (C++ infrastructure) is complete. The remaining work focuses on creating the Python layer that connects FastAPI-compatible decorators to the high-performance C++ backend.

**Architecture Principle**: Python code runs ONLY at route registration time (cold path). All request handling happens in C++ (hot path).

---

## Phase 3: Python Bindings for C++ Components

**Goal**: Expose C++ functionality to Python using pybind11 or similar.

### 3.1 Binding Strategy

**File**: `src/cpp/bindings/fastapi_bindings.cpp`

**Approach**:
- Use existing pybind11 infrastructure (already in project)
- Create Python module `fasterapi._fastapi_native`
- Expose minimal API surface (only what Python decorators need)

### 3.2 Required Bindings

#### RouteRegistry Bindings
```cpp
// Expose RouteRegistry to Python
py::class_<RouteRegistry>(m, "RouteRegistry")
    .def(py::init<>())
    .def("register_route", [](RouteRegistry& self,
                              const std::string& method,
                              const std::string& path,
                              py::object handler,
                              const std::vector<std::tuple<std::string, std::string, std::string>>& params,
                              const std::string& request_schema,
                              const std::string& response_schema,
                              const std::string& summary) {
        // Build RouteMetadata from Python arguments
        RouteMetadata metadata(method, path);

        // Convert Python parameter tuples to ParameterInfo
        for (const auto& [name, type_str, location_str] : params) {
            ParameterInfo param;
            param.name = name;
            param.type = string_to_schema_type(type_str);
            param.location = string_to_param_location(location_str);
            metadata.parameters.push_back(param);
        }

        metadata.request_body_schema = request_schema;
        metadata.response_schema = response_schema;
        metadata.summary = summary;

        // IMPORTANT: Increment refcount since C++ will hold this
        metadata.handler = handler.ptr();
        Py_INCREF(metadata.handler);

        return self.register_route(std::move(metadata));
    })
    .def("get_all_routes", &RouteRegistry::get_all_routes,
         py::return_value_policy::reference_internal);
```

#### SchemaRegistry Bindings
```cpp
// Expose SchemaRegistry to Python
py::class_<SchemaRegistry>(m, "SchemaRegistry")
    .def_static("instance", &SchemaRegistry::instance,
                py::return_value_policy::reference)
    .def("register_schema", [](SchemaRegistry& self,
                               const std::string& name,
                               const std::vector<std::tuple<std::string, std::string, bool>>& fields) {
        auto schema = std::make_shared<Schema>(name);

        for (const auto& [field_name, type_str, required] : fields) {
            SchemaType type = string_to_schema_type(type_str);
            schema->add_field(field_name, type, required);
        }

        self.register_schema(name, schema);
    })
    .def("has_schema", &SchemaRegistry::has_schema);
```

#### OpenAPI Generation Bindings
```cpp
// Expose OpenAPIGenerator
m.def("generate_openapi", [](const RouteRegistry& registry,
                             const std::string& title,
                             const std::string& version,
                             const std::string& description) {
    return OpenAPIGenerator::generate(
        registry.get_all_routes(),
        title,
        version,
        description
    );
}, py::arg("registry"),
   py::arg("title") = "FasterAPI",
   py::arg("version") = "0.1.0",
   py::arg("description") = "");
```

#### Static Docs Bindings
```cpp
// Expose StaticDocs
m.def("generate_swagger_ui_response", &StaticDocs::generate_swagger_ui_response,
      py::arg("openapi_url") = "/openapi.json",
      py::arg("title") = "API Documentation");

m.def("generate_redoc_response", &StaticDocs::generate_redoc_response,
      py::arg("openapi_url") = "/openapi.json",
      py::arg("title") = "API Documentation");
```

#### ValidationErrorFormatter Bindings
```cpp
// Expose ValidationErrorFormatter
py::class_<ValidationResult>(m, "ValidationResult")
    .def_readonly("valid", &ValidationResult::valid)
    .def_readonly("errors", &ValidationResult::errors);

m.def("format_validation_error", &ValidationErrorFormatter::format_as_http_response,
      "Format validation errors as FastAPI 422 response");
```

### 3.3 Helper Functions

```cpp
SchemaType string_to_schema_type(const std::string& type_str) {
    if (type_str == "str" || type_str == "string") return SchemaType::STRING;
    if (type_str == "int" || type_str == "integer") return SchemaType::INTEGER;
    if (type_str == "float" || type_str == "number") return SchemaType::FLOAT;
    if (type_str == "bool" || type_str == "boolean") return SchemaType::BOOLEAN;
    if (type_str == "list" || type_str == "array") return SchemaType::ARRAY;
    if (type_str == "dict" || type_str == "object") return SchemaType::OBJECT;
    return SchemaType::ANY;
}

ParameterLocation string_to_param_location(const std::string& loc_str) {
    if (loc_str == "path") return ParameterLocation::PATH;
    if (loc_str == "query") return ParameterLocation::QUERY;
    if (loc_str == "body") return ParameterLocation::BODY;
    if (loc_str == "header") return ParameterLocation::HEADER;
    if (loc_str == "cookie") return ParameterLocation::COOKIE;
    return ParameterLocation::QUERY;
}
```

---

## Phase 4: Python Schema Extraction from Pydantic

**Goal**: Convert Pydantic models to C++ schemas at route registration time.

### 4.1 Schema Extractor

**File**: `fasterapi/schema_extractor.py`

```python
from typing import Type, get_type_hints, get_origin, get_args
from pydantic import BaseModel
import inspect

def extract_pydantic_schema(model: Type[BaseModel]) -> dict:
    """
    Extract schema from Pydantic model for C++ registration.

    Returns:
        {
            "name": str,
            "fields": [
                (field_name: str, type_str: str, required: bool),
                ...
            ]
        }
    """
    schema_name = model.__name__
    fields = []

    # Get Pydantic fields
    for field_name, field_info in model.model_fields.items():
        # Get field type
        field_type = field_info.annotation
        type_str = python_type_to_str(field_type)

        # Check if required
        required = field_info.is_required()

        fields.append((field_name, type_str, required))

    return {
        "name": schema_name,
        "fields": fields
    }

def python_type_to_str(py_type) -> str:
    """
    Convert Python type to string for C++ binding.

    Examples:
        str -> "string"
        int -> "integer"
        float -> "number"
        bool -> "boolean"
        list[str] -> "array"
        dict -> "object"
        Optional[str] -> "string"
    """
    # Handle Optional (Union with None)
    origin = get_origin(py_type)
    if origin is Union:
        args = get_args(py_type)
        # Filter out NoneType
        non_none_types = [arg for arg in args if arg is not type(None)]
        if len(non_none_types) == 1:
            py_type = non_none_types[0]
            origin = get_origin(py_type)

    # Handle List/list
    if origin is list or py_type is list:
        return "array"

    # Handle Dict/dict
    if origin is dict or py_type is dict:
        return "object"

    # Basic types
    type_map = {
        str: "string",
        int: "integer",
        float: "number",
        bool: "boolean",
    }

    return type_map.get(py_type, "string")

def register_schema_with_cpp(model: Type[BaseModel], registry):
    """Register Pydantic model with C++ SchemaRegistry."""
    schema_dict = extract_pydantic_schema(model)

    # Register with C++ (through bindings)
    registry.register_schema(
        schema_dict["name"],
        schema_dict["fields"]
    )
```

### 4.2 Type Annotation Parser

**File**: `fasterapi/type_parser.py`

```python
from typing import get_type_hints, get_origin, get_args, Union
from pydantic import BaseModel
import inspect

def extract_function_parameters(func):
    """
    Extract parameter information from function signature.

    Returns:
        {
            "path_params": [(name, type_str), ...],
            "query_params": [(name, type_str, required, default), ...],
            "body_param": (name, model_class) or None,
            "return_type": model_class or None
        }
    """
    sig = inspect.signature(func)
    hints = get_type_hints(func)

    path_params = []
    query_params = []
    body_param = None
    return_type = None

    # Parse parameters
    for param_name, param in sig.parameters.items():
        # Skip self/cls
        if param_name in ('self', 'cls'):
            continue

        # Get type hint
        param_type = hints.get(param_name, str)

        # Check if it's a Pydantic model (body parameter)
        if inspect.isclass(param_type) and issubclass(param_type, BaseModel):
            body_param = (param_name, param_type)
        else:
            # It's a path or query parameter
            type_str = python_type_to_str(param_type)
            required = param.default == inspect.Parameter.empty
            default = None if required else param.default

            # We'll determine path vs query based on route pattern later
            query_params.append((param_name, type_str, required, default))

    # Parse return type
    if 'return' in hints:
        ret_type = hints['return']
        if inspect.isclass(ret_type) and issubclass(ret_type, BaseModel):
            return_type = ret_type

    return {
        "path_params": path_params,
        "query_params": query_params,
        "body_param": body_param,
        "return_type": return_type
    }

def identify_path_params(path_pattern: str, all_params: list) -> tuple:
    """
    Identify which parameters are path parameters based on route pattern.

    Example:
        path_pattern = "/users/{user_id}/posts/{post_id}"
        all_params = [("user_id", "integer", True, None), ("q", "string", False, "")]

        Returns:
            path_params = [("user_id", "integer"), ("post_id", "integer")]
            query_params = [("q", "string", False, "")]
    """
    import re

    # Extract {param} from path
    path_param_names = re.findall(r'\{(\w+)\}', path_pattern)

    path_params = []
    query_params = []

    for param_name, type_str, required, default in all_params:
        if param_name in path_param_names:
            path_params.append((param_name, type_str))
        else:
            query_params.append((param_name, type_str, required, default))

    return path_params, query_params
```

---

## Phase 5: FastAPI-Compatible Decorators

**Goal**: Create `@app.get()`, `@app.post()`, etc. that work like FastAPI.

### 5.1 Enhanced App Class

**File**: `fasterapi/fastapi_app.py`

```python
from fasterapi._fastapi_native import RouteRegistry, SchemaRegistry, generate_openapi
from fasterapi.schema_extractor import register_schema_with_cpp, extract_pydantic_schema
from fasterapi.type_parser import extract_function_parameters, identify_path_params
from typing import Optional, Type, Callable
from pydantic import BaseModel

class FastAPI:
    """
    FastAPI-compatible application class.

    Differences from regular App:
    - Automatic parameter extraction from type hints
    - Pydantic model validation
    - OpenAPI schema generation
    - Interactive docs (/docs, /redoc)
    """

    def __init__(
        self,
        title: str = "FastAPI",
        version: str = "0.1.0",
        description: str = "",
        **kwargs
    ):
        # Initialize C++ route registry
        self._route_registry = RouteRegistry()
        self._schema_registry = SchemaRegistry.instance()

        # Store OpenAPI metadata
        self.title = title
        self.version = version
        self.description = description

        # Register built-in routes
        self._register_docs_routes()

    def get(self, path: str, **decorator_kwargs):
        """FastAPI-compatible GET decorator."""
        return self._create_route_decorator("GET", path, **decorator_kwargs)

    def post(self, path: str, **decorator_kwargs):
        """FastAPI-compatible POST decorator."""
        return self._create_route_decorator("POST", path, **decorator_kwargs)

    def put(self, path: str, **decorator_kwargs):
        """FastAPI-compatible PUT decorator."""
        return self._create_route_decorator("PUT", path, **decorator_kwargs)

    def delete(self, path: str, **decorator_kwargs):
        """FastAPI-compatible DELETE decorator."""
        return self._create_route_decorator("DELETE", path, **decorator_kwargs)

    def _create_route_decorator(
        self,
        method: str,
        path: str,
        response_model: Optional[Type[BaseModel]] = None,
        summary: str = "",
        description: str = "",
        tags: list[str] = None,
        **kwargs
    ):
        """
        Create a route decorator that extracts parameters and registers with C++.

        This runs at REGISTRATION TIME (cold path), not request time.
        """
        def decorator(func: Callable):
            # Extract function signature
            param_info = extract_function_parameters(func)

            # Identify path vs query parameters
            path_params, query_params = identify_path_params(
                path,
                param_info["query_params"]
            )

            # Register body schema if present
            request_schema_name = ""
            if param_info["body_param"]:
                body_param_name, body_model = param_info["body_param"]
                request_schema_name = body_model.__name__
                register_schema_with_cpp(body_model, self._schema_registry)

            # Register response schema if present
            response_schema_name = ""
            if response_model:
                response_schema_name = response_model.__name__
                register_schema_with_cpp(response_model, self._schema_registry)
            elif param_info["return_type"]:
                response_schema_name = param_info["return_type"].__name__
                register_schema_with_cpp(param_info["return_type"], self._schema_registry)

            # Build parameter list for C++
            cpp_params = []

            # Add path parameters
            for name, type_str in path_params:
                cpp_params.append((name, type_str, "path"))

            # Add query parameters
            for name, type_str, required, default in query_params:
                cpp_params.append((name, type_str, "query"))

            # Create wrapper that will be called by C++ request handler
            def wrapped_handler(request, response):
                """
                This is called from C++ during request handling.

                C++ has already:
                - Extracted and validated path/query parameters
                - Validated request body against schema

                We just need to:
                - Parse validated JSON body to Pydantic model
                - Call user function with typed parameters
                - Serialize response
                """
                # TODO: Get validated params from request
                # For now, call original function
                result = func()

                # Serialize response
                if isinstance(result, BaseModel):
                    response.json(result.model_dump())
                elif isinstance(result, dict):
                    response.json(result)
                else:
                    response.send(str(result))

            # Register route with C++
            self._route_registry.register_route(
                method=method,
                path=path,
                handler=wrapped_handler,
                params=cpp_params,
                request_schema=request_schema_name,
                response_schema=response_schema_name,
                summary=summary or func.__doc__ or ""
            )

            return func

        return decorator

    def _register_docs_routes(self):
        """Register /docs, /redoc, and /openapi.json routes."""
        from fasterapi._fastapi_native import (
            generate_swagger_ui_response,
            generate_redoc_response
        )

        # /openapi.json endpoint
        def openapi_handler(request, response):
            spec = generate_openapi(
                self._route_registry,
                self.title,
                self.version,
                self.description
            )
            response.set_header("Content-Type", "application/json")
            response.send(spec)

        # /docs endpoint (Swagger UI)
        def docs_handler(request, response):
            html = generate_swagger_ui_response("/openapi.json", self.title)
            response.send(html)

        # /redoc endpoint
        def redoc_handler(request, response):
            html = generate_redoc_response("/openapi.json", self.title)
            response.send(html)

        # Register these routes
        # Note: These bypass the normal decorator registration
        # since they don't need parameter extraction
        self._route_registry.register_route(
            method="GET",
            path="/openapi.json",
            handler=openapi_handler,
            params=[],
            request_schema="",
            response_schema="",
            summary="OpenAPI schema"
        )
        # Similar for /docs and /redoc...
```

---

## Phase 6: Request Handler Integration

**Goal**: Modify C++ request handler to use RouteMetadata for validation.

### 6.1 Enhanced Request Flow

**File**: `src/cpp/http/http1_handler.cpp` (or wherever request handling is)

```cpp
void handle_fastapi_request(Request& req, Response& res) {
    // 1. Route matching with metadata
    auto route = route_registry->match(req.method, req.path);
    if (!route) {
        res.status(404).send("Not Found");
        return;
    }

    // 2. Extract path parameters (NEW)
    auto path_params = route->compiled_pattern.extract(req.path);

    // 3. Extract query parameters (NEW)
    auto query_params = ParameterExtractor::get_query_params(req.url);

    // 4. Validate request body if schema specified (NEW)
    if (!route->request_body_schema.empty()) {
        auto schema = SchemaRegistry::instance().get_schema(route->request_body_schema);
        if (schema) {
            auto validation = schema->validate_json(req.body);
            if (!validation.valid) {
                std::string error_response =
                    ValidationErrorFormatter::format_as_http_response(validation);
                res.send_raw(error_response);
                return;
            }
        }
    }

    // 5. Store validated params in request for Python handler
    // TODO: Add params to Request object

    // 6. Call Python handler
    if (route->handler) {
        PyGILState_STATE gstate = PyGILState_Ensure();

        // Create Python request/response objects
        // Call handler(request, response)
        PyObject* result = PyObject_CallFunctionObjArgs(
            route->handler,
            py_request,
            py_response,
            NULL
        );

        if (!result) {
            PyErr_Print();
            res.status(500).send("Internal Server Error");
        } else {
            Py_DECREF(result);
        }

        PyGILState_Release(gstate);
    }
}
```

### 6.2 Request Object Enhancement

**Add to Request class**:
```cpp
class Request {
    // ... existing fields ...

    // NEW: Validated parameters
    std::unordered_map<std::string, std::string> path_params;
    std::unordered_map<std::string, std::string> query_params;
    std::string validated_body_json;  // Already validated by C++
};
```

---

## Phase 7: Example Application & Testing

### 7.1 Simple Example

**File**: `examples/fastapi_hello.py`

```python
from fasterapi import FastAPI
from pydantic import BaseModel
from typing import Optional

app = FastAPI(title="Hello FastAPI", version="1.0.0")

class Item(BaseModel):
    name: str
    price: float
    description: Optional[str] = None

@app.get("/")
def read_root():
    return {"message": "Hello World"}

@app.get("/items/{item_id}")
def read_item(item_id: int, q: Optional[str] = None):
    return {"item_id": item_id, "q": q}

@app.post("/items/")
def create_item(item: Item) -> Item:
    return item

if __name__ == "__main__":
    app.run(port=8000)
```

### 7.2 Test Suite

**File**: `tests/test_fastapi_compat.py`

```python
import pytest
from fasterapi import FastAPI
from pydantic import BaseModel

def test_path_param_extraction():
    """Test that path parameters are extracted correctly."""
    app = FastAPI()

    @app.get("/users/{user_id}")
    def get_user(user_id: int):
        return {"user_id": user_id}

    # Test with client
    response = client.get("/users/123")
    assert response.status_code == 200
    assert response.json() == {"user_id": 123}

def test_query_param_extraction():
    """Test that query parameters are extracted correctly."""
    app = FastAPI()

    @app.get("/search")
    def search(q: str, limit: int = 10):
        return {"q": q, "limit": limit}

    response = client.get("/search?q=test&limit=20")
    assert response.json() == {"q": "test", "limit": 20}

def test_request_body_validation():
    """Test that request body is validated against Pydantic model."""
    app = FastAPI()

    class Item(BaseModel):
        name: str
        price: float

    @app.post("/items/")
    def create_item(item: Item) -> Item:
        return item

    # Valid request
    response = client.post("/items/", json={"name": "Widget", "price": 9.99})
    assert response.status_code == 200

    # Invalid request (missing field)
    response = client.post("/items/", json={"name": "Widget"})
    assert response.status_code == 422

def test_openapi_generation():
    """Test that OpenAPI spec is generated correctly."""
    app = FastAPI(title="Test API", version="1.0.0")

    @app.get("/test")
    def test_endpoint():
        return {"status": "ok"}

    response = client.get("/openapi.json")
    assert response.status_code == 200

    spec = response.json()
    assert spec["openapi"] == "3.0.0"
    assert spec["info"]["title"] == "Test API"
    assert "/test" in spec["paths"]

def test_docs_endpoints():
    """Test that /docs and /redoc endpoints work."""
    app = FastAPI()

    response = client.get("/docs")
    assert response.status_code == 200
    assert "swagger" in response.text.lower()

    response = client.get("/redoc")
    assert response.status_code == 200
    assert "redoc" in response.text.lower()
```

---

## Implementation Order

1. **Start with bindings** - Get C++ talking to Python
2. **Test bindings** - Ensure RouteRegistry works from Python
3. **Build schema extractor** - Convert Pydantic to C++ schemas
4. **Implement decorators** - Create FastAPI-compatible API
5. **Integrate request handler** - Wire up validation in C++
6. **Create example** - Build working FastAPI app
7. **Write tests** - Comprehensive test coverage

---

## Success Criteria

- [ ] `@app.get("/users/{id}")` extracts path parameter
- [ ] `def func(q: str = None)` extracts optional query parameter
- [ ] `def func(item: Item)` validates request body with Pydantic
- [ ] `-> Item` validates response body
- [ ] `/openapi.json` returns valid OpenAPI 3.0 spec
- [ ] `/docs` displays Swagger UI
- [ ] `/redoc` displays ReDoc
- [ ] 422 errors match FastAPI format
- [ ] Performance: 10x faster than FastAPI for simple routes

---

## Performance Targets

- Parameter extraction: < 100ns
- Schema validation: < 1μs
- OpenAPI generation: < 1ms (cold, cached after first call)
- Request handling: < 10μs (excluding Python handler execution)
- **Total overhead vs raw C++**: < 2μs

The key is that ALL expensive operations happen in C++, not Python.
