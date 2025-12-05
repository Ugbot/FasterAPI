/**
 * Native HTTP/2 server using TcpListener + EventLoop + Pure C++ HTTP/2
 *
 * Demonstrates:
 * - Multi-threaded HTTP/2 server with h2c (cleartext)
 * - Pure C++ HTTP/2 implementation for framing and multiplexing
 * - High-performance request handling
 */

#include "../src/cpp/net/tcp_listener.h"
#include "../src/cpp/net/tcp_socket.h"
#include "../src/cpp/net/event_loop.h"
#include "../src/cpp/http/http2_server.h"
#include <iostream>
#include <csignal>
#include <memory>
#include <unordered_map>
#include <cstring>
#include <unistd.h>

using namespace fasterapi::net;

// Global listener pointer for signal handler
static std::unique_ptr<TcpListener> g_listener;

// Signal handler for Ctrl+C
void signal_handler(int sig) {
    if (sig == SIGINT) {
        std::cout << "\nStopping server..." << std::endl;
        if (g_listener) {
            g_listener->stop();
        }
    }
}

// HTTP/2 connection state
struct Http2Connection {
    int fd;
    char read_buffer[16384];   // 16KB read buffer
    size_t read_pos;
    std::vector<uint8_t> write_buffer;  // Dynamic write buffer
    EventLoop* event_loop;
    nghttp2_session* session;
    bool want_write;
};

// Per-worker connection storage (thread-local)
thread_local std::unordered_map<int, std::unique_ptr<Http2Connection>> t_connections;

// Send callback for nghttp2
ssize_t send_callback(nghttp2_session* session, const uint8_t* data,
                      size_t length, int flags, void* user_data) {
    auto* conn = static_cast<Http2Connection*>(user_data);

    // Buffer the data to write later
    conn->write_buffer.insert(conn->write_buffer.end(), data, data + length);
    conn->want_write = true;

    return length;
}

// Callback when stream receives headers
int on_header_callback(nghttp2_session* session, const nghttp2_frame* frame,
                       const uint8_t* name, size_t namelen,
                       const uint8_t* value, size_t valuelen,
                       uint8_t flags, void* user_data) {
    // We can process headers here if needed
    return 0;
}

// Stream data for responses
struct StreamData {
    std::string body;
    size_t offset;
};

// Per-connection stream data (stored in connection)
thread_local std::unordered_map<int32_t, std::unique_ptr<StreamData>> t_stream_data;

// Data provider callback
ssize_t data_provider_callback(nghttp2_session* session, int32_t stream_id,
                                uint8_t* buf, size_t length, uint32_t* data_flags,
                                nghttp2_data_source* source, void* user_data) {
    auto* stream_data = static_cast<StreamData*>(source->ptr);

    size_t remaining = stream_data->body.size() - stream_data->offset;
    size_t to_copy = std::min(remaining, length);

    if (to_copy > 0) {
        memcpy(buf, stream_data->body.data() + stream_data->offset, to_copy);
        stream_data->offset += to_copy;
    }

    if (stream_data->offset >= stream_data->body.size()) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }

    return to_copy;
}

// Callback when frame is received
int on_frame_recv_callback(nghttp2_session* session, const nghttp2_frame* frame,
                           void* user_data) {
    auto* conn = static_cast<Http2Connection*>(user_data);

    // Handle HEADERS frame (request received)
    if (frame->hd.type == NGHTTP2_HEADERS &&
        frame->hd.flags & NGHTTP2_FLAG_END_HEADERS) {

        int32_t stream_id = frame->hd.stream_id;

        // Create stream data
        auto stream_data = std::make_unique<StreamData>();
        stream_data->body = "Hello from FasterAPI HTTP/2!\n";
        stream_data->offset = 0;

        nghttp2_nv headers[] = {
            {(uint8_t*)":status", (uint8_t*)"200", 7, 3, NGHTTP2_NV_FLAG_NONE},
            {(uint8_t*)"content-type", (uint8_t*)"text/plain", 12, 10, NGHTTP2_NV_FLAG_NONE},
            {(uint8_t*)"server", (uint8_t*)"FasterAPI", 6, 9, NGHTTP2_NV_FLAG_NONE}
        };

        // Create data provider
        nghttp2_data_provider data_prd;
        data_prd.source.ptr = stream_data.get();
        data_prd.read_callback = data_provider_callback;

        // Store stream data
        t_stream_data[stream_id] = std::move(stream_data);

        // Submit response
        int rv = nghttp2_submit_response(session, stream_id, headers, 3, &data_prd);
        if (rv != 0) {
            std::cerr << "[HTTP/2] Failed to submit response: " << nghttp2_strerror(rv) << std::endl;
        }
    }

    return 0;
}

// Callback when stream is closed
int on_stream_close_callback(nghttp2_session* session, int32_t stream_id,
                             uint32_t error_code, void* user_data) {
    // Clean up stream data
    t_stream_data.erase(stream_id);

    return 0;
}

/**
 * Handle HTTP/2 client events
 */
void handle_http2_client(int fd, IOEvent events, void* user_data) {
    auto it = t_connections.find(fd);
    if (it == t_connections.end()) {
        return;
    }

    Http2Connection* conn = it->second.get();

    // Handle errors
    if (events & IOEvent::ERROR) {
        nghttp2_session_del(conn->session);
        conn->event_loop->remove_fd(fd);
        close(fd);
        t_connections.erase(it);
        return;
    }

    // Handle readable event
    if (events & IOEvent::READ) {
        // Read data
        ssize_t n = recv(fd, conn->read_buffer + conn->read_pos,
                         sizeof(conn->read_buffer) - conn->read_pos, 0);

        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;  // No data available
            }

            // Connection closed
            nghttp2_session_del(conn->session);
            conn->event_loop->remove_fd(fd);
            close(fd);
            t_connections.erase(it);
            return;
        }

        conn->read_pos += n;

        // Process HTTP/2 frames (nghttp2 will handle the preface validation)
        if (conn->read_pos > 0) {
            ssize_t processed = nghttp2_session_mem_recv(
                conn->session,
                (const uint8_t*)conn->read_buffer,
                conn->read_pos
            );

            if (processed < 0) {
                std::cerr << "[HTTP/2] Error processing frames: " << nghttp2_strerror(processed) << std::endl;
                nghttp2_session_del(conn->session);
                conn->event_loop->remove_fd(fd);
                close(fd);
                t_connections.erase(it);
                return;
            }

            // Remove processed data from buffer
            if (processed > 0) {
                memmove(conn->read_buffer, conn->read_buffer + processed,
                       conn->read_pos - processed);
                conn->read_pos -= processed;
            }

            // Generate frames to send (including initial SETTINGS)
            nghttp2_session_send(conn->session);
        }
    }

    // Send any buffered data
    if (!conn->write_buffer.empty()) {
        ssize_t sent = send(fd, conn->write_buffer.data(),
                           conn->write_buffer.size(), 0);

        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Can't send now, enable write event
                conn->event_loop->modify_fd(fd, IOEvent::READ | IOEvent::WRITE | IOEvent::EDGE);
                return;
            }
            // Error sending
            nghttp2_session_del(conn->session);
            conn->event_loop->remove_fd(fd);
            close(fd);
            t_connections.erase(it);
            return;
        }

        // Remove sent data from buffer
        if (sent > 0) {
            conn->write_buffer.erase(conn->write_buffer.begin(),
                                    conn->write_buffer.begin() + sent);
        }

        // Check if we still have data to send
        if (!conn->write_buffer.empty()) {
            // Still have data, enable write event
            conn->event_loop->modify_fd(fd, IOEvent::READ | IOEvent::WRITE | IOEvent::EDGE);
        } else {
            // All data sent
            conn->want_write = false;
        }
    }
}

/**
 * Handle new HTTP/2 connection
 */
void on_http2_connection(TcpSocket socket, EventLoop* event_loop) {
    // Set non-blocking
    if (socket.set_nonblocking() < 0) {
        std::cerr << "Failed to set non-blocking: " << strerror(errno) << std::endl;
        return;
    }

    // Disable Nagle's algorithm
    socket.set_nodelay();

    int fd = socket.fd();

    // Create connection state
    auto conn = std::make_unique<Http2Connection>();
    conn->fd = fd;
    conn->read_pos = 0;
    conn->event_loop = event_loop;
    conn->want_write = false;

    // Create nghttp2 session callbacks
    nghttp2_session_callbacks* callbacks;
    nghttp2_session_callbacks_new(&callbacks);
    nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);

    // Create nghttp2 session
    nghttp2_session_server_new(&conn->session, callbacks, conn.get());
    nghttp2_session_callbacks_del(callbacks);

    // Send server connection preface (SETTINGS frame) immediately
    nghttp2_submit_settings(conn->session, NGHTTP2_FLAG_NONE, nullptr, 0);
    nghttp2_session_send(conn->session);

    // Send any buffered data from SETTINGS frame
    if (!conn->write_buffer.empty()) {
        ssize_t sent = send(fd, conn->write_buffer.data(), conn->write_buffer.size(), 0);
        if (sent > 0) {
            conn->write_buffer.erase(conn->write_buffer.begin(), conn->write_buffer.begin() + sent);
        }
    }

    // Add to event loop
    if (event_loop->add_fd(fd, IOEvent::READ | IOEvent::EDGE, handle_http2_client, nullptr) < 0) {
        std::cerr << "Failed to add client to event loop: " << strerror(errno) << std::endl;
        nghttp2_session_del(conn->session);
        return;
    }

    // Release ownership and store connection
    socket.release();
    t_connections[fd] = std::move(conn);
}

int main(int argc, char* argv[]) {
    uint16_t port = 8080;
    uint16_t num_workers = 0;  // Auto

    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }
    if (argc > 2) {
        num_workers = static_cast<uint16_t>(std::atoi(argv[2]));
    }

    std::cout << "Native HTTP/2 server (h2c - cleartext)" << std::endl;
    std::cout << "Port: " << port << std::endl;
    std::cout << "Workers: " << (num_workers == 0 ? "auto" : std::to_string(num_workers)) << std::endl;

    // Configure listener
    TcpListenerConfig config;
    config.port = port;
    config.host = "0.0.0.0";
    config.num_workers = num_workers;
    config.use_reuseport = true;

    // Create listener
    g_listener = std::make_unique<TcpListener>(config, on_http2_connection);

    // Setup signal handler
    signal(SIGINT, signal_handler);

    // Start listening (blocks until stop() is called)
    std::cout << "Starting server..." << std::endl;
    int result = g_listener->start();

    if (result < 0) {
        std::cerr << "Failed to start server" << std::endl;
        return 1;
    }

    std::cout << "Server stopped." << std::endl;
    return 0;
}
