#pragma once

#include "../core/future.h"
#include <Python.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <atomic>

namespace fasterapi {
namespace python {

using namespace fasterapi::core;

/**
 * Python executor with thread pool for non-blocking Python execution.
 * 
 * Manages a pool of worker threads that can safely execute Python code
 * without blocking the reactor threads.
 * 
 * Features:
 * - Proper GIL management (acquire/release)
 * - Lock-free task queue
 * - Future-based async results
 * - Timeout support
 * - Optional sub-interpreter isolation
 * - Worker thread affinity
 * 
 * Design goals:
 * - Correctness: Proper GIL handling, no data races
 * - Performance: <10µs dispatch overhead
 * - Safety: Exception handling, timeout detection
 */
class PythonExecutor {
public:
    /**
     * Executor configuration.
     */
    struct Config {
        uint32_t num_workers;
        bool use_subinterpreters;
        uint32_t queue_size;
        bool pin_workers;
        
        Config()
            : num_workers(0),
              use_subinterpreters(false),
              queue_size(10000),
              pin_workers(false) {}
    };
    
    /**
     * Initialize the Python executor.
     * 
     * Must be called after Python is initialized.
     * 
     * @param config Executor configuration
     * @return 0 on success, error code otherwise
     */
    static int initialize(const Config& config = Config{}) noexcept;
    
    /**
     * Shutdown the executor.
     * 
     * Waits for all tasks to complete.
     * 
     * @param timeout_ms Max time to wait for tasks (0 = infinite)
     * @return 0 on success
     */
    static int shutdown(uint32_t timeout_ms = 0) noexcept;
    
    /**
     * Check if executor is initialized.
     */
    static bool is_initialized() noexcept;
    
    /**
     * Submit a Python callable for execution.
     * 
     * The callable will be executed on a worker thread with GIL acquired.
     * Returns a future that resolves when execution completes.
     * 
     * @param callable Python callable (function, lambda, etc.)
     * @return future<PyObject*> that resolves with result
     * 
     * Example:
     *   auto result = PythonExecutor::submit(py_handler);
     *   result.then([](PyObject* obj) {
     *       // Process result
     *   });
     */
    static future<PyObject*> submit(PyObject* callable) noexcept;
    
    /**
     * Submit with timeout.
     * 
     * Task will be cancelled if it doesn't complete within timeout.
     * 
     * @param callable Python callable
     * @param timeout_ns Timeout in nanoseconds
     * @return future<PyObject*>
     */
    static future<PyObject*> submit_timeout(
        PyObject* callable,
        uint64_t timeout_ns
    ) noexcept;
    
    /**
     * Submit with arguments.
     * 
     * @param callable Python callable
     * @param args Python tuple of arguments
     * @param kwargs Python dict of keyword arguments (nullable)
     * @return future<PyObject*>
     */
    static future<PyObject*> submit_call(
        PyObject* callable,
        PyObject* args,
        PyObject* kwargs = nullptr
    ) noexcept;
    
    /**
     * Get executor statistics.
     */
    struct Stats {
        uint64_t tasks_submitted{0};
        uint64_t tasks_completed{0};
        uint64_t tasks_failed{0};
        uint64_t tasks_timeout{0};
        uint64_t tasks_queued{0};
        uint32_t active_workers{0};
        uint64_t avg_task_time_ns{0};
        uint64_t total_task_time_ns{0};
    };
    
    static Stats get_stats() noexcept;
    
    /**
     * Get number of worker threads.
     */
    static uint32_t num_workers() noexcept;
    
private:
    // Singleton instance
    static PythonExecutor* instance_;
    
    PythonExecutor(const Config& config);
    ~PythonExecutor();
    
    // Implementation
    struct Impl;
    std::unique_ptr<Impl> impl_;
    
    Config config_;
    std::atomic<bool> initialized_{false};
    std::atomic<uint64_t> tasks_submitted_{0};
    std::atomic<uint64_t> tasks_completed_{0};
    std::atomic<uint64_t> tasks_failed_{0};
    std::atomic<uint64_t> tasks_timeout_{0};
};

} // namespace python
} // namespace fasterapi

