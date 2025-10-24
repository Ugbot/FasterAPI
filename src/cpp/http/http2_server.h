/**
 * Native HTTP/2 Server with Python Integration
 *
 * High-performance HTTP/2 server using:
 * - Native event loop (kqueue/epoll) for 100K+ req/s
 * - Pure C++ HTTP/2 implementation (no external dependencies)
 * - Multi-threaded with SO_REUSEPORT
 * - PythonCallbackBridge for Python route handlers
 *
 * Features:
 * - h2c (HTTP/2 Cleartext) and TLS support
 * - Stream multiplexing
 * - Server push (future)
 * - Header compression (HPACK)
 * - Async coroutine execution with wake-based resumption
 */

#pragma once

#include "../net/tcp_listener.h"
#include "../net/event_loop.h"
#include "python_callback_bridge.h"
#include "../core/async_io.h"
#include "../core/coro_resumer.h"
#include <memory>
#include <unordered_map>
#include <atomic>

namespace fasterapi {
namespace http {

/**
 * HTTP/2 Server Configuration
 */
struct Http2ServerConfig {
    // Network configuration
    uint16_t port = 8080;
    std::string host = "0.0.0.0";
    bool use_reuseport = true;
    bool enable_tls = false;   // TLS support (future)

    // Worker configuration (hybrid model)
    uint16_t num_pinned_workers = 0;        // Workers with dedicated sub-interpreters (0 = auto = CPU count)
    uint16_t num_pooled_workers = 0;        // Additional workers sharing pooled interpreters (0 = none)
    uint16_t num_pooled_interpreters = 0;   // Size of shared interpreter pool (0 = auto = pooled_workers/2)
};

/**
 * HTTP/2 Server with Python integration
 *
 * Usage:
 *   Http2Server server(config);
 *   server.start();  // Blocks until stop()
 */
class Http2Server {
public:
    /**
     * Create HTTP/2 server with configuration
     */
    explicit Http2Server(const Http2ServerConfig& config);

    /**
     * Destructor - ensures clean shutdown
     */
    ~Http2Server();

    /**
     * Start the HTTP/2 server (blocks until stop())
     *
     * Returns 0 on success, -1 on error
     */
    int start();

    /**
     * Stop the HTTP/2 server
     */
    void stop();

    /**
     * Check if server is running
     */
    bool is_running() const noexcept {
        return listener_ != nullptr && !shutdown_flag_.load(std::memory_order_relaxed);
    }

private:
    Http2ServerConfig config_;
    std::unique_ptr<net::TcpListener> listener_;
    std::atomic<bool> shutdown_flag_{false};

    // Coroutine resumption infrastructure
    std::unique_ptr<core::async_io> wake_io_;
    std::unique_ptr<core::CoroResumer> coro_resumer_;

    /**
     * Connection handler callback
     */
    static void on_connection(net::TcpSocket socket, net::EventLoop* event_loop);
};

/**
 * C API for Python bindings
 */
extern "C" {
    /**
     * Create HTTP/2 server
     *
     * @param port Server port
     * @param num_workers Number of worker threads (0 = auto)
     * @return Server handle (opaque pointer)
     */
    void* http2_server_create(uint16_t port, uint16_t num_workers);

    /**
     * Start HTTP/2 server (blocking)
     *
     * @param server Server handle
     * @return 0 on success, -1 on error
     */
    int http2_server_start(void* server);

    /**
     * Stop HTTP/2 server
     *
     * @param server Server handle
     */
    void http2_server_stop(void* server);

    /**
     * Destroy HTTP/2 server
     *
     * @param server Server handle
     */
    void http2_server_destroy(void* server);
}

} // namespace http
} // namespace fasterapi
