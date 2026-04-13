/**
 * FasterAPI IOCP UDP Socket - Async UDP via Windows I/O Completion Ports
 *
 * Proactor-based asynchronous UDP for Windows. All I/O is initiated with
 * overlapped WSARecvFrom / WSASendTo and completed through the IOCP handle.
 *
 * Design:
 * - Pre-allocated operation pool (no malloc/free in hot path)
 * - Lock-free operation acquisition via atomic index scan
 * - Multiple outstanding recv operations for throughput (recv depth)
 * - Integrates with C++20 coroutines via awaitable send/recv
 * - Ring buffer for completed datagrams
 *
 * Usage:
 *   auto iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
 *   UdpSocket sock(false);
 *   sock.bind("0.0.0.0", 443);
 *
 *   IOCPUdpContext ctx(iocp, sock.native_handle());
 *   ctx.start_receiving();  // Posts recv_depth overlapped recvs
 *
 *   // In event loop: completions arrive via GetQueuedCompletionStatusEx
 *   // Process them with ctx.process_completion(overlapped, bytes, error)
 */

#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <atomic>
#include <array>
#include <functional>
#include <coroutine>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

namespace fasterapi {
namespace net {

// --------------------------------------------------------------------------
// Constants
// --------------------------------------------------------------------------

/** Maximum UDP datagram size. Matches typical QUIC/UDP MTU ceiling. */
constexpr size_t UDP_IOCP_BUFFER_SIZE = 65536;

/** Default number of pre-posted recv operations for throughput. */
constexpr uint32_t UDP_IOCP_DEFAULT_RECV_DEPTH = 16;

/** Total size of the operation pool. Must be power-of-two for fast modulo. */
constexpr uint32_t UDP_IOCP_POOL_SIZE = 512;

static_assert((UDP_IOCP_POOL_SIZE & (UDP_IOCP_POOL_SIZE - 1)) == 0,
              "Pool size must be power of two");

// --------------------------------------------------------------------------
// Operation types
// --------------------------------------------------------------------------

enum class UdpIOCPOpType : uint8_t {
    NONE = 0,
    RECV_FROM,
    SEND_TO
};

// --------------------------------------------------------------------------
// Per-operation overlapped context
// --------------------------------------------------------------------------

/**
 * Extended OVERLAPPED for a single UDP async operation.
 *
 * OVERLAPPED must be the first member so CONTAINING_RECORD works.
 * The buffer is inline to avoid a pointer chase and to keep the
 * entire operation in a contiguous allocation.
 */
struct alignas(64) UdpIOCPOperation {
    OVERLAPPED          overlapped;
    UdpIOCPOpType       op_type;
    WSABUF              wsabuf;
    DWORD               flags;
    DWORD               bytes_transferred;

    // Source / destination address storage (large enough for IPv6)
    struct sockaddr_storage remote_addr;
    INT                 remote_addr_len;

    // Pool tracking - atomic for lock-free acquire/release
    std::atomic<bool>   in_use;

    // Inline buffer -- no allocation
    alignas(16) char    buffer[UDP_IOCP_BUFFER_SIZE];

    UdpIOCPOperation() noexcept {
        reset();
    }

    void reset() noexcept {
        std::memset(&overlapped, 0, sizeof(OVERLAPPED));
        op_type = UdpIOCPOpType::NONE;
        wsabuf.buf = buffer;
        wsabuf.len = UDP_IOCP_BUFFER_SIZE;
        flags = 0;
        bytes_transferred = 0;
        std::memset(&remote_addr, 0, sizeof(remote_addr));
        remote_addr_len = sizeof(remote_addr);
        in_use.store(false, std::memory_order_release);
    }
};

// --------------------------------------------------------------------------
// Lock-free operation pool
// --------------------------------------------------------------------------

/**
 * Pre-allocated pool of UdpIOCPOperation objects.
 *
 * Acquire scans from a rotating hint index using atomic CAS on each slot's
 * in_use flag. This avoids any mutex and distributes contention across slots.
 *
 * The pool is intentionally over-sized relative to recv_depth so that
 * send operations always find a free slot without blocking.
 */
class UdpIOCPOperationPool {
public:
    UdpIOCPOperationPool() noexcept
        : hint_(0)
    {
        // All slots start as not-in-use (constructor of UdpIOCPOperation handles this)
    }

    /**
     * Acquire a free operation from the pool.
     * Returns nullptr if pool is exhausted (caller must handle).
     * Lock-free: uses atomic CAS per slot.
     */
    UdpIOCPOperation* acquire() noexcept {
        uint32_t start = hint_.fetch_add(1, std::memory_order_relaxed) & (UDP_IOCP_POOL_SIZE - 1);
        for (uint32_t i = 0; i < UDP_IOCP_POOL_SIZE; ++i) {
            uint32_t idx = (start + i) & (UDP_IOCP_POOL_SIZE - 1);
            bool expected = false;
            if (pool_[idx].in_use.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                // Won this slot
                std::memset(&pool_[idx].overlapped, 0, sizeof(OVERLAPPED));
                pool_[idx].op_type = UdpIOCPOpType::NONE;
                pool_[idx].wsabuf.buf = pool_[idx].buffer;
                pool_[idx].wsabuf.len = UDP_IOCP_BUFFER_SIZE;
                pool_[idx].flags = 0;
                pool_[idx].bytes_transferred = 0;
                pool_[idx].remote_addr_len = sizeof(pool_[idx].remote_addr);
                return &pool_[idx];
            }
        }
        // Pool exhausted
        return nullptr;
    }

    /**
     * Release an operation back to the pool.
     * Lock-free: single atomic store.
     */
    void release(UdpIOCPOperation* op) noexcept {
        if (!op) return;
        op->in_use.store(false, std::memory_order_release);
    }

    /**
     * Check if an operation belongs to this pool.
     */
    bool owns(const UdpIOCPOperation* op) const noexcept {
        return op >= pool_.data() && op < pool_.data() + UDP_IOCP_POOL_SIZE;
    }

private:
    std::array<UdpIOCPOperation, UDP_IOCP_POOL_SIZE> pool_;
    std::atomic<uint32_t> hint_;  // Rotating start index for acquire scan
};

// --------------------------------------------------------------------------
// Datagram result (for completed recvs)
// --------------------------------------------------------------------------

struct UdpDatagram {
    const char*             data;
    uint32_t                length;
    struct sockaddr_storage from_addr;
    INT                     from_addr_len;
};

// --------------------------------------------------------------------------
// Callback types
// --------------------------------------------------------------------------

/**
 * Called when a datagram is received.
 * The data pointer is valid only for the duration of the callback.
 * Must not block.
 */
using UdpRecvCallback = std::function<void(
    const UdpDatagram& dgram,
    SOCKET socket
)>;

/**
 * Called when a send completes.
 * @param bytes_sent Number of bytes actually sent, or -1 on error.
 */
using UdpSendCallback = std::function<void(ssize_t bytes_sent)>;

// --------------------------------------------------------------------------
// IOCP UDP Context
// --------------------------------------------------------------------------

/**
 * Manages async UDP I/O for a single socket via IOCP.
 *
 * Owns the operation pool and manages the lifecycle of overlapped operations.
 * The socket and IOCP handle are NOT owned -- caller manages their lifetime.
 *
 * Thread safety:
 * - start_receiving(), send_to(), send_to_async() are safe to call from any thread
 * - process_completion() must be called from the IOCP event loop thread
 * - All internal state uses atomics, no mutexes
 */
class IOCPUdpContext {
public:
    /**
     * Construct an IOCP UDP context.
     *
     * @param iocp_handle  The IOCP handle (from CreateIoCompletionPort)
     * @param socket       The UDP SOCKET (must already be created and bound)
     * @param recv_depth   Number of simultaneous recv operations to post
     * @param recv_cb      Callback for received datagrams
     */
    IOCPUdpContext(HANDLE iocp_handle, SOCKET socket,
                   uint32_t recv_depth = UDP_IOCP_DEFAULT_RECV_DEPTH,
                   UdpRecvCallback recv_cb = nullptr) noexcept;

    ~IOCPUdpContext() noexcept;

    // Non-copyable, non-movable
    IOCPUdpContext(const IOCPUdpContext&) = delete;
    IOCPUdpContext& operator=(const IOCPUdpContext&) = delete;
    IOCPUdpContext(IOCPUdpContext&&) = delete;
    IOCPUdpContext& operator=(IOCPUdpContext&&) = delete;

    /**
     * Associate the socket with the IOCP handle and begin receiving.
     * Posts recv_depth overlapped WSARecvFrom operations.
     *
     * @return 0 on success, -1 on error
     */
    int start_receiving() noexcept;

    /**
     * Stop receiving. Cancels all pending I/O on the socket.
     * Outstanding completions will arrive with ERROR_OPERATION_ABORTED.
     */
    void stop_receiving() noexcept;

    /**
     * Send a datagram asynchronously via IOCP.
     *
     * @param data    Pointer to data to send (copied into operation buffer)
     * @param len     Length of data
     * @param addr    Destination address
     * @param addrlen Destination address length
     * @return 0 on success (send posted), -1 on error (pool exhausted or WSA error)
     */
    int send_to(const void* data, size_t len,
                const struct sockaddr* addr, int addrlen) noexcept;

    /**
     * Process a completed IOCP operation.
     * Called by the event loop when GetQueuedCompletionStatusEx returns.
     *
     * @param overlapped   The OVERLAPPED pointer from the completion entry
     * @param bytes        Bytes transferred
     * @param error        Windows error code (0 = success)
     */
    void process_completion(LPOVERLAPPED overlapped,
                            DWORD bytes, DWORD error) noexcept;

    /**
     * Set the receive callback (can be changed at runtime).
     */
    void set_recv_callback(UdpRecvCallback cb) noexcept {
        recv_cb_ = std::move(cb);
    }

    /**
     * Get the underlying socket.
     */
    SOCKET socket() const noexcept { return socket_; }

    /**
     * Check if currently receiving.
     */
    bool is_receiving() const noexcept {
        return receiving_.load(std::memory_order_acquire);
    }

    /**
     * Statistics (atomic, lock-free reads).
     */
    uint64_t stat_recv_count() const noexcept {
        return stat_recvs_.load(std::memory_order_relaxed);
    }
    uint64_t stat_send_count() const noexcept {
        return stat_sends_.load(std::memory_order_relaxed);
    }
    uint64_t stat_recv_bytes() const noexcept {
        return stat_recv_bytes_.load(std::memory_order_relaxed);
    }
    uint64_t stat_send_bytes() const noexcept {
        return stat_send_bytes_.load(std::memory_order_relaxed);
    }
    uint64_t stat_errors() const noexcept {
        return stat_errors_.load(std::memory_order_relaxed);
    }
    uint64_t stat_pool_misses() const noexcept {
        return stat_pool_misses_.load(std::memory_order_relaxed);
    }

    /**
     * Get a pointer to the operation pool (for external ownership/sharing).
     */
    UdpIOCPOperationPool* pool() noexcept { return &op_pool_; }

private:
    /**
     * Post a single WSARecvFrom overlapped operation.
     * @return 0 on success, -1 on error
     */
    int post_recv() noexcept;

    HANDLE              iocp_handle_;
    SOCKET              socket_;
    uint32_t            recv_depth_;
    UdpRecvCallback     recv_cb_;

    std::atomic<bool>   receiving_{false};
    std::atomic<uint32_t> outstanding_recvs_{0};

    UdpIOCPOperationPool op_pool_;

    // Statistics
    std::atomic<uint64_t> stat_recvs_{0};
    std::atomic<uint64_t> stat_sends_{0};
    std::atomic<uint64_t> stat_recv_bytes_{0};
    std::atomic<uint64_t> stat_send_bytes_{0};
    std::atomic<uint64_t> stat_errors_{0};
    std::atomic<uint64_t> stat_pool_misses_{0};
};

// --------------------------------------------------------------------------
// Coroutine awaitables for async UDP
// --------------------------------------------------------------------------

/**
 * Awaitable for async UDP recv.
 *
 * co_await udp_recv_awaitable(ctx) suspends the coroutine until a
 * datagram arrives, then resumes with the UdpDatagram result.
 *
 * Implementation:
 * - Posts a WSARecvFrom and stores the coroutine handle
 * - When completion arrives, the event loop resumes the coroutine
 * - Zero allocation (operation from pool)
 */
class UdpRecvAwaitable {
public:
    struct Awaiter {
        IOCPUdpContext* ctx_;
        UdpIOCPOperation* op_ = nullptr;
        UdpDatagram result_{};
        bool has_error_ = false;

        explicit Awaiter(IOCPUdpContext* ctx) noexcept : ctx_(ctx) {}

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> h) noexcept;

        /**
         * Return the received datagram.
         *
         * IMPORTANT: The data pointer in the returned UdpDatagram points into
         * a pool-managed buffer. The data is valid until the NEXT co_await or
         * until the coroutine yields. Copy any data you need to keep before
         * the next suspension point. This matches the callback-based API
         * where data is only valid during the callback invocation.
         *
         * The operation slot is released back to the pool here. The buffer
         * memory is not zeroed or freed (pool reuse), so the data survives
         * briefly, but must not be relied upon across suspension points.
         */
        UdpDatagram await_resume() noexcept {
            if (op_ && !has_error_) {
                result_.data = op_->buffer;
                result_.length = op_->bytes_transferred;
                std::memcpy(&result_.from_addr, &op_->remote_addr, op_->remote_addr_len);
                result_.from_addr_len = op_->remote_addr_len;

                ctx_->pool()->release(op_);
                op_ = nullptr;
            }
            return result_;
        }
    };

    explicit UdpRecvAwaitable(IOCPUdpContext* ctx) noexcept : ctx_(ctx) {}

    Awaiter operator co_await() noexcept {
        return Awaiter{ctx_};
    }

private:
    IOCPUdpContext* ctx_;
};

/**
 * Awaitable for async UDP send.
 *
 * co_await udp_send_awaitable(ctx, data, len, addr, addrlen) suspends
 * until the send completes, then resumes with the byte count.
 */
class UdpSendAwaitable {
public:
    struct Awaiter {
        IOCPUdpContext*     ctx_;
        const void*         data_;
        size_t              len_;
        const sockaddr*     addr_;
        int                 addrlen_;
        UdpIOCPOperation*   op_ = nullptr;
        ssize_t             result_ = -1;

        Awaiter(IOCPUdpContext* ctx, const void* data, size_t len,
                const sockaddr* addr, int addrlen) noexcept
            : ctx_(ctx), data_(data), len_(len), addr_(addr), addrlen_(addrlen) {}

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> h) noexcept;

        ssize_t await_resume() noexcept {
            if (op_) {
                result_ = static_cast<ssize_t>(op_->bytes_transferred);
                ctx_->pool()->release(op_);
                op_ = nullptr;
            }
            return result_;
        }
    };

    UdpSendAwaitable(IOCPUdpContext* ctx, const void* data, size_t len,
                     const sockaddr* addr, int addrlen) noexcept
        : ctx_(ctx), data_(data), len_(len), addr_(addr), addrlen_(addrlen) {}

    Awaiter operator co_await() noexcept {
        return Awaiter{ctx_, data_, len_, addr_, addrlen_};
    }

private:
    IOCPUdpContext* ctx_;
    const void*     data_;
    size_t          len_;
    const sockaddr* addr_;
    int             addrlen_;
};

} // namespace net
} // namespace fasterapi

#endif // _WIN32
