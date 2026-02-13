// Worker Thread Pool for Coroutine Execution
//
// Fixed-size thread pool that executes coroutines from a work queue.
// Designed for the coroutine-on-threadpool architecture where:
// - 1-2 event loops dispatch I/O events
// - N worker threads execute coroutines from the pool
//
// Features:
// - Lock-free MPMC task queue
// - Work stealing for load balancing (future)
// - Blocking work offload to separate pool
// - Integration with CoroPool for zero-allocation execution

#pragma once

#include "coro_pool.h"
#include "coro_task.h"
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace fasterapi {
namespace core {

// =============================================================================
// MPMC Bounded Queue (for coroutine handle submission)
// =============================================================================

/// Bounded MPMC queue for coroutine handles
/// Uses sequence numbers for lock-free operation
template <size_t Capacity = 4096>
class MPMCQueue {
    static_assert(Capacity > 0, "Capacity must be positive");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

public:
    using ValueType = std::coroutine_handle<>;

    MPMCQueue() : head_(0), tail_(0) {
        for (size_t i = 0; i < Capacity; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    ~MPMCQueue() = default;

    // Non-copyable, non-movable
    MPMCQueue(const MPMCQueue&) = delete;
    MPMCQueue& operator=(const MPMCQueue&) = delete;

    /// Try to push (multiple producers)
    bool try_push(ValueType handle) noexcept {
        Cell* cell;
        size_t pos = tail_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &buffer_[pos & kMask];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // Queue full
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }

        cell->data = handle;
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    /// Try to pop (multiple consumers)
    std::optional<ValueType> try_pop() noexcept {
        Cell* cell;
        size_t pos = head_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &buffer_[pos & kMask];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return std::nullopt;  // Queue empty
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }

        ValueType data = cell->data;
        cell->sequence.store(pos + Capacity, std::memory_order_release);
        return data;
    }

    /// Check if empty (approximate)
    bool empty() const noexcept {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_relaxed);
        return head >= tail;
    }

    /// Get approximate size
    size_t size() const noexcept {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_relaxed);
        return tail > head ? tail - head : 0;
    }

private:
    static constexpr size_t kMask = Capacity - 1;

    struct Cell {
        std::atomic<size_t> sequence;
        ValueType data;
    };

    alignas(kCacheLineSize) std::atomic<size_t> head_;
    alignas(kCacheLineSize) std::atomic<size_t> tail_;
    alignas(kCacheLineSize) Cell buffer_[Capacity];
};

// =============================================================================
// Worker Thread Pool
// =============================================================================

/// Configuration for WorkerThreadPool
struct WorkerPoolConfig {
    size_t num_workers = 0;           // 0 = hardware_concurrency
    size_t task_queue_size = 4096;    // Bounded queue capacity
    size_t blocking_workers = 2;      // Threads for blocking operations
    bool enable_work_stealing = false; // Future: work stealing between workers
};

/// Thread pool that executes coroutine handles
class WorkerThreadPool {
public:
    /// Create pool with configuration
    explicit WorkerThreadPool(const WorkerPoolConfig& config = {});

    /// Create pool with simple worker count
    explicit WorkerThreadPool(size_t num_workers);

    ~WorkerThreadPool();

    // Non-copyable, non-movable
    WorkerThreadPool(const WorkerThreadPool&) = delete;
    WorkerThreadPool& operator=(const WorkerThreadPool&) = delete;

    /// Start the worker threads
    void start();

    /// Stop the pool (waits for workers to finish)
    void stop();

    /// Submit a coroutine handle for execution
    /// Returns true if submitted, false if queue full
    bool submit(std::coroutine_handle<> handle) noexcept;

    /// Submit and block until space available
    void submit_wait(std::coroutine_handle<> handle);

    /// Submit blocking work (file I/O, DNS lookup, etc.)
    /// Returns a future that will hold the result
    template <typename F>
    auto submit_blocking(F&& func) -> std::future<std::invoke_result_t<F>>;

    /// Get number of worker threads
    size_t num_workers() const noexcept { return workers_.size(); }

    /// Get number of pending tasks (approximate)
    size_t pending_tasks() const noexcept { return task_queue_.size(); }

    /// Check if pool is running
    bool is_running() const noexcept {
        return running_.load(std::memory_order_relaxed);
    }

    /// Statistics
    struct Stats {
        uint64_t tasks_executed;
        uint64_t tasks_submitted;
        uint64_t blocking_tasks_executed;
    };

    Stats get_stats() const noexcept;

private:
    void worker_loop(size_t worker_id);
    void blocking_worker_loop(size_t worker_id);

    WorkerPoolConfig config_;
    std::vector<std::thread> workers_;
    std::vector<std::thread> blocking_workers_;

    MPMCQueue<4096> task_queue_;

    // Blocking task queue (with mutex since it's not hot path)
    std::mutex blocking_mutex_;
    std::condition_variable blocking_cv_;
    std::vector<std::function<void()>> blocking_tasks_;

    // Control
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_requested_{false};

    // Stats
    alignas(kCacheLineSize) std::atomic<uint64_t> tasks_executed_{0};
    alignas(kCacheLineSize) std::atomic<uint64_t> tasks_submitted_{0};
    alignas(kCacheLineSize) std::atomic<uint64_t> blocking_tasks_executed_{0};
};

// =============================================================================
// Implementation
// =============================================================================

inline WorkerThreadPool::WorkerThreadPool(const WorkerPoolConfig& config)
    : config_(config) {
    if (config_.num_workers == 0) {
        config_.num_workers = std::thread::hardware_concurrency();
        if (config_.num_workers == 0) {
            config_.num_workers = 4;  // Fallback
        }
    }
}

inline WorkerThreadPool::WorkerThreadPool(size_t num_workers)
    : WorkerThreadPool(WorkerPoolConfig{.num_workers = num_workers}) {}

inline WorkerThreadPool::~WorkerThreadPool() {
    stop();
}

inline void WorkerThreadPool::start() {
    if (running_.exchange(true)) {
        return;  // Already running
    }

    shutdown_requested_.store(false, std::memory_order_relaxed);

    // Start main workers
    workers_.reserve(config_.num_workers);
    for (size_t i = 0; i < config_.num_workers; ++i) {
        workers_.emplace_back(&WorkerThreadPool::worker_loop, this, i);
    }

    // Start blocking workers
    blocking_workers_.reserve(config_.blocking_workers);
    for (size_t i = 0; i < config_.blocking_workers; ++i) {
        blocking_workers_.emplace_back(&WorkerThreadPool::blocking_worker_loop, this, i);
    }
}

inline void WorkerThreadPool::stop() {
    if (!running_.exchange(false)) {
        return;  // Not running
    }

    shutdown_requested_.store(true, std::memory_order_release);

    // Wake blocking workers
    blocking_cv_.notify_all();

    // Join all workers
    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    workers_.clear();

    for (auto& t : blocking_workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    blocking_workers_.clear();
}

inline bool WorkerThreadPool::submit(std::coroutine_handle<> handle) noexcept {
    if (!handle || shutdown_requested_.load(std::memory_order_relaxed)) {
        return false;
    }

    if (task_queue_.try_push(handle)) {
        tasks_submitted_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

inline void WorkerThreadPool::submit_wait(std::coroutine_handle<> handle) {
    while (!submit(handle)) {
        if (shutdown_requested_.load(std::memory_order_relaxed)) {
            return;
        }
        std::this_thread::yield();
    }
}

template <typename F>
auto WorkerThreadPool::submit_blocking(F&& func) -> std::future<std::invoke_result_t<F>> {
    using ReturnType = std::invoke_result_t<F>;
    auto task = std::make_shared<std::packaged_task<ReturnType()>>(std::forward<F>(func));
    auto future = task->get_future();

    {
        std::lock_guard<std::mutex> lock(blocking_mutex_);
        blocking_tasks_.emplace_back([task]() { (*task)(); });
    }
    blocking_cv_.notify_one();

    return future;
}

inline WorkerThreadPool::Stats WorkerThreadPool::get_stats() const noexcept {
    return Stats{
        .tasks_executed = tasks_executed_.load(std::memory_order_relaxed),
        .tasks_submitted = tasks_submitted_.load(std::memory_order_relaxed),
        .blocking_tasks_executed = blocking_tasks_executed_.load(std::memory_order_relaxed),
    };
}

inline void WorkerThreadPool::worker_loop(size_t /*worker_id*/) {
    while (!shutdown_requested_.load(std::memory_order_acquire)) {
        auto handle_opt = task_queue_.try_pop();
        if (handle_opt) {
            auto handle = *handle_opt;
            if (handle && !handle.done()) {
                handle.resume();
                tasks_executed_.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            // No work - brief pause then check again
            // In production, would use futex or similar for wake-up
            std::this_thread::yield();
        }
    }

    // Drain remaining tasks on shutdown
    while (auto handle_opt = task_queue_.try_pop()) {
        auto handle = *handle_opt;
        if (handle && !handle.done()) {
            handle.resume();
            tasks_executed_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

inline void WorkerThreadPool::blocking_worker_loop(size_t /*worker_id*/) {
    while (!shutdown_requested_.load(std::memory_order_acquire)) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(blocking_mutex_);
            blocking_cv_.wait(lock, [this]() {
                return shutdown_requested_.load(std::memory_order_relaxed) ||
                       !blocking_tasks_.empty();
            });

            if (shutdown_requested_.load(std::memory_order_relaxed) &&
                blocking_tasks_.empty()) {
                return;
            }

            if (!blocking_tasks_.empty()) {
                task = std::move(blocking_tasks_.back());
                blocking_tasks_.pop_back();
            }
        }

        if (task) {
            task();
            blocking_tasks_executed_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// =============================================================================
// Global Pool Access
// =============================================================================

/// Get the global worker pool instance
WorkerThreadPool& global_worker_pool();

/// Initialize global worker pool
/// Must be called before submitting work
void init_global_worker_pool(const WorkerPoolConfig& config = {});

}  // namespace core
}  // namespace fasterapi
