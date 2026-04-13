/**
 * FasterAPI IOCP UDP Socket - Implementation
 *
 * Asynchronous UDP I/O via Windows I/O Completion Ports.
 *
 * Architecture:
 * - Pre-allocated pool of UdpIOCPOperation (512 slots, ~32MB for 64KB buffers)
 * - Lock-free pool acquire via atomic CAS with rotating hint
 * - Multiple outstanding WSARecvFrom for receive throughput
 * - WSASendTo with overlapped for async send
 * - Coroutine awaitables store handle in OVERLAPPED hEvent field
 *
 * Hot path (recv completion -> callback -> re-post):
 * - Zero allocations (pool acquire + release)
 * - No mutex, no lock
 * - Single CAS for pool acquire, single atomic store for release
 *
 * Coroutine integration:
 * - The OVERLAPPED::hEvent field is repurposed to store a coroutine handle
 *   when the operation is coroutine-driven (hEvent is unused with IOCP)
 * - The completion handler checks for a stored coroutine handle and resumes it
 */

#ifdef _WIN32

#include "udp_socket_iocp.h"
#include <iostream>
#include <cassert>

namespace fasterapi {
namespace net {

// --------------------------------------------------------------------------
// Coroutine handle packing into OVERLAPPED::hEvent
//
// IOCP ignores hEvent when the handle is associated with a completion port.
// We repurpose it to store coroutine_handle<> for coroutine-driven operations.
// The low bit is set to 1 to distinguish coroutine handles from NULL/real events.
// --------------------------------------------------------------------------

static void store_coro_handle(OVERLAPPED* ov, std::coroutine_handle<> h) noexcept {
    auto addr = reinterpret_cast<uintptr_t>(h.address());
    // Set low bit as marker (coroutine handles are always aligned > 1)
    ov->hEvent = reinterpret_cast<HANDLE>(addr | 1);
}

static std::coroutine_handle<> load_coro_handle(OVERLAPPED* ov) noexcept {
    auto val = reinterpret_cast<uintptr_t>(ov->hEvent);
    if (val & 1) {
        auto addr = reinterpret_cast<void*>(val & ~uintptr_t(1));
        return std::coroutine_handle<>::from_address(addr);
    }
    return nullptr;  // Not a coroutine operation
}

static bool has_coro_handle(OVERLAPPED* ov) noexcept {
    return (reinterpret_cast<uintptr_t>(ov->hEvent) & 1) != 0;
}

// --------------------------------------------------------------------------
// IOCPUdpContext
// --------------------------------------------------------------------------

IOCPUdpContext::IOCPUdpContext(HANDLE iocp_handle, SOCKET socket,
                               uint32_t recv_depth,
                               UdpRecvCallback recv_cb) noexcept
    : iocp_handle_(iocp_handle)
    , socket_(socket)
    , recv_depth_(recv_depth)
    , recv_cb_(std::move(recv_cb))
{
}

IOCPUdpContext::~IOCPUdpContext() noexcept {
    stop_receiving();
}

int IOCPUdpContext::start_receiving() noexcept {
    if (iocp_handle_ == INVALID_HANDLE_VALUE || socket_ == INVALID_SOCKET) {
        return -1;
    }

    // Associate the UDP socket with the IOCP handle.
    // Completion key is the socket value for identification.
    HANDLE result = CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(socket_),
        iocp_handle_,
        static_cast<ULONG_PTR>(socket_),
        0
    );

    if (result == NULL) {
        stat_errors_.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }

    receiving_.store(true, std::memory_order_release);

    // Pre-post recv_depth_ overlapped receives.
    // This is the key to high-throughput IOCP UDP: multiple outstanding recvs
    // means the kernel always has a buffer ready when a datagram arrives.
    for (uint32_t i = 0; i < recv_depth_; ++i) {
        if (post_recv() != 0) {
            // If we posted at least one recv, continue; if zero, fail.
            if (outstanding_recvs_.load(std::memory_order_acquire) == 0) {
                receiving_.store(false, std::memory_order_release);
                return -1;
            }
            break;
        }
    }

    return 0;
}

void IOCPUdpContext::stop_receiving() noexcept {
    if (!receiving_.exchange(false, std::memory_order_acq_rel)) {
        return;  // Already stopped
    }

    // Cancel all pending I/O on this socket.
    // Outstanding operations will complete with ERROR_OPERATION_ABORTED.
    if (socket_ != INVALID_SOCKET) {
        CancelIoEx(reinterpret_cast<HANDLE>(socket_), nullptr);
    }
}

int IOCPUdpContext::post_recv() noexcept {
    UdpIOCPOperation* op = op_pool_.acquire();
    if (!op) {
        stat_pool_misses_.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }

    op->op_type = UdpIOCPOpType::RECV_FROM;
    op->flags = 0;
    op->remote_addr_len = sizeof(op->remote_addr);

    int result = WSARecvFrom(
        socket_,
        &op->wsabuf,
        1,                          // Buffer count
        nullptr,                    // Bytes received (NULL for overlapped)
        &op->flags,
        reinterpret_cast<struct sockaddr*>(&op->remote_addr),
        &op->remote_addr_len,
        &op->overlapped,
        nullptr                     // No completion routine (using IOCP)
    );

    if (result == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            // Real error
            op_pool_.release(op);
            stat_errors_.fetch_add(1, std::memory_order_relaxed);
            return -1;
        }
        // WSA_IO_PENDING is the expected success case for overlapped I/O
    }

    outstanding_recvs_.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

int IOCPUdpContext::send_to(const void* data, size_t len,
                            const struct sockaddr* addr, int addrlen) noexcept {
    if (len > UDP_IOCP_BUFFER_SIZE) {
        return -1;  // Datagram too large
    }

    UdpIOCPOperation* op = op_pool_.acquire();
    if (!op) {
        stat_pool_misses_.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }

    op->op_type = UdpIOCPOpType::SEND_TO;

    // Copy data into operation buffer (zero-copy not possible for send --
    // the caller's buffer may go out of scope before IOCP completes)
    std::memcpy(op->buffer, data, len);
    op->wsabuf.buf = op->buffer;
    op->wsabuf.len = static_cast<ULONG>(len);

    int result = WSASendTo(
        socket_,
        &op->wsabuf,
        1,                          // Buffer count
        nullptr,                    // Bytes sent (NULL for overlapped)
        0,                          // Flags
        addr,
        addrlen,
        &op->overlapped,
        nullptr                     // No completion routine
    );

    if (result == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            op_pool_.release(op);
            stat_errors_.fetch_add(1, std::memory_order_relaxed);
            return -1;
        }
    }

    return 0;
}

void IOCPUdpContext::process_completion(LPOVERLAPPED overlapped,
                                        DWORD bytes, DWORD error) noexcept {
    if (!overlapped) return;

    // Recover our operation from the OVERLAPPED pointer.
    // OVERLAPPED is the first member of UdpIOCPOperation so
    // CONTAINING_RECORD gives us the enclosing struct.
    UdpIOCPOperation* op = CONTAINING_RECORD(overlapped, UdpIOCPOperation, overlapped);
    if (!op) return;

    switch (op->op_type) {
        case UdpIOCPOpType::RECV_FROM: {
            outstanding_recvs_.fetch_sub(1, std::memory_order_relaxed);

            if (error == ERROR_OPERATION_ABORTED) {
                // Socket was cancelled (stop_receiving)
                op_pool_.release(op);
                return;
            }

            if (error != 0 || bytes == 0) {
                stat_errors_.fetch_add(1, std::memory_order_relaxed);
            } else {
                stat_recvs_.fetch_add(1, std::memory_order_relaxed);
                stat_recv_bytes_.fetch_add(bytes, std::memory_order_relaxed);

                // Check for coroutine-driven operation
                if (has_coro_handle(&op->overlapped)) {
                    // Store bytes in the operation for the awaiter to read
                    op->bytes_transferred = bytes;
                    auto coro = load_coro_handle(&op->overlapped);
                    // The awaiter reads from op before releasing; do NOT release here.
                    // The awaiter is responsible for releasing via its own cleanup.
                    coro.resume();
                    return;  // Do not re-post or release -- awaiter handles it
                }

                // Callback-driven path
                if (recv_cb_) {
                    UdpDatagram dgram;
                    dgram.data = op->buffer;
                    dgram.length = bytes;
                    std::memcpy(&dgram.from_addr, &op->remote_addr, op->remote_addr_len);
                    dgram.from_addr_len = op->remote_addr_len;

                    recv_cb_(dgram, socket_);
                }
            }

            op_pool_.release(op);

            // Re-post a recv to maintain recv depth (if still receiving)
            if (receiving_.load(std::memory_order_acquire)) {
                post_recv();
            }
            break;
        }

        case UdpIOCPOpType::SEND_TO: {
            if (error == ERROR_OPERATION_ABORTED) {
                op_pool_.release(op);
                return;
            }

            if (error != 0) {
                stat_errors_.fetch_add(1, std::memory_order_relaxed);
            } else {
                stat_sends_.fetch_add(1, std::memory_order_relaxed);
                stat_send_bytes_.fetch_add(bytes, std::memory_order_relaxed);
            }

            // Check for coroutine-driven send
            if (has_coro_handle(&op->overlapped)) {
                op->bytes_transferred = bytes;
                auto coro = load_coro_handle(&op->overlapped);
                coro.resume();
                return;  // Awaiter releases op
            }

            op_pool_.release(op);
            break;
        }

        default:
            // Unknown operation type
            op_pool_.release(op);
            break;
    }
}

// --------------------------------------------------------------------------
// Coroutine awaitables
// --------------------------------------------------------------------------

bool UdpRecvAwaitable::Awaiter::await_suspend(std::coroutine_handle<> h) noexcept {
    // Acquire an operation from the pool
    op_ = ctx_->pool()->acquire();
    if (!op_) {
        // Pool exhausted - resume immediately with error
        has_error_ = true;
        result_.data = nullptr;
        result_.length = 0;
        return false;  // Don't suspend, resume immediately
    }

    op_->op_type = UdpIOCPOpType::RECV_FROM;
    op_->flags = 0;
    op_->remote_addr_len = sizeof(op_->remote_addr);

    // Store coroutine handle in OVERLAPPED::hEvent
    store_coro_handle(&op_->overlapped, h);

    int rc = WSARecvFrom(
        ctx_->socket(),
        &op_->wsabuf,
        1,
        nullptr,
        &op_->flags,
        reinterpret_cast<struct sockaddr*>(&op_->remote_addr),
        &op_->remote_addr_len,
        &op_->overlapped,
        nullptr
    );

    if (rc == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            // Immediate error - resume coroutine now
            ctx_->pool()->release(op_);
            op_ = nullptr;
            has_error_ = true;
            result_.data = nullptr;
            result_.length = 0;
            return false;  // Don't suspend
        }
    }

    // Operation is pending - coroutine will be resumed by process_completion
    return true;
}

bool UdpSendAwaitable::Awaiter::await_suspend(std::coroutine_handle<> h) noexcept {
    if (len_ > UDP_IOCP_BUFFER_SIZE) {
        result_ = -1;
        return false;
    }

    op_ = ctx_->pool()->acquire();
    if (!op_) {
        result_ = -1;
        return false;
    }

    op_->op_type = UdpIOCPOpType::SEND_TO;
    std::memcpy(op_->buffer, data_, len_);
    op_->wsabuf.buf = op_->buffer;
    op_->wsabuf.len = static_cast<ULONG>(len_);

    // Store coroutine handle so process_completion can resume us
    store_coro_handle(&op_->overlapped, h);

    int rc = WSASendTo(
        ctx_->socket(),
        &op_->wsabuf,
        1,
        nullptr,
        0,
        addr_,
        addrlen_,
        &op_->overlapped,
        nullptr
    );

    if (rc == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            ctx_->pool()->release(op_);
            op_ = nullptr;
            result_ = -1;
            return false;
        }
    }

    // Operation is pending. process_completion will set op_->bytes_transferred
    // and resume this coroutine. await_resume reads bytes and releases op_.
    return true;
}

} // namespace net
} // namespace fasterapi

#endif // _WIN32
