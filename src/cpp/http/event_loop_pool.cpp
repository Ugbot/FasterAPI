/**
 * EventLoopPool Implementation
 *
 * Implements multi-threaded event loop with platform-specific optimizations
 */

#include "event_loop_pool.h"
#include "server.h"
#include "http1_parser.h"
#include "python_callback_bridge.h"
#include "request.h"
#include "response.h"

#include <iostream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <netinet/tcp.h>

using namespace NNet;

namespace fasterapi {
namespace http {

// Shared constants (from http1_coroio_handler.cpp)
static constexpr size_t READ_BUFFER_SIZE = 16384;  // 16KB
static constexpr size_t MAX_REQUEST_SIZE = 1024 * 1024;  // 1MB

/**
 * Handle a single HTTP connection (coroutine)
 *
 * This is the same logic from http1_coroio_handler.cpp
 * It handles keep-alive, parsing, routing, and response
 */
template<typename TSocket>
static NNet::TVoidTask handle_connection(
    TSocket socket,
    HttpServer* server,
    std::atomic<bool>* shutdown_requested
) {
    char buffer[READ_BUFFER_SIZE];
    bool keep_alive = true;
    int requests_handled = 0;

    // Enable TCP_NODELAY for lower latency
    int fd = socket.Fd();
    int optval = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));

    try {
        // Keep-alive loop
        while (keep_alive && !shutdown_requested->load(std::memory_order_relaxed)) {
            std::string accumulated;
            accumulated.reserve(1024);

            // Read request
            while (true) {
                auto bytes_read = co_await socket.ReadSome(buffer, sizeof(buffer));

                if (bytes_read <= 0) {
                    co_return;
                }

                accumulated.append(buffer, bytes_read);

                if (accumulated.find("\r\n\r\n") != std::string::npos) {
                    break;
                }

                if (accumulated.size() > MAX_REQUEST_SIZE) {
                    const char* error_response =
                        "HTTP/1.1 413 Payload Too Large\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n\r\n";
                    co_await socket.WriteSome(error_response, strlen(error_response));
                    co_return;
                }
            }

            // Parse HTTP request
            HTTP1Parser parser;
            HTTP1Request parsed_request;
            size_t consumed = 0;

            auto parse_result = parser.parse(
                reinterpret_cast<const uint8_t*>(accumulated.data()),
                accumulated.size(),
                parsed_request,
                consumed
            );

            if (parse_result != 0) {
                const char* error_response =
                    "HTTP/1.1 400 Bad Request\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n\r\n";
                co_await socket.WriteSome(error_response, strlen(error_response));
                co_return;
            }

            // Extract request details
            std::string method(parsed_request.method_str);
            std::string path(parsed_request.path);

            // Convert headers to map
            std::unordered_map<std::string, std::string> headers_map;
            for (size_t i = 0; i < parsed_request.header_count; i++) {
                const auto& h = parsed_request.headers[i];
                headers_map[std::string(h.name)] = std::string(h.value);
            }

            // Get body
            std::string body;
            size_t header_end = accumulated.find("\r\n\r\n");
            if (header_end != std::string::npos && accumulated.size() > header_end + 4) {
                body = accumulated.substr(header_end + 4);
            }

            // Check Connection header for keep-alive
            auto connection_it = headers_map.find("Connection");
            if (connection_it != headers_map.end()) {
                std::string conn_value = connection_it->second;
                std::transform(conn_value.begin(), conn_value.end(), conn_value.begin(), ::tolower);
                keep_alive = (conn_value.find("keep-alive") != std::string::npos);
            } else {
                keep_alive = (parsed_request.version == HTTP1Version::HTTP_1_1);
            }

            // Build response
            std::string response;

            // Try C++ routes first
            const auto& routes = server->get_routes();
            auto method_it = routes.find(method);
            bool handled_by_cpp = false;

            if (method_it != routes.end()) {
                auto route_it = method_it->second.find(path);
                if (route_it != method_it->second.end()) {
                    handled_by_cpp = true;

                    HttpRequest request = HttpRequest::from_parsed_data(method, path, headers_map, body);
                    HttpResponse response_obj;

                    route_it->second(&request, &response_obj);

                    response = response_obj.to_http_wire_format(keep_alive);
                }
            }

            // Fallback to Python handlers
            if (!handled_by_cpp) {
                auto result = PythonCallbackBridge::invoke_handler(
                    method, path, headers_map, body
                );

                std::ostringstream response_stream;
                response_stream << "HTTP/1.1 " << result.status_code;

                switch (result.status_code) {
                    case 200: response_stream << " OK"; break;
                    case 404: response_stream << " Not Found"; break;
                    case 500: response_stream << " Internal Server Error"; break;
                    default: response_stream << " Unknown"; break;
                }
                response_stream << "\r\n";

                response_stream << "Content-Type: " << result.content_type << "\r\n";
                response_stream << "Content-Length: " << result.body.size() << "\r\n";

                if (keep_alive) {
                    response_stream << "Connection: keep-alive\r\n";
                } else {
                    response_stream << "Connection: close\r\n";
                }

                response_stream << "\r\n";
                response_stream << result.body;

                response = response_stream.str();
            }

            // Send response
            co_await socket.WriteSome(response.data(), response.size());

            requests_handled++;
        }
    } catch (const std::exception& e) {
        // Connection error (client closed, timeout, etc.)
    }

    co_return;
}

EventLoopPool::EventLoopPool(const Config& config)
    : config_(config)
{
    // Auto-detect worker count if not specified
    if (config.num_workers == 0) {
        uint16_t hw_concurrency = std::thread::hardware_concurrency();
        num_workers_ = (hw_concurrency > 2) ? (hw_concurrency - 2) : 1;
    } else {
        num_workers_ = config.num_workers;
    }

    std::cout << "EventLoopPool: Using " << num_workers_ << " worker threads" << std::endl;

#ifdef __linux__
    std::cout << "EventLoopPool: Linux detected - using SO_REUSEPORT strategy" << std::endl;
#else
    std::cout << "EventLoopPool: Non-Linux platform - using acceptor+queue strategy" << std::endl;

    // Create lockfree queues for each worker
    for (uint16_t i = 0; i < num_workers_; i++) {
        worker_queues_.push_back(std::make_unique<WorkerQueue>(config.queue_size));
    }
#endif
}

EventLoopPool::~EventLoopPool() {
    stop();
}

int EventLoopPool::start() {
    if (running_.load()) {
        return 1;  // Already running
    }

    running_.store(true);

#ifdef __linux__
    // Linux: Spawn N workers, each binds to same port with SO_REUSEPORT
    for (uint16_t i = 0; i < num_workers_; i++) {
        workers_.push_back(std::make_unique<std::thread>(
            &EventLoopPool::run_worker_with_reuseport, this, i
        ));
    }
#else
    // Non-Linux: Spawn 1 acceptor + N workers
    acceptor_thread_ = std::make_unique<std::thread>(
        &EventLoopPool::run_acceptor, this
    );

    for (uint16_t i = 0; i < num_workers_; i++) {
        workers_.push_back(std::make_unique<std::thread>(
            &EventLoopPool::run_worker, this, i
        ));
    }
#endif

    // Give threads time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    return 0;
}

void EventLoopPool::stop() {
    if (!running_.load()) {
        return;
    }

    std::cout << "EventLoopPool: Initiating graceful shutdown..." << std::endl;

    // Signal shutdown
    config_.shutdown_flag->store(true, std::memory_order_relaxed);
    running_.store(false);

    // Join all threads
#ifndef __linux__
    if (acceptor_thread_ && acceptor_thread_->joinable()) {
        acceptor_thread_->join();
    }
#endif

    for (auto& worker : workers_) {
        if (worker && worker->joinable()) {
            worker->join();
        }
    }

    workers_.clear();

    std::cout << "EventLoopPool: All workers stopped cleanly" << std::endl;
}

#ifdef __linux__
/**
 * Linux worker: Binds to same port with SO_REUSEPORT
 * Kernel load-balances incoming connections across workers
 */
void EventLoopPool::run_worker_with_reuseport(int worker_id) {
    try {
        // Initialize CoroIO on this thread
        NNet::TInitializer init;

        // Create event loop on this thread
        NNet::TLoop<TDefaultPoller> loop;

        // Poll handler registrations (only worker 0 needs to do this)
        if (worker_id == 0) {
            PythonCallbackBridge::poll_registrations();
        }

        // Create listening address
        TAddress addr(config_.host.c_str(), config_.port);

        // Create socket
        typename TDefaultPoller::TSocket listen_socket(loop.Poller(), addr.Domain());

        // Enable socket options
        int fd = listen_socket.Fd();
        int optval = 1;

        // SO_REUSEADDR: Allow rapid restart
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

        // SO_REUSEPORT: Allow multiple threads to bind to same port (Linux only)
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));

        // Bind
        listen_socket.Bind(addr);

        // Listen with large backlog
        listen_socket.Listen(4096);

        if (worker_id == 0) {
            std::cout << "EventLoopPool: Worker 0 listening on " << config_.host << ":" << config_.port << std::endl;
            std::cout << "EventLoopPool: SO_REUSEPORT enabled - " << num_workers_ << " workers accepting" << std::endl;
        }

        // Accept loop
        while (!config_.shutdown_flag->load(std::memory_order_relaxed)) {
            auto client_socket = co_await listen_socket.Accept();

            if (config_.shutdown_flag->load(std::memory_order_relaxed)) {
                break;
            }

            // Spawn coroutine to handle this connection
            handle_connection(std::move(client_socket), config_.server, config_.shutdown_flag);
        }

        std::cout << "EventLoopPool: Worker " << worker_id << " exiting" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "EventLoopPool: Worker " << worker_id << " error: " << e.what() << std::endl;
    }
}
#else
/**
 * Non-Linux acceptor: Accepts connections and distributes via round-robin
 */
void EventLoopPool::run_acceptor() {
    try {
        // Initialize CoroIO on this thread
        NNet::TInitializer init;

        // Create event loop on this thread
        NNet::TLoop<TDefaultPoller> loop;

        // Poll handler registrations
        PythonCallbackBridge::poll_registrations();

        // Create listening address
        TAddress addr(config_.host.c_str(), config_.port);

        // Create socket
        typename TDefaultPoller::TSocket listen_socket(loop.Poller(), addr.Domain());

        // Enable socket options
        int fd = listen_socket.Fd();
        int optval = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

        // Bind and listen
        listen_socket.Bind(addr);
        listen_socket.Listen(4096);

        std::cout << "EventLoopPool: Acceptor listening on " << config_.host << ":" << config_.port << std::endl;
        std::cout << "EventLoopPool: Distributing to " << num_workers_ << " workers" << std::endl;

        // Accept loop
        while (!config_.shutdown_flag->load(std::memory_order_relaxed)) {
            auto client_socket = co_await listen_socket.Accept();

            if (config_.shutdown_flag->load(std::memory_order_relaxed)) {
                break;
            }

            // Round-robin distribution to workers
            uint32_t worker_id = next_worker_.fetch_add(1, std::memory_order_relaxed) % num_workers_;

            // Allocate socket on heap for queue transfer
            // Worker will take ownership and delete it
            auto* socket_ptr = new typename TDefaultPoller::TSocket(std::move(client_socket));

            // Try to push to worker queue
            if (!worker_queues_[worker_id]->try_push(socket_ptr)) {
                // Queue full - drop connection
                delete socket_ptr;
                std::cerr << "EventLoopPool: Worker " << worker_id << " queue full, dropping connection" << std::endl;
            }
        }

        std::cout << "EventLoopPool: Acceptor exiting" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "EventLoopPool: Acceptor error: " << e.what() << std::endl;
    }
}

/**
 * Non-Linux worker: Polls queue for connections from acceptor
 */
void EventLoopPool::run_worker(int worker_id) {
    try {
        // Initialize CoroIO on this thread
        NNet::TInitializer init;

        // Create event loop on this thread
        NNet::TLoop<TDefaultPoller> loop;

        std::cout << "EventLoopPool: Worker " << worker_id << " started" << std::endl;

        // Worker loop
        while (!config_.shutdown_flag->load(std::memory_order_relaxed)) {
            // Poll queue for new connections (non-blocking)
            void* socket_ptr = worker_queues_[worker_id]->try_pop();

            if (socket_ptr) {
                // Take ownership of socket
                auto* sock = static_cast<typename TDefaultPoller::TSocket*>(socket_ptr);

                // Spawn coroutine to handle this connection
                handle_connection(std::move(*sock), config_.server, config_.shutdown_flag);

                // Delete socket wrapper
                delete sock;
            }

            // Run event loop iteration (process I/O events)
            // Note: This is a simplified version - in production we'd use loop.Poll() with timeout
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        std::cout << "EventLoopPool: Worker " << worker_id << " exiting" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "EventLoopPool: Worker " << worker_id << " error: " << e.what() << std::endl;
    }
}
#endif

} // namespace http
} // namespace fasterapi
