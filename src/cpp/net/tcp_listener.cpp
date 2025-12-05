/**
 * FasterAPI TCP Listener - Implementation
 */

#include "tcp_listener.h"
#include <iostream>
#include <csignal>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

namespace fasterapi {
namespace net {

TcpListener::TcpListener(const TcpListenerConfig& config, ConnectionCallback connection_cb)
    : config_(config)
    , connection_cb_(std::move(connection_cb))
{
    // Auto-detect number of workers
    if (config_.num_workers == 0) {
        config_.num_workers = recommended_worker_count();
    }
}

TcpListener::~TcpListener() {
    stop();
}

int TcpListener::start() {
    if (running_.load()) {
        return -1;  // Already running
    }

    running_.store(true);

    std::cout << "Starting TCP listener on " << config_.host << ":" << config_.port << std::endl;
    std::cout << "Workers: " << config_.num_workers << std::endl;
    std::cout << "SO_REUSEPORT: " << (config_.use_reuseport ? "enabled" : "disabled") << std::endl;

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

void TcpListener::stop() {
    running_.store(false);

    // Stop all event loops
    std::lock_guard<std::mutex> lock(event_loops_mutex_);
    for (auto* event_loop : event_loops_) {
        if (event_loop) {
            event_loop->stop();
        }
    }
}

bool TcpListener::is_running() const {
    return running_.load();
}

void TcpListener::worker_thread(int worker_id) {
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

    // Create listen socket
    int listen_fd = create_listen_socket();
    if (listen_fd < 0) {
        std::cerr << "Worker " << worker_id << ": Failed to create listen socket: " << strerror(errno) << std::endl;
        return;
    }

    std::cout << "Worker " << worker_id << ": Listening on fd " << listen_fd << std::endl;

    // Accept handler
    auto accept_handler = [this, event_loop = event_loop.get()](int fd, IOEvent events, void* user_data) {
        if (!(events & IOEvent::READ)) {
            return;
        }

        // Accept all pending connections (edge-triggered)
        while (true) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);

            // Accept directly without wrapping listen fd
            int client_fd = ::accept(fd, (struct sockaddr*)&client_addr, &addr_len);

            if (client_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // No more connections
                    break;
                }
                std::cerr << "Accept error: " << strerror(errno) << std::endl;
                continue;
            }

            // Wrap client fd in TcpSocket
            TcpSocket socket(client_fd);

            // Call connection callback
            connection_cb_(std::move(socket), event_loop);
        }
    };

    // Add listen socket to event loop
    if (event_loop->add_fd(listen_fd, IOEvent::READ | IOEvent::EDGE, accept_handler, nullptr) < 0) {
        std::cerr << "Worker " << worker_id << ": Failed to add listen socket to event loop: " << strerror(errno) << std::endl;
        close(listen_fd);
        return;
    }

    // Run event loop
    std::cout << "Worker " << worker_id << ": Running event loop" << std::endl;
    event_loop->run();

    // Cleanup
    event_loop->remove_fd(listen_fd);
    close(listen_fd);

    std::cout << "Worker " << worker_id << " stopped" << std::endl;
}

int TcpListener::create_listen_socket() {
    TcpSocket socket;

    if (!socket.is_valid()) {
        return -1;
    }

    // Set socket options
    if (socket.set_reuseaddr() < 0) {
        std::cerr << "Failed to set SO_REUSEADDR: " << strerror(errno) << std::endl;
        return -1;
    }

    // Set SO_REUSEPORT if enabled (allows multiple processes/threads to bind to same port)
    if (config_.use_reuseport) {
        if (socket.set_reuseport() < 0) {
            // SO_REUSEPORT might not be available, continue anyway
            std::cerr << "Warning: Failed to set SO_REUSEPORT: " << strerror(errno) << std::endl;
        }
    }

    // Set non-blocking
    if (socket.set_nonblocking() < 0) {
        std::cerr << "Failed to set non-blocking: " << strerror(errno) << std::endl;
        return -1;
    }

    // Bind
    if (socket.bind(config_.host, config_.port) < 0) {
        std::cerr << "Failed to bind to " << config_.host << ":" << config_.port << ": " << strerror(errno) << std::endl;
        return -1;
    }

    // Listen
    if (socket.listen(config_.backlog) < 0) {
        std::cerr << "Failed to listen: " << strerror(errno) << std::endl;
        return -1;
    }

    // Release ownership and return fd
    return socket.release();
}

} // namespace net
} // namespace fasterapi
