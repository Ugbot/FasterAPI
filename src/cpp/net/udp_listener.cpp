/**
 * FasterAPI UDP Listener - Implementation
 */

#include "udp_listener.h"
#include <iostream>
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
        std::cerr << "Warning: Multi-worker setup requires SO_REUSEPORT. Enabling it." << std::endl;
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

    std::cout << "Starting UDP listener on " << config_.host << ":" << config_.port << std::endl;
    std::cout << "Workers: " << config_.num_workers << std::endl;
    std::cout << "SO_REUSEPORT: " << (config_.use_reuseport ? "enabled" : "disabled") << std::endl;
    std::cout << "Address family: " << (config_.address_family == AF_INET ? "IPv4" : "IPv6") << std::endl;
    std::cout << "Max datagram size: " << config_.max_datagram_size << " bytes" << std::endl;
    std::cout << "Socket buffer size: " << config_.recv_buffer_size << " bytes" << std::endl;

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
    std::cout << "Worker " << worker_id << " starting" << std::endl;

    // Create event loop for this worker
    auto event_loop = create_event_loop();
    if (!event_loop) {
        std::cerr << "Worker " << worker_id << ": Failed to create event loop" << std::endl;
        return;
    }

    std::cout << "Worker " << worker_id << ": Using " << event_loop->platform_name() << " event loop" << std::endl;

    // Register event loop for shutdown
    {
        std::lock_guard<std::mutex> lock(event_loops_mutex_);
        event_loops_.push_back(event_loop.get());
    }

    // Create UDP socket
    int udp_fd = create_udp_socket();
    if (udp_fd < 0) {
        std::cerr << "Worker " << worker_id << ": Failed to create UDP socket: " << strerror(errno) << std::endl;
        return;
    }

    std::cout << "Worker " << worker_id << ": Listening on fd " << udp_fd << std::endl;

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
                std::cerr << "recvfrom error: " << strerror(errno) << std::endl;
                continue;
            }

            if (n == 0) {
                // Empty datagram (shouldn't happen with UDP, but handle it)
                continue;
            }

            // Call datagram callback
            datagram_cb_(recv_buffer.data(), static_cast<size_t>(n),
                        (struct sockaddr*)&addr, addr_len, event_loop);
        }
    };

    // Add UDP socket to event loop (edge-triggered for maximum throughput)
    if (event_loop->add_fd(udp_fd, IOEvent::READ | IOEvent::EDGE, recv_handler, nullptr) < 0) {
        std::cerr << "Worker " << worker_id << ": Failed to add UDP socket to event loop: "
                  << strerror(errno) << std::endl;
        close(udp_fd);
        return;
    }

    // Run event loop
    std::cout << "Worker " << worker_id << ": Running event loop" << std::endl;
    event_loop->run();

    // Cleanup
    event_loop->remove_fd(udp_fd);
    close(udp_fd);

    std::cout << "Worker " << worker_id << " stopped" << std::endl;
}

int UdpListener::create_udp_socket() noexcept {
    UdpSocket socket(config_.address_family == AF_INET6);

    if (!socket.is_valid()) {
        std::cerr << "Failed to create UDP socket: " << strerror(errno) << std::endl;
        return -1;
    }

    // Set socket options
    if (socket.set_reuseaddr() < 0) {
        std::cerr << "Failed to set SO_REUSEADDR: " << strerror(errno) << std::endl;
        return -1;
    }

    // Set SO_REUSEPORT if enabled (required for multi-worker)
    if (config_.use_reuseport) {
        if (socket.set_reuseport() < 0) {
            std::cerr << "Failed to set SO_REUSEPORT: " << strerror(errno) << std::endl;
            return -1;
        }
    }

    // Set non-blocking
    if (socket.set_nonblocking() < 0) {
        std::cerr << "Failed to set non-blocking: " << strerror(errno) << std::endl;
        return -1;
    }

    // Set receive buffer size (important for high-throughput UDP)
    if (socket.set_recv_buffer_size(config_.recv_buffer_size) < 0) {
        std::cerr << "Warning: Failed to set receive buffer size to "
                  << config_.recv_buffer_size << ": " << strerror(errno) << std::endl;
        // Continue anyway - not critical
    }

    // Enable packet info (to get destination address)
    if (config_.enable_pktinfo) {
        if (socket.set_recv_pktinfo(true) < 0) {
            std::cerr << "Warning: Failed to enable IP_PKTINFO: " << strerror(errno) << std::endl;
            // Continue anyway - not critical for basic operation
        }
    }

    // Enable TOS/ECN info (for congestion control)
    if (config_.enable_tos) {
        if (socket.set_recv_tos(true) < 0) {
            std::cerr << "Warning: Failed to enable IP_RECVTOS: " << strerror(errno) << std::endl;
            // Continue anyway - not critical for basic operation
        }
    }

    // Bind to address
    if (socket.bind(config_.host, config_.port) < 0) {
        std::cerr << "Failed to bind to " << config_.host << ":" << config_.port
                  << ": " << strerror(errno) << std::endl;
        return -1;
    }

    // Release ownership and return fd
    return socket.release();
}

} // namespace net
} // namespace fasterapi
