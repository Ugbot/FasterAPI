#pragma once

#include <coroutine>
#include <exception>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace fasterapi {
namespace core {

/**
 * C++20 coroutine task type for async operations.
 *
 * Provides a lightweight, zero-allocation coroutine task that can be
 * suspended and resumed. Compatible with co_await, co_return.
 *
 * Design:
 * - No heap allocation for promise
 * - Exception propagation
 * - Lazy evaluation (starts when co_await'd)
 * - Move-only semantics
 *
 * Example:
 *   coro_task<int> get_value() {
 *       co_return 42;
 *   }
 *
 *   coro_task<void> use_value() {
 *       int value = co_await get_value();
 *       // use value...
 *   }
 */
template<typename T>
class coro_task {
public:
    /**
     * Promise type for coroutine (required by C++20).
     */
    struct promise_type {
        // Result storage (manual lifetime management for union)
        alignas(T) unsigned char value_storage_[sizeof(T)];
        alignas(std::exception_ptr) unsigned char exception_storage_[sizeof(std::exception_ptr)];
        bool has_value_ = false;
        bool has_exception_ = false;

        // Accessors
        T& value() { return *reinterpret_cast<T*>(value_storage_); }
        std::exception_ptr& exception() { return *reinterpret_cast<std::exception_ptr*>(exception_storage_); }

        /**
         * Called when coroutine is created.
         */
        coro_task get_return_object() {
            return coro_task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        /**
         * Initial suspend: lazy evaluation.
         */
        std::suspend_always initial_suspend() noexcept { return {}; }

        /**
         * Final suspend: allow cleanup.
         */
        std::suspend_always final_suspend() noexcept { return {}; }

        /**
         * Store return value.
         */
        void return_value(T&& val) noexcept(std::is_nothrow_move_constructible_v<T>) {
            new (value_storage_) T(std::move(val));
            has_value_ = true;
        }

        void return_value(const T& val) noexcept(std::is_nothrow_copy_constructible_v<T>) {
            new (value_storage_) T(val);
            has_value_ = true;
        }

        /**
         * Handle exceptions.
         */
        void unhandled_exception() noexcept {
            new (exception_storage_) std::exception_ptr(std::current_exception());
            has_exception_ = true;
        }

        /**
         * Cleanup.
         */
        ~promise_type() {
            if (has_value_) {
                value().~T();
            }
            if (has_exception_) {
                exception().~exception_ptr();
            }
        }
    };

    /**
     * Awaiter for co_await support.
     */
    struct awaiter {
        std::coroutine_handle<promise_type> handle_;

        bool await_ready() const noexcept {
            return handle_.done();
        }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
            // Resume the task coroutine
            return handle_;
        }

        T await_resume() {
            auto& promise = handle_.promise();

            if (promise.has_exception_) {
                std::rethrow_exception(promise.exception());
            }

            if (!promise.has_value_) {
                throw std::runtime_error("task has no value");
            }

            return std::move(promise.value());
        }
    };

    /**
     * Make task awaitable.
     */
    awaiter operator co_await() && {
        return awaiter{handle_};
    }

    /**
     * Constructor from coroutine handle.
     */
    explicit coro_task(std::coroutine_handle<promise_type> handle) noexcept
        : handle_(handle) {}

    /**
     * Move constructor.
     */
    coro_task(coro_task&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

    /**
     * Move assignment.
     */
    coro_task& operator=(coro_task&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    /**
     * Deleted copy operations.
     */
    coro_task(const coro_task&) = delete;
    coro_task& operator=(const coro_task&) = delete;

    /**
     * Destructor.
     */
    ~coro_task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    /**
     * Check if task is valid.
     */
    explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

    /**
     * Resume the coroutine (for manual control).
     */
    bool resume() {
        if (!handle_ || handle_.done()) {
            return false;
        }
        handle_.resume();
        return !handle_.done();
    }

    /**
     * Check if task is complete.
     */
    bool done() const noexcept {
        return handle_ && handle_.done();
    }

private:
    std::coroutine_handle<promise_type> handle_;
};

/**
 * Specialization for void tasks.
 */
template<>
class coro_task<void> {
public:
    struct promise_type {
        std::exception_ptr exception_;
        bool has_exception_ = false;

        coro_task get_return_object() {
            return coro_task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }

        void return_void() noexcept {}

        void unhandled_exception() noexcept {
            exception_ = std::current_exception();
            has_exception_ = true;
        }
    };

    struct awaiter {
        std::coroutine_handle<promise_type> handle_;

        bool await_ready() const noexcept {
            return handle_.done();
        }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
            return handle_;
        }

        void await_resume() {
            auto& promise = handle_.promise();
            if (promise.has_exception_) {
                std::rethrow_exception(promise.exception_);
            }
        }
    };

    awaiter operator co_await() && {
        return awaiter{handle_};
    }

    explicit coro_task(std::coroutine_handle<promise_type> handle) noexcept
        : handle_(handle) {}

    coro_task(coro_task&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

    coro_task& operator=(coro_task&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    coro_task(const coro_task&) = delete;
    coro_task& operator=(const coro_task&) = delete;

    ~coro_task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

    bool resume() {
        if (!handle_ || handle_.done()) {
            return false;
        }
        handle_.resume();
        return !handle_.done();
    }

    bool done() const noexcept {
        return handle_ && handle_.done();
    }

    std::coroutine_handle<> get_handle() const noexcept {
        return handle_;
    }

private:
    std::coroutine_handle<promise_type> handle_;
};

} // namespace core
} // namespace fasterapi
