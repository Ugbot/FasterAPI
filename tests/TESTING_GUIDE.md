# FasterAPI Testing Guide

## Quick Start

### Run All Working Tests
```bash
cd /Users/bengamble/FasterAPI

# 1. C++ unit tests (16 tests)
./build/tests/test_parameter_extractor

# 2. Integration tests (5 scenarios)
DYLD_LIBRARY_PATH=build/lib python3.13 tests/verify_param_fix.py
```

## Test Architecture

```
┌─────────────────────────────────────────┐
│  Layer 3: Python Integration Tests      │
│  - Full stack validation                │
│  - FastAPI decorators                   │
│  - Parameter extraction E2E             │
│  📁 verify_param_fix.py         ✅      │
└─────────────────────────────────────────┘
                  ↓
┌─────────────────────────────────────────┐
│  Layer 2: Python-C++ Bridge             │
│  - Callback invocation                  │
│  - Type conversion                      │
│  - Parameter passing                    │
│  🔗 python_callback_bridge.cpp  ✅      │
└─────────────────────────────────────────┘
                  ↓
┌─────────────────────────────────────────┐
│  Layer 1: C++ Parameter Extraction      │
│  - Query param parsing                  │
│  - Path param extraction                │
│  - URL decoding                         │
│  📁 test_parameter_extractor.cpp  ✅    │
└─────────────────────────────────────────┘
```

## Test Files

### ✅ Working Tests

#### 1. `tests/test_parameter_extractor.cpp`
**Purpose**: Unit test C++ parameter extraction
**Tests**: 16
**Runtime**: < 1 second
**Port**: None (pure C++ unit test)

**What it tests**:
```cpp
// Query params
get_query_params("/search?q=test&limit=10")
→ {"q": "test", "limit": "10"}

// URL decoding
get_query_params("?text=hello+world")
→ {"text": "hello world"}

// Path params
CompiledRoutePattern("/items/{id}").extract("/items/123")
→ {"id": "123"}
```

#### 2. `tests/verify_param_fix.py`
**Purpose**: Validate the http1_connection.cpp fix
**Tests**: 5 scenarios
**Runtime**: ~3 seconds
**Port**: 9999

**What it tests**:
```python
# Query parameters
GET /test_query?name=alice&age=30
→ {"name": "alice", "age": 30}

# Path parameters
GET /test_path/123
→ {"item_id": 123}

# Combined
GET /test_both/99?active=no
→ {"user_id": 99, "active": "no"}
```

### 📋 Template Tests (For Future Development)

#### 3. `tests/e2e_cpp_api_test.py`
**Purpose**: Test native C++ server API
**Status**: Template - needs routing integration
**Port**: 8765

**Planned Coverage**:
- HTTP methods (GET, POST, PUT, DELETE)
- Concurrent requests
- Error handling
- Performance testing

#### 4. `tests/e2e_python_api_test.py`
**Purpose**: Comprehensive FastAPI testing
**Status**: Template - needs routing integration
**Port**: 8766

**Planned Coverage**:
- All parameter combinations
- Type conversion
- URL encoding
- 50+ concurrent requests
- Stress testing

## Building Tests

### C++ Tests

```bash
cd /Users/bengamble/FasterAPI/build
cmake ..
ninja test_parameter_extractor
```

### Python Tests
No build needed - they're executable scripts.

## Test Results Examples

### C++ Unit Tests
```
Running query_params_simple... ✅ PASS
Running query_params_plus_to_space... ✅ PASS
Running query_params_url_encoded... ✅ PASS
Running path_params_single... ✅ PASS
Running path_params_multiple... ✅ PASS
...
Results: 16 passed, 0 failed
```

### Integration Tests
```
Test: Query params
  URL: /test_query?name=alice&age=30
  Response: {"name": "alice", "age": 30, "test": "query"}
  ✅ PASS

Test: Path param
  URL: /test_path/123
  Response: {"item_id": 123, "test": "path"}
  ✅ PASS
```

## Test Coverage Matrix

| Feature | C++ Unit | Integration | E2E (Future) |
|---------|----------|-------------|--------------|
| Query params | ✅ | ✅ | 📋 |
| Path params | ✅ | ✅ | 📋 |
| URL decoding | ✅ | ✅ | 📋 |
| Type conversion | ⚠️ | ✅ | 📋 |
| Default values | ⚠️ | ✅ | 📋 |
| HTTP methods | ❌ | ❌ | 📋 |
| Concurrency | ❌ | ❌ | 📋 |
| Error handling | ❌ | ❌ | 📋 |

**Legend**: ✅ Tested | ⚠️ Partial | ❌ Not tested | 📋 Template ready

## Common Issues

### Port Already in Use
```bash
# Kill existing test servers
pkill -f "test.*py"
pkill -f "verify_param"
```

### Library Not Found
```bash
# Check library exists
ls -l build/lib/libfasterapi_http.dylib

# Rebuild if needed
cd build && ninja fasterapi_http
```

### Python Import Errors
```bash
# Verify path setup
export DYLD_LIBRARY_PATH=/Users/bengamble/FasterAPI/build/lib:$DYLD_LIBRARY_PATH
python3.13 -c "from fasterapi._fastapi_native import register_route; print('✅ OK')"
```

## Test Development Guidelines

### 1. Use Randomized Data
```python
# ❌ BAD
def test():
    return {"value": 42}

# ✅ GOOD
def test():
    return {"value": random.randint(1, 1000)}
```

### 2. Test Multiple Routes
```python
# ❌ BAD - only one route
@app.get("/test")
def test(): ...

# ✅ GOOD - multiple routes
@app.get("/users/{id}")
def get_user(id: int): ...

@app.get("/items")
def list_items(skip: int = 0, limit: int = 10): ...
```

### 3. Vary HTTP Methods
```python
# ✅ Test all methods
@app.get("/items")
@app.post("/items")
@app.put("/items/{id}")
@app.delete("/items/{id}")
```

### 4. Include Concurrency Tests
```python
# ✅ Test concurrent requests
with ThreadPoolExecutor(max_workers=10) as executor:
    futures = [executor.submit(make_request, i) for i in range(50)]
    results = [f.result() for f in futures]
```

## Future Test Additions

### High Priority
- [ ] HTTP/2 parameter extraction
- [ ] Request body parsing (JSON, form data)
- [ ] Response validation
- [ ] WebSocket parameter handling

### Medium Priority
- [ ] TLS/HTTPS testing
- [ ] Compression testing
- [ ] Performance benchmarks
- [ ] Memory leak detection

### Low Priority
- [ ] HTTP/3 support
- [ ] Advanced routing (regex, wildcards)
- [ ] Middleware testing
- [ ] Rate limiting tests

## Continuous Integration

### Recommended CI Pipeline
```yaml
# .github/workflows/test.yml
- name: Build C++ libraries
  run: cd build && ninja fasterapi_http

- name: Run C++ tests
  run: ./build/tests/test_parameter_extractor

- name: Run integration tests
  run: DYLD_LIBRARY_PATH=build/lib python3.13 tests/verify_param_fix.py
```

## Getting Help

- **Documentation**: See `tests/README_E2E_TESTS.md`
- **Test Summary**: See `tests/TEST_SUMMARY.md`
- **Examples**: Check `examples/` directory
- **Issues**: File bugs with test output attached
