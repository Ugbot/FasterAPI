/**
 * HTTP/1.1 Server using CoroIO
 *
 * Coroutine-based HTTP/1.1 server implementation using the CoroIO library.
 * Provides cross-platform async I/O with kqueue (macOS), epoll (Linux),
 * io_uring (Linux), and IOCP (Windows).
 *
 * Features:
 * - Multi-threaded event loop pool (scales to CPU cores)
 * - Coroutine-per-connection pattern
 * - Zero-copy HTTP parsing
 * - Keep-alive support
 * - Lock-free architecture
 * - Linux: SO_REUSEPORT for kernel-level load balancing
 * - Non-Linux: Acceptor thread + lockfree queue distribution
 */

#pragma once

#include "server.h"
#include "http1_parser.h"
#include <memory>
#include <atomic>
#include <thread>

// Forward declare HttpServer to avoid circular dependency
class HttpServer;

namespace fasterapi {
namespace http {

// Forward declare EventLoopPool
class EventLoopPool;

/**
 * CoroIO-based HTTP/1.1 handler
 */
class Http1CoroioHandler {
public:
    explicit Http1CoroioHandler(HttpServer* server);
    ~Http1CoroioHandler();

    /**
     * Start HTTP/1.1 server on specified port and host.
     *
     * This starts a multi-threaded event loop pool.
     * Linux: Each worker binds to same port with SO_REUSEPORT
     * Non-Linux: Acceptor thread + lockfree queue distribution
     *
     * The event loops will:
     * 1. Accept incoming connections (distributed across workers)
     * 2. Spawn a coroutine for each connection
     * 3. Parse HTTP/1.1 requests
     * 4. Route to handlers
     * 5. Send responses
     *
     * @param port Port to listen on
     * @param host Host address to bind to
     * @param num_workers Number of worker threads (0 = auto: hw_concurrency - 2)
     * @param queue_size Per-worker queue size for non-Linux platforms
     * @return 0 on success, error code otherwise
     */
    int start(uint16_t port, const std::string& host, uint16_t num_workers = 0, size_t queue_size = 1024);

    /**
     * Stop the HTTP/1.1 server.
     *
     * @return 0 on success
     */
    int stop();

    /**
     * Check if server is running.
     *
     * @return true if running
     */
    bool is_running() const noexcept;

private:
    HttpServer* server_;
    std::atomic<bool> running_{false};
    std::unique_ptr<EventLoopPool> event_loop_pool_;

    // Internal implementation details
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace http
} // namespace fasterapi
