#pragma once

#include "subinterpreter_pool.h"
#include "py_executor.h"
#include "../core/future.h"
#include "../core/result.h"
#include "../core/lockfree_queue.h"
#include <Python.h>
#include <vector>
#include <atomic>
#include <thread>
#include <functional>

namespace fasterapi {
namespace python {

using namespace fasterapi::core;

/**
 * Hybrid sub-interpreter executor with pinned + pooled workers.
 *
 * Architecture:
 * - Pinned workers: Each has dedicated sub-interpreter (zero contention)
 * - Pooled workers: Share a pool of sub-interpreters (handle overflow)
 *
 * Configuration example:
 *   SubinterpreterExecutor::Config config;
 *   config.num_pinned_workers = 8;        // 8 dedicated interpreters
 *   config.num_pooled_workers = 4;        // 4 additional workers
 *   config.num_pooled_interpreters = 2;   // sharing 2 interpreters
 *
 * Benefits:
 * - True parallelism: Each interpreter has own GIL (PEP 684)
 * - Scalable: Pinned workers for steady load, pooled for bursts
 * - Efficient: Zero GIL contention on pinned workers
 * - Tunable: Adjust based on workload characteristics
 */
class SubinterpreterExecutor {
public:
    /**
     * Executor configuration.
     */
    struct Config {
        uint32_t num_pinned_workers;        // Workers with dedicated interpreters
        uint32_t num_pooled_workers;        // Workers sharing pooled interpreters
        uint32_t num_pooled_interpreters;   // Size of shared interpreter pool
        uint32_t task_queue_size;           // Per-worker queue size
        bool pin_to_cores;                  // CPU affinity

        Config()
            : num_pinned_workers(0),         // 0 = auto (hardware_concurrency)
              num_pooled_workers(0),         // 0 = no pooled workers
              num_pooled_interpreters(0),    // 0 = num_pooled_workers / 2
              task_queue_size(10000),
              pin_to_cores(true) {}
    };

    /**
     * Task type for execution.
     */
    struct Task {
        PyObject* callable;
        PyObject* args;
        PyObject* kwargs;
        std::function<void(result<PyObject*>)> callback;
    };

    /**
     * Statistics.
     */
    struct Stats {
        uint64_t tasks_submitted{0};
        uint64_t tasks_completed{0};
        uint64_t tasks_failed{0};
        uint64_t pinned_tasks{0};          // Tasks on pinned workers
        uint64_t pooled_tasks{0};          // Tasks on pooled workers
        uint32_t active_pinned_workers{0};
        uint32_t active_pooled_workers{0};
        uint64_t avg_task_time_ns{0};
    };

    /**
     * Initialize the executor.
     *
     * Must be called after Py_Initialize().
     *
     * @param config Configuration
     * @return result<void> success or error
     */
    static result<void> initialize(const Config& config = Config{}) noexcept;

    /**
     * Shutdown the executor.
     *
     * Waits for all tasks to complete.
     *
     * @param timeout_ms Max wait time (0 = infinite)
     * @return result<void>
     */
    static result<void> shutdown(uint32_t timeout_ms = 0) noexcept;

    /**
     * Check if initialized.
     */
    static bool is_initialized() noexcept;

    /**
     * Submit task for execution.
     *
     * Executes on a pinned worker if available, otherwise pooled worker.
     *
     * @param callable Python callable
     * @param args Python tuple of arguments (optional)
     * @param kwargs Python dict of kwargs (optional)
     * @return future<result<PyObject*>>
     */
    static future<result<PyObject*>> submit(
        PyObject* callable,
        PyObject* args = nullptr,
        PyObject* kwargs = nullptr
    ) noexcept;

    /**
     * Submit task to specific pinned worker.
     *
     * Useful when you want consistent interpreter affinity.
     *
     * @param worker_id Pinned worker ID (0 to num_pinned_workers-1)
     * @param callable Python callable
     * @param args Arguments
     * @param kwargs Keyword arguments
     * @return future<result<PyObject*>>
     */
    static future<result<PyObject*>> submit_to_pinned(
        uint32_t worker_id,
        PyObject* callable,
        PyObject* args = nullptr,
        PyObject* kwargs = nullptr
    ) noexcept;

    /**
     * Submit task to pooled worker.
     *
     * @param callable Python callable
     * @param args Arguments
     * @param kwargs Keyword arguments
     * @return future<result<PyObject*>>
     */
    static future<result<PyObject*>> submit_to_pooled(
        PyObject* callable,
        PyObject* args = nullptr,
        PyObject* kwargs = nullptr
    ) noexcept;

    /**
     * Get statistics.
     */
    static Stats get_stats() noexcept;

    /**
     * Get configuration.
     */
    static Config get_config() noexcept;

private:
    /**
     * Pinned worker (dedicated interpreter).
     */
    struct PinnedWorker {
        uint32_t id;
        std::unique_ptr<Subinterpreter> interpreter;
        std::thread thread;
        AeronSPSCQueue<Task> task_queue;
        std::atomic<bool> running{true};
        std::atomic<uint64_t> tasks_completed{0};

        explicit PinnedWorker(uint32_t worker_id, uint32_t queue_size)
            : id(worker_id), task_queue(queue_size) {}
    };

    /**
     * Pooled worker (shares interpreters).
     */
    struct PooledWorker {
        uint32_t id;
        std::thread thread;
        AeronSPSCQueue<Task> task_queue;
        std::atomic<bool> running{true};
        std::atomic<uint64_t> tasks_completed{0};

        explicit PooledWorker(uint32_t worker_id, uint32_t queue_size)
            : id(worker_id), task_queue(queue_size) {}
    };

    /**
     * Pooled interpreter state.
     */
    struct PooledInterpreter {
        std::unique_ptr<Subinterpreter> interpreter;
        std::atomic<bool> in_use{false};
        std::atomic<uint64_t> tasks_completed{0};
    };

    // Singleton instance
    static SubinterpreterExecutor* instance_;

    // Configuration
    Config config_;

    // Pinned workers (1:1 worker:interpreter)
    std::vector<std::unique_ptr<PinnedWorker>> pinned_workers_;

    // Pooled workers (N:M workers:interpreters)
    std::vector<std::unique_ptr<PooledWorker>> pooled_workers_;
    std::vector<std::unique_ptr<PooledInterpreter>> pooled_interpreters_;

    // Statistics
    std::atomic<uint64_t> tasks_submitted_{0};
    std::atomic<uint64_t> tasks_completed_{0};
    std::atomic<uint64_t> tasks_failed_{0};
    std::atomic<uint64_t> pinned_tasks_{0};
    std::atomic<uint64_t> pooled_tasks_{0};

    // Lifecycle
    std::atomic<bool> initialized_{false};
    std::atomic<bool> shutting_down_{false};

    // Constructor
    explicit SubinterpreterExecutor(const Config& config);

    // Destructor
    ~SubinterpreterExecutor();

    // Worker thread functions
    static void pinned_worker_loop(PinnedWorker* worker);
    static void pooled_worker_loop(PooledWorker* worker,
                                    std::vector<std::unique_ptr<PooledInterpreter>>* pool);

    // Task execution
    static result<PyObject*> execute_task(Subinterpreter* interp, const Task& task) noexcept;

    // Helper: Get next available pooled interpreter
    static PooledInterpreter* acquire_pooled_interpreter(
        std::vector<std::unique_ptr<PooledInterpreter>>* pool
    ) noexcept;

    static void release_pooled_interpreter(PooledInterpreter* interp) noexcept;
};

} // namespace python
} // namespace fasterapi
