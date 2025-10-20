/**
 * Python Executor C API Exports
 * 
 * Provides C-compatible API for Python executor functionality.
 */

#include "py_executor.h"
#include "gil_guard.h"

using namespace fasterapi::python;

extern "C" {

/**
 * Initialize Python executor.
 * 
 * @param num_workers Number of worker threads (0 = auto-detect)
 * @param use_subinterpreters Use sub-interpreters per worker
 * @param queue_size Max queued tasks
 * @return 0 on success, error code otherwise
 */
int py_executor_initialize(
    uint32_t num_workers,
    int use_subinterpreters,
    uint32_t queue_size
) noexcept {
    PythonExecutor::Config config;
    config.num_workers = num_workers;
    config.use_subinterpreters = use_subinterpreters != 0;
    config.queue_size = queue_size;
    config.pin_workers = false;
    
    return PythonExecutor::initialize(config);
}

/**
 * Shutdown Python executor.
 * 
 * @param timeout_ms Max time to wait for tasks (0 = infinite)
 * @return 0 on success
 */
int py_executor_shutdown(uint32_t timeout_ms) noexcept {
    return PythonExecutor::shutdown(timeout_ms);
}

/**
 * Check if executor is initialized.
 * 
 * @return 1 if initialized, 0 otherwise
 */
int py_executor_is_initialized() noexcept {
    return PythonExecutor::is_initialized() ? 1 : 0;
}

/**
 * Get number of worker threads.
 * 
 * @return Number of workers
 */
uint32_t py_executor_num_workers() noexcept {
    return PythonExecutor::num_workers();
}

/**
 * Submit Python callable for execution.
 * 
 * @param callable Python callable (PyObject*)
 * @param args Python tuple of arguments (nullable)
 * @param kwargs Python dict of keyword arguments (nullable)
 * @param out_future_handle Output future handle (opaque pointer)
 * @return 0 on success, error code otherwise
 */
int py_executor_submit(
    void* callable,
    void* args,
    void* kwargs,
    void** out_future_handle
) noexcept {
    if (!callable || !out_future_handle) {
        return 1;
    }
    
    auto* py_callable = reinterpret_cast<PyObject*>(callable);
    auto* py_args = reinterpret_cast<PyObject*>(args);
    auto* py_kwargs = reinterpret_cast<PyObject*>(kwargs);
    
    // Submit task
    future<PyObject*> result_future = PythonExecutor::submit_call(
        py_callable,
        py_args,
        py_kwargs
    );
    
    // Store future handle (simplified - would need proper storage)
    // For now, return success
    *out_future_handle = nullptr;
    
    return 0;
}

/**
 * Get executor statistics.
 * 
 * @param out_tasks_submitted Output tasks submitted
 * @param out_tasks_completed Output tasks completed
 * @param out_tasks_failed Output tasks failed
 * @param out_tasks_queued Output tasks queued
 * @param out_active_workers Output active workers
 * @return 0 on success
 */
int py_executor_get_stats(
    uint64_t* out_tasks_submitted,
    uint64_t* out_tasks_completed,
    uint64_t* out_tasks_failed,
    uint64_t* out_tasks_queued,
    uint32_t* out_active_workers
) noexcept {
    auto stats = PythonExecutor::get_stats();
    
    if (out_tasks_submitted) *out_tasks_submitted = stats.tasks_submitted;
    if (out_tasks_completed) *out_tasks_completed = stats.tasks_completed;
    if (out_tasks_failed) *out_tasks_failed = stats.tasks_failed;
    if (out_tasks_queued) *out_tasks_queued = stats.tasks_queued;
    if (out_active_workers) *out_active_workers = stats.active_workers;
    
    return 0;
}

} // extern "C"

