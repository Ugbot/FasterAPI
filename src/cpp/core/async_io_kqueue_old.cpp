/**
 * kqueue-based async I/O implementation (macOS/BSD)
 * 
 * High-performance async I/O using kqueue.
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

#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <mutex>

namespace fasterapi {
namespace core {

/**
 * Pending I/O operation
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
 * kqueue implementation
 */
struct kqueue_io::impl {
    int kq_fd{-1};
    async_io_config config;
    
    std::atomic<bool> running{false};
    std::atomic<bool> stop_requested{false};
    
    // Pending operations indexed by FD
    std::mutex ops_mutex;
    std::unordered_map<int, std::vector<std::unique_ptr<pending_op>>> pending_ops;
    
    // Statistics
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
    
    int register_op(std::unique_ptr<pending_op> op, int16_t filter) {
        std::lock_guard<std::mutex> lock(ops_mutex);
        
        // Add kqueue event
        struct kevent kev;
        EV_SET(&kev, op->fd, filter, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, 0, op.get());
        
        if (kevent(kq_fd, &kev, 1, nullptr, 0, nullptr) < 0) {
            stat_errors.fetch_add(1, std::memory_order_relaxed);
            return -1;
        }
        
        // Store operation
        pending_ops[op->fd].push_back(std::move(op));
        return 0;
    }
    
    pending_op* find_and_remove_op(int fd, uintptr_t udata) {
        std::lock_guard<std::mutex> lock(ops_mutex);
        
        auto it = pending_ops.find(fd);
        if (it == pending_ops.end()) {
            return nullptr;
        }
        
        // Find matching operation by address
        for (auto op_it = it->second.begin(); op_it != it->second.end(); ++op_it) {
            if (op_it->get() == reinterpret_cast<pending_op*>(udata)) {
                pending_op* op = op_it->release();
                it->second.erase(op_it);
                
                // Clean up empty vectors
                if (it->second.empty()) {
                    pending_ops.erase(it);
                }
                
                return op;
            }
        }
        
        return nullptr;
    }
};

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
    
    auto op = std::make_unique<pending_op>();
    op->operation = io_op::accept;
    op->fd = listen_fd;
    op->callback = std::move(callback);
    op->user_data = user_data;
    
    impl_->stat_accepts.fetch_add(1, std::memory_order_relaxed);
    return impl_->register_op(std::move(op), EVFILT_READ);
}

int kqueue_io::read_async(
    int fd,
    void* buffer,
    size_t size,
    io_callback callback,
    void* user_data
) noexcept {
    impl_->set_nonblocking(fd);
    
    auto op = std::make_unique<pending_op>();
    op->operation = io_op::read;
    op->fd = fd;
    op->buffer = buffer;
    op->size = size;
    op->callback = std::move(callback);
    op->user_data = user_data;
    
    impl_->stat_reads.fetch_add(1, std::memory_order_relaxed);
    return impl_->register_op(std::move(op), EVFILT_READ);
}

int kqueue_io::write_async(
    int fd,
    const void* buffer,
    size_t size,
    io_callback callback,
    void* user_data
) noexcept {
    impl_->set_nonblocking(fd);
    
    auto op = std::make_unique<pending_op>();
    op->operation = io_op::write;
    op->fd = fd;
    op->buffer = const_cast<void*>(buffer);
    op->size = size;
    op->callback = std::move(callback);
    op->user_data = user_data;
    
    impl_->stat_writes.fetch_add(1, std::memory_order_relaxed);
    return impl_->register_op(std::move(op), EVFILT_WRITE);
}

int kqueue_io::connect_async(
    int fd,
    const struct sockaddr* addr,
    socklen_t addrlen,
    io_callback callback,
    void* user_data
) noexcept {
    impl_->set_nonblocking(fd);
    
    auto op = std::make_unique<pending_op>();
    op->operation = io_op::connect;
    op->fd = fd;
    op->callback = std::move(callback);
    op->user_data = user_data;
    memcpy(&op->addr, addr, addrlen);
    op->addrlen = addrlen;
    
    // Start connection
    int ret = connect(fd, addr, addrlen);
    if (ret < 0 && errno != EINPROGRESS) {
        impl_->stat_errors.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    
    impl_->stat_connects.fetch_add(1, std::memory_order_relaxed);
    return impl_->register_op(std::move(op), EVFILT_WRITE);
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
    
    // Process events
    for (int i = 0; i < n; ++i) {
        const struct kevent& kev = events[i];
        
        // Find and remove pending operation
        pending_op* op = impl_->find_and_remove_op(kev.ident, (uintptr_t)kev.udata);
        if (!op) continue;
        
        std::unique_ptr<pending_op> op_guard(op);
        
        // Execute operation
        io_event event;
        event.operation = op->operation;
        event.fd = op->fd;
        event.user_data = op->user_data;
        event.flags = kev.flags;
        event.result = 0;
        
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
        return;  // Already running
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

