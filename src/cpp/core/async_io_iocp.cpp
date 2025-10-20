/**
 * IOCP-based async I/O implementation (Windows)
 * 
 * High-performance async I/O using I/O Completion Ports.
 */

#include "async_io.h"

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>

#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <mutex>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

namespace fasterapi {
namespace core {

/**
 * OVERLAPPED structure with operation data
 */
struct iocp_op {
    OVERLAPPED overlapped;
    io_op operation;
    SOCKET sock;
    io_callback callback;
    void* user_data;
    
    // For read/write
    WSABUF wsabuf;
    char* buffer{nullptr};
    DWORD bytes_transferred{0};
    DWORD flags{0};
    
    // For accept
    SOCKET accept_socket{INVALID_SOCKET};
    char accept_buffer[2 * (sizeof(sockaddr_in) + 16)];
    
    // For connect
    struct sockaddr_storage addr;
    int addrlen{0};
    
    iocp_op() {
        ZeroMemory(&overlapped, sizeof(overlapped));
    }
};

/**
 * IOCP implementation
 */
struct iocp_io::impl {
    HANDLE iocp_handle{INVALID_HANDLE_VALUE};
    async_io_config config;
    
    std::atomic<bool> running{false};
    std::atomic<bool> stop_requested{false};
    
    // AcceptEx function pointer (loaded dynamically)
    LPFN_ACCEPTEX AcceptEx{nullptr};
    LPFN_CONNECTEX ConnectEx{nullptr};
    
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
        // Initialize Winsock
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        
        // Create IOCP
        iocp_handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
        if (iocp_handle == NULL) {
            // Handle error
        }
        
        // Load AcceptEx and ConnectEx
        load_extension_functions();
    }
    
    ~impl() {
        if (iocp_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(iocp_handle);
        }
        WSACleanup();
    }
    
    void load_extension_functions() {
        // Create temporary socket to get extension functions
        SOCKET temp_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (temp_socket == INVALID_SOCKET) return;
        
        // Get AcceptEx
        GUID acceptex_guid = WSAID_ACCEPTEX;
        DWORD bytes;
        WSAIoctl(temp_socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
                &acceptex_guid, sizeof(acceptex_guid),
                &AcceptEx, sizeof(AcceptEx),
                &bytes, NULL, NULL);
        
        // Get ConnectEx
        GUID connectex_guid = WSAID_CONNECTEX;
        WSAIoctl(temp_socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
                &connectex_guid, sizeof(connectex_guid),
                &ConnectEx, sizeof(ConnectEx),
                &bytes, NULL, NULL);
        
        closesocket(temp_socket);
    }
    
    int associate_socket(SOCKET sock) {
        HANDLE h = CreateIoCompletionPort((HANDLE)sock, iocp_handle, (ULONG_PTR)sock, 0);
        return (h == iocp_handle) ? 0 : -1;
    }
};

iocp_io::iocp_io(const async_io_config& config)
    : impl_(std::make_unique<impl>(config)) {
}

iocp_io::~iocp_io() {
    stop();
}

int iocp_io::accept_async(
    int listen_fd,
    io_callback callback,
    void* user_data
) noexcept {
    SOCKET listen_socket = (SOCKET)listen_fd;
    
    // Associate with IOCP
    impl_->associate_socket(listen_socket);
    
    auto* op = new iocp_op();
    op->operation = io_op::accept;
    op->sock = listen_socket;
    op->callback = std::move(callback);
    op->user_data = user_data;
    
    // Create accept socket
    op->accept_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (op->accept_socket == INVALID_SOCKET) {
        delete op;
        impl_->stat_errors.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    
    // Start accept operation
    DWORD bytes_received = 0;
    BOOL result = impl_->AcceptEx(
        listen_socket,
        op->accept_socket,
        op->accept_buffer,
        0,  // No initial receive
        sizeof(sockaddr_in) + 16,
        sizeof(sockaddr_in) + 16,
        &bytes_received,
        &op->overlapped
    );
    
    if (!result && WSAGetLastError() != ERROR_IO_PENDING) {
        closesocket(op->accept_socket);
        delete op;
        impl_->stat_errors.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    
    impl_->stat_accepts.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

int iocp_io::read_async(
    int fd,
    void* buffer,
    size_t size,
    io_callback callback,
    void* user_data
) noexcept {
    SOCKET sock = (SOCKET)fd;
    impl_->associate_socket(sock);
    
    auto* op = new iocp_op();
    op->operation = io_op::read;
    op->sock = sock;
    op->buffer = static_cast<char*>(buffer);
    op->callback = std::move(callback);
    op->user_data = user_data;
    
    op->wsabuf.buf = op->buffer;
    op->wsabuf.len = static_cast<ULONG>(size);
    
    // Start receive operation
    DWORD flags = 0;
    int result = WSARecv(sock, &op->wsabuf, 1, &op->bytes_transferred, &flags, &op->overlapped, NULL);
    
    if (result != 0 && WSAGetLastError() != WSA_IO_PENDING) {
        delete op;
        impl_->stat_errors.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    
    impl_->stat_reads.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

int iocp_io::write_async(
    int fd,
    const void* buffer,
    size_t size,
    io_callback callback,
    void* user_data
) noexcept {
    SOCKET sock = (SOCKET)fd;
    impl_->associate_socket(sock);
    
    auto* op = new iocp_op();
    op->operation = io_op::write;
    op->sock = sock;
    op->buffer = const_cast<char*>(static_cast<const char*>(buffer));
    op->callback = std::move(callback);
    op->user_data = user_data;
    
    op->wsabuf.buf = op->buffer;
    op->wsabuf.len = static_cast<ULONG>(size);
    
    // Start send operation
    int result = WSASend(sock, &op->wsabuf, 1, &op->bytes_transferred, 0, &op->overlapped, NULL);
    
    if (result != 0 && WSAGetLastError() != WSA_IO_PENDING) {
        delete op;
        impl_->stat_errors.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    
    impl_->stat_writes.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

int iocp_io::connect_async(
    int fd,
    const struct sockaddr* addr,
    socklen_t addrlen,
    io_callback callback,
    void* user_data
) noexcept {
    SOCKET sock = (SOCKET)fd;
    impl_->associate_socket(sock);
    
    auto* op = new iocp_op();
    op->operation = io_op::connect;
    op->sock = sock;
    op->callback = std::move(callback);
    op->user_data = user_data;
    memcpy(&op->addr, addr, addrlen);
    op->addrlen = addrlen;
    
    // Bind to any address (required for ConnectEx)
    struct sockaddr_in local_addr;
    ZeroMemory(&local_addr, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = 0;
    bind(sock, (struct sockaddr*)&local_addr, sizeof(local_addr));
    
    // Start connect operation
    BOOL result = impl_->ConnectEx(
        sock,
        (struct sockaddr*)&op->addr,
        op->addrlen,
        NULL,  // No send buffer
        0,
        NULL,
        &op->overlapped
    );
    
    if (!result && WSAGetLastError() != ERROR_IO_PENDING) {
        delete op;
        impl_->stat_errors.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    
    impl_->stat_connects.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

int iocp_io::close_async(int fd) noexcept {
    impl_->stat_closes.fetch_add(1, std::memory_order_relaxed);
    return closesocket((SOCKET)fd);
}

int iocp_io::poll(uint32_t timeout_us) noexcept {
    if (impl_->iocp_handle == INVALID_HANDLE_VALUE) return -1;
    
    impl_->stat_polls.fetch_add(1, std::memory_order_relaxed);
    
    DWORD timeout_ms = timeout_us / 1000;
    
    // Get completion status
    DWORD bytes_transferred;
    ULONG_PTR completion_key;
    LPOVERLAPPED overlapped;
    
    BOOL result = GetQueuedCompletionStatus(
        impl_->iocp_handle,
        &bytes_transferred,
        &completion_key,
        &overlapped,
        timeout_ms
    );
    
    if (!result && overlapped == NULL) {
        // Timeout or error
        return 0;
    }
    
    // Get operation from OVERLAPPED
    iocp_op* op = CONTAINING_RECORD(overlapped, iocp_op, overlapped);
    std::unique_ptr<iocp_op> op_guard(op);
    
    impl_->stat_events.fetch_add(1, std::memory_order_relaxed);
    
    // Create event
    io_event event;
    event.operation = op->operation;
    event.fd = (int)op->sock;
    event.user_data = op->user_data;
    event.flags = 0;
    
    if (!result) {
        // Operation failed
        event.result = -1;
    } else {
        switch (op->operation) {
            case io_op::accept:
                event.result = (ssize_t)op->accept_socket;
                break;
                
            case io_op::read:
            case io_op::write:
                event.result = bytes_transferred;
                break;
                
            case io_op::connect:
                event.result = 0;  // Success
                break;
                
            default:
                event.result = 0;
                break;
        }
    }
    
    // Invoke callback
    if (op->callback) {
        op->callback(event);
    }
    
    return 1;
}

void iocp_io::run() noexcept {
    if (impl_->running.exchange(true)) {
        return;  // Already running
    }
    
    impl_->stop_requested.store(false);
    
    while (!impl_->stop_requested.load(std::memory_order_acquire)) {
        poll(impl_->config.poll_timeout_us);
    }
    
    impl_->running.store(false);
}

void iocp_io::stop() noexcept {
    impl_->stop_requested.store(true, std::memory_order_release);
    
    // Post quit message to wake up IOCP
    if (impl_->iocp_handle != INVALID_HANDLE_VALUE) {
        PostQueuedCompletionStatus(impl_->iocp_handle, 0, 0, NULL);
    }
}

bool iocp_io::is_running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

async_io::stats iocp_io::get_stats() const noexcept {
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

#endif // _WIN32



