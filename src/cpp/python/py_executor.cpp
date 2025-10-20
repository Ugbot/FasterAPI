#include "py_executor.h"
#include "gil_guard.h"
#include "pyobject_pool.h"
#include "../core/lockfree_queue.h"
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <iostream>
#include <chrono>
#include <atomic>

namespace fasterapi {
namespace python {

// ============================================================================
// Python Task
// ============================================================================

struct PythonTask {
    PyObject* callable;     // Python callable to execute
    PyObject* args;         // Arguments (nullable)
    PyObject* kwargs;       // Keyword arguments (nullable)
    promise<PyObject*> result_promise;  // Promise for result
    uint64_t submit_time_ns;
    uint64_t timeout_ns;    // 0 = no timeout
    
    PythonTask(PyObject* c, PyObject* a = nullptr, PyObject* kw = nullptr)
        : callable(c), args(a), kwargs(kw),
          submit_time_ns(0), timeout_ns(0) {
        // Increment reference counts
        if (callable) Py_INCREF(callable);
        if (args) Py_INCREF(args);
        if (kwargs) Py_INCREF(kwargs);
    }
    
    ~PythonTask() {
        // Decrement reference counts (must hold GIL!)
        // For safety, we'll let the worker thread clean up
    }
    
    void cleanup_refs() {
        // Called by worker thread with GIL held
        if (callable) { Py_DECREF(callable); callable = nullptr; }
        if (args) { Py_DECREF(args); args = nullptr; }
        if (kwargs) { Py_DECREF(kwargs); kwargs = nullptr; }
    }
};

// ============================================================================
// Configuration flags
// ============================================================================

// Enable lock-free queue (faster but uses busy-wait)
#define USE_LOCKFREE_QUEUE 1

// Enable PyObject pooling (reduces allocation overhead)
#define USE_PYOBJECT_POOL 1

// ============================================================================
// Worker Thread
// ============================================================================

class PythonWorker {
public:
    PythonWorker(uint32_t worker_id, bool use_subinterpreter)
        : worker_id_(worker_id),
          use_subinterpreter_(use_subinterpreter),
          interpreter_(nullptr),
          running_(false) {
    }
    
    ~PythonWorker() {
        stop();
    }
    
    void start(std::queue<PythonTask*>* task_queue,
               std::mutex* queue_mutex,
               std::condition_variable* queue_cv,
               std::atomic<bool>* shutdown_flag) {
        
        running_ = true;
        
        thread_ = std::thread([this, task_queue, queue_mutex, queue_cv, shutdown_flag]() {
            // Initialize sub-interpreter if requested
            if (use_subinterpreter_) {
                GILGuard gil;
                interpreter_ = Py_NewInterpreter();
                if (!interpreter_) {
                    std::cerr << "Failed to create sub-interpreter for worker "
                              << worker_id_ << std::endl;
                    return;
                }
            }
            
            std::cout << "Python worker " << worker_id_ << " started" << std::endl;
            
            // Worker loop
            while (running_ && !shutdown_flag->load()) {
                PythonTask* task = nullptr;
                
                // Get task from queue
                {
                    std::unique_lock<std::mutex> lock(*queue_mutex);
                    queue_cv->wait_for(lock, std::chrono::milliseconds(100), [&]() {
                        return !task_queue->empty() || shutdown_flag->load();
                    });
                    
                    if (!task_queue->empty()) {
                        task = task_queue->front();
                        task_queue->pop();
                    }
                }
                
                // Execute task
                if (task) {
                    process_task(task);
                    delete task;
                }
            }
            
            // Cleanup sub-interpreter
            if (interpreter_) {
                GILGuard gil;
                Py_EndInterpreter(interpreter_);
            }
            
            std::cout << "Python worker " << worker_id_ << " stopped" << std::endl;
        });
    }
    
    void stop() {
        if (running_) {
            running_ = false;
            if (thread_.joinable()) {
                thread_.join();
            }
        }
    }
    
private:
    void process_task(PythonTask* task) {
        if (!task || !task->callable) {
            return;
        }
        
        // Acquire GIL before calling Python
        GILGuard gil;
        
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Call Python callable
        PyObject* result = nullptr;
        
        if (task->args && task->kwargs) {
            result = PyObject_Call(task->callable, task->args, task->kwargs);
        } else if (task->args) {
            result = PyObject_CallObject(task->callable, task->args);
        } else {
            result = PyObject_CallObject(task->callable, nullptr);
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end_time - start_time
        ).count();
        
        // Check for Python exceptions
        if (PyErr_Occurred()) {
            PyErr_Print();
            PyErr_Clear();
            
            // Set exception in promise
            task->result_promise.set_exception("Python exception occurred");
        } else if (result) {
            // Set result in promise
            task->result_promise.set_value(result);
        } else {
            // No result and no exception (shouldn't happen)
            task->result_promise.set_exception("No result from Python callable");
        }
        
        // Cleanup references
        task->cleanup_refs();
    }
    
    uint32_t worker_id_;
    bool use_subinterpreter_;
    PyThreadState* interpreter_;
    std::atomic<bool> running_;
    std::thread thread_;
};

// ============================================================================
// Executor Implementation
// ============================================================================

struct PythonExecutor::Impl {
    std::vector<std::unique_ptr<PythonWorker>> workers;
    std::queue<PythonTask*> task_queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::atomic<bool> shutdown_flag{false};
    std::atomic<uint64_t> queued_tasks{0};
    std::atomic<uint64_t> total_task_time_ns{0};
};

// Static instance
PythonExecutor* PythonExecutor::instance_ = nullptr;

PythonExecutor::PythonExecutor(const Config& config)
    : impl_(std::make_unique<Impl>()),
      config_(config) {
}

PythonExecutor::~PythonExecutor() {
    shutdown(0);
}

int PythonExecutor::initialize(const Config& config) noexcept {
    if (instance_) {
        std::cerr << "PythonExecutor already initialized" << std::endl;
        return 1;
    }
    
    // Create executor instance
    instance_ = new PythonExecutor(config);
    
    // Initialize Python threading
    int result = initialize_python_threading();
    if (result != 0) {
        delete instance_;
        instance_ = nullptr;
        return result;
    }
    
    // Determine number of workers
    uint32_t num_workers = config.num_workers;
    if (num_workers == 0) {
        num_workers = std::thread::hardware_concurrency();
        if (num_workers == 0) num_workers = 4;  // Fallback
    }
    
    // Create worker threads
    instance_->impl_->workers.reserve(num_workers);
    for (uint32_t i = 0; i < num_workers; ++i) {
        auto worker = std::make_unique<PythonWorker>(i, config.use_subinterpreters);
        worker->start(
            &instance_->impl_->task_queue,
            &instance_->impl_->queue_mutex,
            &instance_->impl_->queue_cv,
            &instance_->impl_->shutdown_flag
        );
        instance_->impl_->workers.push_back(std::move(worker));
    }
    
    instance_->initialized_.store(true);
    
    std::cout << "PythonExecutor initialized with " << num_workers << " workers" << std::endl;
    
    return 0;
}

int PythonExecutor::shutdown(uint32_t timeout_ms) noexcept {
    if (!instance_) {
        return 0;
    }
    
    std::cout << "Shutting down PythonExecutor..." << std::endl;
    
    // Signal shutdown
    instance_->impl_->shutdown_flag.store(true);
    instance_->impl_->queue_cv.notify_all();
    
    // Wait for workers to finish
    for (auto& worker : instance_->impl_->workers) {
        worker->stop();
    }
    
    instance_->impl_->workers.clear();
    
    // Cleanup
    instance_->initialized_.store(false);
    delete instance_;
    instance_ = nullptr;
    
    // Shutdown Python threading
    shutdown_python_threading();
    
    std::cout << "PythonExecutor shutdown complete" << std::endl;
    
    return 0;
}

bool PythonExecutor::is_initialized() noexcept {
    return instance_ && instance_->initialized_.load();
}

future<PyObject*> PythonExecutor::submit(PyObject* callable) noexcept {
    if (!instance_ || !callable) {
        return make_exception_future<PyObject*>("Executor not initialized or null callable");
    }
    
    // Create task
    auto* task = new PythonTask(callable);
    task->submit_time_ns = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    
    // Get future before queuing
    future<PyObject*> result = task->result_promise.get_future();
    
    // Queue task
    {
        std::lock_guard<std::mutex> lock(instance_->impl_->queue_mutex);
        instance_->impl_->task_queue.push(task);
        instance_->impl_->queued_tasks.fetch_add(1);
    }
    
    // Notify a worker
    instance_->impl_->queue_cv.notify_one();
    
    // Track submission
    instance_->tasks_submitted_.fetch_add(1);
    
    return result;
}

future<PyObject*> PythonExecutor::submit_timeout(
    PyObject* callable,
    uint64_t timeout_ns
) noexcept {
    if (!instance_ || !callable) {
        return make_exception_future<PyObject*>("Executor not initialized or null callable");
    }
    
    // Create task with timeout
    auto* task = new PythonTask(callable);
    task->submit_time_ns = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    task->timeout_ns = timeout_ns;
    
    // Get future before queuing
    future<PyObject*> result = task->result_promise.get_future();
    
    // Queue task
    {
        std::lock_guard<std::mutex> lock(instance_->impl_->queue_mutex);
        instance_->impl_->task_queue.push(task);
        instance_->impl_->queued_tasks.fetch_add(1);
    }
    
    // Notify a worker
    instance_->impl_->queue_cv.notify_one();
    
    // Track submission
    instance_->tasks_submitted_.fetch_add(1);
    
    // TODO: Add timeout monitoring thread
    
    return result;
}

future<PyObject*> PythonExecutor::submit_call(
    PyObject* callable,
    PyObject* args,
    PyObject* kwargs
) noexcept {
    if (!instance_ || !callable) {
        return make_exception_future<PyObject*>("Executor not initialized or null callable");
    }
    
    // Create task with args
    auto* task = new PythonTask(callable, args, kwargs);
    task->submit_time_ns = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    
    // Get future before queuing
    future<PyObject*> result = task->result_promise.get_future();
    
    // Queue task
    {
        std::lock_guard<std::mutex> lock(instance_->impl_->queue_mutex);
        instance_->impl_->task_queue.push(task);
        instance_->impl_->queued_tasks.fetch_add(1);
    }
    
    // Notify a worker
    instance_->impl_->queue_cv.notify_one();
    
    // Track submission
    instance_->tasks_submitted_.fetch_add(1);
    
    return result;
}

PythonExecutor::Stats PythonExecutor::get_stats() noexcept {
    if (!instance_) {
        return Stats{};
    }
    
    Stats stats;
    stats.tasks_submitted = instance_->tasks_submitted_.load();
    stats.tasks_completed = instance_->tasks_completed_.load();
    stats.tasks_failed = instance_->tasks_failed_.load();
    stats.tasks_timeout = instance_->tasks_timeout_.load();
    stats.tasks_queued = instance_->impl_->queued_tasks.load();
    stats.active_workers = instance_->impl_->workers.size();
    stats.total_task_time_ns = instance_->impl_->total_task_time_ns.load();
    
    if (stats.tasks_completed > 0) {
        stats.avg_task_time_ns = stats.total_task_time_ns / stats.tasks_completed;
    }
    
    return stats;
}

uint32_t PythonExecutor::num_workers() noexcept {
    if (!instance_) {
        return 0;
    }
    return instance_->impl_->workers.size();
}

} // namespace python
} // namespace fasterapi

