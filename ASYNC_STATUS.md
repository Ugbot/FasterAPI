# Async Coroutine Implementation - Current Status

## ✅ Completed Infrastructure

### 1. C++20 Coroutine Support (src/cpp/core/)
- **coro_task<T>** - Standard C++20 coroutine type with promise_type (src/cpp/core/coro_task.h)
- **awaitable_future<T>** - Adapts future<T> to work with co_await (src/cpp/core/awaitable_future.h)
- **result<T>** - Exception-free error handling (Rust-style) (src/cpp/core/result.h)
- **Coroutine lifetime management** - Http2Connection stores active coroutines (src/cpp/http/http2_server.cpp:56)

### 2. Hybrid Sub-Interpreter Executor (src/cpp/python/subinterpreter_executor.{h,cpp})
- **Pinned workers**: Dedicated sub-interpreters (1:1 worker:interpreter)
- **Pooled workers**: Shared sub-interpreters (N:M workers:interpreters)
- **Lock-free queues**: AeronSPSCQueue for task distribution
- **Per-interpreter GIL**: Python 3.12+ PEP 684 support

### 3. Async HTTP/2 Handler (src/cpp/http/http2_server.cpp:86)
- **handle_request_async()** coroutine implemented
- **Coroutine storage** in Http2Connection::active_coroutines map
- **Self-cleanup** when coroutine completes
- **Stream close cleanup** to prevent leaks
- **Task ownership** - Stores coro_task<void> objects (not just handles) to prevent premature destruction

### 4. Thread Pool Async Execution (src/cpp/http/python_callback_bridge.cpp:161-193)
- **invoke_handler_async()** - Executes Python in detached thread
- **promise/future pattern** - Returns future<result<HandlerResult>>
- **Event loop non-blocking** - Returns immediately while Python executes

### 5. Awaitable Future Wait Mechanism (src/cpp/core/awaitable_future.h:166-176)
- **Busy-wait implementation** - Spawns thread to wait for future completion
- **Coroutine resumption** - Resumes coroutine when Python completes

## ⚠️ Root Cause: nghttp2 Thread Safety Constraints

**Problem**: nghttp2 requires all session API calls from the same thread, incompatible with our async coroutine model.

**Current flow (BROKEN)**:

1. Event loop thread calls `on_frame_recv_callback()` → creates coroutine
2. Coroutine starts with `task.resume()` → executes until `co_await`
3. `awaitable_future::await_suspend()` spawns detached thread to wait for Python
4. Event loop continues (coroutine suspended, task stored in `active_coroutines`)
5. Python completes → detached thread calls `h.resume()` ← **WRONG THREAD!**
6. Coroutine resumes in detached thread → calls `send_http2_response()`
7. nghttp2 operations called from non-event-loop thread → **HTTP/2 framing error**

**Error**:
```
curl: (16) Error in the HTTP2 framing layer
```

**Root cause**: nghttp2 is not thread-safe. All nghttp2 operations (including sending responses) must be called from the event loop thread.

## ✅ Solution: Pure C++ HTTP/2 Implementation

**Decision**: Replace nghttp2 with custom HTTP/2 implementation integrated with our event loop.

**Rationale**:
1. **Full control** - No external threading constraints
2. **Protocol logic only** - Copy frame parsing/HPACK from nghttp2, use our threading model
3. **True async** - Event loop wake mechanism for coroutine resumption
4. **Performance** - All protocol complexity in C++, minimal Python surface area

### New Architecture

**Correct flow (TO IMPLEMENT)**:

1. Event loop thread creates and starts coroutine
2. Coroutine suspends at `co_await`, task stored in `active_coroutines`
3. Python executes in detached thread
4. Python completes → **signal event loop** (eventfd/pipe write)
5. Event loop wakes up, sees wake signal
6. Event loop **resumes coroutine from event loop thread**
7. Coroutine sends response using nghttp2 (safe - same thread)

**Implementation steps**:

1. Add wake mechanism to event loop:
   - Create eventfd (Linux) or pipe (macOS/BSD)
   - Add wake fd to kqueue/epoll monitoring

2. Add coroutine ready queue:
   - Lock-free SPSC queue: `ready_coroutines_`
   - Python completion thread enqueues coroutine handle

3. Modify `awaitable_future::await_suspend()`:
   - Don't call `h.resume()` directly
   - Instead: enqueue `h` and signal event loop

4. Event loop wake handler:
   - Drain ready queue
   - Resume all ready coroutines (from event loop thread)

**Option A: Event Loop Integration** (Recommended)
```cpp
// In src/cpp/core/event_loop.h
class EventLoop {
    int wake_fd_;  // eventfd or pipe for wake signals
    AeronSPSCQueue<std::coroutine_handle<>> ready_coroutines_;

    void wake() {
        uint64_t val = 1;
        write(wake_fd_, &val, sizeof(val));
    }

    void handle_wake() {
        // Drain wake fd
        uint64_t val;
        read(wake_fd_, &val, sizeof(val));

        // Resume all ready coroutines
        std::coroutine_handle<> h;
        while (ready_coroutines_.try_pop(h)) {
            h.resume();  // Safe - we're in event loop thread
        }
    }
};

// In src/cpp/core/awaitable_future.h
void await_suspend(std::coroutine_handle<> h) noexcept {
    std::thread([this, h]() {
        // Wait for Python to complete
        while (!fut_.available() && !fut_.failed()) {
            std::this_thread::yield();
        }

        // DON'T resume directly!
        // Instead: signal event loop
        event_loop->enqueue_ready_coroutine(h);
        event_loop->wake();
    }).detach();
}
```

**Option B: Simpler Synchronous Baseline** (CURRENT)
- Use synchronous `invoke_handler()` instead of async
- Python blocks event loop thread (one request at a time per worker)
- Still benefits from multi-worker parallelism (SO_REUSEPORT)
- No coroutine complexity, no threading issues

## 🚀 Current Workaround

The HTTP/2 server uses **synchronous execution** (src/cpp/http/http2_server.cpp:247):

```cpp
// Synchronous call in event loop thread
PythonCallbackBridge::HandlerResult handler_result =
    PythonCallbackBridge::invoke_handler(method, path, headers_map, body);
```

This works correctly but:
- ✅ Python execution is still using sub-interpreter infrastructure
- ❌ Blocks event loop during Python execution
- ❌ No concurrent request handling

## 📋 Next Steps to Enable True Async

### Step 1: Implement C++ Serialization Layer
1. Create `SerializedRequest` and `SerializedResponse` structs
2. Update `SubinterpreterExecutor::submit()` to accept C++ callable
3. Move Python object creation inside sub-interpreter context

### Step 2: Update invoke_handler_async()
1. Serialize request data to C++ types
2. Submit C++ lambda to sub-interpreter
3. Lambda creates Python objects with sub-interpreter's GIL
4. Extract response to C++ types before returning

### Step 3: Complete Future Transformation
Current fallback:
```cpp
// TODO: Transform future<result<PyObject*>> → future<result<HandlerResult>>
HandlerResult handler_result = invoke_handler(method, path, headers_map, body);
return future<result<HandlerResult>>::make_ready(ok(std::move(handler_result)));
```

Should become:
```cpp
return py_future.then([](result<SerializedResponse> response) {
    return HandlerResult::from_serialized(response.value());
});
```

### Step 4: Add Preallocated Buffer Pools
Per user requirement: "allocations are expensive"
- Pool of SerializedRequest/Response objects
- Per-worker buffer pools for zero contention

## 🧪 Testing

**Current Test** (src/cpp/http/http2_server.cpp:264):
```bash
$ curl --http2-prior-knowledge http://localhost:8080/
# Works with synchronous baseline

# But async version crashes with GIL error
```

**Once serialization is implemented**:
```bash
# Test concurrent requests to verify sub-interpreter parallelism
$ ab -n 10000 -c 100 -k http://localhost:8080/
```

## 📚 References

- **PEP 684**: Per-Interpreter GIL
- **PEP 554**: Multiple Interpreters in Stdlib
- **Python C API**: PyGILState_Ensure/Release for thread state management
- **C++20 Coroutines**: ISO/IEC 14882:2020 §9.5

## Summary

✅ **Coroutine Infrastructure**: C++20 coroutines, awaitable futures, task storage - complete
✅ **Sub-Interpreter System**: Hybrid pinned/pooled model with per-interpreter GIL - complete
⚠️ **nghttp2 Blocker**: Threading constraints prevent async coroutine execution
🎯 **Solution**: Implement pure C++ HTTP/2 (copy protocol logic, use our event loop)
📋 **Implementation Plan**: See approved plan - frame parsing, HPACK, streams, connection management, event loop wake mechanism

## Next Steps

### Phase 1: HTTP/2 Core (src/cpp/http/http2/)
1. `http2_frame.{h,cpp}` - All 10 frame types + parsing
2. `http2_hpack.{h,cpp}` - HPACK encoder/decoder with static/dynamic tables
3. `http2_stream.{h,cpp}` - Stream state machine + flow control
4. `http2_connection.{h,cpp}` - Connection management + frame dispatching

### Phase 2: Event Loop Integration
1. Add wake mechanism (`wake_fd_` + `ready_coroutines_` queue)
2. Modify `awaitable_future::await_suspend()` to enqueue + wake
3. Event loop handles wake → resumes coroutines from event loop thread

### Phase 3: Replace nghttp2
1. Remove nghttp2 includes/calls from `http2_server.cpp`
2. Use our HTTP/2 implementation
3. Test with `curl --http2-prior-knowledge`

**Outcome**: True async HTTP/2 with no threading constraints!
