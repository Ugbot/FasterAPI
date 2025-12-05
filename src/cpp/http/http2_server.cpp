/**
 * Native HTTP/2 Server with Python Integration
 *
 * Pure C++ HTTP/2 implementation integrated with PythonCallbackBridge
 */

#include "http2_server.h"
#include "http2_connection.h"
#include "python_callback_bridge.h"
#include "../core/coro_task.h"
#include "../core/awaitable_future.h"
#include "../core/coro_resumer.h"
#include "../core/async_io.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <vector>
#include <cerrno>
#include <thread>

namespace fasterapi {
namespace http {

// HTTP/2 server connection state (renamed to avoid collision with http2::Http2Connection)
struct Http2ServerConnection {
    net::TcpSocket socket;
    int fd;  // Cached fd for convenience
    http2::Http2Connection* http2_conn;  // Pure C++ HTTP/2 connection
    core::async_io* async_io_engine;     // For wake mechanism
    net::EventLoop* event_loop;

    // Read buffer
    char read_buffer[65536];
    size_t read_pos = 0;

    // Response data storage (stream_id -> response data)
    // CRITICAL: Must store response strings so pointers remain valid during send
    struct ResponseData {
        std::string status_str;
        std::string content_type;
        std::string body;
        std::unordered_map<std::string, std::string> headers;
    };
    std::unordered_map<int32_t, ResponseData> stream_responses;

    // Active coroutines (stream_id -> coroutine task)
    // CRITICAL: Keeps coroutines alive while they're executing
    // Must store the task object itself (not just handle) to prevent premature destruction
    std::unordered_map<int32_t, core::coro_task<void>> active_coroutines;

    // Constructor
    explicit Http2ServerConnection(net::TcpSocket sock)
        : socket(std::move(sock)), fd(socket.fd()), http2_conn(nullptr), async_io_engine(nullptr) {}

    ~Http2ServerConnection() {
        delete http2_conn;
    }
};

// Forward declarations
static void send_http2_response(
    Http2ServerConnection* conn,
    int32_t stream_id,
    const PythonCallbackBridge::HandlerResult& result
);

/**
 * Async coroutine to handle Python execution
 * Python can block with its GIL, but event loop continues thanks to wake-based resumption
 */
static core::coro_task<void> handle_request_async(
    Http2ServerConnection* conn,
    int32_t stream_id,
    const std::string& method,
    const std::string& path,
    const std::unordered_map<std::string, std::string>& headers_map,
    const std::string& body
) {
    // Submit to sub-interpreter executor (returns immediately)
    auto result_future = PythonCallbackBridge::invoke_handler_async(method, path, headers_map, body);

    // Co-await the Python execution with wake-based resumption
    // Worker thread queues coroutine for resumption from event loop thread
    auto result = co_await core::make_awaitable(std::move(result_future));

    // Python execution complete, send response
    if (!result.is_ok()) {
        // Error case - send 500
        PythonCallbackBridge::HandlerResult error_result;
        error_result.status_code = 500;
        error_result.content_type = "text/plain";
        error_result.body = "Internal Server Error";

        send_http2_response(conn, stream_id, error_result);
    } else {
        // Success - send response
        send_http2_response(conn, stream_id, result.value());
    }

    // Clean up: Remove coroutine from active set
    auto it = conn->active_coroutines.find(stream_id);
    if (it != conn->active_coroutines.end()) {
        conn->active_coroutines.erase(it);
    }

    co_return;
}

/**
 * Helper to send HTTP/2 response using pure C++ HTTP/2 implementation
 */
static void send_http2_response(
    Http2ServerConnection* conn,
    int32_t stream_id,
    const PythonCallbackBridge::HandlerResult& result
) {
    // Build headers map for our pure C++ API
    std::unordered_map<std::string, std::string> headers;

    // Add content-type
    headers["content-type"] = result.content_type;

    // Add server header
    headers["server"] = "FasterAPI-HTTP2";

    // Add additional headers from handler
    for (const auto& [key, value] : result.headers) {
        headers[key] = value;
    }

    // Send response using pure C++ HTTP/2 connection
    auto send_result = conn->http2_conn->send_response(
        stream_id,
        result.status_code,
        headers,
        result.body
    );

    if (send_result.is_err()) {
        // Log error but continue (connection will handle cleanup)
        std::cerr << "Failed to send HTTP/2 response for stream " << stream_id << std::endl;
    }
}

/**
 * Handle HTTP/2 client connection using pure C++ implementation
 */
static void handle_http2_client(Http2ServerConnection* conn) {
    // Read data from socket
    ssize_t nread = recv(conn->fd, conn->read_buffer + conn->read_pos,
                         sizeof(conn->read_buffer) - conn->read_pos, 0);

    if (nread > 0) {
        conn->read_pos += nread;

        // Process input through our pure C++ HTTP/2 connection
        auto process_result = conn->http2_conn->process_input(
            (const uint8_t*)conn->read_buffer,
            conn->read_pos
        );

        if (process_result.is_err()) {
            // Protocol error - close connection
            std::cerr << "HTTP/2 protocol error, closing connection" << std::endl;

            // Clean up all active coroutines (C4 requirement)
            conn->active_coroutines.clear();

            conn->event_loop->remove_fd(conn->fd);
            close(conn->fd);
            delete conn;
            return;
        }

        // Remove processed data
        size_t processed = process_result.value();
        if (processed > 0) {
            memmove(conn->read_buffer, conn->read_buffer + processed,
                   conn->read_pos - processed);
            conn->read_pos -= processed;
        }
    } else if (nread == 0 || (nread < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        // Connection closed or error
        // Clean up all active coroutines (C4 requirement)
        // Coroutines will be destroyed, preventing dangling references
        conn->active_coroutines.clear();

        conn->event_loop->remove_fd(conn->fd);
        close(conn->fd);
        delete conn;
        return;
    }

    // Send buffered output data from HTTP/2 connection
    const uint8_t* output_data;
    size_t output_len;
    while (conn->http2_conn->get_output(&output_data, &output_len)) {
        if (output_len == 0) break;

        ssize_t sent = send(conn->fd, output_data, output_len, 0);

        if (sent > 0) {
            conn->http2_conn->commit_output(sent);
            if (sent < (ssize_t)output_len) {
                // Partial send - will resume next time
                break;
            }
        } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            // Send error
            // Clean up all active coroutines (C4 requirement)
            conn->active_coroutines.clear();

            conn->event_loop->remove_fd(conn->fd);
            close(conn->fd);
            delete conn;
            return;
        } else {
            // EAGAIN - will send next time
            break;
        }
    }
}

/**
 * Initialize new HTTP/2 connection with pure C++ implementation
 */
static void on_http2_connection(net::TcpSocket socket, net::EventLoop* event_loop) {
    // Set socket to non-blocking mode (critical for edge-triggered I/O)
    if (socket.set_nonblocking() < 0) {
        return;
    }

    // Set TCP_NODELAY for low latency
    socket.set_nodelay();

    // Create connection state (moves socket)
    auto* conn = new Http2ServerConnection(std::move(socket));
    conn->event_loop = event_loop;

    int fd = conn->fd;

    // Get async_io engine from event loop (for wake mechanism)
    // TODO: Need to pass async_io through event loop or store globally
    conn->async_io_engine = nullptr;  // Will be set when CoroResumer is initialized

    // Create pure C++ HTTP/2 connection
    // Constructor automatically queues initial SETTINGS frame (server connection preface)
    conn->http2_conn = new http2::Http2Connection(true);  // is_server = true

    // Set request callback - this is called when a complete HTTP/2 request is received
    conn->http2_conn->set_request_callback([conn](http2::Http2Stream* stream) {
        if (!stream) return;

        // Extract request details from stream
        uint32_t stream_id = stream->id();
        std::string method, path;
        std::unordered_map<std::string, std::string> headers_map;

        // Get headers from stream
        for (const auto& [key, value] : stream->request_headers()) {
            if (key == ":method") method = value;
            else if (key == ":path") path = value;
            else headers_map[key] = value;
        }

        // Get request body
        std::string body = stream->request_body();

        // Use async coroutine execution with wake-based resumption
        // This allows event loop to continue while Python executes in worker thread
        // Coroutine suspends at co_await, worker thread signals wake, event loop resumes coroutine
        auto coro = handle_request_async(conn, stream_id, method, path, headers_map, body);

        // Store coroutine to keep it alive (C3 requirement)
        // The coroutine will self-cleanup when it completes (see handle_request_async)
        // Use emplace since coro_task is move-only (no default constructor)
        conn->active_coroutines.emplace(stream_id, std::move(coro));
    });

    // Add to event loop
    event_loop->add_fd(fd, net::IOEvent::READ | net::IOEvent::EDGE,
        [](int fd, net::IOEvent events, void* user_data) {
            auto* conn = static_cast<Http2ServerConnection*>(user_data);
            handle_http2_client(conn);
        },
        conn);

    // Send initial SETTINGS frame (server connection preface) before reading client data
    // This is required by RFC 7540 Section 3.5
    const uint8_t* initial_data;
    size_t initial_len;
    while (conn->http2_conn->get_output(&initial_data, &initial_len)) {
        if (initial_len == 0) break;

        ssize_t sent = send(fd, initial_data, initial_len, 0);
        if (sent > 0) {
            conn->http2_conn->commit_output(sent);
            if (sent < (ssize_t)initial_len) {
                // Partial send - remaining will be sent later
                break;
            }
        } else {
            // Send error or EAGAIN - will retry later
            break;
        }
    }
}

// Http2Server implementation

Http2Server::Http2Server(const Http2ServerConfig& config)
    : config_(config) {
    // Initialize Python callback bridge
    PythonCallbackBridge::initialize();
}

Http2Server::~Http2Server() {
    stop();
    PythonCallbackBridge::cleanup();
}

int Http2Server::start() {
    if (listener_) {
        return -1;  // Already running
    }

    shutdown_flag_.store(false, std::memory_order_relaxed);

    // ProcessPoolExecutor is initialized globally by http_lib_init()
    // No per-server initialization needed

    // Initialize coroutine resumption infrastructure
    // Create dedicated async_io for wake mechanism
    core::async_io_config wake_config{};
    wake_config.backend = core::io_backend::auto_detect;
    wake_io_ = core::async_io::create(wake_config);
    if (!wake_io_) {
        std::cerr << "Failed to create async_io for wake mechanism" << std::endl;
        return -1;
    }

    // Create CoroResumer with wake_io
    coro_resumer_ = core::CoroResumer::create(wake_io_.get());
    if (!coro_resumer_) {
        std::cerr << "Failed to create CoroResumer" << std::endl;
        return -1;
    }

    // Set global CoroResumer so awaitable_future can access it
    core::CoroResumer::set_global(coro_resumer_.get());

    std::cout << "CoroResumer initialized with " << wake_io_->backend_name() << " backend" << std::endl;

    // Start wake_io event loop in dedicated thread to process wake events
    // This thread will wake up when worker threads complete Python execution
    // and call wake(), then resume coroutines from event loop thread
    std::thread wake_thread([this]() {
        while (!shutdown_flag_.load(std::memory_order_acquire)) {
            wake_io_->poll(1000);  // 1ms timeout
        }
    });
    wake_thread.detach();

    // Determine number of event loop workers
    // Use pinned workers count, or auto-detect if not set
    uint16_t num_workers = config_.num_pinned_workers;
    if (num_workers == 0) {
        num_workers = std::thread::hardware_concurrency();
    }

    // Create TCP listener config
    net::TcpListenerConfig listener_config{};
    listener_config.port = config_.port;
    listener_config.host = config_.host;
    listener_config.num_workers = num_workers;
    listener_config.use_reuseport = config_.use_reuseport;

    // Create TCP listener with connection callback
    listener_ = std::make_unique<net::TcpListener>(listener_config, on_http2_connection);

    std::cout << "Starting HTTP/2 server on " << config_.host << ":" << config_.port << std::endl;
    std::cout << "Event loop workers: " << num_workers << std::endl;
    std::cout << "Pinned sub-interpreters: " << config_.num_pinned_workers << std::endl;
    std::cout << "Pooled workers: " << config_.num_pooled_workers << std::endl;
    std::cout << "Pooled sub-interpreters: " << config_.num_pooled_interpreters << std::endl;

    // Start listener (blocks until shutdown)
    return listener_->start();
}

void Http2Server::stop() {
    if (listener_) {
        shutdown_flag_.store(true, std::memory_order_relaxed);
        listener_.reset();
    }

    // Clear global CoroResumer before destroying it
    if (coro_resumer_) {
        core::CoroResumer::set_global(nullptr);
        coro_resumer_.reset();
    }

    // Stop wake_io (will cause wake thread to exit)
    if (wake_io_) {
        wake_io_->stop();
        // Give wake thread time to exit
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        wake_io_.reset();
    }

    // ProcessPoolExecutor is managed globally, no per-server shutdown needed
}

// C API for Python bindings

extern "C" {

void* http2_server_create(uint16_t port, uint16_t num_workers) {
    Http2ServerConfig config{};
    config.port = port;
    config.num_pinned_workers = num_workers;  // Map legacy param to pinned workers
    config.host = "0.0.0.0";
    config.use_reuseport = true;

    return new Http2Server(config);
}

int http2_server_start(void* server) {
    if (!server) return -1;
    return static_cast<Http2Server*>(server)->start();
}

void http2_server_stop(void* server) {
    if (server) {
        static_cast<Http2Server*>(server)->stop();
    }
}

void http2_server_destroy(void* server) {
    if (server) {
        delete static_cast<Http2Server*>(server);
    }
}

} // extern "C"

} // namespace http
} // namespace fasterapi
