# Python Executor Design

## Problem Statement

Currently, Python handlers are called directly on the reactor thread, which:
- ❌ Blocks the reactor while Python code runs
- ❌ Holds the GIL, preventing true parallelism
- ❌ Can't leverage multiple cores for Python code
- ❌ Mixes I/O threads with compute threads

## Solution: Python Executor Thread Pool

Design a C++ thread pool specifically for executing Python code safely and asynchronously.

## Architecture

```
C++ Reactor Threads (I/O)     Python Executor Threads (Compute)
─────────────────────────     ─────────────────────────────────
                              
Request arrives                
    ↓                          
Router match (30ns)            
    ↓                          
Dispatch to executor ────────→ Acquire GIL
    ↓ (non-blocking)           ↓
Continue serving               Execute Python handler
other requests                 ↓
    ↓                          Release GIL
    ↓                          ↓
Receive result ←──────────────Return result via future
    ↓                          
Send response                  
```

## Core Components

### 1. Python Executor (`src/cpp/python/py_executor.h`)

```cpp
class PythonExecutor {
public:
    struct Config {
        uint32_t num_workers = 0;  // 0 = num_cores
        bool use_subinterpreters = false;  // PEP 554
        uint32_t queue_size = 10000;
    };
    
    // Initialize executor with worker threads
    static int initialize(const Config& config);
    
    // Shutdown executor
    static int shutdown();
    
    // Submit Python callable for execution
    // Returns future that resolves when Python code completes
    template<typename Func>
    static future<PyObject*> submit(Func&& callable);
    
    // Submit with timeout
    template<typename Func>
    static future<PyObject*> submit_timeout(Func&& callable, uint64_t timeout_ns);
    
    // Get executor stats
    struct Stats {
        uint64_t tasks_executed;
        uint64_t tasks_queued;
        uint64_t tasks_failed;
        uint64_t avg_execution_time_ns;
    };
    static Stats get_stats();
};
```

### 2. GIL Guard (`src/cpp/python/gil_guard.h`)

```cpp
class GILGuard {
public:
    GILGuard();   // Acquire GIL
    ~GILGuard();  // Release GIL
    
    // Non-copyable
    GILGuard(const GILGuard&) = delete;
    GILGuard& operator=(const GILGuard&) = delete;
    
private:
    PyGILState_STATE state_;
};

class GILRelease {
public:
    GILRelease();   // Release GIL
    ~GILRelease();  // Reacquire GIL
    
private:
    PyThreadState* state_;
};
```

### 3. Python Task Queue (`src/cpp/python/py_task_queue.h`)

```cpp
template<typename T>
class LockFreeSPSCQueue {  // Single-producer, single-consumer
    // Ring buffer implementation
    // Lock-free for reactor->worker communication
};

template<typename T>
class LockFreeMPMCQueue {  // Multi-producer, multi-consumer
    // For worker->worker or reactor->multiple workers
};

class PythonTaskQueue {
    // Task queue with backpressure
    // Integrates with futures for async result delivery
};
```

### 4. Worker Thread (`src/cpp/python/py_worker.h`)

```cpp
class PythonWorker {
public:
    PythonWorker(uint32_t worker_id, bool use_subinterpreter);
    ~PythonWorker();
    
    // Run worker loop
    void run();
    
    // Stop worker
    void stop();
    
private:
    uint32_t worker_id_;
    PyThreadState* interpreter_;  // Sub-interpreter if enabled
    bool running_;
    
    // Process single task
    void process_task(PythonTask* task);
};
```

## Implementation Phases

### Phase 1: Basic Thread Pool ✅ (Target)
- [x] Worker thread creation
- [x] Task queue (lock-free SPSC)
- [x] GIL management
- [x] Basic dispatch/receive

### Phase 2: Future Integration ✅ (Target)
- [x] Python tasks return futures
- [x] Async result delivery
- [x] Exception propagation
- [x] Timeout support

### Phase 3: Advanced Features 🔄
- [ ] Sub-interpreter support (PEP 554/684)
- [ ] Worker thread pinning
- [ ] Priority queues
- [ ] Backpressure handling

### Phase 4: Optimization 🔄
- [ ] Zero-copy task submission
- [ ] Batch task submission
- [ ] Work stealing
- [ ] NUMA-aware allocation

## Usage Examples

### C++ Side

```cpp
// In route handler (reactor thread)
HttpServer::RouteHandler handler = [](HttpRequest* req, HttpResponse* res) {
    // Dispatch Python handler to executor (non-blocking)
    auto result_future = PythonExecutor::submit([=]() {
        // This runs on worker thread with GIL acquired
        PyObject* handler = get_python_handler(handler_id);
        PyObject* result = PyObject_CallFunction(handler, "OO", req, res);
        return result;
    });
    
    // Attach continuation to send response
    result_future.then([res](PyObject* result) {
        // Convert Python result to HTTP response
        send_python_result(res, result);
    });
};
```

### Python Side (No Changes!)

```python
# User code remains unchanged
@app.get("/user/{id}")
async def get_user(id: int):
    # This automatically runs on executor thread pool
    user = await db.query("SELECT ...")
    return user

# Blocking code also works
@app.get("/cpu-heavy")
def cpu_heavy_task():
    # Runs on worker thread, doesn't block reactor
    result = expensive_computation()
    return result
```

## Performance Targets

| Metric | Target | Rationale |
|--------|--------|-----------|
| Task dispatch | <1µs | Submit to queue + notify |
| GIL acquire | <2µs | OS thread scheduling |
| Context switch | <5µs | Worker wake-up |
| Total overhead | <10µs | End-to-end latency |

## Safety Guarantees

1. **GIL Safety** ✅
   - Always acquired before calling Python
   - Released during C++ I/O
   - Proper thread state management

2. **Memory Safety** ✅
   - RAII guards for GIL
   - Reference counting for PyObjects
   - No dangling pointers

3. **Exception Safety** ✅
   - Python exceptions caught and propagated
   - C++ exceptions disabled (-fno-exceptions)
   - Error codes for all failures

4. **Thread Safety** ✅
   - Lock-free queues where possible
   - Atomic operations for state
   - No data races

## Comparison with Alternatives

### Current: Direct Call (Blocking)
```
Reactor Thread: [I/O]──[Python Handler (GIL)]──[I/O]
                       ↑ Blocks everything ↑
```

### Alternative 1: ThreadPoolExecutor (Python)
```python
executor = ThreadPoolExecutor(max_workers=10)
@app.get("/")
async def handler():
    result = await loop.run_in_executor(executor, blocking_func)
```
- ✅ Non-blocking
- ❌ Python-managed threads (GIL contention)
- ❌ High overhead (~50µs)

### Alternative 2: AsyncIO (Python)
```python
@app.get("/")
async def handler():
    await asyncio.sleep(1)  # Yields to other tasks
```
- ✅ Non-blocking
- ❌ Requires async/await everywhere
- ❌ Can't handle blocking I/O

### Our Solution: C++ Thread Pool (Hybrid)
```
Reactor (C++): [I/O]──dispatch──[I/O]──[I/O]  ← Never blocks
                       ↓
Worker (Python):      [GIL][Handler][GIL]     ← Isolated
```
- ✅ Non-blocking reactor
- ✅ True parallelism (multiple workers)
- ✅ Low overhead (~10µs)
- ✅ Works with any Python code
- ✅ Automatic for users

## Integration Points

### 1. Reactor Integration
- Reactor thread receives request
- Dispatches to executor (non-blocking)
- Continues serving other requests
- Receives result via future callback

### 2. Future Integration
- Python tasks return `future<PyObject*>`
- Integrates with existing future chains
- Can compose with DB futures, etc.

### 3. Error Handling
- Python exceptions become failed futures
- Timeouts detected by executor
- Error codes propagated to response

## Testing Strategy

1. **Unit Tests** - GIL acquisition, task dispatch, futures
2. **Integration Tests** - Python handler execution end-to-end
3. **Concurrency Tests** - Multiple workers, race conditions
4. **Performance Tests** - Overhead measurement, throughput
5. **Stress Tests** - Many concurrent requests

## Next Steps

1. Implement `py_executor.h/cpp` with basic thread pool
2. Implement `gil_guard.h/cpp` for GIL management
3. Create task queue (lock-free SPSC)
4. Integrate with route handlers
5. Add comprehensive tests
6. Benchmark overhead
7. Optimize based on measurements

