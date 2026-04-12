// Coroutine Frame Pool (Seastar-Inspired)
//
// Pre-allocates coroutine frames to eliminate per-request allocation overhead.
// Coroutines are acquired from the pool and released back when complete.
// Zero allocations in the hot path after initial pool creation.
//
// Design inspired by Seastar's task_quota and coroutine scheduling:
// - Fixed-size frames accommodate typical HTTP handler coroutines
// - Lock-free stack for fast acquire/release from multiple threads
// - ABA-safe with tagged pointers to prevent use-after-free

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace fasterapi {
namespace core {

// Cache line size for alignment (avoid false sharing)
constexpr size_t kCacheLineSize = 64;

// Default coroutine frame size - accommodates typical HTTP handler state
// Includes: TLS socket pointer, buffer pointers, HTTP parser state, response builder
// Tune this based on actual coroutine frame sizes (use -fcoroutines-ts -Wframe-larger-than)
constexpr size_t kDefaultCoroFrameSize = 4096;

// =============================================================================
// Lock-Free Stack (for pool free list)
// =============================================================================

/// ABA-safe lock-free stack using tagged pointers
/// Optimized for pool acquire/release patterns
template <typename T>
class LockFreeStack {
public:
    LockFreeStack() : head_(pack(nullptr, 0)) {}

    ~LockFreeStack() = default;

    // Non-copyable, non-movable
    LockFreeStack(const LockFreeStack&) = delete;
    LockFreeStack& operator=(const LockFreeStack&) = delete;

    /// Push an item onto the stack
    void push(T* item) noexcept {
        uint64_t old_head = head_.load(std::memory_order_relaxed);
        T* old_ptr;
        uint32_t old_tag;

        do {
            unpack(old_head, old_ptr, old_tag);
            item->next_ = old_ptr;
        } while (!head_.compare_exchange_weak(
            old_head, pack(item, old_tag + 1),
            std::memory_order_release,
            std::memory_order_relaxed));
    }

    /// Pop an item from the stack (returns nullptr if empty)
    T* pop() noexcept {
        uint64_t old_head = head_.load(std::memory_order_acquire);
        T* old_ptr;
        uint32_t old_tag;

        do {
            unpack(old_head, old_ptr, old_tag);
            if (old_ptr == nullptr) {
                return nullptr;
            }
        } while (!head_.compare_exchange_weak(
            old_head, pack(old_ptr->next_, old_tag + 1),
            std::memory_order_acquire,
            std::memory_order_relaxed));

        return old_ptr;
    }

    /// Check if stack is empty (approximate)
    bool empty() const noexcept {
        T* ptr;
        uint32_t tag;
        unpack(head_.load(std::memory_order_relaxed), ptr, tag);
        return ptr == nullptr;
    }

private:
    // Pack pointer and tag into 64-bit value
    // Lower 48 bits: pointer (x86-64 canonical addresses)
    // Upper 16 bits: ABA counter
    static uint64_t pack(T* ptr, uint32_t tag) noexcept {
        return (static_cast<uint64_t>(tag) << 48) |
               (reinterpret_cast<uint64_t>(ptr) & 0x0000FFFFFFFFFFFF);
    }

    static void unpack(uint64_t packed, T*& ptr, uint32_t& tag) noexcept {
        tag = static_cast<uint32_t>(packed >> 48);
        // Sign-extend for canonical addresses
        uint64_t raw_ptr = packed & 0x0000FFFFFFFFFFFF;
        if (raw_ptr & 0x0000800000000000) {
            raw_ptr |= 0xFFFF000000000000;
        }
        ptr = reinterpret_cast<T*>(raw_ptr);
    }

    alignas(kCacheLineSize) std::atomic<uint64_t> head_;
};

// =============================================================================
// Pooled Coroutine Frame
// =============================================================================

/// Fixed-size frame for coroutine storage
/// Contains aligned storage for the coroutine state machine
struct PooledCoroFrame {
    // Link for free list (when not in use)
    PooledCoroFrame* next_ = nullptr;

    // Index in pool (for debugging and back-reference)
    uint32_t pool_index_ = 0;

    // Generation counter (for detecting stale references)
    uint32_t generation_ = 0;

    // Aligned storage for coroutine frame
    // The compiler will place the coroutine state machine here
    alignas(std::max_align_t) std::byte storage_[kDefaultCoroFrameSize];

    // Get pointer to storage
    void* data() noexcept { return storage_; }
    const void* data() const noexcept { return storage_; }

    // Check if storage can fit a frame of given size
    static constexpr bool fits(size_t frame_size) noexcept {
        return frame_size <= kDefaultCoroFrameSize;
    }
};

static_assert(sizeof(PooledCoroFrame) <= 4096 + 64,
              "PooledCoroFrame should fit in ~4KB + overhead");

// =============================================================================
// Coroutine Pool
// =============================================================================

/// Pre-allocated pool of coroutine frames
/// Eliminates per-request heap allocation for coroutines
class CoroPool {
public:
    /// Create pool with specified number of frames
    /// @param pool_size Number of frames to pre-allocate
    explicit CoroPool(size_t pool_size)
        : pool_size_(pool_size)
        , allocated_(0)
        , peak_allocated_(0) {
        // Pre-allocate all frames
        frames_.reserve(pool_size);
        for (size_t i = 0; i < pool_size; ++i) {
            auto frame = std::make_unique<PooledCoroFrame>();
            frame->pool_index_ = static_cast<uint32_t>(i);
            frame->generation_ = 0;
            free_list_.push(frame.get());
            frames_.push_back(std::move(frame));
        }
    }

    ~CoroPool() = default;

    // Non-copyable, non-movable
    CoroPool(const CoroPool&) = delete;
    CoroPool& operator=(const CoroPool&) = delete;

    /// Acquire a frame from the pool
    /// @return Frame pointer, or nullptr if pool exhausted
    PooledCoroFrame* acquire() noexcept {
        PooledCoroFrame* frame = free_list_.pop();
        if (frame) {
            frame->generation_++;
            size_t current = allocated_.fetch_add(1, std::memory_order_relaxed) + 1;
            // Update peak (relaxed, for stats only)
            size_t peak = peak_allocated_.load(std::memory_order_relaxed);
            while (current > peak &&
                   !peak_allocated_.compare_exchange_weak(
                       peak, current, std::memory_order_relaxed)) {
            }
        }
        return frame;
    }

    /// Release a frame back to the pool
    /// @param frame Frame to release (must have been acquired from this pool)
    void release(PooledCoroFrame* frame) noexcept {
        if (!frame) return;

        // Verify frame belongs to this pool
        assert(frame->pool_index_ < pool_size_);
        assert(frames_[frame->pool_index_].get() == frame);

        allocated_.fetch_sub(1, std::memory_order_relaxed);
        free_list_.push(frame);
    }

    /// Get number of currently allocated frames
    size_t allocated() const noexcept {
        return allocated_.load(std::memory_order_relaxed);
    }

    /// Get number of available frames
    size_t available() const noexcept {
        return pool_size_ - allocated_.load(std::memory_order_relaxed);
    }

    /// Get total pool capacity
    size_t capacity() const noexcept {
        return pool_size_;
    }

    /// Get peak allocation count
    size_t peak_allocated() const noexcept {
        return peak_allocated_.load(std::memory_order_relaxed);
    }

    /// Check if pool is exhausted
    bool exhausted() const noexcept {
        return allocated_.load(std::memory_order_relaxed) >= pool_size_;
    }

private:
    size_t pool_size_;
    std::vector<std::unique_ptr<PooledCoroFrame>> frames_;
    LockFreeStack<PooledCoroFrame> free_list_;
    alignas(kCacheLineSize) std::atomic<size_t> allocated_;
    alignas(kCacheLineSize) std::atomic<size_t> peak_allocated_;
};

// =============================================================================
// Global Pool Access
// =============================================================================

/// Get the global coroutine pool instance
/// Lazily initialized on first access
CoroPool& global_coro_pool();

/// Initialize global pool with specified size
/// Must be called before any coroutines are created
/// @param pool_size Number of frames to pre-allocate (default: 4096)
void init_global_coro_pool(size_t pool_size = 4096);

// =============================================================================
// Pool-Backed Promise Allocator
// =============================================================================

/// Custom allocator for coroutine promises that uses the pool
/// Add this as a base class to your promise_type to enable pooling
///
/// Usage:
///   struct my_promise : public PooledPromiseBase<sizeof(my_coroutine_frame)> {
///       // ... rest of promise implementation
///   };
///
template <size_t FrameSize>
struct PooledPromiseBase {
    static_assert(PooledCoroFrame::fits(FrameSize),
                  "Coroutine frame too large for pool");

    /// Allocate from pool
    static void* operator new(size_t size) {
        if (size > kDefaultCoroFrameSize) {
            // Frame too large, fall back to heap (should not happen if static_assert passes)
            return ::operator new(size);
        }

        PooledCoroFrame* frame = global_coro_pool().acquire();
        if (!frame) {
            // Pool exhausted, fall back to heap allocation
            // This should be rare in well-sized pools
            return ::operator new(size);
        }

        return frame->data();
    }

    /// Return to pool
    static void operator delete(void* ptr, size_t size) {
        if (!ptr) return;

        // Check if this came from the pool by checking if ptr points into a frame's storage
        // The frame is at a fixed offset before the storage
        constexpr size_t storage_offset = offsetof(PooledCoroFrame, storage_);
        auto* frame = reinterpret_cast<PooledCoroFrame*>(
            static_cast<std::byte*>(ptr) - storage_offset);

        // Verify this looks like a pool frame (basic sanity check)
        // A more robust check would use a magic number or pool bounds checking
        if (frame->pool_index_ < global_coro_pool().capacity()) {
            global_coro_pool().release(frame);
        } else {
            // Not from pool, use regular delete
            ::operator delete(ptr);
        }
    }
};

// =============================================================================
// RAII Frame Guard
// =============================================================================

/// RAII wrapper for automatic frame release
class FrameGuard {
public:
    explicit FrameGuard(PooledCoroFrame* frame, CoroPool& pool)
        : frame_(frame), pool_(pool) {}

    ~FrameGuard() {
        if (frame_) {
            pool_.release(frame_);
        }
    }

    // Move-only
    FrameGuard(FrameGuard&& other) noexcept
        : frame_(std::exchange(other.frame_, nullptr)), pool_(other.pool_) {}

    FrameGuard& operator=(FrameGuard&& other) noexcept {
        if (this != &other) {
            if (frame_) {
                pool_.release(frame_);
            }
            frame_ = std::exchange(other.frame_, nullptr);
        }
        return *this;
    }

    FrameGuard(const FrameGuard&) = delete;
    FrameGuard& operator=(const FrameGuard&) = delete;

    PooledCoroFrame* get() const noexcept { return frame_; }
    PooledCoroFrame* release() noexcept { return std::exchange(frame_, nullptr); }

private:
    PooledCoroFrame* frame_;
    CoroPool& pool_;
};

}  // namespace core
}  // namespace fasterapi
