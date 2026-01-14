/**
 * WebSocket Connection Handlers
 * 
 * Split from unified_server.cpp for maintainability.
 */

#include "unified_server_internal.h"
#include "unified_server.h"
#include "core/logger.h"
#include "../python/process_pool_executor.h"
#include "../python/ipc_protocol.h"
#include <unistd.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <cstring>

namespace fasterapi {
namespace http {

// ============================================================================
// Wake Pipe Implementation
// ============================================================================

bool init_wake_pipe() {
    if (t_wake_pipe_read_fd >= 0) return true;  // Already initialized

    int fds[2];
    if (pipe(fds) < 0) {
        LOG_ERROR("WS", "Failed to create wake pipe: %s", strerror(errno));
        return false;
    }

    // Make both ends non-blocking
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);

    t_wake_pipe_read_fd = fds[0];
    t_wake_pipe_write_fd = fds[1];

    // Register write fd globally for response thread to signal
    {
        std::lock_guard<std::mutex> lock(s_wake_pipes_mutex);
        s_wake_pipe_write_fds.push_back(t_wake_pipe_write_fd);
    }

    LOG_DEBUG("WS", "Wake pipe initialized: read=%d write=%d", t_wake_pipe_read_fd, t_wake_pipe_write_fd);
    return true;
}

void signal_ws_response_ready() {
    std::lock_guard<std::mutex> lock(s_wake_pipes_mutex);
    for (int write_fd : s_wake_pipe_write_fds) {
        // Write a single byte to wake the event loop
        char c = 1;
        ssize_t n = write(write_fd, &c, 1);
        (void)n;  // Ignore result - may fail if pipe full (that's ok)
    }
}

void register_wake_pipe_with_event_loop(net::EventLoop* event_loop) {
    if (t_wake_pipe_registered) return;  // Already registered

    if (!init_wake_pipe()) {
        LOG_ERROR("WS", "Failed to initialize wake pipe");
        return;
    }

    // Add read end of pipe to event loop
    if (event_loop->add_fd(t_wake_pipe_read_fd, net::IOEvent::READ | net::IOEvent::EDGE,
                          [](int fd, net::IOEvent events, void* data) {
                              // Drain the pipe (consume all wake signals)
                              char buf[64];
                              while (read(fd, buf, sizeof(buf)) > 0) {}

                              // Dispatch pending WebSocket responses
                              dispatch_pending_ws_responses();
                          }, nullptr) < 0) {
        LOG_ERROR("WS", "Failed to add wake pipe to event loop");
        return;
    }

    t_wake_pipe_registered = true;
    LOG_DEBUG("WS", "Wake pipe registered with event loop");
}

void cleanup_websocket_connection(int fd) {
    auto it = t_websocket_connections.find(fd);
    if (it != t_websocket_connections.end()) {
        // Clean up reverse lookup first
        uint64_t conn_id = it->second->get_id();
        t_ws_conn_id_to_fd.erase(conn_id);
        // Then erase the connection
        t_websocket_connections.erase(it);
    }
}

// ============================================================================
// WebSocket Connection Handler
// ============================================================================

void handle_websocket_connection(
    int fd,
    net::IOEvent events,
    net::EventLoop* event_loop,
    WebSocketConnection* ws_conn
) {
    auto it = t_websocket_connections.find(fd);
    if (it == t_websocket_connections.end()) {
        LOG_ERROR("WebSocket", "Connection not found for fd=%d", fd);
        event_loop->remove_fd(fd);
        ::close(fd);
        return;
    }

    // With edge-triggered events, we must keep processing in a loop.
    bool keep_processing = true;
    char buffer[8192];

    while (keep_processing) {
        keep_processing = false;

        // Handle read events
        if (events & net::IOEvent::READ) {
            bool connection_closed = false;
            bool received_data = false;

            fprintf(stderr, "[WS_HANDLER] fd=%d handle_websocket_connection READ event\n", fd);
            fflush(stderr);

            // Loop to drain all available data (required for edge-triggered mode)
            while (true) {
                ssize_t n = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);

                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        fprintf(stderr, "[WS_HANDLER] fd=%d recv returned EAGAIN - no more data\n", fd);
                        fflush(stderr);
                        break;
                    }
                    LOG_ERROR("WebSocket", "Read error on fd=%d: errno=%d", fd, errno);
                    event_loop->remove_fd(fd);
                    cleanup_websocket_connection(fd);
                    ::close(fd);
                    return;
                }

                if (n == 0) {
                    connection_closed = true;
                    fprintf(stderr, "[WS_HANDLER] fd=%d recv returned 0 - connection closed\n", fd);
                    fflush(stderr);
                    break;
                }

                fprintf(stderr, "[WS_HANDLER] fd=%d recv got %zd bytes\n", fd, n);
                fflush(stderr);

                received_data = true;

                // Process WebSocket frames
                int result = ws_conn->handle_frame(reinterpret_cast<uint8_t*>(buffer), n);
                fprintf(stderr, "[WS_HANDLER] fd=%d handle_frame returned %d\n", fd, result);
                fflush(stderr);
                if (result < 0) {
                    LOG_ERROR("WebSocket", "Frame handling error on fd=%d: %d", fd, result);
                }

                // Check if connection was closed by frame handler
                if (!ws_conn->is_open()) {
                    LOG_DEBUG("WebSocket", "Connection closed by handler on fd=%d", fd);
                    event_loop->remove_fd(fd);
                    cleanup_websocket_connection(fd);
                    ::close(fd);
                    return;
                }
            }

            if (connection_closed) {
                LOG_DEBUG("WebSocket", "Connection closed on fd=%d", fd);
                event_loop->remove_fd(fd);
                cleanup_websocket_connection(fd);
                ::close(fd);
                return;
            }

            if (received_data) {
                dispatch_pending_ws_responses();
            }
        }

        // Send pending output
        while (ws_conn->has_pending_output()) {
            const std::string* frame = ws_conn->get_pending_output();
            if (!frame) break;

            ssize_t sent = ::send(fd, frame->data(), frame->size(), MSG_DONTWAIT);
            if (sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    event_loop->modify_fd(fd, net::IOEvent::READ | net::IOEvent::WRITE | net::IOEvent::EDGE);
                    return;
                }
                LOG_ERROR("WebSocket", "Send error on fd=%d: errno=%d", fd, errno);
                event_loop->remove_fd(fd);
                cleanup_websocket_connection(fd);
                ::close(fd);
                return;
            }

            if (static_cast<size_t>(sent) < frame->size()) {
                LOG_WARN("WebSocket", "Partial send on fd=%d: %zd/%zu", fd, sent, frame->size());
            }

            ws_conn->pop_pending_output();
            keep_processing = true;
        }
    }

    // Back to read-only mode
    event_loop->modify_fd(fd, net::IOEvent::READ | net::IOEvent::EDGE);

    // Check for data that may have arrived during processing
    char peek_buf[1];
    ssize_t peek = ::recv(fd, peek_buf, 1, MSG_PEEK | MSG_DONTWAIT);
    if (peek > 0) {
        LOG_DEBUG("WebSocket", "fd=%d has pending data after modify_fd, recursing", fd);
        handle_websocket_connection(fd, net::IOEvent::READ, event_loop, ws_conn);
    }
}

// ============================================================================
// WebSocket Response Dispatch
// ============================================================================

void dispatch_pending_ws_responses() {
    auto* executor = python::ProcessPoolExecutor::get_instance();
    if (!executor) return;

    python::ProcessPoolExecutor::WsResponse response;
    size_t dispatched = 0;

    while (executor->poll_ws_response(response)) {
        // Look up fd by connection_id
        auto fd_it = t_ws_conn_id_to_fd.find(response.connection_id);
        if (fd_it == t_ws_conn_id_to_fd.end()) {
            LOG_WARN("WebSocket", "Connection ID %lu not found for response dispatch",
                     response.connection_id);
            continue;
        }

        int fd = fd_it->second;
        auto ws_it = t_websocket_connections.find(fd);
        if (ws_it == t_websocket_connections.end()) {
            LOG_WARN("WebSocket", "Connection fd=%d not found for conn_id=%lu",
                     fd, response.connection_id);
            t_ws_conn_id_to_fd.erase(fd_it);
            continue;
        }

        auto& ws = ws_it->second;

        if (response.type == python::MessageType::WS_SEND) {
            if (response.is_binary) {
                ws->send_binary(reinterpret_cast<const uint8_t*>(response.payload.data()),
                               response.payload.size());
            } else {
                ws->send_text(response.payload);
            }

            // Send pending output immediately
            while (ws->has_pending_output()) {
                const std::string* frame = ws->get_pending_output();
                if (!frame) break;

                ssize_t sent = ::send(fd, frame->data(), frame->size(), MSG_NOSIGNAL);
                if (sent < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        LOG_DEBUG("WebSocket", "Socket buffer full for fd=%d, queued for later", fd);
                        break;
                    }
                    LOG_ERROR("WebSocket", "Send error for fd=%d: %s", fd, strerror(errno));
                    break;
                }
                LOG_DEBUG("WebSocket", "Sent %zd bytes to fd=%d", sent, fd);
                ws->pop_pending_output();
            }

            LOG_DEBUG("WebSocket", "Dispatched %s message to conn_id=%lu fd=%d (%zu bytes)",
                     response.is_binary ? "binary" : "text",
                     response.connection_id, fd, response.payload.size());

            // Check for incoming data after sending response
            char recv_buffer[8192];
            while (true) {
                ssize_t n = ::recv(fd, recv_buffer, sizeof(recv_buffer), MSG_DONTWAIT);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    LOG_ERROR("WebSocket", "Recv error on fd=%d after dispatch: %s", fd, strerror(errno));
                    break;
                }
                if (n == 0) {
                    LOG_DEBUG("WebSocket", "Connection closed on fd=%d during dispatch recv", fd);
                    break;
                }

                LOG_DEBUG("WebSocket", "Received %zd bytes on fd=%d after dispatch", n, fd);
                int result = ws->handle_frame(reinterpret_cast<uint8_t*>(recv_buffer), n);
                if (result < 0) {
                    LOG_ERROR("WebSocket", "Frame handling error on fd=%d: %d", fd, result);
                }
            }
        } else if (response.type == python::MessageType::WS_CLOSE) {
            ws->close(response.close_code, nullptr);

            // Send close frame immediately
            while (ws->has_pending_output()) {
                const std::string* frame = ws->get_pending_output();
                if (!frame) break;

                ::send(fd, frame->data(), frame->size(), MSG_NOSIGNAL);
                ws->pop_pending_output();
            }

            LOG_DEBUG("WebSocket", "Dispatched close to conn_id=%lu fd=%d code=%d",
                     response.connection_id, fd, response.close_code);
        }

        dispatched++;
    }

    if (dispatched > 0) {
        LOG_DEBUG("WebSocket", "Dispatched %zu responses from Python", dispatched);
    }
}

} // namespace http
} // namespace fasterapi
