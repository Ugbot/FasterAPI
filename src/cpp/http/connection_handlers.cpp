/**
 * HTTP/1.1, HTTP/2, and TLS Connection Handlers
 * 
 * Split from unified_server.cpp for maintainability.
 */

#include "unified_server_internal.h"
#include "unified_server.h"
#include "app.h"
#include "core/logger.h"
#include "websocket_parser.h"
#include "websocket.h"
#include "python_callback_bridge.h"
#include "../python/process_pool_executor.h"
#include "../python/ipc_protocol.h"
#include <atomic>
#include <unistd.h>
#include <sys/socket.h>
#include <cstring>

namespace fasterapi {
namespace http {

// ============================================================================
// TLS Connection Handler
// ============================================================================

void UnifiedServer::handle_tls_connection(
    net::TcpSocket socket,
    net::EventLoop* event_loop,
    std::shared_ptr<net::TlsContext> tls_context
) {
    // Set non-blocking
    if (socket.set_nonblocking() < 0) {
        LOG_ERROR("Server", "Failed to set non-blocking on TLS connection");
        return;
    }

    socket.set_nodelay();

    int fd = socket.fd();

    // Create TLS socket
    auto tls_socket = net::TlsSocket::accept(std::move(socket), tls_context);
    if (!tls_socket) {
        LOG_ERROR("Server", "Failed to create TLS socket");
        return;
    }

    // Store TLS socket
    t_tls_sockets[fd] = std::move(tls_socket);

    // Add to event loop for TLS handshake
    auto handshake_handler = [](int fd, net::IOEvent events, void* user_data) {
        auto it = t_tls_sockets.find(fd);
        if (it == t_tls_sockets.end()) {
            return;
        }

        net::TlsSocket* tls_sock = it->second.get();
        net::EventLoop* loop = static_cast<net::EventLoop*>(user_data);

        // Handle errors
        if (events & net::IOEvent::ERROR) {
            LOG_ERROR("Server", "TLS socket error on fd=%d", fd);
            loop->remove_fd(fd);
            t_tls_sockets.erase(it);
            ::close(fd);
            return;
        }

        // Process incoming data for handshake
        if (events & net::IOEvent::READ) {
            ssize_t result = tls_sock->process_incoming();
            if (result < 0) {
                LOG_ERROR("Server", "TLS process_incoming failed on fd=%d", fd);
                loop->remove_fd(fd);
                t_tls_sockets.erase(it);
                ::close(fd);
                return;
            }
        }

        // Perform handshake
        int hs_result = tls_sock->handshake();

        if (hs_result == 0) {
            // Handshake complete! Get ALPN protocol
            std::string alpn_protocol = tls_sock->get_alpn_protocol();

            LOG_INFO("Server", "TLS handshake complete on fd=%d, ALPN: %s", fd,
                     alpn_protocol.empty() ? "(none)" : alpn_protocol.c_str());

            // Remove from event loop (will re-add with protocol-specific handler)
            loop->remove_fd(fd);

            // Route based on ALPN
            if (alpn_protocol == "h2") {
                // HTTP/2
                LOG_DEBUG("Server", "Routing fd=%d to HTTP/2", fd);

                // Create HTTP/2 connection
                auto http2_conn = new http2::Http2Connection(true);
                t_http2_connections[fd] = std::unique_ptr<http2::Http2Connection>(http2_conn);

                // Add to event loop with HTTP/2 handler
                loop->add_fd(fd, net::IOEvent::READ | net::IOEvent::EDGE,
                           [](int fd, net::IOEvent events, void* data) {
                               handle_http2_connection(fd, static_cast<net::EventLoop*>(data),
                                                     t_http2_connections[fd].get());
                           }, loop);

            } else {
                // HTTP/1.1 (default fallback)
                LOG_DEBUG("Server", "Routing fd=%d to HTTP/1.1", fd);

                // Create HTTP/1.1 connection
                auto http1_conn = new Http1Connection(fd);
                http1_conn->set_request_callback([](
                    const std::string& method,
                    const std::string& path,
                    const std::unordered_map<std::string, std::string>& headers,
                    const std::string& body
                ) -> Http1Response {
                    Http1Response response;

                    // Check for WebSocket upgrade request
                    auto upgrade_it = headers.find("Upgrade");
                    auto connection_it = headers.find("Connection");
                    auto ws_key_it = headers.find("Sec-WebSocket-Key");
                    auto ws_version_it = headers.find("Sec-WebSocket-Version");

                    // Validate WebSocket upgrade
                    if (upgrade_it != headers.end() && ws_key_it != headers.end()) {
                        std::string upgrade_val = upgrade_it->second;
                        std::string connection_val = connection_it != headers.end() ? connection_it->second : "";
                        std::string ws_version = ws_version_it != headers.end() ? ws_version_it->second : "";
                        std::string ws_key = ws_key_it->second;

                        if (websocket::HandshakeUtils::validate_upgrade_request(
                                method, upgrade_val, connection_val, ws_version, ws_key)) {
                            // Valid WebSocket upgrade - build 101 Switching Protocols response
                            LOG_INFO("WebSocket", "Valid upgrade request for path: %s", path.c_str());

                            std::string accept_key = websocket::HandshakeUtils::compute_accept_key(ws_key);

                            response.status = 101;
                            response.status_message = "Switching Protocols";
                            response.add_header("Upgrade", "websocket");
                            response.add_header("Connection", "Upgrade");
                            response.add_header("Sec-WebSocket-Accept", accept_key);
                            response.mark_websocket_upgrade(path);

                            return response;
                        }
                    }

                    // Regular HTTP request - invoke global request handler if registered
                    if (s_request_handler_) {
                        std::unordered_map<std::string, std::string> response_headers;
                        std::string response_body;
                        uint16_t status_code = 200;

                        // Call the handler with a send_response callback
                        s_request_handler_(
                            method,
                            path,
                            headers,
                            body,
                            [&status_code, &response_headers, &response_body](
                                uint16_t status,
                                const std::unordered_map<std::string, std::string>& hdrs,
                                const std::string& body
                            ) {
                                status_code = status;
                                response_headers = hdrs;
                                response_body = body;
                            }
                        );

                        // Transfer to Http1Response
                        response.status = status_code;
                        response.body = response_body;
                        response.headers = response_headers;
                    } else {
                        // Fallback if no handler registered
                        response.status = 503;
                        response.body = "Service Unavailable - No request handler registered\n";
                        response.set_content_type("text/plain");
                    }

                    return response;
                });

                t_http1_connections[fd] = std::unique_ptr<Http1Connection>(http1_conn);

                // Capture connection pointer before lambda to avoid map lookup issues
                Http1Connection* conn_ptr = http1_conn;

                // Add to event loop with HTTP/1.1 handler
                loop->add_fd(fd, net::IOEvent::READ | net::IOEvent::EDGE,
                           [conn_ptr](int fd, net::IOEvent events, void* data) {
                               handle_http1_connection(fd, events, static_cast<net::EventLoop*>(data), conn_ptr);
                           }, loop);
            }

        } else if (hs_result > 0) {
            // Need more I/O - flush output
            bool flushed = tls_sock->flush();
            if (!flushed) {
                // Need to wait for writable
                loop->modify_fd(fd, net::IOEvent::READ | net::IOEvent::WRITE | net::IOEvent::EDGE);
            }
        } else {
            // Error
            LOG_ERROR("Server", "TLS handshake failed on fd=%d: %s", fd, tls_sock->get_error().c_str());
            loop->remove_fd(fd);
            t_tls_sockets.erase(it);
            ::close(fd);
        }
    };

    if (event_loop->add_fd(fd, net::IOEvent::READ | net::IOEvent::EDGE, handshake_handler, event_loop) < 0) {
        LOG_ERROR("Server", "Failed to add TLS socket fd=%d to event loop", fd);
        t_tls_sockets.erase(fd);
    }
}

// ============================================================================
// Cleartext HTTP/1.1 Connection Handler
// ============================================================================

void UnifiedServer::on_cleartext_connection(net::TcpSocket socket, net::EventLoop* event_loop) {
    int fd = socket.fd();
    LOG_DEBUG("HTTP1", "Cleartext connection accepted on fd=%d", fd);

    // Track connection for graceful shutdown - reject if draining
    if (!do_track_connection_open()) {
        LOG_DEBUG("HTTP1", "Rejecting connection fd=%d - server draining", fd);
        return;  // Socket will be closed by TcpSocket destructor
    }

    // Set non-blocking
    if (socket.set_nonblocking() < 0) {
        LOG_ERROR("HTTP1", "Failed to set non-blocking on fd=%d", fd);
        do_track_connection_close();  // Decrement counter
        return;
    }

    socket.set_nodelay();

    // Create HTTP/1.1 connection
    auto http1_conn = new Http1Connection(fd);
    
    // Set ultra-fast callback if available (for paths that can bypass routing)
    if (s_ultra_fast_callback_) {
        http1_conn->set_ultra_fast_callback(s_ultra_fast_callback_);
    }

    // Always set fast_request_callback for App router fallback
    // This handles paths not covered by ultra_fast_callback
    http1_conn->set_fast_request_callback([](const Http1RequestView& view) -> Http1Response {
            Http1Response response;

            // Check for WebSocket upgrade request first
            auto upgrade_val = view.get_header("Upgrade");
            auto connection_val = view.get_header("Connection");
            auto ws_key = view.get_header("Sec-WebSocket-Key");
            auto ws_version = view.get_header("Sec-WebSocket-Version");

            if (!upgrade_val.empty() && !ws_key.empty()) {
                if (websocket::HandshakeUtils::validate_upgrade_request(
                        std::string(view.method),
                        std::string(upgrade_val),
                        std::string(connection_val),
                        std::string(ws_version),
                        std::string(ws_key))) {
                    // Valid WebSocket upgrade - build 101 Switching Protocols response
                    LOG_INFO("WebSocket", "Cleartext: Valid upgrade request for path: %.*s",
                             static_cast<int>(view.path.size()), view.path.data());

                    std::string accept_key = websocket::HandshakeUtils::compute_accept_key(std::string(ws_key));

                    response.status = 101;
                    response.status_message = "Switching Protocols";
                    response.add_header("Upgrade", "websocket");
                    response.add_header("Connection", "Upgrade");
                    response.add_header("Sec-WebSocket-Accept", accept_key);
                    response.mark_websocket_upgrade(std::string(view.path));

                    return response;
                }
            }

            // Fast path: call App::handle_http1_fast() directly if available
            if (s_app_instance_) {
                return s_app_instance_->handle_http1_fast(view);
            }

            // Fallback to old path if App not set (for backward compatibility)
            if (s_request_handler_) {
                // Build headers map for legacy handler
                std::unordered_map<std::string, std::string> headers;
                headers.reserve(view.header_count);
                for (size_t i = 0; i < view.header_count; ++i) {
                    headers.emplace(
                        std::string(view.headers[i].first),
                        std::string(view.headers[i].second)
                    );
                }
                
                std::unordered_map<std::string, std::string> response_headers;
                std::string response_body;
                uint16_t status_code = 200;

                // Reconstruct full URL with query string
                std::string full_url(view.path);
                if (!view.query_string.empty()) {
                    full_url += '?';
                    full_url += view.query_string;
                }

                s_request_handler_(
                    std::string(view.method),
                    full_url,
                    headers,
                    std::string(view.body),
                    [&status_code, &response_headers, &response_body](
                        uint16_t status,
                        const std::unordered_map<std::string, std::string>& hdrs,
                        const std::string& body
                    ) {
                        status_code = status;
                        response_headers = hdrs;
                        response_body = body;
                    }
                );

                response.status = status_code;
                response.body = response_body;
                response.headers = response_headers;
            } else {
                // No handler registered
                response.status = 503;
                response.body = "Service Unavailable - No request handler registered\n";
                response.set_content_type("text/plain");
            }

            return response;
        });

    t_http1_connections[fd] = std::unique_ptr<Http1Connection>(http1_conn);

    // Release socket ownership
    socket.release();

    // Capture connection pointer before lambda to avoid map lookup issues
    Http1Connection* conn_ptr = http1_conn;

    // Add to event loop
    if (event_loop->add_fd(fd, net::IOEvent::READ | net::IOEvent::EDGE,
                          [conn_ptr](int fd, net::IOEvent events, void* data) {
                              handle_http1_connection(fd, events, static_cast<net::EventLoop*>(data), conn_ptr);
                          }, event_loop) < 0) {
        LOG_ERROR("HTTP1", "Failed to add cleartext socket fd=%d to event loop", fd);
        t_http1_connections.erase(fd);
        do_track_connection_close();
    }
}

// ============================================================================
// HTTP/2 Connection Handler
// ============================================================================

void UnifiedServer::handle_http2_connection(
    int fd,
    net::EventLoop* event_loop,
    http2::Http2Connection* http2_conn
) {
    // TODO: Implement HTTP/2 connection handling with TLS I/O
    // This needs to read/write through the TLS socket
    LOG_WARN("HTTP2", "HTTP/2 connection handling not yet implemented for fd=%d", fd);
}

// ============================================================================
// HTTP/1.1 Connection Handler (main handler)
// ============================================================================

void UnifiedServer::handle_http1_connection(
    int fd,
    net::IOEvent events,
    net::EventLoop* event_loop,
    Http1Connection* http1_conn
) {
    LOG_DEBUG("HTTP1", "handle_http1_connection fd=%d", fd);

    // Safety check: ensure connection pointer is valid
    if (!http1_conn) {
        LOG_ERROR("HTTP1", "null connection pointer for fd=%d", fd);
        return;
    }

    auto it = t_http1_connections.find(fd);
    if (it == t_http1_connections.end()) {
        LOG_ERROR("HTTP1", "Connection fd=%d not found in map", fd);
        return;
    }

    // Check if using TLS
    auto tls_it = t_tls_sockets.find(fd);
    bool using_tls = (tls_it != t_tls_sockets.end());

    LOG_DEBUG("HTTP1", "fd=%d State: %d TLS: %d", fd, static_cast<int>(http1_conn->get_state()), using_tls);

    // Get timeout configuration from server instance
    auto* server = UnifiedServer::get_instance();
    uint32_t request_timeout_ms = server ? server->get_request_timeout_ms() : 30000;
    uint32_t idle_timeout_ms = server ? server->get_idle_timeout_ms() : 60000;

    // Check for request timeout (while reading request headers or body)
    if (http1_conn->get_state() == Http1State::READING_REQUEST ||
        http1_conn->get_state() == Http1State::READING_BODY) {
        if (http1_conn->is_request_timed_out(request_timeout_ms)) {
            LOG_INFO("HTTP1", "fd=%d request timeout after %llu ms", fd, static_cast<unsigned long long>(http1_conn->get_request_elapsed_ms()));
            http1_conn->send_timeout_response();
            // Response is now queued - fall through to send it
        }
    }

    // Check for idle timeout (keep-alive connection waiting for next request)
    if (http1_conn->get_state() == Http1State::KEEPALIVE) {
        if (http1_conn->is_idle_timed_out(idle_timeout_ms)) {
            LOG_INFO("HTTP1", "fd=%d idle timeout after %llu ms", fd, static_cast<unsigned long long>(http1_conn->get_idle_time_ms()));
            // Just close the connection - no response needed for idle timeout
            event_loop->remove_fd(fd);
            t_http1_connections.erase(it);
            if (using_tls) t_tls_sockets.erase(tls_it);
            ::close(fd);
            do_track_connection_close();
            return;
        }
    }

    // Handle readable event
    if (http1_conn->get_state() == Http1State::READING_REQUEST ||
        http1_conn->get_state() == Http1State::READING_BODY ||
        http1_conn->get_state() == Http1State::KEEPALIVE) {
        LOG_DEBUG("HTTP1", "fd=%d Reading...", fd);

        char buffer[8192];
        ssize_t n;

        if (using_tls) {
            // Read through TLS
            tls_it->second->process_incoming();  // Feed encrypted data
            n = tls_it->second->read(buffer, sizeof(buffer));
        } else {
            // Read cleartext
            n = ::recv(fd, buffer, sizeof(buffer), 0);
        }

        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                // Error
                LOG_ERROR("HTTP1", "Read error on fd=%d: errno=%d", fd, errno);
                event_loop->remove_fd(fd);
                t_http1_connections.erase(it);
                if (using_tls) t_tls_sockets.erase(tls_it);
                ::close(fd);
                do_track_connection_close();
            }
            return;
        }

        if (n == 0) {
            // Connection closed by peer
            LOG_DEBUG("HTTP1", "Connection closed on fd=%d", fd);
            event_loop->remove_fd(fd);
            t_http1_connections.erase(it);
            if (using_tls) t_tls_sockets.erase(tls_it);
            ::close(fd);
            do_track_connection_close();
            return;
        }

        // Process input
        LOG_DEBUG("HTTP1", "fd=%d Processing %zd bytes...", fd, n);
        auto result = http1_conn->process_input(reinterpret_cast<uint8_t*>(buffer), n);
        if (result.is_err()) {
            LOG_ERROR("HTTP1", "Process error on fd=%d", fd);
            event_loop->remove_fd(fd);
            t_http1_connections.erase(it);
            if (using_tls) t_tls_sockets.erase(tls_it);
            ::close(fd);
            do_track_connection_close();
            return;
        }
        LOG_DEBUG("HTTP1", "fd=%d Processed successfully, new state: %d", fd, static_cast<int>(http1_conn->get_state()));

        // Check body size after headers are parsed (READING_BODY or PROCESSING state)
        size_t max_body_size = server ? server->get_max_body_size() : 10 * 1024 * 1024;
        if ((http1_conn->get_state() == Http1State::READING_BODY ||
             http1_conn->get_state() == Http1State::PROCESSING ||
             http1_conn->get_state() == Http1State::WRITING_RESPONSE) &&
            http1_conn->is_body_too_large(max_body_size)) {
            LOG_INFO("HTTP1", "fd=%d body too large: %zu > %zu",
                     fd, http1_conn->get_content_length(), max_body_size);
            http1_conn->send_payload_too_large_response();
            // Response queued - fall through to send it
        }
    }

    // Handle writable event / send response
    LOG_DEBUG("HTTP1", "fd=%d Checking pending output: %d", fd, http1_conn->has_pending_output());
    if (http1_conn->has_pending_output()) {
        LOG_DEBUG("HTTP1", "fd=%d Sending response...", fd);
        const uint8_t* data;
        size_t len;

        if (http1_conn->get_output(&data, &len)) {
            ssize_t sent;

            if (using_tls) {
                // Write through TLS (buffered)
                sent = tls_it->second->write(data, len);

                // Commit immediately since write() always succeeds (buffered)
                if (sent > 0) {
                    http1_conn->commit_output(sent);
                }

                // Try to flush encrypted data
                bool flush_complete = tls_it->second->flush();

                if (!flush_complete) {
                    // Socket would block - register for WRITE events to retry later
                    LOG_DEBUG("HTTP1", "fd=%d TLS flush incomplete, registering WRITE event", fd);
                    event_loop->modify_fd(fd, net::IOEvent::READ | net::IOEvent::WRITE | net::IOEvent::EDGE);
                } else {
                    LOG_DEBUG("HTTP1", "fd=%d TLS flush complete", fd);

                    // Check if connection should be closed (non-keep-alive)
                    if (http1_conn->get_state() == Http1State::CLOSING) {
                        LOG_DEBUG("HTTP1", "fd=%d TLS connection closing (non-keep-alive)", fd);
                        event_loop->remove_fd(fd);
                        t_http1_connections.erase(it);
                        t_tls_sockets.erase(tls_it);
                        ::close(fd);
                        do_track_connection_close();
                        return;
                    }

                    // Check if we transitioned to a reading state (keep-alive/pipelined requests)
                    if (http1_conn->get_state() == Http1State::READING_REQUEST ||
                        http1_conn->get_state() == Http1State::KEEPALIVE) {
                        LOG_DEBUG("HTTP1", "fd=%d TLS: State transitioned to reading - checking for pipelined requests", fd);

                        // Try to read immediately for pipelined requests
                        tls_it->second->process_incoming();  // Feed encrypted data
                        char buffer[8192];
                        ssize_t n = tls_it->second->read(buffer, sizeof(buffer));

                        if (n > 0) {
                            LOG_DEBUG("HTTP1", "fd=%d TLS: Found pipelined request (%zd bytes)", fd, n);
                            auto result = http1_conn->process_input(reinterpret_cast<uint8_t*>(buffer), n);
                            if (result.is_err()) {
                                LOG_ERROR("HTTP1", "fd=%d TLS: pipelined request process error", fd);
                                event_loop->remove_fd(fd);
                                t_http1_connections.erase(it);
                                t_tls_sockets.erase(tls_it);
                                ::close(fd);
                                do_track_connection_close();
                                return;
                            }
                            LOG_DEBUG("HTTP1", "fd=%d TLS: Pipelined request processed, new state: %d", fd, static_cast<int>(http1_conn->get_state()));

                            // If we now have output to send, send it immediately
                            if (http1_conn->has_pending_output()) {
                                const uint8_t* resp_data;
                                size_t resp_len;
                                if (http1_conn->get_output(&resp_data, &resp_len)) {
                                    ssize_t resp_sent = tls_it->second->write(resp_data, resp_len);
                                    if (resp_sent > 0) {
                                        http1_conn->commit_output(resp_sent);
                                        tls_it->second->flush();  // Try to flush
                                    }
                                }
                            }
                        }
                        // n <= 0 or EAGAIN: no pipelined data, which is normal
                    }

                    // Flush successful - back to READ-only
                    if (tls_it->second->needs_write_event()) {
                        // Still has buffered data (shouldn't happen, but check anyway)
                        event_loop->modify_fd(fd, net::IOEvent::READ | net::IOEvent::WRITE | net::IOEvent::EDGE);
                    } else {
                        event_loop->modify_fd(fd, net::IOEvent::READ | net::IOEvent::EDGE);
                    }
                }

                return;  // TLS path handled
            } else {
                // Write cleartext
                sent = ::send(fd, data, len, 0);
                LOG_DEBUG("HTTP1", "fd=%d Sent %zd bytes (of %zu)", fd, sent, len);
            }

            if (sent < 0) {
                LOG_DEBUG("HTTP1", "fd=%d Send failed: errno=%d", fd, errno);
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    // Error
                    event_loop->remove_fd(fd);
                    t_http1_connections.erase(it);
                    if (using_tls) t_tls_sockets.erase(tls_it);
                    ::close(fd);
                    do_track_connection_close();
                }
                return;
            }

            if (sent > 0) {
                http1_conn->commit_output(sent);
                LOG_DEBUG("HTTP1", "fd=%d After commit_output, state=%d", fd, static_cast<int>(http1_conn->get_state()));

                // Check if connection should be closed (non-keep-alive)
                if (http1_conn->get_state() == Http1State::CLOSING) {
                    LOG_DEBUG("HTTP1", "fd=%d Connection closing (non-keep-alive)", fd);
                    event_loop->remove_fd(fd);
                    t_http1_connections.erase(it);
                    if (using_tls) t_tls_sockets.erase(tls_it);
                    ::close(fd);
                    do_track_connection_close();
                    return;
                }

                // Check for WebSocket upgrade transition
                // If the 101 response is fully sent, transition to WebSocket mode
                if (http1_conn->is_websocket_upgrade() && !http1_conn->has_pending_output()) {
                    LOG_INFO("WebSocket", "Transitioning fd=%d to WebSocket mode, path=%s",
                             fd, http1_conn->get_websocket_path().c_str());

                    // Create WebSocket connection
                    static std::atomic<uint64_t> ws_connection_id{0};
                    auto ws_conn = std::make_unique<WebSocketConnection>(++ws_connection_id);
                    ws_conn->set_socket_fd(fd);
                    ws_conn->set_path(http1_conn->get_websocket_path());

                    // Get connection ID and path for callbacks
                    uint64_t conn_id = ws_conn->get_id();
                    std::string ws_path = ws_conn->get_path();

                    // Check for pure C++ handler first (bypasses ZMQ entirely)
                    WebSocketHandler* cpp_handler = UnifiedServer::get_websocket_handler(ws_path);
                    fprintf(stderr, "[WS_HANDLER_CHECK] cpp_handler=%p for path=%s\n", (void*)cpp_handler, ws_path.c_str());
                    fflush(stderr);
                    if (cpp_handler) {
                        // Pure C++ mode - no ZMQ involved
                        LOG_INFO("WebSocket", "Using pure C++ handler for %s (no ZMQ)", ws_path.c_str());
                        (*cpp_handler)(*ws_conn);  // Let handler set up callbacks
                    } else {
                        // Fall back to ZMQ forwarding to Python
                        ws_conn->on_text_message = [fd, conn_id, ws_path](const std::string& message) {
                            LOG_DEBUG("WebSocket", "fd=%d Received text: %s", fd, message.c_str());

                            // Forward to Python handler via ZMQ
                            auto* executor = python::ProcessPoolExecutor::get_instance();
                            if (executor) {
                                executor->send_ws_event(
                                    python::MessageType::WS_MESSAGE,
                                    conn_id,
                                    ws_path,
                                    message,
                                    false  // is_binary
                                );
                            }
                        };

                        ws_conn->on_binary_message = [fd, conn_id, ws_path](const uint8_t* data, size_t len) {
                            LOG_DEBUG("WebSocket", "fd=%d Received binary: %zu bytes", fd, len);

                            // Forward to Python handler via ZMQ
                            auto* executor = python::ProcessPoolExecutor::get_instance();
                            if (executor) {
                                std::string payload(reinterpret_cast<const char*>(data), len);
                                executor->send_ws_event(
                                    python::MessageType::WS_MESSAGE,
                                    conn_id,
                                    ws_path,
                                    payload,
                                    true  // is_binary
                                );
                            }
                        };

                        ws_conn->on_close = [fd, conn_id, ws_path](uint16_t code, const char* reason) {
                            LOG_INFO("WebSocket", "fd=%d Connection closed: %d %s", fd, code, reason ? reason : "");

                            // Notify Python of disconnect
                            auto* executor = python::ProcessPoolExecutor::get_instance();
                            if (executor) {
                                executor->send_ws_event(
                                    python::MessageType::WS_DISCONNECT,
                                    conn_id,
                                    ws_path,
                                    "",
                                    false
                                );
                            }
                        };

                        ws_conn->on_error = [fd](const char* error) {
                            LOG_ERROR("WebSocket", "fd=%d Error: %s", fd, error ? error : "unknown");
                        };

                        // Send WS_CONNECT event to Python with handler metadata
                        auto* executor = python::ProcessPoolExecutor::get_instance();
                        fprintf(stderr, "[WS_CONNECT] executor=%p path=%s conn_id=%lu\n",
                                 (void*)executor, ws_path.c_str(), conn_id);
                        fflush(stderr);
                        LOG_INFO("WebSocket", "WS_CONNECT: executor=%p path=%s conn_id=%lu",
                                 (void*)executor, ws_path.c_str(), conn_id);
                        if (executor) {
                            // Look up handler metadata for this WebSocket path
                            std::string payload;
                            auto* ws_meta = PythonCallbackBridge::get_websocket_handler_metadata(ws_path);
                            if (ws_meta) {
                                // Build JSON payload with handler info
                                payload = "{\"module\":\"" + ws_meta->module_name +
                                         "\",\"function\":\"" + ws_meta->function_name + "\"}";
                                LOG_INFO("WebSocket", "WS_CONNECT with metadata: %s", payload.c_str());
                            } else {
                                LOG_WARN("WebSocket", "No handler metadata for path: %s", ws_path.c_str());
                            }
                            bool sent = executor->send_ws_event(
                                python::MessageType::WS_CONNECT,
                                conn_id,
                                ws_path,
                                payload,
                                false
                            );
                            fprintf(stderr, "[WS_CONNECT] send_ws_event returned %d\n", sent ? 1 : 0);
                            fflush(stderr);
                            LOG_INFO("WebSocket", "WS_CONNECT sent=%d", sent ? 1 : 0);
                        } else {
                            LOG_ERROR("WebSocket", "ProcessPoolExecutor instance is NULL!");
                        }
                    }

                    // Get raw pointer before moving
                    WebSocketConnection* ws_ptr = ws_conn.get();

                    // Transfer to WebSocket connections map
                    t_websocket_connections[fd] = std::move(ws_conn);

                    // Add reverse lookup for dispatch
                    t_ws_conn_id_to_fd[conn_id] = fd;

                    // Remove from HTTP/1.1 connections
                    t_http1_connections.erase(it);

                    // Re-register fd with WebSocket handler
                    event_loop->remove_fd(fd);
                    event_loop->add_fd(fd, net::IOEvent::READ | net::IOEvent::EDGE,
                                       [ws_ptr](int fd, net::IOEvent events, void* data) {
                                           handle_websocket_connection(fd, events, static_cast<net::EventLoop*>(data), ws_ptr);
                                       }, event_loop);

                    // Register wake pipe for this thread if not already done
                    register_wake_pipe_with_event_loop(event_loop);

                    LOG_INFO("WebSocket", "fd=%d WebSocket mode activated", fd);

                    // Immediately process any pending data
                    handle_websocket_connection(fd, net::IOEvent::READ, event_loop, ws_ptr);
                    return;  // Connection is now in WebSocket mode
                }

                // Check if we transitioned to a reading state (keep-alive/pipelined requests)
                if (http1_conn->get_state() == Http1State::READING_REQUEST ||
                    http1_conn->get_state() == Http1State::KEEPALIVE) {
                    LOG_DEBUG("HTTP1", "fd=%d State transitioned to reading - checking for pipelined requests", fd);

                    // Try to read immediately for pipelined requests
                    char buffer[8192];
                    ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);

                    if (n > 0) {
                        LOG_DEBUG("HTTP1", "fd=%d Found pipelined request (%zd bytes)", fd, n);
                        auto result = http1_conn->process_input(reinterpret_cast<uint8_t*>(buffer), n);
                        if (result.is_err()) {
                            LOG_ERROR("HTTP1", "fd=%d pipelined request process error", fd);
                            event_loop->remove_fd(fd);
                            t_http1_connections.erase(it);
                            ::close(fd);
                            do_track_connection_close();
                            return;
                        }
                        LOG_DEBUG("HTTP1", "fd=%d Pipelined request processed, new state: %d", fd, static_cast<int>(http1_conn->get_state()));

                        // If we now have output to send, send it immediately
                        if (http1_conn->has_pending_output()) {
                            const uint8_t* resp_data;
                            size_t resp_len;
                            if (http1_conn->get_output(&resp_data, &resp_len)) {
                                ssize_t resp_sent = ::send(fd, resp_data, resp_len, 0);
                                if (resp_sent > 0) {
                                    http1_conn->commit_output(resp_sent);
                                }
                            }
                        }
                    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        // Read error
                        event_loop->remove_fd(fd);
                        t_http1_connections.erase(it);
                        ::close(fd);
                        do_track_connection_close();
                        return;
                    }
                    // n == 0 or EAGAIN: no pipelined data, which is normal
                }
            }
        }
    }

    // Handle WRITE event for TLS connections with pending data
    if (using_tls && (events & net::IOEvent::WRITE) && tls_it->second->needs_write_event()) {
        LOG_DEBUG("HTTP1", "fd=%d WRITE event - retrying TLS flush", fd);
        bool flush_complete = tls_it->second->flush();

        if (flush_complete) {
            LOG_DEBUG("HTTP1", "fd=%d TLS flush now complete", fd);
            // All data sent - back to READ-only
            event_loop->modify_fd(fd, net::IOEvent::READ | net::IOEvent::EDGE);
        }
        // else: still would block, keep WRITE registered
    }

    // Check if done
    if (!http1_conn->should_keep_alive() && !http1_conn->has_pending_output()) {
        event_loop->remove_fd(fd);
        t_http1_connections.erase(it);
        if (using_tls) t_tls_sockets.erase(tls_it);
        ::close(fd);
        do_track_connection_close();
        return;
    }

    // Check for WebSocket upgrade transition (ultra-fast callback path)
    // This catches WebSocket upgrades that were detected in the ultra-fast callback
    // path where the 101 response is written directly to the buffer
    if (http1_conn->is_websocket_upgrade() && !http1_conn->has_pending_output()) {
        fprintf(stderr, "[WS_TRANSITION_END] fd=%d path=%s\n", fd, http1_conn->get_websocket_path().c_str());
        fflush(stderr);
        LOG_INFO("WebSocket", "Transitioning fd=%d to WebSocket mode (end of handler), path=%s",
                 fd, http1_conn->get_websocket_path().c_str());

        // Create WebSocket connection
        static std::atomic<uint64_t> ws_connection_id_end{1000000};  // Start at high number to avoid conflicts
        auto ws_conn = std::make_unique<WebSocketConnection>(++ws_connection_id_end);
        ws_conn->set_socket_fd(fd);
        ws_conn->set_path(http1_conn->get_websocket_path());

        // Get connection ID and path for callbacks
        uint64_t conn_id = ws_conn->get_id();
        std::string ws_path = ws_conn->get_path();

        // Check for pure C++ handler first (bypasses ZMQ entirely)
        WebSocketHandler* cpp_handler = UnifiedServer::get_websocket_handler(ws_path);
        fprintf(stderr, "[WS_HANDLER_CHECK_END] cpp_handler=%p for path=%s\n", (void*)cpp_handler, ws_path.c_str());
        fflush(stderr);
        if (cpp_handler) {
            // Pure C++ mode - no ZMQ involved
            LOG_INFO("WebSocket", "Using pure C++ handler for %s (no ZMQ)", ws_path.c_str());
            (*cpp_handler)(*ws_conn);  // Let handler set up callbacks
        } else {
            // Fall back to ZMQ forwarding to Python
            ws_conn->on_text_message = [fd, conn_id, ws_path](const std::string& message) {
                LOG_DEBUG("WebSocket", "fd=%d Received text: %s", fd, message.c_str());

                // Forward to Python handler via ZMQ
                auto* executor = python::ProcessPoolExecutor::get_instance();
                if (executor) {
                    executor->send_ws_event(
                        python::MessageType::WS_MESSAGE,
                        conn_id,
                        ws_path,
                        message,
                        false  // is_binary
                    );
                }
            };

            ws_conn->on_binary_message = [fd, conn_id, ws_path](const uint8_t* data, size_t len) {
                LOG_DEBUG("WebSocket", "fd=%d Received binary: %zu bytes", fd, len);

                // Forward to Python handler via ZMQ
                auto* executor = python::ProcessPoolExecutor::get_instance();
                if (executor) {
                    std::string payload(reinterpret_cast<const char*>(data), len);
                    executor->send_ws_event(
                        python::MessageType::WS_MESSAGE,
                        conn_id,
                        ws_path,
                        payload,
                        true  // is_binary
                    );
                }
            };

            ws_conn->on_close = [fd, conn_id, ws_path](uint16_t code, const char* reason) {
                LOG_INFO("WebSocket", "fd=%d Connection closed: %d %s", fd, code, reason ? reason : "");

                // Notify Python of disconnect
                auto* executor = python::ProcessPoolExecutor::get_instance();
                if (executor) {
                    executor->send_ws_event(
                        python::MessageType::WS_DISCONNECT,
                        conn_id,
                        ws_path,
                        "",
                        false
                    );
                }
            };

            ws_conn->on_error = [fd](const char* error) {
                LOG_ERROR("WebSocket", "fd=%d Error: %s", fd, error ? error : "unknown");
            };

            // Send WS_CONNECT event to Python with handler metadata
            auto* executor = python::ProcessPoolExecutor::get_instance();
            fprintf(stderr, "[WS_CONNECT_END] executor=%p path=%s conn_id=%lu\n",
                     (void*)executor, ws_path.c_str(), conn_id);
            fflush(stderr);
            LOG_INFO("WebSocket", "WS_CONNECT: executor=%p path=%s conn_id=%lu",
                     (void*)executor, ws_path.c_str(), conn_id);
            if (executor) {
                // Look up handler metadata for this WebSocket path
                std::string payload;
                auto* ws_meta = PythonCallbackBridge::get_websocket_handler_metadata(ws_path);
                if (ws_meta) {
                    // Build JSON payload with handler info
                    payload = "{\"module\":\"" + ws_meta->module_name +
                              "\",\"function\":\"" + ws_meta->function_name + "\"}";
                }
                executor->send_ws_event(
                    python::MessageType::WS_CONNECT,
                    conn_id,
                    ws_path,
                    payload,
                    false
                );
            }
        }

        // Store WebSocket connection in thread-local map
        t_websocket_connections[fd] = std::move(ws_conn);

        // Remove HTTP1 connection - it's now a WebSocket
        t_http1_connections.erase(it);

        // Re-register for WebSocket events
        event_loop->modify_fd(fd, net::IOEvent::READ | net::IOEvent::EDGE);
    }
}

} // namespace http
} // namespace fasterapi
