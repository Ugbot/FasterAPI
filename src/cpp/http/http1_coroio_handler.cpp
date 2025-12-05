/**
 * HTTP/1.1 Server using CoroIO + Multi-threaded Event Loop Pool
 *
 * Architecture:
 * - Multi-threaded event loop pool for CPU core utilization
 * - CoroIO provides coroutine-based async I/O
 * - HTTP parsing, routing, and response on dedicated worker threads
 * - Linux: SO_REUSEPORT for kernel-level load balancing
 * - Non-Linux: Acceptor thread + lockfree queue distribution
 */

#include "http1_coroio_handler.h"
#include "event_loop_pool.h"
#include "http1_parser.h"
#include "python_callback_bridge.h"
#include "request.h"
#include "response.h"

#include <coroio/all.hpp>
#include <iostream>
#include <cstring>
#include <memory>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <sys/socket.h>
#include <netinet/tcp.h>

using namespace NNet;

namespace fasterapi {
namespace http {

/**
 * Implementation details (hidden from header)
 */
struct Http1CoroioHandler::Impl {
    HttpServer* server;
    uint16_t port;
    std::string host;
    std::atomic<bool> shutdown_requested{false};

    Impl(HttpServer* srv) : server(srv), port(0) {}
};

Http1CoroioHandler::Http1CoroioHandler(HttpServer* server)
    : server_(server),
      impl_(std::make_unique<Impl>(server)) {
}

Http1CoroioHandler::~Http1CoroioHandler() {
    stop();
}

// Note: handle_connection and server_listener are now in event_loop_pool.cpp
// The EventLoopPool handles all the multi-threaded event loop logic

int Http1CoroioHandler::start(uint16_t port, const std::string& host, uint16_t num_workers, size_t queue_size) {
    if (running_.load()) {
        return 1;  // Already running
    }

    impl_->port = port;
    impl_->host = host;
    impl_->shutdown_requested.store(false, std::memory_order_relaxed);

    std::cout << "==================================================================\n";
    std::cout << "🚀 Starting FasterAPI HTTP/1.1 Multi-Threaded Server\n";
    std::cout << "==================================================================\n";

    // Create event loop pool configuration
    EventLoopPool::Config pool_config;
    pool_config.port = port;
    pool_config.host = host;
    pool_config.num_workers = num_workers;  // 0 = auto (hw_concurrency - 2)
    pool_config.queue_size = queue_size;
    pool_config.server = server_;
    pool_config.shutdown_flag = &impl_->shutdown_requested;

    // Create and start the event loop pool
    event_loop_pool_ = std::make_unique<EventLoopPool>(pool_config);

    int result = event_loop_pool_->start();
    if (result != 0) {
        std::cerr << "❌ Failed to start event loop pool" << std::endl;
        return result;
    }

    running_.store(true);

    std::cout << "==================================================================\n";
    std::cout << "✅ Server started with " << event_loop_pool_->num_workers() << " worker threads\n";
    std::cout << "==================================================================\n";

    return 0;
}

int Http1CoroioHandler::stop() {
    if (!running_.load()) {
        return 0;  // Not running
    }

    std::cout << "\n==================================================================\n";
    std::cout << "🛑 Stopping FasterAPI HTTP/1.1 Server\n";
    std::cout << "==================================================================\n";

    // Signal shutdown (lockfree atomic flag)
    impl_->shutdown_requested.store(true, std::memory_order_relaxed);
    running_.store(false);

    // Stop the event loop pool (waits for all workers to finish)
    if (event_loop_pool_) {
        event_loop_pool_->stop();
        event_loop_pool_.reset();
    }

    std::cout << "==================================================================\n";
    std::cout << "✅ Server stopped cleanly\n";
    std::cout << "==================================================================\n";

    return 0;
}

bool Http1CoroioHandler::is_running() const noexcept {
    return running_.load();
}

} // namespace http
} // namespace fasterapi
