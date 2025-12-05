/**
 * FasterAPI UDP Listener - Multi-threaded UDP server
 *
 * Features:
 * - SO_REUSEPORT for kernel-level load balancing
 * - Automatic worker thread creation
 * - Integration with EventLoop
 * - Thread-per-core architecture
 * - Pre-allocated receive buffers (no allocations in hot path)
 *
 * Designed for HTTP/3/QUIC where each worker handles its own
 * connection state independently.
 */

#pragma once

#include "event_loop.h"
#include "udp_socket.h"
#include <cstdint>
#include <string>
#include <vector>
#include <thread>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>

namespace fasterapi {
namespace net {

/**
 * Datagram callback
 *
 * Called when a datagram is received.
 * @param data Pointer to datagram data
 * @param length Length of datagram
 * @param addr Source address
 * @param addrlen Length of address structure
 * @param event_loop The event loop for this worker thread
 *
 * IMPORTANT: The data pointer is only valid during the callback.
 * Copy data if needed beyond callback scope.
 */
using DatagramCallback = std::function<void(
    const uint8_t* data,
    size_t length,
    const struct sockaddr* addr,
    socklen_t addrlen,
    EventLoop* event_loop
)>;

/**
 * UDP Listener configuration
 */
struct UdpListenerConfig {
    std::string host = "0.0.0.0";      // Bind address
    uint16_t port = 443;               // Bind port (default QUIC/HTTP3 port)
    uint16_t num_workers = 0;          // 0 = auto (recommended_worker_count())
    bool use_reuseport = true;         // Use SO_REUSEPORT (required for multi-worker)
    size_t recv_buffer_size = 2 * 1024 * 1024;  // 2MB socket receive buffer
    size_t max_datagram_size = 65535;  // Maximum datagram size (64KB)
    int address_family = AF_INET;      // AF_INET or AF_INET6
    bool enable_pktinfo = true;        // Enable IP_PKTINFO/IPV6_RECVPKTINFO
    bool enable_tos = true;            // Enable IP_RECVTOS/IPV6_RECVTCLASS (for ECN)
};

/**
 * Multi-threaded UDP Listener
 *
 * Creates multiple worker threads, each with:
 * - Its own UDP socket bound to same port (via SO_REUSEPORT)
 * - Its own event loop
 * - Pre-allocated receive buffer (no allocations in hot path)
 *
 * The kernel distributes incoming datagrams across workers for
 * optimal multi-core scaling.
 *
 * Usage example:
 *
 *   UdpListenerConfig config;
 *   config.port = 443;
 *   config.num_workers = 4;
 *
 *   UdpListener listener(config, [](const uint8_t* data, size_t len,
 *                                     const sockaddr* addr, socklen_t addrlen,
 *                                     EventLoop* loop) {
 *       // Process QUIC packet
 *       quic_connection_manager->process_packet(data, len, addr, addrlen);
 *   });
 *
 *   listener.start();  // Blocks until stop() is called
 */
class UdpListener {
public:
    /**
     * Create a UDP listener
     * @param config Listener configuration
     * @param datagram_cb Callback for received datagrams
     */
    UdpListener(const UdpListenerConfig& config, DatagramCallback datagram_cb) noexcept;

    /**
     * Destructor stops the listener
     */
    ~UdpListener() noexcept;

    // Non-copyable, non-movable
    UdpListener(const UdpListener&) = delete;
    UdpListener& operator=(const UdpListener&) = delete;
    UdpListener(UdpListener&&) = delete;
    UdpListener& operator=(UdpListener&&) = delete;

    /**
     * Start listening and receiving datagrams
     * This creates worker threads and blocks until stop() is called.
     * @return 0 on success, -1 on error
     */
    int start() noexcept;

    /**
     * Stop the listener
     * Thread-safe. Can be called from any thread.
     */
    void stop() noexcept;

    /**
     * Check if listener is running
     */
    bool is_running() const noexcept;

    /**
     * Get number of worker threads
     */
    uint16_t num_workers() const noexcept { return config_.num_workers; }

    /**
     * Get listener configuration
     */
    const UdpListenerConfig& config() const noexcept { return config_; }

private:
    void worker_thread(int worker_id) noexcept;
    int create_udp_socket() noexcept;

    UdpListenerConfig config_;
    DatagramCallback datagram_cb_;
    std::vector<std::thread> worker_threads_;
    std::vector<EventLoop*> event_loops_;  // Track event loops for shutdown
    std::atomic<bool> running_{false};
    std::mutex event_loops_mutex_;  // Protect event_loops_ vector
};

} // namespace net
} // namespace fasterapi
