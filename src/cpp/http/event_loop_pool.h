/**
 * Multi-threaded Event Loop Pool for FasterAPI
 *
 * Architecture:
 * - Linux: SO_REUSEPORT - each worker accepts on same port (kernel load-balancing)
 * - Non-Linux: Single acceptor thread distributes via lockfree queues
 *
 * Performance:
 * - Scales linearly with CPU cores
 * - No locks on hot path
 * - Round-robin connection distribution (non-Linux)
 */

#pragma once

#include <coroio/all.hpp>
#include <thread>
#include <vector>
#include <memory>
#include <atomic>
#include <functional>

namespace fasterapi {
namespace http {

// Forward declarations
class HttpServer;

/**
 * Socket wrapper for queue distribution (non-Linux platforms)
 */
template<typename TSocket>
struct PendingConnection {
    TSocket socket;

    PendingConnection(TSocket&& sock) : socket(std::move(sock)) {}

    // Move-only
    PendingConnection(PendingConnection&& other) noexcept
        : socket(std::move(other.socket)) {}
    PendingConnection& operator=(PendingConnection&& other) noexcept {
        socket = std::move(other.socket);
        return *this;
    }
    PendingConnection(const PendingConnection&) = delete;
    PendingConnection& operator=(const PendingConnection&) = delete;
};

/**
 * Multi-threaded event loop pool
 *
 * Two strategies:
 * 1. Linux (SO_REUSEPORT): Each worker binds to same port, kernel distributes
 * 2. Non-Linux: Acceptor thread distributes via lockfree queues
 */
class EventLoopPool {
public:
    /**
     * Configuration for event loop pool
     */
    struct Config {
        uint16_t port;
        std::string host;
        uint16_t num_workers;      // 0 = auto (hardware_concurrency - 2)
        size_t queue_size;         // Per-worker queue size (non-Linux only)
        HttpServer* server;
        std::atomic<bool>* shutdown_flag;
    };

    /**
     * Create event loop pool
     */
    explicit EventLoopPool(const Config& config);

    /**
     * Destructor - ensures clean shutdown
     */
    ~EventLoopPool();

    /**
     * Start the event loop pool
     *
     * Linux: Spawns N worker threads, each binds to same port with SO_REUSEPORT
     * Non-Linux: Spawns 1 acceptor + N workers with lockfree queue distribution
     *
     * Returns 0 on success, non-zero on error
     */
    int start();

    /**
     * Stop the event loop pool
     *
     * Gracefully shuts down all worker threads
     */
    void stop();

    /**
     * Check if pool is running
     */
    bool is_running() const noexcept {
        return running_.load(std::memory_order_relaxed);
    }

    /**
     * Get number of worker threads
     */
    uint16_t num_workers() const noexcept {
        return num_workers_;
    }

private:
    Config config_;
    uint16_t num_workers_;
    std::atomic<bool> running_{false};

    // Worker threads
    std::vector<std::unique_ptr<std::thread>> workers_;

    // Non-Linux only: acceptor thread + distribution queues
#ifndef __linux__
    std::unique_ptr<std::thread> acceptor_thread_;
    std::atomic<uint32_t> next_worker_{0};  // Round-robin counter

    // Simple lockfree queue using atomic flag + vector
    // (We could use Aeron SPSC queue, but this is simpler for socket distribution)
    struct WorkerQueue {
        std::vector<void*> items;  // Store socket pointers
        std::atomic<size_t> head{0};
        std::atomic<size_t> tail{0};
        size_t capacity;

        explicit WorkerQueue(size_t cap) : capacity(cap) {
            items.resize(cap, nullptr);
        }

        bool try_push(void* item) {
            size_t current_tail = tail.load(std::memory_order_relaxed);
            size_t next_tail = (current_tail + 1) % capacity;

            if (next_tail == head.load(std::memory_order_acquire)) {
                return false;  // Queue full
            }

            items[current_tail] = item;
            tail.store(next_tail, std::memory_order_release);
            return true;
        }

        void* try_pop() {
            size_t current_head = head.load(std::memory_order_relaxed);

            if (current_head == tail.load(std::memory_order_acquire)) {
                return nullptr;  // Queue empty
            }

            void* item = items[current_head];
            head.store((current_head + 1) % capacity, std::memory_order_release);
            return item;
        }
    };

    std::vector<std::unique_ptr<WorkerQueue>> worker_queues_;
#endif

    // Platform-specific implementations
#ifdef __linux__
    void run_worker_with_reuseport(int worker_id);
#else
    void run_acceptor();
    void run_worker(int worker_id);
#endif
};

} // namespace http
} // namespace fasterapi
