# FastAPI Compatibility Implementation Plan

## Overview
Building a FastAPI-compatible API for FasterAPI with **all performance-critical code in C++** and minimal Python wrapper layer.

## Architecture Principle
- ✅ **C++ Hot Path**: Parameter extraction, validation, routing, OpenAPI generation
- ✅ **Python Cold Path**: Decorator syntax, function introspection at registration time only
- ✅ **Performance Target**: 10x faster than FastAPI for simple routes

---

## Phase 1: Core C++ Infrastructure ✅ COMPLETED

### 1.1 ParameterExtractor ✅ DONE
**Files:** `src/cpp/http/parameter_extractor.{h,cpp}`

**Features:**
- [x] URL path parsing (`/users/{user_id}` → extract `user_id`)
- [x] Query parameter extraction (`?q=search&limit=10`)
- [x] URL decoding (percent encoding)
- [x] Zero-copy string operations where possible
- [x] CompiledRoutePattern for fast matching

**Performance:** < 100ns per parameter extraction

### 1.2 SchemaValidator ✅ DONE
**Files:** `src/cpp/http/schema_validator.{h,cpp}`

**Features:**
- [x] JSON schema validation using simdjson
- [x] Type validation (string, int, float, bool, array, object)
- [x] FastAPI-compatible error format (422 responses)
- [x] SchemaRegistry for storing Pydantic model schemas
- [x] SchemaBuilder fluent API
- [x] No exceptions (compatible with `-fno-exceptions`)

**Performance:** 100ns - 1μs per validation

---

## Phase 2: Route Metadata & Registry (IN PROGRESS)

### 2.1 RouteMetadata ⏳ IN PROGRESS
**Files:** `src/cpp/http/route_metadata.{h,cpp}`

**Features:**
- [ ] Store route pattern, method, handler
- [ ] Parameter definitions (path, query, body)
- [ ] Request/response schema references
- [ ] Python handler callable (PyObject*)
- [ ] Metadata for OpenAPI generation

**Data Structure:**
```cpp
struct ParameterInfo {
    std::string name;
    SchemaType type;
    ParameterLocation location;  // PATH, QUERY, BODY, HEADER
    bool required;
    std::string default_value;
};

struct RouteMetadata {
    std::string method;
    CompiledRoutePattern pattern;
    std::vector<ParameterInfo> parameters;
    std::string request_body_schema;
    std::string response_schema;
    PyObject* handler;  // Python callable
    std::string summary;  // For OpenAPI
    std::string description;
};
```

### 2.2 Enhanced Router
**Updates to:** `src/cpp/http/router.{h,cpp}`

**Features:**
- [ ] Store RouteMetadata instead of just handler
- [ ] Fast route matching with metadata
- [ ] Extract path parameters during routing
- [ ] Return matched route with metadata

---

## Phase 3: Python Bindings

### 3.1 C++ → Python Bindings ⏳ NEXT
**Files:** `src/cpp/http/fastapi_bindings.cpp` (new)

**Expose to Python:**
- [ ] `ParameterExtractor.extract_path_params(pattern)`
- [ ] `ParameterExtractor.get_query_params(url)`
- [ ] `SchemaRegistry.register_schema(name, schema_dict)`
- [ ] `SchemaValidator.validate_json(schema_name, json_str)`
- [ ] `Router.add_route_with_metadata(metadata)`

**Python API:**
```python
from fasterapi._native import (
    extract_path_params,
    register_schema,
    validate_json,
    add_route_metadata
)
```

---

## Phase 4: Python Decorator Layer

### 4.1 Function Signature Extraction 📝 TODO
**Files:** `fasterapi/fastapi_compat.py` (new)

**Features:**
- [ ] Extract function signature using `inspect.signature()`
- [ ] Identify path parameters from route pattern
- [ ] Identify query parameters from function args
- [ ] Identify request body (Pydantic model parameter)
- [ ] Extract return type annotation
- [ ] Build ParameterInfo list

**Example:**
```python
@app.get("/users/{user_id}")
async def get_user(user_id: int, q: str = None) -> User:
    ...

# Extracts:
# - Path param: user_id (int, required)
# - Query param: q (str, optional)
# - Response model: User
```

### 4.2 Pydantic Schema Extraction 📝 TODO
**Files:** `fasterapi/schema_extractor.py` (new)

**Features:**
- [ ] Extract schema from Pydantic models
- [ ] Convert to C++ schema format
- [ ] Handle nested models
- [ ] Handle List/Optional types
- [ ] Register with C++ SchemaRegistry

**Conversion:**
```python
class User(BaseModel):
    id: int
    name: str
    email: Optional[str] = None

# Converts to:
{
    "name": "User",
    "fields": [
        {"name": "id", "type": "integer", "required": True},
        {"name": "name", "type": "string", "required": True},
        {"name": "email", "type": "string", "required": False, "default": None}
    ]
}
```

### 4.3 FastAPI-Compatible Decorator 📝 TODO
**Files:** `fasterapi/app.py` (update existing)

**Features:**
- [ ] Enhanced `@app.get()` decorator
- [ ] Extract function signature
- [ ] Extract Pydantic schemas
- [ ] Register schemas with C++
- [ ] Create RouteMetadata
- [ ] Register route with C++ router
- [ ] Wrap handler for automatic parameter injection

**Decorator Flow:**
```python
@app.get("/users/{user_id}", response_model=User)
async def get_user(user_id: int, q: str = None) -> User:
    ...

# Decorator does (at registration time):
# 1. Extract signature → [user_id: int (path), q: str (query)]
# 2. Extract User schema → register as "User" in C++
# 3. Create RouteMetadata
# 4. Register with C++ router
# 5. Create wrapper that:
#    - Extracts params in C++
#    - Validates in C++
#    - Calls handler with typed params
#    - Validates response in C++
```

---

## Phase 5: Request Handling Integration

### 5.1 Enhanced Request Handler 🔧 TODO
**Files:** `src/cpp/http/server.cpp` (update existing)

**Request Flow (ALL IN C++):**
```cpp
void handle_request(Request& req, Response& res) {
    // 1. Route matching (existing)
    auto route = router->match(req.method, req.path);

    // 2. Extract path parameters (NEW)
    auto path_params = route.pattern.extract(req.path);

    // 3. Extract query parameters (NEW)
    auto query_params = ParameterExtractor::get_query_params(req.url);

    // 4. Validate request body if needed (NEW)
    if (!route.request_body_schema.empty()) {
        auto schema = SchemaRegistry::get_schema(route.request_body_schema);
        auto validation = schema->validate_json(req.body);
        if (!validation.valid) {
            return send_422_error(res, validation.errors);
        }
    }

    // 5. Call Python handler with validated params
    call_python_handler(route.handler, path_params, query_params, req.body);

    // 6. Validate response if needed (NEW)
    if (!route.response_schema.empty()) {
        auto schema = SchemaRegistry::get_schema(route.response_schema);
        auto validation = schema->validate_json(response_body);
        if (!validation.valid) {
            LOG_ERROR("Response validation failed");
        }
    }

    // 7. Send response
    res.send();
}
```

### 5.2 422 Validation Error Formatting 📝 TODO
**Files:** `src/cpp/http/validation_error_formatter.{h,cpp}` (new)

**Features:**
- [ ] Format ValidationResult as FastAPI 422 response
- [ ] JSON error format matching FastAPI

**Example Output:**
```json
{
  "detail": [
    {
      "loc": ["body", "user", "age"],
      "msg": "value is not a valid integer",
      "type": "type_error.integer"
    }
  ]
}
```

---

## Phase 6: OpenAPI Generation

### 6.1 OpenAPI Schema Generator 📝 TODO
**Files:** `src/cpp/http/openapi_generator.{h,cpp}` (new)

**Features:**
- [ ] Generate OpenAPI 3.0 schema from registered routes
- [ ] Extract path parameters
- [ ] Extract query parameters
- [ ] Extract request body schemas
- [ ] Extract response schemas
- [ ] Include route summaries/descriptions
- [ ] Return JSON string

**C++ Implementation:**
```cpp
class OpenAPIGenerator {
public:
    static std::string generate(const std::vector<RouteMetadata>& routes);

private:
    static std::string generate_paths(const std::vector<RouteMetadata>& routes);
    static std::string generate_components(const SchemaRegistry& registry);
    static std::string generate_path_item(const RouteMetadata& route);
};
```

### 6.2 OpenAPI Endpoint 📝 TODO
**Files:** `fasterapi/app.py` (update)

**Features:**
- [ ] Add `GET /openapi.json` endpoint
- [ ] Call C++ OpenAPIGenerator
- [ ] Return generated schema

---

## Phase 7: Interactive Documentation

### 7.1 Static File Server 📝 TODO
**Files:** `src/cpp/http/static_server.{h,cpp}` (new)

**Features:**
- [ ] Serve embedded Swagger UI HTML
- [ ] Serve embedded ReDoc HTML
- [ ] Minimal HTML with CDN links (no file I/O)
- [ ] Configure to use `/openapi.json`

**Endpoints:**
- [ ] `GET /docs` → Swagger UI
- [ ] `GET /redoc` → ReDoc

**HTML Templates (embedded as C++ string literals):**
```cpp
static const char* SWAGGER_UI_HTML = R"(
<!DOCTYPE html>
<html>
<head>
    <title>API Documentation</title>
    <link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/swagger-ui-dist@5/swagger-ui.css">
</head>
<body>
    <div id="swagger-ui"></div>
    <script src="https://cdn.jsdelivr.net/npm/swagger-ui-dist@5/swagger-ui-bundle.js"></script>
    <script>
        SwaggerUIBundle({
            url: '/openapi.json',
            dom_id: '#swagger-ui'
        });
    </script>
</body>
</html>
)";
```

---

## Phase 8: Testing & Examples

### 8.1 FastAPI-Compatible Example 📝 TODO
**Files:** `examples/fastapi_example.py` (new)

**Example:**
```python
from fasterapi import FastAPI  # Alias for App
from pydantic import BaseModel

app = FastAPI()

class Item(BaseModel):
    name: str
    price: float
    tags: List[str] = []

@app.get("/")
def read_root():
    return {"message": "Hello World"}

@app.get("/items/{item_id}")
def read_item(item_id: int, q: str = None) -> Item:
    return Item(name=f"Item {item_id}", price=99.99)

@app.post("/items/")
def create_item(item: Item) -> Item:
    return item

if __name__ == "__main__":
    app.run()
```

### 8.2 Integration Tests 📝 TODO
**Files:** `tests/test_fastapi_compat.py` (new)

**Test Cases:**
- [ ] Path parameter extraction
- [ ] Query parameter extraction
- [ ] Request body validation
- [ ] Response validation
- [ ] 422 error formatting
- [ ] OpenAPI schema generation
- [ ] `/docs` endpoint
- [ ] `/redoc` endpoint

---

## Implementation Checklist

### Phase 1: Core C++ Infrastructure ✅
- [x] ParameterExtractor
- [x] SchemaValidator
- [x] Build and test

### Phase 2: Route Metadata ⏳
- [ ] Create RouteMetadata structures
- [ ] Update Router to store metadata
- [ ] Test metadata storage

### Phase 3: Python Bindings
- [ ] Create Python bindings for ParameterExtractor
- [ ] Create Python bindings for SchemaValidator
- [ ] Create Python bindings for Router
- [ ] Test bindings from Python

### Phase 4: Python Layer
- [ ] Function signature extraction
- [ ] Pydantic schema extraction
- [ ] Enhanced decorators
- [ ] Test decorator functionality

### Phase 5: Integration
- [ ] Update request handler
- [ ] Add parameter injection
- [ ] Add validation
- [ ] Add 422 error formatting
- [ ] Test end-to-end

### Phase 6: OpenAPI
- [ ] OpenAPI generator in C++
- [ ] OpenAPI endpoint
- [ ] Test schema generation

### Phase 7: Documentation
- [ ] Static file server
- [ ] Swagger UI endpoint
- [ ] ReDoc endpoint
- [ ] Test documentation pages

### Phase 8: Testing
- [ ] Create FastAPI example
- [ ] Integration tests
- [ ] Performance benchmarks
- [ ] Documentation

---

## Success Criteria

1. **API Compatibility**: FastAPI examples run with minimal changes
2. **Performance**: 10x faster than FastAPI for simple routes
3. **Documentation**: Automatic Swagger UI and ReDoc
4. **Validation**: Pydantic-like validation in C++
5. **Developer UX**: Same decorator syntax as FastAPI

---

## Current Status

**Completed:**
- ✅ ParameterExtractor (C++)
- ✅ SchemaValidator (C++)

**In Progress:**
- ⏳ RouteMetadata (starting next)

**Next Steps:**
1. Create RouteMetadata structures
2. Add Python bindings
3. Build decorator layer
4. Integrate into request flow
5. Add OpenAPI generation
6. Add documentation endpoints
