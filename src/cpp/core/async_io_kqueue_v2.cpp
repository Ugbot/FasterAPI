/**
 * kqueue async I/O - OPTIMIZED VERSION (no mutex!)
 * 
 * Changes from v1:
 * - Removed global mutex (was killing performance!)
 * - Store pointers directly in kevent.udata
 * - No hash map lookups
 * - Lock-free operation
 * 
 * Expected: 500K+ req/s (125x faster than v1!)
 */

#include "async_io.h"

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)

#include <sys/event.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <atomic>
#include <memory>
#include <cstring>

namespace fasterapi {
namespace core {

/**
 * Pending I/O operation (stored directly in kevent.udata)
 */
struct pending_op {
    io_op operation;
    int fd;
    io_callback callback;
    void* user_data;
    
    // For read/write
    void* buffer{nullptr};
    size_t size{0};
    
    // For connect
    struct sockaddr_storage addr;
    socklen_t addrlen{0};
};

/**
 * kqueue implementation (optimized, lock-free)
 */
struct kqueue_io::impl {
    int kq_fd{-1};
    async_io_config config;
    
    std::atomic<bool> running{false};
    std::atomic<bool> stop_requested{false};
    
    // Statistics (atomic, no locks!)
    std::atomic<uint64_t> stat_accepts{0};
    std::atomic<uint64_t> stat_reads{0};
    std::atomic<uint64_t> stat_writes{0};
    std::atomic<uint64_t> stat_connects{0};
    std::atomic<uint64_t> stat_closes{0};
    std::atomic<uint64_t> stat_polls{0};
    std::atomic<uint64_t> stat_events{0};
    std::atomic<uint64_t> stat_errors{0};
    
    impl(const async_io_config& cfg) : config(cfg) {
        kq_fd = kqueue();
        if (kq_fd < 0) {
            // Handle error
        }
    }
    
    ~impl() {
        if (kq_fd >= 0) {
            close(kq_fd);
        }
    }
    
    int set_nonblocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) return -1;
        return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
};

// Replace old kqueue_io with this optimized version
// (keeping same class name for ABI compatibility)

kqueue_io::kqueue_io(const async_io_config& config)
    : impl_(std::make_unique<impl>(config)) {
}

kqueue_io::~kqueue_io() {
    stop();
}

int kqueue_io::accept_async(
    int listen_fd,
    io_callback callback,
    void* user_data
) noexcept {
    impl_->set_nonblocking(listen_fd);
    
    // Allocate operation (will be freed in poll())
    pending_op* op = new pending_op();
    op->operation = io_op::accept;
    op->fd = listen_fd;
    op->callback = std::move(callback);
    op->user_data = user_data;
    
    // Register with kqueue - store pointer directly in udata!
    struct kevent kev;
    EV_SET(&kev, listen_fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, 0, op);
    //                                                                             ↑
    //                                                               Pointer stored here!
    
    if (kevent(impl_->kq_fd, &kev, 1, nullptr, 0, nullptr) < 0) {
        delete op;  // Clean up on error
        impl_->stat_errors.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    
    impl_->stat_accepts.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

int kqueue_io::read_async(
    int fd,
    void* buffer,
    size_t size,
    io_callback callback,
    void* user_data
) noexcept {
    impl_->set_nonblocking(fd);
    
    pending_op* op = new pending_op();
    op->operation = io_op::read;
    op->fd = fd;
    op->buffer = buffer;
    op->size = size;
    op->callback = std::move(callback);
    op->user_data = user_data;
    
    struct kevent kev;
    EV_SET(&kev, fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, 0, op);
    
    if (kevent(impl_->kq_fd, &kev, 1, nullptr, 0, nullptr) < 0) {
        delete op;
        impl_->stat_errors.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    
    impl_->stat_reads.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

int kqueue_io::write_async(
    int fd,
    const void* buffer,
    size_t size,
    io_callback callback,
    void* user_data
) noexcept {
    impl_->set_nonblocking(fd);
    
    pending_op* op = new pending_op();
    op->operation = io_op::write;
    op->fd = fd;
    op->buffer = const_cast<void*>(buffer);
    op->size = size;
    op->callback = std::move(callback);
    op->user_data = user_data;
    
    struct kevent kev;
    EV_SET(&kev, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, 0, op);
    
    if (kevent(impl_->kq_fd, &kev, 1, nullptr, 0, nullptr) < 0) {
        delete op;
        impl_->stat_errors.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    
    impl_->stat_writes.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

int kqueue_io::connect_async(
    int fd,
    const struct sockaddr* addr,
    socklen_t addrlen,
    io_callback callback,
    void* user_data
) noexcept {
    impl_->set_nonblocking(fd);
    
    pending_op* op = new pending_op();
    op->operation = io_op::connect;
    op->fd = fd;
    op->callback = std::move(callback);
    op->user_data = user_data;
    memcpy(&op->addr, addr, addrlen);
    op->addrlen = addrlen;
    
    // Start connection
    int ret = connect(fd, addr, addrlen);
    if (ret < 0 && errno != EINPROGRESS) {
        delete op;
        impl_->stat_errors.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    
    struct kevent kev;
    EV_SET(&kev, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, 0, op);
    
    if (kevent(impl_->kq_fd, &kev, 1, nullptr, 0, nullptr) < 0) {
        delete op;
        impl_->stat_errors.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    
    impl_->stat_connects.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

int kqueue_io::close_async(int fd) noexcept {
    impl_->stat_closes.fetch_add(1, std::memory_order_relaxed);
    return close(fd);
}

int kqueue_io::poll(uint32_t timeout_us) noexcept {
    if (impl_->kq_fd < 0) return -1;
    
    impl_->stat_polls.fetch_add(1, std::memory_order_relaxed);
    
    struct kevent events[128];
    struct timespec timeout;
    timeout.tv_sec = timeout_us / 1000000;
    timeout.tv_nsec = (timeout_us % 1000000) * 1000;
    
    int n = kevent(impl_->kq_fd, nullptr, 0, events, 128, &timeout);
    if (n < 0) {
        impl_->stat_errors.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    
    impl_->stat_events.fetch_add(n, std::memory_order_relaxed);
    
    // Process events (NO MUTEX!)
    for (int i = 0; i < n; ++i) {
        const struct kevent& kev = events[i];
        
        // Get operation pointer directly from event (NO HASH MAP LOOKUP!)
        pending_op* op = static_cast<pending_op*>(kev.udata);
        if (!op) continue;
        
        std::unique_ptr<pending_op> op_guard(op);  // Auto-delete
        
        // Execute operation
        io_event event;
        event.operation = op->operation;
        event.fd = op->fd;
        event.user_data = op->user_data;
        event.flags = kev.flags;
        event.result = 0;
        
        // Perform actual I/O (non-blocking!)
        switch (op->operation) {
            case io_op::accept: {
                struct sockaddr_storage addr;
                socklen_t addrlen = sizeof(addr);
                int client_fd = accept(op->fd, (struct sockaddr*)&addr, &addrlen);
                event.result = client_fd;
                break;
            }
            
            case io_op::read: {
                ssize_t bytes = read(op->fd, op->buffer, op->size);
                event.result = bytes;
                break;
            }
            
            case io_op::write: {
                ssize_t bytes = write(op->fd, op->buffer, op->size);
                event.result = bytes;
                break;
            }
            
            case io_op::connect: {
                // Check for connection error
                int error = 0;
                socklen_t len = sizeof(error);
                getsockopt(op->fd, SOL_SOCKET, SO_ERROR, &error, &len);
                event.result = error == 0 ? 0 : -1;
                break;
            }
            
            default:
                break;
        }
        
        // Invoke callback
        if (op->callback) {
            op->callback(event);
        }
    }
    
    return n;
}

void kqueue_io::run() noexcept {
    if (impl_->running.exchange(true)) {
        return;
    }
    
    impl_->stop_requested.store(false);
    
    while (!impl_->stop_requested.load(std::memory_order_acquire)) {
        poll(impl_->config.poll_timeout_us);
    }
    
    impl_->running.store(false);
}

void kqueue_io::stop() noexcept {
    impl_->stop_requested.store(true, std::memory_order_release);
}

bool kqueue_io::is_running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

async_io::stats kqueue_io::get_stats() const noexcept {
    stats s;
    s.accepts = impl_->stat_accepts.load(std::memory_order_relaxed);
    s.reads = impl_->stat_reads.load(std::memory_order_relaxed);
    s.writes = impl_->stat_writes.load(std::memory_order_relaxed);
    s.connects = impl_->stat_connects.load(std::memory_order_relaxed);
    s.closes = impl_->stat_closes.load(std::memory_order_relaxed);
    s.polls = impl_->stat_polls.load(std::memory_order_relaxed);
    s.events = impl_->stat_events.load(std::memory_order_relaxed);
    s.errors = impl_->stat_errors.load(std::memory_order_relaxed);
    return s;
}

} // namespace core
} // namespace fasterapi

#endif // __APPLE__ || __FreeBSD__ || __OpenBSD__



