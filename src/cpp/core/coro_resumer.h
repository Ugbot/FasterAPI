#pragma once

#include "async_io.h"
#include "ring_buffer.h"
#include <coroutine>
#include <atomic>
#include <memory>

namespace fasterapi {
namespace core {

/**
 * Coroutine Resumer - Thread-safe coroutine resumption via event loop
 *
 * Allows worker threads to safely queue coroutine handles for resumption
 * from the event loop thread. Solves the threading issue with nghttp2/HTTP2
 * where coroutines were being resumed from Python worker threads.
 *
 * Design:
 * - Lock-free SPSC ring buffer for queuing handles
 * - Integrates with async_io wake() mechanism
 * - Event loop processes queue when woken
 *
 * Usage:
 *   // Setup (event loop thread)
 *   auto resumer = CoroResumer::create(io_engine);
 *
 *   // Queue coroutine (any thread)
 *   resumer->queue(coro_handle);
 */
class CoroResumer {
public:
    /**
     * Create coroutine resumer tied to event loop.
     *
     * @param io Event loop to integrate with
     * @return Unique pointer to resumer
     */
    static std::unique_ptr<CoroResumer> create(async_io* io);

    /**
     * Queue coroutine handle for resumption.
     *
     * Thread-safe. Can be called from any thread.
     * Will wake the event loop to process the queue.
     *
     * @param handle Coroutine to resume
     * @return True if queued successfully
     */
    bool queue(std::coroutine_handle<> handle) noexcept;

    /**
     * Process all queued coroutines.
     *
     * MUST be called from event loop thread only.
     * Automatically called via wake callback.
     *
     * @return Number of coroutines resumed
     */
    size_t process_queue() noexcept;

    /**
     * Get global instance (if set).
     *
     * Allows accessing resumer from awaitable_future without passing it around.
     * Must be set explicitly via set_global().
     */
    static CoroResumer* get_global() noexcept;

    /**
     * Set global instance.
     *
     * Should be called once during server initialization.
     */
    static void set_global(CoroResumer* resumer) noexcept;

private:
    explicit CoroResumer(async_io* io);

    async_io* io_;

    // Lock-free SPSC ring buffer for queuing handles
    // Producer: worker threads calling queue()
    // Consumer: event loop thread calling process_queue()
    SPSCRingBuffer<std::coroutine_handle<>, 1024> pending_queue_;

    std::atomic<uint64_t> queued_count_{0};
    std::atomic<uint64_t> resumed_count_{0};

    static std::atomic<CoroResumer*> global_instance_;
};

// Global instance pointer
inline std::atomic<CoroResumer*> CoroResumer::global_instance_{nullptr};

// Implementation

inline CoroResumer::CoroResumer(async_io* io)
    : io_(io) {

    // Register wake callback to process queue
    io_->set_wake_callback([this]() {
        this->process_queue();
    });
}

inline std::unique_ptr<CoroResumer> CoroResumer::create(async_io* io) {
    return std::unique_ptr<CoroResumer>(new CoroResumer(io));
}

inline bool CoroResumer::queue(std::coroutine_handle<> handle) noexcept {
    if (!handle) return false;

    // Try to enqueue the handle
    if (!pending_queue_.try_write(handle)) {
        // Queue full - this is a problem, but we can't block
        // Could expand queue or log error
        return false;
    }

    queued_count_.fetch_add(1, std::memory_order_relaxed);

    // Wake the event loop to process this handle
    io_->wake();

    return true;
}

inline size_t CoroResumer::process_queue() noexcept {
    size_t count = 0;
    std::coroutine_handle<> handle;

    // Process all pending handles
    while (pending_queue_.try_read(handle)) {
        if (handle && !handle.done()) {
            handle.resume();
            count++;
        }
    }

    resumed_count_.fetch_add(count, std::memory_order_relaxed);
    return count;
}

inline CoroResumer* CoroResumer::get_global() noexcept {
    return global_instance_.load(std::memory_order_acquire);
}

inline void CoroResumer::set_global(CoroResumer* resumer) noexcept {
    global_instance_.store(resumer, std::memory_order_release);
}

} // namespace core
} // namespace fasterapi
