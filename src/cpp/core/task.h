#pragma once

#include <cstdint>
#include <atomic>

namespace fasterapi {
namespace core {

/**
 * Task abstraction for continuations.
 * 
 * A task represents a unit of work that can be scheduled on the reactor.
 * Tasks form the building blocks of future continuations.
 */
class task {
public:
    virtual ~task() = default;
    
    /**
     * Execute the task.
     */
    virtual void run() noexcept = 0;
    
    /**
     * Get task priority (lower = higher priority).
     */
    virtual uint32_t priority() const noexcept { return 0; }
};

/**
 * Lambda task wrapper.
 * 
 * Wraps a lambda/function object as a task.
 */
template<typename Func>
class lambda_task final : public task {
public:
    explicit lambda_task(Func&& f) : func_(static_cast<Func&&>(f)) {}
    
    void run() noexcept override {
        func_();
    }
    
private:
    Func func_;
};

/**
 * Make a task from a callable.
 */
template<typename Func>
inline task* make_task(Func&& f) {
    return new lambda_task<Func>(static_cast<Func&&>(f));
}

} // namespace core
} // namespace fasterapi

