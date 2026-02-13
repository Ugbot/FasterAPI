/**
 * FasterAPI UDP Listener - Implementation
 */

#include "udp_listener.h"
#include "../core/logger.h"
#include <csignal>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

namespace fasterapi {
namespace net {

UdpListener::UdpListener(const UdpListenerConfig& config, DatagramCallback datagram_cb) noexcept
    : config_(config)
    , datagram_cb_(std::move(datagram_cb))
{
    // Auto-detect number of workers
    if (config_.num_workers == 0) {
        config_.num_workers = recommended_worker_count();
    }

    // Ensure SO_REUSEPORT is enabled for multi-worker setup
    if (config_.num_workers > 1 && !config_.use_reuseport) {
        LOG_WARN("UDP", "Multi-worker setup requires SO_REUSEPORT. Enabling it.");
        config_.use_reuseport = true;
    }
}

UdpListener::~UdpListener() noexcept {
    stop();
}

int UdpListener::start() noexcept {
    if (running_.load()) {
        return -1;  // Already running
    }

    running_.store(true);

    LOG_INFO("UDP", "Starting on %s:%d (workers: %d, reuseport: %s, AF: %s)",
             config_.host.c_str(), config_.port, config_.num_workers,
             config_.use_reuseport ? "enabled" : "disabled",
             config_.address_family == AF_INET ? "IPv4" : "IPv6");
    LOG_INFO("UDP", "Max datagram: %zu bytes, socket buffer: %zu bytes",
             config_.max_datagram_size, config_.recv_buffer_size);

    // Create worker threads
    for (uint16_t i = 0; i < config_.num_workers; i++) {
        worker_threads_.emplace_back([this, i]() {
            worker_thread(i);
        });
    }

    // Wait for all workers to finish
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    worker_threads_.clear();
    return 0;
}

void UdpListener::stop() noexcept {
    running_.store(false);

    // Stop all event loops
    std::lock_guard<std::mutex> lock(event_loops_mutex_);
    for (auto* event_loop : event_loops_) {
        if (event_loop) {
            event_loop->stop();
        }
    }
}

bool UdpListener::is_running() const noexcept {
    return running_.load();
}

void UdpListener::worker_thread(int worker_id) noexcept {
    LOG_INFO("UDP", "Worker %d starting", worker_id);

    // Create event loop for this worker
    auto event_loop = create_event_loop();
    if (!event_loop) {
        LOG_ERROR("UDP", "Worker %d: Failed to create event loop", worker_id);
        return;
    }

    LOG_INFO("UDP", "Worker %d: Using %s event loop", worker_id, event_loop->platform_name());

    // Register event loop for shutdown
    {
        std::lock_guard<std::mutex> lock(event_loops_mutex_);
        event_loops_.push_back(event_loop.get());
    }

    // Create UDP socket
    int udp_fd = create_udp_socket();
    if (udp_fd < 0) {
        LOG_ERROR("UDP", "Worker %d: Failed to create UDP socket: %s", worker_id, strerror(errno));
        return;
    }

    LOG_INFO("UDP", "Worker %d: Listening on fd %d", worker_id, udp_fd);

    // Pre-allocate receive buffer (no allocations in hot path)
    std::vector<uint8_t> recv_buffer(config_.max_datagram_size);

    // Datagram receive handler
    auto recv_handler = [this, event_loop = event_loop.get(), &recv_buffer](
        int fd, IOEvent events, void* user_data) {

        if (!(events & IOEvent::READ)) {
            return;
        }

        // Receive all pending datagrams (edge-triggered)
        while (true) {
            struct sockaddr_storage addr;
            socklen_t addr_len = sizeof(addr);

            // Receive datagram
            ssize_t n = ::recvfrom(fd, recv_buffer.data(), recv_buffer.size(),
                                   0, (struct sockaddr*)&addr, &addr_len);

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // No more datagrams
                    break;
                }
                LOG_ERROR("UDP", "recvfrom error: %s", strerror(errno));
                continue;
            }

            if (n == 0) {
                // Empty datagram (shouldn't happen with UDP, but handle it)
                continue;
            }

            // Call datagram callback with socket fd for response sending
            datagram_cb_(recv_buffer.data(), static_cast<size_t>(n),
                        (struct sockaddr*)&addr, addr_len, event_loop, fd);
        }
    };

    // Add UDP socket to event loop (edge-triggered for maximum throughput)
    if (event_loop->add_fd(udp_fd, IOEvent::READ | IOEvent::EDGE, recv_handler, nullptr) < 0) {
        LOG_ERROR("UDP", "Worker %d: Failed to add UDP socket to event loop: %s",
                  worker_id, strerror(errno));
        close(udp_fd);
        return;
    }

    // Run event loop
    LOG_INFO("UDP", "Worker %d: Running event loop", worker_id);
    event_loop->run();

    // Cleanup
    event_loop->remove_fd(udp_fd);
    close(udp_fd);

    LOG_INFO("UDP", "Worker %d stopped", worker_id);
}

int UdpListener::create_udp_socket() noexcept {
    UdpSocket socket(config_.address_family == AF_INET6);

    if (!socket.is_valid()) {
        LOG_ERROR("UDP", "Failed to create UDP socket: %s", strerror(errno));
        return -1;
    }

    // Set socket options
    if (socket.set_reuseaddr() < 0) {
        LOG_ERROR("UDP", "Failed to set SO_REUSEADDR: %s", strerror(errno));
        return -1;
    }

    // Set SO_REUSEPORT if enabled (required for multi-worker)
    if (config_.use_reuseport) {
        if (socket.set_reuseport() < 0) {
            LOG_ERROR("UDP", "Failed to set SO_REUSEPORT: %s", strerror(errno));
            return -1;
        }
    }

    // Set non-blocking
    if (socket.set_nonblocking() < 0) {
        LOG_ERROR("UDP", "Failed to set non-blocking: %s", strerror(errno));
        return -1;
    }

    // Set receive buffer size (important for high-throughput UDP)
    if (socket.set_recv_buffer_size(config_.recv_buffer_size) < 0) {
        LOG_WARN("UDP", "Failed to set receive buffer size to %zu: %s",
                 config_.recv_buffer_size, strerror(errno));
        // Continue anyway - not critical
    }

    // Enable packet info (to get destination address)
    if (config_.enable_pktinfo) {
        if (socket.set_recv_pktinfo(true) < 0) {
            LOG_WARN("UDP", "Failed to enable IP_PKTINFO: %s", strerror(errno));
            // Continue anyway - not critical for basic operation
        }
    }

    // Enable TOS/ECN info (for congestion control)
    if (config_.enable_tos) {
        if (socket.set_recv_tos(true) < 0) {
            LOG_WARN("UDP", "Failed to enable IP_RECVTOS: %s", strerror(errno));
            // Continue anyway - not critical for basic operation
        }
    }

    // Bind to address
    if (socket.bind(config_.host, config_.port) < 0) {
        LOG_ERROR("UDP", "Failed to bind to %s:%d: %s",
                  config_.host.c_str(), config_.port, strerror(errno));
        return -1;
    }

    // Release ownership and return fd
    return socket.release();
}

} // namespace net
} // namespace fasterapi
