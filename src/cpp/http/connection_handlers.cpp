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
    DEBUG_LOG_TLS("handle_tls_connection called, fd=%d, tls_context=%p",
                  socket.fd(), (void*)tls_context.get());

    // Set non-blocking
    if (socket.set_nonblocking() < 0) {
        DEBUG_LOG_TLS("Failed to set non-blocking on fd=%d", socket.fd());
        LOG_ERROR("Server", "Failed to set non-blocking on TLS connection");
        return;
    }

    socket.set_nodelay();

    int fd = socket.fd();

    // Create TLS socket
    DEBUG_LOG_TLS("Creating TLS socket for fd=%d", fd);
    auto tls_socket = net::TlsSocket::accept(std::move(socket), tls_context);
    if (!tls_socket) {
        DEBUG_LOG_TLS("Failed to create TLS socket for fd=%d", fd);
        LOG_ERROR("Server", "Failed to create TLS socket");
        return;
    }
    DEBUG_LOG_TLS("TLS socket created for fd=%d", fd);

    // Store TLS socket
    t_tls_sockets[fd] = std::move(tls_socket);

    // Add to event loop for TLS handshake
    auto handshake_handler = [](int fd, net::IOEvent events, void* user_data) {
        DEBUG_LOG_TLS("handshake_handler called fd=%d events=%d", fd, static_cast<int>(events));

        auto it = t_tls_sockets.find(fd);
        if (it == t_tls_sockets.end()) {
            DEBUG_LOG_TLS("fd=%d not found in t_tls_sockets", fd);
            return;
        }

        net::TlsSocket* tls_sock = it->second.get();
        net::EventLoop* loop = static_cast<net::EventLoop*>(user_data);

        // Handle errors
        if (events & net::IOEvent::ERROR) {
            DEBUG_LOG_TLS("ERROR event on fd=%d", fd);
            LOG_ERROR("Server", "TLS socket error on fd=%d", fd);
            loop->remove_fd(fd);
            t_tls_sockets.erase(it);
            ::close(fd);
            return;
        }

        // Process incoming data for handshake
        if (events & net::IOEvent::READ) {
            DEBUG_LOG_TLS("READ event on fd=%d, calling process_incoming", fd);
            ssize_t result = tls_sock->process_incoming();
            DEBUG_LOG_TLS("process_incoming returned %zd", result);
            if (result < 0) {
                DEBUG_LOG_TLS("process_incoming failed fd=%d", fd);
                LOG_ERROR("Server", "TLS process_incoming failed on fd=%d", fd);
                loop->remove_fd(fd);
                t_tls_sockets.erase(it);
                ::close(fd);
                return;
            }
        }

        // Perform handshake
        DEBUG_LOG_TLS("Calling handshake for fd=%d", fd);
        int hs_result = tls_sock->handshake();
        DEBUG_LOG_TLS("handshake returned %d for fd=%d, error: %s",
                      hs_result, fd, tls_sock->get_error().c_str());

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

                // Create HTTP/2 connection (constructor queues initial SETTINGS frame)
                auto http2_conn = new http2::Http2Connection(true);
                t_http2_connections[fd] = std::unique_ptr<http2::Http2Connection>(http2_conn);

                // Set request callback - routes requests through App or legacy handler
                // Capture http2_conn pointer since Http2Stream doesn't track connection
                http2_conn->set_request_callback([http2_conn](http2::Http2Stream* stream) {
                    if (!stream) return;

                    // Extract request details from stream
                    std::string method, path;
                    std::unordered_map<std::string, std::string> headers_map;

                    for (const auto& [key, value] : stream->request_headers()) {
                        DEBUG_LOG_H2("Callback header: '%s' = '%s'", key.c_str(), value.c_str());
                        if (key == ":method") method = value;
                        else if (key == ":path") path = value;
                        else if (key.empty() || key[0] != ':') {  // Skip pseudo-headers
                            headers_map[key] = value;
                        }
                    }

                    std::string body = stream->request_body();
                    uint32_t stream_id = stream->id();

                    DEBUG_LOG_H2("Extracted: method='%s' path='%s' body_len=%zu stream=%u",
                                 method.c_str(), path.c_str(), body.size(), stream_id);

                    // Route request through registered handler
                    std::unordered_map<std::string, std::string> response_headers;
                    std::string response_body;
                    uint16_t status_code = 200;

                    // HTTP/2 handler priority: App instance (unified routing) > legacy callback > 503
                    DEBUG_LOG_H2("HTTP/2 request: s_app_instance_=%p, s_request_handler_=%s",
                                 (void*)s_app_instance_,
                                 UnifiedServer::s_request_handler_ ? "valid" : "null");
                    if (s_app_instance_) {
                        // Use App's handle_http2_request for unified HTTP/1 and HTTP/2 routing
                        s_app_instance_->handle_http2_request(
                            method, path, headers_map, body,
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
                    } else if (UnifiedServer::s_request_handler_) {
                        // Fallback to legacy request handler callback
                        UnifiedServer::s_request_handler_(
                            method, path, headers_map, body,
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
                    } else {
                        status_code = 503;
                        response_body = "Service Unavailable - No request handler registered\n";
                        response_headers["content-type"] = "text/plain";
                    }

                    // Send HTTP/2 response
                    auto result = http2_conn->send_response(stream_id, status_code, response_headers, response_body);
                    if (result.is_err()) {
                        LOG_ERROR("HTTP2", "Failed to send response for stream=%u", stream_id);
                    }
                });

                // Send initial SETTINGS frame through TLS (server connection preface)
                const uint8_t* initial_data;
                size_t initial_len;
                int initial_count = 0;
                DEBUG_LOG_H2("Checking for initial output to send");
                while (http2_conn->get_output(&initial_data, &initial_len)) {
                    initial_count++;
                    DEBUG_LOG_H2("get_output returned %zu bytes (call %d)", initial_len, initial_count);
                    DEBUG_LOG_H2("Frame hex: %s", fasterapi::core::hex_dump(initial_data, initial_len, 20).c_str());

                    if (initial_len == 0) break;

                    ssize_t written = tls_sock->write(initial_data, initial_len);
                    DEBUG_LOG_H2("tls_sock->write returned %zd", written);
                    if (written > 0) {
                        http2_conn->commit_output(written);
                        if (written < (ssize_t)initial_len) break;
                    } else {
                        break;
                    }
                }
                DEBUG_LOG_H2("Calling flush for initial SETTINGS");
                bool init_flush = tls_sock->flush();
                DEBUG_LOG_H2("Initial flush returned %d", init_flush);

                // Add to event loop with HTTP/2 handler
                loop->add_fd(fd, net::IOEvent::READ | net::IOEvent::EDGE,
                           [](int fd, net::IOEvent events, void* data) {
                               handle_http2_connection(fd, static_cast<net::EventLoop*>(data),
                                                     t_http2_connections[fd].get());
                           }, loop);

                // CRITICAL: Check for pending decrypted data after TLS handshake
                // (same fix as HTTP/1.1 - edge-triggered kqueue won't trigger for buffered data)
                if (tls_sock->has_pending_input()) {
                    LOG_DEBUG("Server", "fd=%d has pending TLS data after handshake - invoking HTTP/2 handler", fd);
                    DEBUG_LOG_TLS("fd=%d has pending input after handshake, invoking HTTP/2 handler", fd);
                    handle_http2_connection(fd, loop, http2_conn);
                }

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
                    DEBUG_LOG_HTTP1("Checking handlers: s_app_instance_=%p s_request_handler_=%s",
                                    (void*)s_app_instance_,
                                    UnifiedServer::s_request_handler_ ? "valid" : "null");

                    // Try App instance first (unified routing), then legacy handler
                    if (s_app_instance_) {
                        s_app_instance_->handle_http2_request(
                            method, path, headers, body,
                            [&response](
                                uint16_t status,
                                const std::unordered_map<std::string, std::string>& hdrs,
                                const std::string& body
                            ) {
                                response.status = status;
                                response.body = body;
                                response.headers = hdrs;
                            }
                        );
                    } else if (UnifiedServer::s_request_handler_) {
                        std::unordered_map<std::string, std::string> response_headers;
                        std::string response_body;
                        uint16_t status_code = 200;

                        // Call the handler with a send_response callback
                        UnifiedServer::s_request_handler_(
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

                // CRITICAL: Check for pending decrypted data after TLS handshake
                // With edge-triggered kqueue, we only get READ events when NEW data arrives.
                // If HTTP request data arrived during the TLS handshake (same TCP segment),
                // it's already decrypted and buffered - no new event will trigger!
                if (tls_sock->has_pending_input()) {
                    LOG_DEBUG("Server", "fd=%d has pending TLS data after handshake - invoking handler", fd);
                    DEBUG_LOG_TLS("fd=%d has pending input after handshake, invoking HTTP handler", fd);
                    // Immediately invoke the HTTP handler with a synthetic READ event
                    handle_http1_connection(fd, net::IOEvent::READ, loop, conn_ptr);
                }
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
                // Check if this is an async route first
                std::string method(view.method);
                std::string path(view.path);

                if (s_app_instance_->has_async_route(method, path)) {
                    // Async route - dispatch using coroutine
                    // For now, run synchronously until coroutine completes

                    // Build headers map
                    std::unordered_map<std::string, std::string> headers;
                    headers.reserve(view.header_count);
                    for (size_t i = 0; i < view.header_count; ++i) {
                        headers.emplace(
                            std::string(view.headers[i].first),
                            std::string(view.headers[i].second)
                        );
                    }

                    // Reconstruct full URL with query string
                    std::string full_path;
                    if (!view.query_string.empty()) {
                        full_path.reserve(path.size() + 1 + view.query_string.size());
                        full_path = path;
                        full_path += '?';
                        full_path.append(view.query_string);
                    } else {
                        full_path = path;
                    }

                    // Create HttpRequest from parsed data
                    HttpRequest req = HttpRequest::from_parsed_data(method, full_path, headers, std::string(view.body));

                    // Create HttpResponse object
                    HttpResponse res;

                    // Create wrapper objects
                    RouteParams params;
                    Request request(&req, params);
                    Response response_obj(&res);

                    // Get async handler and dispatch
                    auto coro = s_app_instance_->dispatch_async(request, response_obj);

                    // Run coroutine to completion (synchronous for now)
                    // This will execute all co_await operations until done
                    while (coro.resume()) {
                        // Keep resuming until coroutine completes
                    }

                    // Build Http1Response from HttpResponse
                    Http1Response http1_response;
                    http1_response.status = static_cast<uint16_t>(res.get_status_code());

                    // Get status message
                    switch (res.get_status_code()) {
                        case HttpResponse::Status::OK: http1_response.status_message = "OK"; break;
                        case HttpResponse::Status::CREATED: http1_response.status_message = "Created"; break;
                        case HttpResponse::Status::NO_CONTENT: http1_response.status_message = "No Content"; break;
                        case HttpResponse::Status::BAD_REQUEST: http1_response.status_message = "Bad Request"; break;
                        case HttpResponse::Status::UNAUTHORIZED: http1_response.status_message = "Unauthorized"; break;
                        case HttpResponse::Status::FORBIDDEN: http1_response.status_message = "Forbidden"; break;
                        case HttpResponse::Status::NOT_FOUND: http1_response.status_message = "Not Found"; break;
                        case HttpResponse::Status::INTERNAL_SERVER_ERROR: http1_response.status_message = "Internal Server Error"; break;
                        default: http1_response.status_message = "OK"; break;
                    }

                    http1_response.headers = res.get_headers();
                    http1_response.body = res.get_body();

                    return http1_response;
                }

                // Sync route - use existing fast path
                return s_app_instance_->handle_http1_fast(view);
            }

            // Fallback to old path if App not set (for backward compatibility)
            if (UnifiedServer::s_request_handler_) {
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

                UnifiedServer::s_request_handler_(
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
    LOG_DEBUG("HTTP2", "handle_http2_connection fd=%d", fd);

    // Safety check
    if (!http2_conn) {
        LOG_ERROR("HTTP2", "null http2_conn pointer for fd=%d", fd);
        return;
    }

    // HTTP/2 over TLS only - look up TLS socket
    auto tls_it = t_tls_sockets.find(fd);
    if (tls_it == t_tls_sockets.end()) {
        LOG_ERROR("HTTP2", "TLS socket not found for fd=%d", fd);
        // Clean up
        auto conn_it = t_http2_connections.find(fd);
        if (conn_it != t_http2_connections.end()) {
            t_http2_connections.erase(conn_it);
        }
        event_loop->remove_fd(fd);
        ::close(fd);
        do_track_connection_close();
        return;
    }

    net::TlsSocket* tls_sock = tls_it->second.get();

    // Process incoming data through TLS
    ssize_t incoming_result = tls_sock->process_incoming();
    if (incoming_result < 0) {
        LOG_ERROR("HTTP2", "TLS process_incoming failed for fd=%d", fd);
        event_loop->remove_fd(fd);
        t_http2_connections.erase(fd);
        t_tls_sockets.erase(tls_it);
        ::close(fd);
        do_track_connection_close();
        return;
    }

    // Read decrypted data and feed to HTTP/2 connection
    char buffer[16384];  // Max HTTP/2 frame size is 16KB
    ssize_t n = tls_sock->read(buffer, sizeof(buffer));

    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_ERROR("HTTP2", "TLS read error for fd=%d: errno=%d", fd, errno);
            event_loop->remove_fd(fd);
            t_http2_connections.erase(fd);
            t_tls_sockets.erase(tls_it);
            ::close(fd);
            do_track_connection_close();
        }
        return;
    }

    if (n == 0) {
        // Connection closed
        LOG_DEBUG("HTTP2", "Connection closed for fd=%d", fd);
        event_loop->remove_fd(fd);
        t_http2_connections.erase(fd);
        t_tls_sockets.erase(tls_it);
        ::close(fd);
        do_track_connection_close();
        return;
    }

    // Process HTTP/2 frames
    LOG_DEBUG("HTTP2", "Processing %zd bytes for fd=%d", n, fd);
    auto result = http2_conn->process_input(reinterpret_cast<const uint8_t*>(buffer), n);
    if (result.is_err()) {
        LOG_ERROR("HTTP2", "HTTP/2 frame processing error for fd=%d", fd);
        event_loop->remove_fd(fd);
        t_http2_connections.erase(fd);
        t_tls_sockets.erase(tls_it);
        ::close(fd);
        do_track_connection_close();
        return;
    }

    // Send any pending output
    const uint8_t* out_data;
    size_t out_len;
    int output_count = 0;
    while (http2_conn->get_output(&out_data, &out_len)) {
        output_count++;
        DEBUG_LOG_H2("get_output returned %zu bytes (call %d)", out_len, output_count);
        DEBUG_LOG_H2("Frame hex: %s", fasterapi::core::hex_dump(out_data, out_len, 30).c_str());
        // Write through TLS
        ssize_t written = tls_sock->write(out_data, out_len);
        DEBUG_LOG_H2("tls_sock->write returned %zd", written);
        if (written <= 0) {
            DEBUG_LOG_H2("write failed, breaking");
            break;
        }
        http2_conn->commit_output(written);
    }
    if (output_count == 0) {
        DEBUG_LOG_H2("No output data to send for fd=%d", fd);
    }

    // Flush TLS output
    DEBUG_LOG_H2("Calling tls_sock->flush()");
    bool flush_complete = tls_sock->flush();
    DEBUG_LOG_H2("flush_complete=%d", flush_complete);
    if (!flush_complete) {
        // Register for WRITE events to complete flush
        event_loop->modify_fd(fd, net::IOEvent::READ | net::IOEvent::WRITE | net::IOEvent::EDGE);
    }

    // Check if connection was closed via GOAWAY
    if (!http2_conn->is_active()) {
        LOG_DEBUG("HTTP2", "HTTP/2 connection closed for fd=%d", fd);
        event_loop->remove_fd(fd);
        t_http2_connections.erase(fd);
        t_tls_sockets.erase(tls_it);
        ::close(fd);
        do_track_connection_close();
    }
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
                    DEBUG_LOG_WS("cpp_handler=%p for path=%s", (void*)cpp_handler, ws_path.c_str());
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
                        DEBUG_LOG_WS("WS_CONNECT executor=%p path=%s conn_id=%lu",
                                     (void*)executor, ws_path.c_str(), conn_id);
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
                            DEBUG_LOG_WS("send_ws_event returned %d", sent ? 1 : 0);
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
        DEBUG_LOG_WS("WS_TRANSITION fd=%d path=%s", fd, http1_conn->get_websocket_path().c_str());
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
        DEBUG_LOG_WS("cpp_handler=%p for path=%s", (void*)cpp_handler, ws_path.c_str());
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
            DEBUG_LOG_WS("WS_CONNECT executor=%p path=%s conn_id=%lu",
                         (void*)executor, ws_path.c_str(), conn_id);
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
