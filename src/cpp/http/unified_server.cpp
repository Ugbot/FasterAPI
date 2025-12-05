/**
 * Unified HTTP Server Implementation
 *
 * Integrates TLS/ALPN with HTTP/2 and HTTP/1.1
 */

#include "unified_server.h"
#include "app.h"
#include "core/logger.h"
#include "net/tls_cert_generator.h"
#include "net/udp_socket.h"
#include "quic/quic_packet.h"
#include "websocket_parser.h"
#include "websocket.h"
#include "../python/process_pool_executor.h"
#include "../python/ipc_protocol.h"
#include "python_callback_bridge.h"
#include "../core/coro_resumer.h"
#include <unordered_map>
#include <fcntl.h>
#include <memory>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <unistd.h>

namespace fasterapi {
namespace http {

// Forward declaration for dispatch function
static void dispatch_pending_ws_responses();

// Global request handler (shared across connections)
HttpRequestHandler UnifiedServer::s_request_handler_;

// Pure C++ WebSocket handlers (path → handler mapping)
std::unordered_map<std::string, WebSocketHandler> UnifiedServer::s_websocket_handlers_;

// Direct App pointer for simplified Http1 handling
static ::fasterapi::App* s_app_instance_ = nullptr;

// Per-thread connection storage
thread_local std::unordered_map<int, std::unique_ptr<net::TlsSocket>> t_tls_sockets;
thread_local std::unordered_map<int, std::unique_ptr<http2::Http2Connection>> t_http2_connections;
thread_local std::unordered_map<int, std::unique_ptr<Http1Connection>> t_http1_connections;

// HTTP/3 and WebTransport connection storage (keyed by connection ID string)
thread_local std::unordered_map<std::string, std::unique_ptr<Http3Connection>> t_http3_connections;
thread_local std::unordered_map<std::string, std::unique_ptr<WebTransportConnection>> t_webtransport_connections;

// WebSocket connection storage (keyed by fd)
thread_local std::unordered_map<int, std::unique_ptr<WebSocketConnection>> t_websocket_connections;

// Reverse lookup: connection_id -> fd (for dispatch from queue)
thread_local std::unordered_map<uint64_t, int> t_ws_conn_id_to_fd;

// Wake pipe for cross-thread signaling (response dispatch)
// read_fd is added to event loop, write_fd is used by response reader thread
thread_local int t_wake_pipe_read_fd = -1;
thread_local int t_wake_pipe_write_fd = -1;
thread_local bool t_wake_pipe_registered = false;

// Global registry of wake pipe write fds (one per worker thread)
// Protected by mutex, used by response reader thread to signal all workers
static std::mutex s_wake_pipes_mutex;
static std::vector<int> s_wake_pipe_write_fds;

// Initialize wake pipe for current thread
static bool init_wake_pipe() {
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

// Signal all worker threads to dispatch WebSocket responses
void signal_ws_response_ready() {
    std::lock_guard<std::mutex> lock(s_wake_pipes_mutex);
    for (int write_fd : s_wake_pipe_write_fds) {
        // Write a single byte to wake the event loop
        char c = 1;
        ssize_t n = write(write_fd, &c, 1);
        (void)n;  // Ignore result - may fail if pipe full (that's ok)
    }
}

// Register wake pipe with event loop (called once per thread when first WebSocket connection established)
static void register_wake_pipe_with_event_loop(net::EventLoop* event_loop) {
    if (t_wake_pipe_registered) return;  // Already registered

    if (!init_wake_pipe()) {
        LOG_ERROR("WS", "Failed to initialize wake pipe");
        return;
    }

    // Add read end of pipe to event loop
    // Handler drains the pipe and dispatches pending WebSocket responses
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

// Helper to clean up WebSocket connection and reverse lookup
static void cleanup_websocket_connection(int fd) {
    auto it = t_websocket_connections.find(fd);
    if (it != t_websocket_connections.end()) {
        // Clean up reverse lookup first
        uint64_t conn_id = it->second->get_id();
        t_ws_conn_id_to_fd.erase(conn_id);
        // Then erase the connection
        t_websocket_connections.erase(it);
    }
}

// Helper: Convert ConnectionID to hex string for map key
static std::string connection_id_to_string(const quic::ConnectionID& conn_id) noexcept {
    std::ostringstream oss;
    for (uint8_t i = 0; i < conn_id.length; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(conn_id.data[i]);
    }
    return oss.str();
}

// Helper: Get current time in microseconds
static uint64_t get_time_us() noexcept {
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

UnifiedServer::UnifiedServer(const UnifiedServerConfig& config)
    : config_(config)
{
}

UnifiedServer::~UnifiedServer() {
    stop();
}

void UnifiedServer::set_request_handler(HttpRequestHandler handler) {
    s_request_handler_ = std::move(handler);
}

void UnifiedServer::add_websocket_handler(const std::string& path, WebSocketHandler handler) {
    s_websocket_handlers_[path] = std::move(handler);
    LOG_INFO("WebSocket", "Registered pure C++ handler for path: %s", path.c_str());
}

WebSocketHandler* UnifiedServer::get_websocket_handler(const std::string& path) {
    auto it = s_websocket_handlers_.find(path);
    if (it != s_websocket_handlers_.end()) {
        return &it->second;
    }
    return nullptr;
}

// Set App instance for direct HTTP/1.1 handling
void UnifiedServer::set_app_instance(void* app) {
    s_app_instance_ = static_cast<::fasterapi::App*>(app);
}

int UnifiedServer::start() {
    // Create TLS context if enabled
    if (config_.enable_tls) {
        net::TlsContextConfig tls_config;
        tls_config.cert_file = config_.cert_file;
        tls_config.key_file = config_.key_file;
        tls_config.cert_data = config_.cert_data;
        tls_config.key_data = config_.key_data;
        tls_config.alpn_protocols = config_.alpn_protocols;

        // Auto-generate certificates if none provided
        bool has_cert_file = !config_.cert_file.empty() && !config_.key_file.empty();
        bool has_cert_data = !config_.cert_data.empty() && !config_.key_data.empty();

        if (!has_cert_file && !has_cert_data) {
            LOG_INFO("Server", "No TLS certificates provided, generating self-signed certificate...");

            net::CertGeneratorConfig cert_config;
            cert_config.common_name = "localhost";
            cert_config.organization = "FasterAPI";
            cert_config.validity_days = 365;

            auto generated = net::TlsCertGenerator::generate(cert_config);
            if (!generated.success) {
                error_message_ = "Failed to generate self-signed certificate: " + generated.error;
                LOG_ERROR("Server", "%s", error_message_.c_str());
                return -1;
            }

            // Use generated certificates (in-memory)
            tls_config.cert_data = generated.cert_pem;
            tls_config.key_data = generated.key_pem;

            LOG_INFO("Server", "Self-signed certificate generated successfully");
        } else if (has_cert_file) {
            LOG_INFO("Server", "Using TLS certificates from files: %s, %s",
                     config_.cert_file.c_str(), config_.key_file.c_str());
        } else {
            LOG_INFO("Server", "Using TLS certificates from memory");
        }

        tls_context_ = net::TlsContext::create_server(tls_config);
        if (!tls_context_ || !tls_context_->is_valid()) {
            error_message_ = "Failed to create TLS context: " +
                           (tls_context_ ? tls_context_->get_error() : "null context");
            LOG_ERROR("Server", "Failed to create TLS context: %s", error_message_.c_str());
            return -1;
        }

        LOG_INFO("Server", "TLS context created with ALPN protocols: %zu configured", config_.alpn_protocols.size());

        // Create TLS listener (port 443)
        net::TcpListenerConfig tls_listener_config;
        tls_listener_config.port = config_.tls_port;
        tls_listener_config.host = config_.host;
        tls_listener_config.num_workers = config_.num_workers;
        tls_listener_config.use_reuseport = config_.use_reuseport;

        // Capture tls_context in lambda
        auto tls_ctx = tls_context_;
        tls_listener_ = std::make_unique<net::TcpListener>(
            tls_listener_config,
            [tls_ctx](net::TcpSocket socket, net::EventLoop* loop) {
                handle_tls_connection(std::move(socket), loop, tls_ctx);
            }
        );

        LOG_INFO("Server", "TLS listener on %s:%d", config_.host.c_str(), config_.tls_port);
    }

    // Create cleartext listener if enabled (port 8080)
    if (config_.enable_http1_cleartext) {
        net::TcpListenerConfig cleartext_config;
        cleartext_config.port = config_.http1_port;
        cleartext_config.host = config_.host;
        cleartext_config.num_workers = config_.num_workers;
        cleartext_config.use_reuseport = config_.use_reuseport;

        cleartext_listener_ = std::make_unique<net::TcpListener>(
            cleartext_config,
            on_cleartext_connection
        );

        LOG_INFO("Server", "Cleartext HTTP/1.1 listener on %s:%d", config_.host.c_str(), config_.http1_port);
    }

    // Create HTTP/3 listener if enabled (UDP)
    if (config_.enable_http3) {
        LOG_INFO("Server", "Starting HTTP/3 (QUIC) on UDP port %d...", config_.http3_port);

        net::UdpListenerConfig quic_config;
        quic_config.host = config_.host;
        quic_config.port = config_.http3_port;
        quic_config.num_workers = config_.num_workers;
        quic_config.max_datagram_size = 65535;
        quic_config.recv_buffer_size = 2 * 1024 * 1024;  // 2MB
        quic_config.enable_pktinfo = true;
        quic_config.enable_tos = true;  // ECN for congestion control

        quic_listener_ = std::make_unique<net::UdpListener>(
            quic_config,
            on_quic_datagram
        );

        LOG_INFO("Server", "HTTP/3 listener on %s:%d (UDP)", config_.host.c_str(), config_.http3_port);
    }

    // Start TLS listener in background thread if both TCP listeners are enabled
    if (tls_listener_ && cleartext_listener_) {
        tls_thread_ = std::thread([this]() {
            LOG_INFO("Server", "Starting TLS listener...");
            tls_listener_->start();
        });
    }

    // Start QUIC listener in background thread if TLS or cleartext also running
    if (quic_listener_ && (tls_listener_ || cleartext_listener_)) {
        quic_thread_ = std::thread([this]() {
            LOG_INFO("Server", "Starting QUIC listener...");
            quic_listener_->start();
        });
    }

    // Start cleartext listener in main thread (blocks)
    if (cleartext_listener_) {
        LOG_INFO("Server", "Starting cleartext listener...");
        return cleartext_listener_->start();
    }

    // If only TLS (no cleartext), run it in main thread
    if (tls_listener_) {
        LOG_INFO("Server", "Starting TLS listener...");
        return tls_listener_->start();
    }

    // If only QUIC (no TCP listeners), run it in main thread
    if (quic_listener_) {
        LOG_INFO("Server", "Starting QUIC listener...");
        return quic_listener_->start();
    }

    error_message_ = "No listeners configured";
    return -1;
}

void UnifiedServer::stop() {
    shutdown_flag_.store(true, std::memory_order_relaxed);

    if (tls_listener_) {
        tls_listener_->stop();
    }
    if (cleartext_listener_) {
        cleartext_listener_->stop();
    }
    if (quic_listener_) {
        quic_listener_->stop();
    }

    // Join TLS thread if running in background
    if (tls_thread_.joinable()) {
        tls_thread_.join();
    }
    // Join QUIC thread if running in background
    if (quic_thread_.joinable()) {
        quic_thread_.join();
    }
}

/**
 * QUIC datagram handler (static method)
 *
 * Called when UDP datagram received on HTTP/3 port.
 * Parses QUIC connection ID, routes to appropriate Http3Connection,
 * and generates response datagrams.
 */
void UnifiedServer::on_quic_datagram(
    const uint8_t* data,
    size_t length,
    const struct sockaddr* addr,
    socklen_t addrlen,
    net::EventLoop* event_loop
) {
    if (length < 5) {
        // Too short to be valid QUIC packet
        return;
    }

    uint64_t now_us = get_time_us();

    // Parse connection ID from QUIC packet
    // For server: we care about Destination Connection ID (DCID) which identifies our connection
    quic::ConnectionID dcid;

    uint8_t first_byte = data[0];
    size_t pos = 1;

    // Long header packet (Initial, Handshake, 0-RTT)
    if ((first_byte & 0x80) != 0) {
        // Skip version (4 bytes)
        if (length < pos + 4) return;
        pos += 4;

        // Read DCID length
        if (length < pos + 1) return;
        uint8_t dcid_len = data[pos++];

        if (dcid_len > 20 || length < pos + dcid_len) return;
        dcid.length = dcid_len;
        std::memcpy(dcid.data, data + pos, dcid_len);
    }
    // Short header packet (1-RTT)
    else {
        // For short header, we need to know the connection ID length from connection state
        // For now, assume 8-byte connection ID (common default)
        // In production, this should be tracked per-connection
        uint8_t dcid_len = 8;
        if (length < pos + dcid_len) return;
        dcid.length = dcid_len;
        std::memcpy(dcid.data, data + pos, dcid_len);
    }

    std::string conn_id_str = connection_id_to_string(dcid);

    // Look up or create HTTP/3 connection
    auto it = t_http3_connections.find(conn_id_str);
    Http3Connection* http3_conn = nullptr;

    if (it == t_http3_connections.end()) {
        // New connection - create Http3Connection
        LOG_INFO("HTTP3", "New QUIC connection: %s", conn_id_str.c_str());

        // Generate local connection ID (server)
        quic::ConnectionID local_cid;
        local_cid.length = 8;
        // In production: use crypto-random generation
        // For now: use simple counter-based ID
        static uint64_t conn_counter = 0;
        uint64_t counter = __atomic_fetch_add(&conn_counter, 1, __ATOMIC_RELAXED);
        std::memcpy(local_cid.data, &counter, sizeof(counter));

        auto new_conn = std::make_unique<Http3Connection>(
            true,  // is_server
            local_cid,
            dcid,
            Http3ConnectionSettings()
        );

        // Initialize connection
        if (new_conn->initialize() < 0) {
            LOG_ERROR("HTTP3", "Failed to initialize HTTP/3 connection");
            return;
        }

        // Set request callback (same as HTTP/2)
        if (s_request_handler_) {
            new_conn->set_request_callback(s_request_handler_);
        }

        http3_conn = new_conn.get();
        t_http3_connections[conn_id_str] = std::move(new_conn);

        LOG_DEBUG("HTTP3", "Created HTTP/3 connection for %s", conn_id_str.c_str());
    } else {
        http3_conn = it->second.get();
    }

    // Process incoming datagram
    int result = http3_conn->process_datagram(data, length, now_us);
    if (result < 0) {
        LOG_ERROR("HTTP3", "Failed to process datagram for connection %s", conn_id_str.c_str());

        // Check if connection closed
        if (http3_conn->is_closed()) {
            LOG_INFO("HTTP3", "Connection %s closed, removing", conn_id_str.c_str());
            t_http3_connections.erase(conn_id_str);
        }
        return;
    }

    // Generate outgoing datagrams (responses, ACKs, etc.)
    uint8_t output_buffer[65535];
    size_t output_len = http3_conn->generate_datagrams(output_buffer, sizeof(output_buffer), now_us);

    if (output_len > 0) {
        // Send response datagram(s) back to client
        // Note: We need a UdpSocket to send - get it from event_loop user_data or similar
        // For now, we'll use recvfrom's fd which should be available in the event loop context

        // TODO: This needs access to the UDP socket FD
        // For now, log that we would send
        LOG_DEBUG("HTTP3", "Generated %zu bytes to send for connection %s", output_len, conn_id_str.c_str());

        // In production: event_loop->get_udp_socket()->sendto(output_buffer, output_len, addr, addrlen);
        // We'll need to modify UdpListener to expose the socket or pass it via user_data
    }

    // Clean up closed connections
    if (http3_conn->is_closed()) {
        LOG_INFO("HTTP3", "Connection %s closed, removing", conn_id_str.c_str());
        t_http3_connections.erase(conn_id_str);
    }
}

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
            return;
        }

        // Process incoming data for handshake
        if (events & net::IOEvent::READ) {
            ssize_t result = tls_sock->process_incoming();
            if (result < 0) {
                LOG_ERROR("Server", "TLS process_incoming failed on fd=%d", fd);
                loop->remove_fd(fd);
                t_tls_sockets.erase(it);
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
        }
    };

    if (event_loop->add_fd(fd, net::IOEvent::READ | net::IOEvent::EDGE, handshake_handler, event_loop) < 0) {
        LOG_ERROR("Server", "Failed to add TLS socket fd=%d to event loop", fd);
        t_tls_sockets.erase(fd);
    }
}

void UnifiedServer::on_cleartext_connection(net::TcpSocket socket, net::EventLoop* event_loop) {
    int fd = socket.fd();
    LOG_DEBUG("HTTP1", "Cleartext connection accepted on fd=%d", fd);

    // Set non-blocking
    if (socket.set_nonblocking() < 0) {
        LOG_ERROR("HTTP1", "Failed to set non-blocking on fd=%d", fd);
        return;
    }

    socket.set_nodelay();

    // Create HTTP/1.1 connection
    auto http1_conn = new Http1Connection(fd);
    http1_conn->set_request_callback([](
        const std::string& method,
        const std::string& path,
        const std::unordered_map<std::string, std::string>& headers,
        const std::string& body
    ) -> Http1Response {
        Http1Response response;

        // Check for WebSocket upgrade request first
        auto upgrade_it = headers.find("Upgrade");
        auto connection_it = headers.find("Connection");
        auto ws_key_it = headers.find("Sec-WebSocket-Key");
        auto ws_version_it = headers.find("Sec-WebSocket-Version");

        if (upgrade_it != headers.end() && ws_key_it != headers.end()) {
            std::string upgrade_val = upgrade_it->second;
            std::string connection_val = connection_it != headers.end() ? connection_it->second : "";
            std::string ws_version = ws_version_it != headers.end() ? ws_version_it->second : "";
            std::string ws_key = ws_key_it->second;

            if (websocket::HandshakeUtils::validate_upgrade_request(
                    method, upgrade_val, connection_val, ws_version, ws_key)) {
                // Valid WebSocket upgrade - build 101 Switching Protocols response
                LOG_INFO("WebSocket", "Cleartext: Valid upgrade request for path: %s", path.c_str());

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

        // Simplified path: call App::handle_http1() directly if available
        if (s_app_instance_) {
            return s_app_instance_->handle_http1(method, path, headers, body);
        }

        // Fallback to old path if App not set (for backward compatibility)
        if (s_request_handler_) {
            std::unordered_map<std::string, std::string> response_headers;
            std::string response_body;
            uint16_t status_code = 200;

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
    }
}

void UnifiedServer::handle_http2_connection(
    int fd,
    net::EventLoop* event_loop,
    http2::Http2Connection* http2_conn
) {
    // TODO: Implement HTTP/2 connection handling with TLS I/O
    // This needs to read/write through the TLS socket
    LOG_WARN("HTTP2", "HTTP/2 connection handling not yet implemented for fd=%d", fd);
}

// WebSocket connection handler
static void handle_websocket_connection(
    int fd,
    net::IOEvent events,
    net::EventLoop* event_loop,
    WebSocketConnection* ws_conn
) {
    auto it = t_websocket_connections.find(fd);
    if (it == t_websocket_connections.end()) {
        LOG_ERROR("WebSocket", "Connection not found for fd=%d", fd);
        event_loop->remove_fd(fd);
        return;
    }

    // With edge-triggered events, we must keep processing in a loop.
    // After sending a response, the client may immediately send the next message.
    // We need to check for new data after each send cycle.
    bool keep_processing = true;
    char buffer[8192];

    while (keep_processing) {
        keep_processing = false;  // Will be set true if we receive data

        // Handle read events
        // With edge-triggered events, we must read in a loop until EAGAIN
        if (events & net::IOEvent::READ) {
            bool connection_closed = false;
            bool received_data = false;

            // Loop to drain all available data (required for edge-triggered mode)
            while (true) {
                ssize_t n = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);

                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // No more data available - this is expected
                        break;
                    }
                    LOG_ERROR("WebSocket", "Read error on fd=%d: errno=%d", fd, errno);
                    event_loop->remove_fd(fd);
                    cleanup_websocket_connection(fd);
                    ::close(fd);
                    return;
                }

                if (n == 0) {
                    // Peer closed connection
                    connection_closed = true;
                    break;
                }

                received_data = true;

                // Process WebSocket frames
                int result = ws_conn->handle_frame(reinterpret_cast<uint8_t*>(buffer), n);
                if (result < 0) {
                    LOG_ERROR("WebSocket", "Frame handling error on fd=%d: %d", fd, result);
                }

                // Check if connection was closed by frame handler (e.g., close frame)
                if (!ws_conn->is_open()) {
                    LOG_DEBUG("WebSocket", "Connection closed by handler on fd=%d", fd);
                    event_loop->remove_fd(fd);
                    cleanup_websocket_connection(fd);
                    ::close(fd);
                    return;
                }
            }

            // Handle peer close after draining all data
            if (connection_closed) {
                LOG_DEBUG("WebSocket", "Connection closed on fd=%d", fd);
                event_loop->remove_fd(fd);
                cleanup_websocket_connection(fd);
                ::close(fd);
                return;
            }

            // If we received data, dispatch responses and continue processing
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
                    // Would block, wait for next write event
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
                // Partial send - this shouldn't happen in practice for small frames
                LOG_WARN("WebSocket", "Partial send on fd=%d: %zd/%zu", fd, sent, frame->size());
            }

            ws_conn->pop_pending_output();

            // After sending a response, check if there's more data to read
            // (the client may have sent the next message already)
            keep_processing = true;
        }
    }

    // Back to read-only mode
    event_loop->modify_fd(fd, net::IOEvent::READ | net::IOEvent::EDGE);

    // CRITICAL: In edge-triggered mode, we must check for data that may have arrived
    // while processing. If we don't, we'll miss messages because the edge transition
    // was consumed while we were busy processing the previous message.
    // Do a non-blocking peek to see if more data is available.
    char peek_buf[1];
    ssize_t peek = ::recv(fd, peek_buf, 1, MSG_PEEK | MSG_DONTWAIT);
    if (peek > 0) {
        // More data available - recursively process it
        LOG_DEBUG("WebSocket", "fd=%d has pending data after modify_fd, recursing", fd);
        handle_websocket_connection(fd, net::IOEvent::READ, event_loop, ws_conn);
    }
}

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
            }
            return;
        }

        if (n == 0) {
            // Connection closed
            LOG_DEBUG("HTTP1", "Connection closed on fd=%d", fd);
            event_loop->remove_fd(fd);
            t_http1_connections.erase(it);
            if (using_tls) t_tls_sockets.erase(tls_it);
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
            return;
        }
        LOG_DEBUG("HTTP1", "fd=%d Processed successfully, new state: %d", fd, static_cast<int>(http1_conn->get_state()));
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
                }
                return;
            }

            if (sent > 0) {
                http1_conn->commit_output(sent);
                LOG_DEBUG("HTTP1", "fd=%d After commit_output, state=%d", fd, static_cast<int>(http1_conn->get_state()));

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
                        if (executor) {
                            // Look up handler metadata for this WebSocket path
                            std::string payload;
                            auto* ws_meta = PythonCallbackBridge::get_websocket_handler_metadata(ws_path);
                            if (ws_meta) {
                                // Build JSON payload with handler info
                                payload = "{\"module\":\"" + ws_meta->module_name +
                                         "\",\"function\":\"" + ws_meta->function_name + "\"}";
                                LOG_DEBUG("WebSocket", "WS_CONNECT with metadata: %s", payload.c_str());
                            } else {
                                LOG_WARN("WebSocket", "No handler metadata for path: %s", ws_path.c_str());
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

                    // Get raw pointer before moving
                    WebSocketConnection* ws_ptr = ws_conn.get();

                    // Transfer to WebSocket connections map
                    t_websocket_connections[fd] = std::move(ws_conn);

                    // Add reverse lookup for dispatch
                    t_ws_conn_id_to_fd[conn_id] = fd;

                    // Remove from HTTP/1.1 connections
                    t_http1_connections.erase(it);

                    // Re-register fd with WebSocket handler
                    // IMPORTANT: The modify/remove/add sequence was causing edge events to be lost
                    // in edge-triggered kqueue mode. If data arrives during remove→add window,
                    // the edge transition is lost and no future events fire.
                    event_loop->remove_fd(fd);
                    event_loop->add_fd(fd, net::IOEvent::READ | net::IOEvent::EDGE,
                                       [ws_ptr](int fd, net::IOEvent events, void* data) {
                                           handle_websocket_connection(fd, events, static_cast<net::EventLoop*>(data), ws_ptr);
                                       }, event_loop);

                    // Register wake pipe for this thread if not already done
                    // This enables cross-thread signaling for WebSocket response dispatch
                    register_wake_pipe_with_event_loop(event_loop);

                    LOG_INFO("WebSocket", "fd=%d WebSocket mode activated", fd);

                    // CRITICAL: Immediately process any pending data that may have arrived
                    // during the fd re-registration window. This prevents losing edge events
                    // in edge-triggered kqueue mode where events only fire on state CHANGES.
                    handle_websocket_connection(fd, net::IOEvent::READ, event_loop, ws_ptr);
                    return;  // Connection is now in WebSocket mode
                }

                // Check if we transitioned to a reading state (keep-alive/pipelined requests)
                // With edge-triggered events, we must check for already-buffered data
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
    }
}

/**
 * Dispatch pending WebSocket responses from Python workers.
 *
 * Called from the event loop wake callback via CoroResumer.
 * Polls the lock-free queue and sends messages to WebSocket connections.
 */
static void dispatch_pending_ws_responses() {
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
            // Clean up stale reverse lookup
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

            // Send pending output immediately using send() for sockets
            while (ws->has_pending_output()) {
                const std::string* frame = ws->get_pending_output();
                if (!frame) break;

                ssize_t sent = ::send(fd, frame->data(), frame->size(), MSG_NOSIGNAL);
                if (sent < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // Socket buffer full, will be sent later via event loop
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

            // After sending a response, check for incoming data
            // With edge-triggered events, new data may have arrived
            // while we were processing the response
            char recv_buffer[8192];
            while (true) {
                ssize_t n = ::recv(fd, recv_buffer, sizeof(recv_buffer), MSG_DONTWAIT);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // No more data - expected
                        break;
                    }
                    LOG_ERROR("WebSocket", "Recv error on fd=%d after dispatch: %s", fd, strerror(errno));
                    break;
                }
                if (n == 0) {
                    // Peer closed connection
                    LOG_DEBUG("WebSocket", "Connection closed on fd=%d during dispatch recv", fd);
                    break;
                }

                // Process the incoming WebSocket frame
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
