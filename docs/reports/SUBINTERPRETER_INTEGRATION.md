# Sub-Interpreter Integration for Multi-Core Python Execution

## Overview

This document describes the integration of Python 3.12+ sub-interpreters into FasterAPI's request handling pipeline to achieve near-linear multi-core scaling.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    HTTP Server (C++)                         │
│  ┌───────────┐  ┌───────────┐  ┌───────────┐               │
│  │ Request 1 │  │ Request 2 │  │ Request 3 │  ... etc      │
│  └─────┬─────┘  └─────┬─────┘  └─────┬─────┘               │
└────────┼──────────────┼──────────────┼─────────────────────┘
         │              │              │
         ▼              ▼              ▼
┌────────────────────────────────────────────────────────────┐
│         PythonCallbackBridge::invoke_handler_async()        │
│              (python_callback_bridge.cpp)                   │
│                                                              │
│  • Validates request body (C++, GIL-free)                  │
│  • Extracts parameters (C++, GIL-free)                     │
│  • Packages for sub-interpreter execution                  │
└───────┬───────────────┬───────────────┬────────────────────┘
        │               │               │
        ▼               ▼               ▼
┌────────────────────────────────────────────────────────────┐
│           SubinterpreterExecutor (C++)                      │
│                                                              │
│  ┌───────────────┐ ┌───────────────┐ ┌───────────────┐   │
│  │ Interpreter 0 │ │ Interpreter 1 │ │ Interpreter 2 │   │
│  │   GIL #0      │ │   GIL #1      │ │   GIL #2      │   │
│  │   Worker 0    │ │   Worker 1    │ │   Worker 2    │   │
│  └───────┬───────┘ └───────┬───────┘ └───────┬───────┘   │
└──────────┼─────────────────┼─────────────────┼────────────┘
           │                 │                 │
           ▼                 ▼                 ▼
      ┌────────┐       ┌────────┐       ┌────────┐
      │ Core 0 │       │ Core 1 │       │ Core 2 │
      └────────┘       └────────┘       └────────┘
```

## Key Benefits

1. **Per-Interpreter GIL** (PEP 684)
   - Each sub-interpreter has its own GIL
   - No GIL contention between requests!
   - Requests execute truly in parallel

2. **Near-Linear Scaling**
   - 8 cores → ~7-8x throughput
   - Validated in Python core team benchmarks

3. **Zero-Copy Validation**
   - Request validation happens in C++ (GIL-free)
   - Only Python handler execution acquires GIL
   - Minimize GIL hold time

## Implementation Steps

### Step 1: Initialize Sub-Interpreter Pool

**Location**: `src/cpp/http/http_server_c_api.cpp` or new init function

```cpp
#include "../python/subinterpreter_executor.h"

int http_init_subinterpreter_pool() {
    using namespace fasterapi::python;

    SubinterpreterExecutor::Config config;
    config.num_pinned_workers = std::thread::hardware_concurrency();
    config.num_pooled_workers = config.num_pinned_workers / 2;
    config.pin_to_cores = true;

    auto result = SubinterpreterExecutor::initialize(config);
    if (!result.is_ok()) {
        LOG_ERROR("HTTP", "Failed to initialize sub-interpreter pool");
        return -1;
    }

    LOG_INFO("HTTP", "Initialized %u sub-interpreters for parallel execution",
             config.num_pinned_workers);
    return 0;
}
```

**Call this** from:
- Python: During `app.run()` or `server.start()`
- C++: In `HttpServer::start()` before accepting connections

### Step 2: Update invoke_handler_async()

**Location**: `src/cpp/http/python_callback_bridge.cpp` lines 395-427

**Current implementation**:
```cpp
auto invoke_handler_async(...) noexcept {
    // Creates std::thread - all requests compete for ONE GIL!
    std::thread([...]) { ... }.detach();
    return fut;
}
```

**New implementation**:
```cpp
fasterapi::core::future<fasterapi::core::result<PythonCallbackBridge::HandlerResult>>
PythonCallbackBridge::invoke_handler_async(...) noexcept {
    using namespace fasterapi::core;
    using namespace fasterapi::python;

    // 1. Pre-process WITHOUT GIL (fast path in C++)
    //    This is already done in invoke_handler()

    // 2. Check if sub-interpreter executor is available
    if (!SubinterpreterExecutor::is_initialized()) {
        // Fallback to synchronous execution in main interpreter
        HandlerResult result = invoke_handler(method, path, headers, body);
        return make_ready_future(ok(std::move(result)));
    }

    // 3. Package handler invocation for sub-interpreter
    //    Need to:
    //    - Find handler (already done in invoke_handler)
    //    - Build kwargs (already done in invoke_handler)
    //    - Submit to sub-interpreter pool

    // CHALLENGE: invoke_handler() acquires GIL and executes inline.
    // We need to defer GIL acquisition until inside sub-interpreter.

    // SOLUTION: Refactor invoke_handler() into two parts:
    //   - prepare_handler_call() - no GIL, returns metadata
    //   - execute_handler_call() - acquires GIL, executes

    auto prep_result = prepare_handler_call(method, path, headers, body);
    if (!prep_result.is_ok()) {
        return make_ready_future(err(prep_result.error()));
    }

    auto [callable, kwargs_dict] = prep_result.value();

    // 4. Submit to sub-interpreter (returns future<result<PyObject*>>)
    auto py_future = SubinterpreterExecutor::submit(callable, nullptr, kwargs_dict);

    // 5. Chain: when Python execution completes, convert to HandlerResult
    return py_future.then([](result<PyObject*> py_result) -> result<HandlerResult> {
        if (!py_result.is_ok()) {
            HandlerResult err_result;
            err_result.status_code = 500;
            err_result.body = "{\"error\":\"Handler execution failed\"}";
            return ok(std::move(err_result));
        }

        // Convert PyObject* to HandlerResult (needs GIL!)
        PyGILState_STATE gstate = PyGILState_Ensure();
        HandlerResult result = convert_py_object_to_result(py_result.value());
        Py_DECREF(py_result.value());
        PyGILState_Release(gstate);

        return ok(std::move(result));
    });
}
```

### Step 3: Refactor invoke_handler()

**Goal**: Split into prepare + execute for sub-interpreter compatibility

```cpp
struct PreparedHandler {
    PyObject* callable;      // Handler to call
    PyObject* kwargs;        // Arguments dict
    const RouteMetadata* metadata;
};

result<PreparedHandler> prepare_handler_call(
    const std::string& method,
    const std::string& path,
    const std::unordered_map<std::string, std::string>& headers,
    const std::string& body
) {
    // Everything from invoke_handler() EXCEPT:
    // - Don't acquire GIL yet
    // - Don't call handler
    // - Return (callable, kwargs) for later execution

    // ... validation, parameter extraction (GIL-free) ...

    // Build kwargs WITHOUT GIL (risky - PyDict_New needs GIL!)
    // SOLUTION: Return raw data, build kwargs in sub-interpreter

    return ok(PreparedHandler{callable, kwargs, metadata});
}

HandlerResult execute_handler_call(PreparedHandler& prep) {
    // This runs INSIDE sub-interpreter with its dedicated GIL

    PyObject* py_result = PyObject_Call(prep.callable, PyTuple_New(0), prep.kwargs);

    // ... rest of invoke_handler() logic ...

    return result;
}
```

### Step 4: Handler Re-Registration Challenge

**Problem**: Handlers registered in main interpreter aren't accessible in sub-interpreters.

**Solutions**:

**Option A: Module Re-Import (Simpler)**
```python
# In each sub-interpreter on startup
import your_app_module
handler = getattr(your_app_module, 'handler_function')
```

**Option B: Callable Serialization (Faster)**
```cpp
// Serialize handler to bytecode
PyObject* code = PyObject_GetAttrString(callable, "__code__");
PyObject* bytecode = PyMarshal_WriteObjectToString(code, Py_MARSHAL_VERSION);

// In sub-interpreter: deserialize
PyObject* code = PyMarshal_ReadObjectFromString(bytecode_data, bytecode_len);
PyObject* func = PyFunction_New(code, globals);
```

**Option C: Shared Handler Registry (Recommended)**
```cpp
// Keep handlers in main interpreter
// Sub-interpreters call back to main for execution
// (defeats purpose of sub-interpreters - DON'T DO THIS)
```

**Recommendation**: Option A for MVP, Option B for production

### Step 5: Configuration API

**Expose to Python**:
```python
from fasterapi import App

app = App(
    num_workers=16,          # Number of sub-interpreters
    pin_to_cores=True,       # CPU affinity
    worker_strategy="auto",  # auto | pinned | pooled
)
```

**Implementation**: Add to `App.__init__()` in `fasterapi/__init__.py`

## Testing Strategy

### Unit Tests
- Sub-interpreter initialization
- Handler preparation (prepare_handler_call)
- Handler execution (execute_handler_call)
- Future chaining

### Integration Tests
- Single request through sub-interpreter
- Concurrent requests (verify no GIL contention)
- Handler state isolation

### Performance Tests
```python
def test_scaling():
    # 1 core baseline
    throughput_1 = benchmark_app(num_workers=1)

    # 8 cores with sub-interpreters
    throughput_8 = benchmark_app(num_workers=8)

    scaling_factor = throughput_8 / throughput_1
    assert scaling_factor >= 6.0  # At least 6x speedup
```

## Fallback Strategy

If sub-interpreter pool fails to initialize:
1. Log warning
2. Fall back to synchronous execution
3. Still works, just slower

## Performance Expectations

| Cores | Expected Throughput | Actual GIL Contention |
|-------|-------------------|----------------------|
| 1     | 500 req/s        | 100% (baseline)       |
| 2     | 950 req/s        | 0% (separate GILs)    |
| 4     | 1900 req/s       | 0%                    |
| 8     | 3800 req/s       | 0%                    |
| 16    | 7500 req/s       | 0%                    |

**Speedup**: Near-linear up to physical core count

## Migration Path

### Phase 1: Basic Integration
- Initialize pool
- Update invoke_handler_async()
- Fallback to main interpreter if fails
- **Goal**: Proof of concept

### Phase 2: Handler Registration
- Re-import handlers in sub-interpreters
- Verify state isolation
- **Goal**: Production-ready

### Phase 3: Optimization
- Handler bytecode caching
- Per-interpreter module caching
- Connection pool per-interpreter
- **Goal**: Maximum performance

### Phase 4: Configuration
- Expose Python API
- Auto-detect optimal strategy
- Performance monitoring
- **Goal**: User-friendly

## Open Questions

1. **Handler State**: How to handle global state in handlers?
   - Answer: Prefer stateless handlers, or per-interpreter init hooks

2. **Module Imports**: Re-import in each interpreter or share?
   - Answer: Re-import for safety, cache for performance

3. **DB Connections**: Pool per interpreter or shared?
   - Answer: Pool per interpreter (no locking!)

4. **Middleware**: Execute in main or sub-interpreter?
   - Answer: Sub-interpreter (full parallelism)

## References

- PEP 684: https://peps.python.org/pep-0684/
- PEP 554: https://peps.python.org/pep-0554/
- PEP 703: https://peps.python.org/pep-0703/ (Python 3.13 nogil)
- CPython sub-interpreter tests: https://github.com/python/cpython/tree/main/Modules/_testinternalcapi

## Status

- [x] Documentation complete
- [ ] Implementation in progress
- [ ] Testing pending
- [ ] Performance validation pending
