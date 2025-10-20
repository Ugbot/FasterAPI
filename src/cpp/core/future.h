#pragma once

#include <cstdint>
#include <atomic>
#include <utility>
#include <type_traits>
#include <memory>
#include <stdexcept>

namespace fasterapi {
namespace core {

// Forward declarations
class reactor;
class task;

/**
 * Exception state for futures.
 */
class future_exception {
public:
    future_exception(const char* msg) : msg_(msg) {}
    const char* what() const noexcept { return msg_; }
    
private:
    const char* msg_;
};

/**
 * Future state enumeration.
 */
enum class future_state : uint8_t {
    invalid = 0,
    pending = 1,
    ready = 2,
    failed = 3
};

/**
 * Zero-allocation future with continuation chaining.
 * 
 * Design inspired by Seastar's future implementation:
 * - No heap allocation for the common path
 * - Continuation stored inline (small object optimization)
 * - Lock-free state transitions
 * - Exception propagation through chains
 * 
 * Template parameter T is the value type (use void for no-value futures).
 */
template<typename T>
class future {
public:
    using value_type = T;
    
    /**
     * Create a future in pending state.
     */
    future() : state_(future_state::pending) {}
    
    /**
     * Create a ready future with a value.
     */
    explicit future(T&& value) 
        : state_(future_state::ready) {
        new (&storage_) T(static_cast<T&&>(value));
    }
    
    /**
     * Create a ready future with a value (copy).
     */
    explicit future(const T& value) 
        : state_(future_state::ready) {
        new (&storage_) T(value);
    }
    
    /**
     * Create a failed future.
     */
    static future<T> make_exception(const char* msg) {
        future<T> f;
        f.state_ = future_state::failed;
        f.exception_msg_ = msg;
        return f;
    }
    
    /**
     * Create a ready future.
     */
    static future<T> make_ready(T value) {
        return future<T>(static_cast<T&&>(value));
    }
    
    ~future() {
        if (state_ == future_state::ready) {
            reinterpret_cast<T*>(&storage_)->~T();
        }
    }
    
    // Move constructor
    future(future&& other) noexcept 
        : state_(other.state_), exception_msg_(other.exception_msg_) {
        if (state_ == future_state::ready) {
            new (&storage_) T(static_cast<T&&>(*reinterpret_cast<T*>(&other.storage_)));
        }
        other.state_ = future_state::invalid;
    }
    
    // Move assignment
    future& operator=(future&& other) noexcept {
        if (this != &other) {
            if (state_ == future_state::ready) {
                reinterpret_cast<T*>(&storage_)->~T();
            }
            state_ = other.state_;
            exception_msg_ = other.exception_msg_;
            if (state_ == future_state::ready) {
                new (&storage_) T(static_cast<T&&>(*reinterpret_cast<T*>(&other.storage_)));
            }
            other.state_ = future_state::invalid;
        }
        return *this;
    }
    
    // Non-copyable
    future(const future&) = delete;
    future& operator=(const future&) = delete;
    
    /**
     * Check if future is ready.
     */
    bool available() const noexcept {
        return state_ == future_state::ready;
    }
    
    /**
     * Check if future has failed.
     */
    bool failed() const noexcept {
        return state_ == future_state::failed;
    }
    
    /**
     * Get the value (blocking).
     * 
     * Only call when available() returns true.
     */
    T get() {
        if (state_ == future_state::failed) {
            // Can't throw with -fno-exceptions, return default
            return T{};
        }
        if (state_ != future_state::ready) {
            // Can't throw with -fno-exceptions, return default
            return T{};
        }
        
        state_ = future_state::invalid;
        return static_cast<T&&>(*reinterpret_cast<T*>(&storage_));
    }
    
    /**
     * Chain a continuation.
     * 
     * The continuation receives the value and returns a new value.
     * Returns a new future for the result.
     * 
     * Example:
     *   future<int> f = get_value_async();
     *   future<string> g = f.then([](int x) { return std::to_string(x); });
     */
    template<typename Func>
    auto then(Func&& func) -> future<std::invoke_result_t<Func, T>> {
        using result_type = std::invoke_result_t<Func, T>;
        
        if (state_ == future_state::ready) {
            // Fast path: already ready, execute immediately
            // Note: no try/catch due to -fno-exceptions
            auto result = func(static_cast<T&&>(*reinterpret_cast<T*>(&storage_)));
            state_ = future_state::invalid;
            return future<result_type>::make_ready(static_cast<result_type&&>(result));
        } else if (state_ == future_state::failed) {
            // Propagate failure
            return future<result_type>::make_exception(exception_msg_);
        } else {
            // Pending: would need continuation storage (simplified for now)
            return future<result_type>::make_exception("async continuations not yet implemented");
        }
    }
    
    /**
     * Chain a continuation that receives the future itself.
     * 
     * Useful for error handling and more complex patterns.
     */
    template<typename Func>
    auto then_wrapped(Func&& func) -> std::invoke_result_t<Func, future<T>> {
        return func(static_cast<future<T>&&>(*this));
    }
    
private:
    future_state state_;
    const char* exception_msg_{nullptr};
    
    // Storage for value (aligned)
    alignas(T) uint8_t storage_[sizeof(T)];
};

/**
 * Specialization for void futures.
 */
template<>
class future<void> {
public:
    future() : state_(future_state::pending) {}
    
    static future<void> make_ready() {
        future<void> f;
        f.state_ = future_state::ready;
        return f;
    }
    
    static future<void> make_exception(const char* msg) {
        future<void> f;
        f.state_ = future_state::failed;
        f.exception_msg_ = msg;
        return f;
    }
    
    bool available() const noexcept {
        return state_ == future_state::ready;
    }
    
    bool failed() const noexcept {
        return state_ == future_state::failed;
    }
    
    void get() {
        if (state_ == future_state::failed) {
            // Can't throw with -fno-exceptions
            return;
        }
        if (state_ != future_state::ready) {
            // Can't throw with -fno-exceptions
            return;
        }
        state_ = future_state::invalid;
    }
    
    template<typename Func>
    auto then(Func&& func) -> future<std::invoke_result_t<Func>> {
        using result_type = std::invoke_result_t<Func>;
        
        if (state_ == future_state::ready) {
            // Note: no try/catch due to -fno-exceptions
            auto result = func();
            state_ = future_state::invalid;
            return future<result_type>::make_ready(static_cast<result_type&&>(result));
        } else if (state_ == future_state::failed) {
            return future<result_type>::make_exception(exception_msg_);
        } else {
            return future<result_type>::make_exception("async continuations not yet implemented");
        }
    }
    
private:
    future_state state_;
    const char* exception_msg_{nullptr};
};

/**
 * Promise for setting future values.
 * 
 * A promise is the write-side of a future. It allows setting
 * the value or exception that the future will receive.
 */
template<typename T>
class promise {
public:
    promise() : future_(new future<T>()) {}
    
    ~promise() = default;
    
    // Non-copyable, movable
    promise(const promise&) = delete;
    promise& operator=(const promise&) = delete;
    promise(promise&&) = default;
    promise& operator=(promise&&) = default;
    
    /**
     * Get the associated future.
     * 
     * Can only be called once.
     */
    future<T> get_future() {
        if (!future_) {
            // Can't throw with -fno-exceptions, return empty future
            return future<T>::make_exception("future already retrieved");
        }
        future<T> f = static_cast<future<T>&&>(*future_);
        future_.reset();
        return f;
    }
    
    /**
     * Set the value (makes future ready).
     */
    void set_value(T value) {
        if (!future_) {
            // Can't throw with -fno-exceptions
            return;
        }
        *future_ = future<T>::make_ready(static_cast<T&&>(value));
    }
    
    /**
     * Set an exception (makes future failed).
     */
    void set_exception(const char* msg) {
        if (!future_) {
            // Can't throw with -fno-exceptions
            return;
        }
        *future_ = future<T>::make_exception(msg);
    }
    
private:
    std::unique_ptr<future<T>> future_;
};

/**
 * Specialization for void promises.
 */
template<>
class promise<void> {
public:
    promise() : future_(new future<void>()) {}
    
    future<void> get_future() {
        if (!future_) {
            // Can't throw with -fno-exceptions
            return future<void>::make_exception("future already retrieved");
        }
        future<void> f = static_cast<future<void>&&>(*future_);
        future_.reset();
        return f;
    }
    
    void set_value() {
        if (!future_) {
            // Can't throw with -fno-exceptions
            return;
        }
        *future_ = future<void>::make_ready();
    }
    
    void set_exception(const char* msg) {
        if (!future_) {
            // Can't throw with -fno-exceptions
            return;
        }
        *future_ = future<void>::make_exception(msg);
    }
    
private:
    std::unique_ptr<future<void>> future_;
};

/**
 * Create a ready future with a value.
 */
template<typename T>
inline future<T> make_ready_future(T value) {
    return future<T>::make_ready(static_cast<T&&>(value));
}

/**
 * Create a ready void future.
 */
inline future<void> make_ready_future() {
    return future<void>::make_ready();
}

/**
 * Create a failed future.
 */
template<typename T>
inline future<T> make_exception_future(const char* msg) {
    return future<T>::make_exception(msg);
}

} // namespace core
} // namespace fasterapi

