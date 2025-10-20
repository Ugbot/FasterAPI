#pragma once

#include <cstdint>
#include <atomic>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace fasterapi {
namespace core {

// Forward declarations
class task;
class reactor;

// Custom deleter for reactor
struct reactor_deleter {
    void operator()(reactor* r) const noexcept;
};

/**
 * Per-core event loop with task queue.
 * 
 * The reactor manages:
 * - Task scheduling and execution
 * - I/O event polling (epoll/kqueue/IOCP)
 * - Timer management
 * - Per-core thread affinity
 * 
 * Design inspired by Seastar's reactor pattern.
 */
class reactor {
public:
    /**
     * Get the reactor for the current thread.
     * 
     * Each thread has its own reactor instance.
     */
    static reactor& local();
    
    /**
     * Initialize reactor subsystem.
     * 
     * @param num_cores Number of reactor cores to create (0 = auto-detect)
     * @return 0 on success, error code otherwise
     */
    static int initialize(uint32_t num_cores = 0) noexcept;
    
    /**
     * Shutdown reactor subsystem.
     * 
     * @return 0 on success
     */
    static int shutdown() noexcept;
    
    /**
     * Get reactor for a specific core.
     * 
     * @param core_id Core ID (0 to num_cores - 1)
     * @return Reactor instance or nullptr if invalid core_id
     */
    static reactor* get(uint32_t core_id) noexcept;
    
    /**
     * Get current core ID.
     * 
     * @return Core ID for current thread
     */
    static uint32_t current_core() noexcept;
    
    /**
     * Get total number of cores.
     */
    static uint32_t num_cores() noexcept;
    
    /**
     * Schedule a task to run on this reactor.
     * 
     * Task will be executed on the reactor's thread.
     * Thread-safe (can be called from any thread).
     * 
     * @param t Task to schedule (reactor takes ownership)
     */
    void schedule(task* t) noexcept;
    
    /**
     * Run the reactor event loop.
     * 
     * Processes tasks and I/O events until stopped.
     * Should be called from reactor's dedicated thread.
     */
    void run() noexcept;
    
    /**
     * Stop the reactor.
     * 
     * Thread-safe.
     */
    void stop() noexcept;
    
    /**
     * Check if reactor is running.
     */
    bool is_running() const noexcept;
    
    /**
     * Get epoll/kqueue file descriptor for I/O integration.
     * 
     * @return Event loop FD, or -1 if not available
     */
    int event_fd() const noexcept;
    
    /**
     * Add a timer.
     * 
     * @param when_ns Absolute time in nanoseconds (from epoch)
     * @param t Task to execute (reactor takes ownership)
     * @return Timer ID, or 0 on error
     */
    uint64_t add_timer(uint64_t when_ns, task* t) noexcept;
    
    /**
     * Cancel a timer.
     * 
     * @param timer_id Timer ID from add_timer()
     * @return 0 on success
     */
    int cancel_timer(uint64_t timer_id) noexcept;
    
    /**
     * Get current time in nanoseconds.
     */
    static uint64_t now_ns() noexcept;
    
    /**
     * Get reactor statistics.
     */
    struct stats {
        uint64_t tasks_executed{0};
        uint64_t tasks_pending{0};
        uint64_t io_events{0};
        uint64_t timers_fired{0};
        uint64_t loops{0};
    };
    
    stats get_stats() const noexcept;
    
    // Non-copyable, non-movable
    reactor(const reactor&) = delete;
    reactor& operator=(const reactor&) = delete;

private:
    friend struct reactor_deleter;
    reactor(uint32_t core_id);
    ~reactor();
    
    uint32_t core_id_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    
    // Task queue (lock-free for same-core scheduling)
    struct task_queue;
    std::unique_ptr<task_queue> task_queue_;
    
    // Timer management
    struct timer_queue;
    std::unique_ptr<timer_queue> timer_queue_;
    
    // I/O event loop
    int event_fd_{-1};
    
    // Statistics
    std::atomic<uint64_t> tasks_executed_{0};
    std::atomic<uint64_t> io_events_{0};
    std::atomic<uint64_t> timers_fired_{0};
    std::atomic<uint64_t> loops_{0};
    
    // Process pending tasks
    void process_tasks() noexcept;
    
    // Process I/O events
    void process_io_events(int timeout_ms) noexcept;
    
    // Process timers
    void process_timers() noexcept;
};

} // namespace core
} // namespace fasterapi

