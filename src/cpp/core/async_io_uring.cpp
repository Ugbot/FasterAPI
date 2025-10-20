/**
 * io_uring-based async I/O implementation (Linux 5.1+)
 * 
 * Next-generation async I/O using io_uring.
 * True async, zero-copy, kernel-bypass operations.
 */

#include "async_io.h"

#ifdef __linux__

// io_uring requires liburing headers
#ifdef HAVE_LIBURING
#include <liburing.h>
#endif

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
#include <cstring>

namespace fasterapi {
namespace core {

#ifdef HAVE_LIBURING

/**
 * Pending I/O operation (for io_uring)
 */
struct uring_op {
    io_op operation;
    int fd;
    io_callback callback;
    void* user_data;
    
    // For read/write
    void* buffer{nullptr};
    size_t size{0};
    
    // For accept
    struct sockaddr_storage client_addr;
    socklen_t client_addrlen{sizeof(client_addr)};
    
    // For connect
    struct sockaddr_storage addr;
    socklen_t addrlen{0};
};

/**
 * io_uring implementation
 */
struct io_uring_io::impl {
    struct io_uring ring;
    async_io_config config;
    
    std::atomic<bool> running{false};
    std::atomic<bool> stop_requested{false};
    
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
        // Initialize io_uring with queue depth
        int ret = io_uring_queue_init(cfg.queue_depth, &ring, 0);
        if (ret < 0) {
            // Handle error - fall back to epoll?
        }
    }
    
    ~impl() {
        io_uring_queue_exit(&ring);
    }
    
    int set_nonblocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) return -1;
        return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
};

io_uring_io::io_uring_io(const async_io_config& config)
    : impl_(std::make_unique<impl>(config)) {
}

io_uring_io::~io_uring_io() {
    stop();
}

int io_uring_io::accept_async(
    int listen_fd,
    io_callback callback,
    void* user_data
) noexcept {
    impl_->set_nonblocking(listen_fd);
    
    auto* op = new uring_op();
    op->operation = io_op::accept;
    op->fd = listen_fd;
    op->callback = std::move(callback);
    op->user_data = user_data;
    
    // Get submission queue entry
    struct io_uring_sqe* sqe = io_uring_get_sqe(&impl_->ring);
    if (!sqe) {
        delete op;
        impl_->stat_errors.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    
    // Prepare accept operation
    io_uring_prep_accept(sqe, listen_fd, 
                        (struct sockaddr*)&op->client_addr,
                        &op->client_addrlen, 0);
    io_uring_sqe_set_data(sqe, op);
    
    // Submit
    io_uring_submit(&impl_->ring);
    
    impl_->stat_accepts.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

int io_uring_io::read_async(
    int fd,
    void* buffer,
    size_t size,
    io_callback callback,
    void* user_data
) noexcept {
    auto* op = new uring_op();
    op->operation = io_op::read;
    op->fd = fd;
    op->buffer = buffer;
    op->size = size;
    op->callback = std::move(callback);
    op->user_data = user_data;
    
    // Get submission queue entry
    struct io_uring_sqe* sqe = io_uring_get_sqe(&impl_->ring);
    if (!sqe) {
        delete op;
        impl_->stat_errors.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    
    // Prepare read operation
    io_uring_prep_read(sqe, fd, buffer, size, 0);
    io_uring_sqe_set_data(sqe, op);
    
    // Submit
    io_uring_submit(&impl_->ring);
    
    impl_->stat_reads.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

int io_uring_io::write_async(
    int fd,
    const void* buffer,
    size_t size,
    io_callback callback,
    void* user_data
) noexcept {
    auto* op = new uring_op();
    op->operation = io_op::write;
    op->fd = fd;
    op->buffer = const_cast<void*>(buffer);
    op->size = size;
    op->callback = std::move(callback);
    op->user_data = user_data;
    
    // Get submission queue entry
    struct io_uring_sqe* sqe = io_uring_get_sqe(&impl_->ring);
    if (!sqe) {
        delete op;
        impl_->stat_errors.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    
    // Prepare write operation
    io_uring_prep_write(sqe, fd, buffer, size, 0);
    io_uring_sqe_set_data(sqe, op);
    
    // Submit
    io_uring_submit(&impl_->ring);
    
    impl_->stat_writes.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

int io_uring_io::connect_async(
    int fd,
    const struct sockaddr* addr,
    socklen_t addrlen,
    io_callback callback,
    void* user_data
) noexcept {
    impl_->set_nonblocking(fd);
    
    auto* op = new uring_op();
    op->operation = io_op::connect;
    op->fd = fd;
    op->callback = std::move(callback);
    op->user_data = user_data;
    memcpy(&op->addr, addr, addrlen);
    op->addrlen = addrlen;
    
    // Get submission queue entry
    struct io_uring_sqe* sqe = io_uring_get_sqe(&impl_->ring);
    if (!sqe) {
        delete op;
        impl_->stat_errors.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    
    // Prepare connect operation
    io_uring_prep_connect(sqe, fd, (struct sockaddr*)&op->addr, op->addrlen);
    io_uring_sqe_set_data(sqe, op);
    
    // Submit
    io_uring_submit(&impl_->ring);
    
    impl_->stat_connects.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

int io_uring_io::close_async(int fd) noexcept {
    impl_->stat_closes.fetch_add(1, std::memory_order_relaxed);
    
    // Can also use io_uring_prep_close for async close
    return close(fd);
}

int io_uring_io::poll(uint32_t timeout_us) noexcept {
    impl_->stat_polls.fetch_add(1, std::memory_order_relaxed);
    
    struct io_uring_cqe* cqe;
    struct __kernel_timespec timeout;
    timeout.tv_sec = timeout_us / 1000000;
    timeout.tv_nsec = (timeout_us % 1000000) * 1000;
    
    // Wait for completion events
    int ret = io_uring_wait_cqe_timeout(&impl_->ring, &cqe, &timeout);
    if (ret < 0) {
        if (ret != -ETIME) {  // Timeout is not an error
            impl_->stat_errors.fetch_add(1, std::memory_order_relaxed);
        }
        return 0;
    }
    
    int events = 0;
    
    // Process all available completions
    unsigned head;
    unsigned i = 0;
    io_uring_for_each_cqe(&impl_->ring, head, cqe) {
        events++;
        
        // Get operation data
        uring_op* op = static_cast<uring_op*>(io_uring_cqe_get_data(cqe));
        if (!op) continue;
        
        std::unique_ptr<uring_op> op_guard(op);
        
        // Create event
        io_event event;
        event.operation = op->operation;
        event.fd = op->fd;
        event.user_data = op->user_data;
        event.result = cqe->res;  // Result from io_uring
        event.flags = cqe->flags;
        
        // For accept, result is the client FD
        if (op->operation == io_op::accept && cqe->res >= 0) {
            event.result = cqe->res;  // Client FD
        }
        
        // Invoke callback
        if (op->callback) {
            op->callback(event);
        }
        
        i++;
        if (i >= 128) break;  // Process max 128 per poll
    }
    
    // Mark completions as seen
    io_uring_cq_advance(&impl_->ring, events);
    
    impl_->stat_events.fetch_add(events, std::memory_order_relaxed);
    return events;
}

void io_uring_io::run() noexcept {
    if (impl_->running.exchange(true)) {
        return;  // Already running
    }
    
    impl_->stop_requested.store(false);
    
    while (!impl_->stop_requested.load(std::memory_order_acquire)) {
        poll(impl_->config.poll_timeout_us);
    }
    
    impl_->running.store(false);
}

void io_uring_io::stop() noexcept {
    impl_->stop_requested.store(true, std::memory_order_release);
}

bool io_uring_io::is_running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

async_io::stats io_uring_io::get_stats() const noexcept {
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

#else  // !HAVE_LIBURING

// Stub implementations when liburing is not available
io_uring_io::io_uring_io(const async_io_config&) : impl_(nullptr) {}
io_uring_io::~io_uring_io() {}
int io_uring_io::accept_async(int, io_callback, void*) noexcept { return -1; }
int io_uring_io::read_async(int, void*, size_t, io_callback, void*) noexcept { return -1; }
int io_uring_io::write_async(int, const void*, size_t, io_callback, void*) noexcept { return -1; }
int io_uring_io::connect_async(int, const struct sockaddr*, socklen_t, io_callback, void*) noexcept { return -1; }
int io_uring_io::close_async(int) noexcept { return -1; }
int io_uring_io::poll(uint32_t) noexcept { return -1; }
void io_uring_io::run() noexcept {}
void io_uring_io::stop() noexcept {}
bool io_uring_io::is_running() const noexcept { return false; }
async_io::stats io_uring_io::get_stats() const noexcept { return {}; }

#endif // HAVE_LIBURING

} // namespace core
} // namespace fasterapi

#endif // __linux__



