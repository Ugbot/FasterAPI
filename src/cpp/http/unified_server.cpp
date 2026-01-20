/**
 * Unified HTTP Server Implementation - Core Server Logic
 *
 * Integrates TLS/ALPN with HTTP/2 and HTTP/1.1.
 * 
 * Split into multiple files for maintainability:
 * - unified_server.cpp: Core server logic (this file)
 * - connection_handlers.cpp: HTTP/1, HTTP/2, TLS handlers
 * - websocket_handlers.cpp: WebSocket handling
 * - quic_handlers.cpp: QUIC/HTTP3 handling
 */

#include "unified_server.h"
#include "unified_server_internal.h"
#include "app.h"
#include "core/logger.h"
#include "net/tls_cert_generator.h"
#include "net/udp_socket.h"
#include <unordered_map>
#include <fcntl.h>
#include <memory>
#include <chrono>
#include <atomic>
#include <unistd.h>
#include <sys/socket.h>
#include <cstring>

namespace fasterapi {
namespace http {

// ============================================================================
// Global State Definitions
// ============================================================================

// Global request handler (shared across connections)
HttpRequestHandler UnifiedServer::s_request_handler_;

// Pure C++ WebSocket handlers (path → handler mapping)
std::unordered_map<std::string, WebSocketHandler> UnifiedServer::s_websocket_handlers_;

// Pure C++ WebTransport handlers (path → handler mapping)
std::unordered_map<std::string, WebTransportHandler> UnifiedServer::s_webtransport_handlers_;

// Global server instance for signal handlers
UnifiedServer* UnifiedServer::s_instance_ = nullptr;

// Direct App pointer for simplified Http1 handling
::fasterapi::App* s_app_instance_ = nullptr;

// Ultra-fast callback for maximum performance (zero allocation)
Http1Connection::UltraFastCallback s_ultra_fast_callback_ = nullptr;

// NOTE: Removed duplicate namespace-level s_request_handler_ - use UnifiedServer::s_request_handler_ instead

// ============================================================================
// Thread-Local Storage Definitions
// ============================================================================

// Per-thread connection storage
thread_local std::unordered_map<int, std::unique_ptr<net::TlsSocket>> t_tls_sockets;
thread_local std::unordered_map<int, std::unique_ptr<http2::Http2Connection>> t_http2_connections;
thread_local std::unordered_map<int, std::unique_ptr<Http1Connection>> t_http1_connections;

// HTTP/3 and WebTransport connection storage (keyed by connection ID string)
thread_local std::unordered_map<std::string, std::unique_ptr<Http3Connection>, 
    core::StringHash, std::equal_to<>> t_http3_connections;
thread_local std::unordered_map<std::string, std::unique_ptr<WebTransportConnection>,
    core::StringHash, std::equal_to<>> t_webtransport_connections;

// WebSocket connection storage (keyed by fd)
thread_local std::unordered_map<int, std::unique_ptr<WebSocketConnection>> t_websocket_connections;

// Reverse lookup: connection_id -> fd (for dispatch from queue)
thread_local std::unordered_map<uint64_t, int> t_ws_conn_id_to_fd;

// Wake pipe for cross-thread signaling (response dispatch)
thread_local int t_wake_pipe_read_fd = -1;
thread_local int t_wake_pipe_write_fd = -1;
thread_local bool t_wake_pipe_registered = false;

// Global registry of wake pipe write fds
std::mutex s_wake_pipes_mutex;
std::vector<int> s_wake_pipe_write_fds;

// ============================================================================
// Connection Tracking Helpers
// ============================================================================

bool do_track_connection_open() {
    auto* server = UnifiedServer::get_instance();
    if (!server) return true;
    bool accepted = server->track_connection_open();
    if (!accepted) {
        LOG_DEBUG("Shutdown", "Rejecting new connection - server is draining");
    } else {
        LOG_DEBUG("Shutdown", "Connection opened, active=%u", server->get_active_connections());
    }
    return accepted;
}

void do_track_connection_close() {
    auto* server = UnifiedServer::get_instance();
    if (!server) return;
    uint32_t before = server->get_active_connections();
    server->track_connection_close();
    LOG_DEBUG("Shutdown", "Connection closed, active=%u -> %u", before, server->get_active_connections());
}

// ============================================================================
// UnifiedServer Implementation
// ============================================================================

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

void UnifiedServer::add_webtransport_handler(const std::string& path, WebTransportHandler handler) {
    s_webtransport_handlers_[path] = std::move(handler);
    LOG_INFO("WebTransport", "Registered handler for path: %s", path.c_str());
}

WebTransportHandler* UnifiedServer::get_webtransport_handler(const std::string& path) {
    auto it = s_webtransport_handlers_.find(path);
    if (it != s_webtransport_handlers_.end()) {
        return &it->second;
    }
    return nullptr;
}

void UnifiedServer::set_app_instance(void* app) {
    s_app_instance_ = static_cast<::fasterapi::App*>(app);
}

void UnifiedServer::set_ultra_fast_callback(Http1Connection::UltraFastCallback callback) {
    s_ultra_fast_callback_ = callback;
}

// ============================================================================
// Signal Handling for Graceful Shutdown
// ============================================================================

void UnifiedServer::signal_handler(int signum) {
    LOG_INFO("Server", "Received signal %d (%s), initiating graceful shutdown...",
             signum, signum == SIGTERM ? "SIGTERM" : signum == SIGINT ? "SIGINT" : "unknown");

    if (s_instance_) {
        ShutdownState expected = ShutdownState::RUNNING;
        if (s_instance_->shutdown_state_.compare_exchange_strong(
                expected, ShutdownState::DRAINING, std::memory_order_acq_rel)) {
            s_instance_->shutdown_cv_.notify_all();
        }
    }
}

void UnifiedServer::install_signal_handlers() {
    if (!config_.enable_signal_handlers) {
        LOG_DEBUG("Server", "Signal handlers disabled by configuration");
        return;
    }

    s_instance_ = this;

    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGTERM, &sa, nullptr) < 0) {
        LOG_WARN("Server", "Failed to install SIGTERM handler: %s", strerror(errno));
    } else {
        LOG_DEBUG("Server", "Installed SIGTERM handler");
    }

    if (sigaction(SIGINT, &sa, nullptr) < 0) {
        LOG_WARN("Server", "Failed to install SIGINT handler: %s", strerror(errno));
    } else {
        LOG_DEBUG("Server", "Installed SIGINT handler");
    }
}

// ============================================================================
// Graceful Shutdown
// ============================================================================

bool UnifiedServer::shutdown_gracefully() {
    LOG_INFO("Server", "Initiating graceful shutdown...");

    ShutdownState expected = ShutdownState::RUNNING;
    if (!shutdown_state_.compare_exchange_strong(
            expected, ShutdownState::DRAINING, std::memory_order_acq_rel)) {
        if (expected == ShutdownState::STOPPED) {
            LOG_DEBUG("Server", "Server already stopped");
            return true;
        }
        LOG_DEBUG("Server", "Server already draining");
    }

    shutdown_flag_.store(true, std::memory_order_relaxed);

    // Stop accepting new connections
    if (tls_listener_) {
        tls_listener_->stop();
        LOG_DEBUG("Server", "Stopped TLS listener");
    }
    if (cleartext_listener_) {
        cleartext_listener_->stop();
        LOG_DEBUG("Server", "Stopped cleartext listener");
    }
    if (quic_listener_) {
        quic_listener_->stop();
        LOG_DEBUG("Server", "Stopped QUIC listener");
    }

    // Wait for active connections to drain with timeout
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(config_.shutdown_timeout_ms);

    uint32_t active = active_connections_.load(std::memory_order_relaxed);
    LOG_INFO("Server", "Waiting for %u active connections to drain (timeout: %ums)...",
             active, config_.shutdown_timeout_ms);

    std::unique_lock<std::mutex> lock(shutdown_mutex_);
    bool drained = shutdown_cv_.wait_until(lock, deadline, [this]() {
        return active_connections_.load(std::memory_order_relaxed) == 0;
    });

    if (drained) {
        LOG_INFO("Server", "All connections drained successfully");
    } else {
        active = active_connections_.load(std::memory_order_relaxed);
        LOG_WARN("Server", "Shutdown timeout reached with %u connections still active, forcing close",
                 active);
    }

    shutdown_state_.store(ShutdownState::STOPPED, std::memory_order_release);

    // Join background threads
    if (tls_thread_.joinable()) {
        tls_thread_.join();
    }
    if (quic_thread_.joinable()) {
        quic_thread_.join();
    }

    LOG_INFO("Server", "Graceful shutdown complete");
    return drained;
}

// ============================================================================
// Server Start
// ============================================================================

int UnifiedServer::start() {
    shutdown_state_.store(ShutdownState::RUNNING, std::memory_order_release);
    shutdown_flag_.store(false, std::memory_order_relaxed);

    install_signal_handlers();

    // Create TLS context if enabled
    if (config_.enable_tls) {
        net::TlsContextConfig tls_config;
        tls_config.alpn_protocols = config_.alpn_protocols;

        // Generate self-signed certificate (always - simple and works)
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

        tls_config.cert_data = generated.cert_pem;
        tls_config.key_data = generated.key_pem;

        tls_context_ = net::TlsContext::create_server(tls_config);
        if (!tls_context_ || !tls_context_->is_valid()) {
            error_message_ = "Failed to create TLS context: " +
                (tls_context_ ? tls_context_->get_error() : "null context");
            LOG_ERROR("Server", "%s", error_message_.c_str());
            return -1;
        }

        LOG_INFO("Server", "TLS enabled with self-signed certificate, ALPN: %zu protocols",
                 config_.alpn_protocols.size());

        // Create TLS listener
        net::TcpListenerConfig tls_listener_config;
        tls_listener_config.port = config_.tls_port;
        tls_listener_config.host = config_.host;
        tls_listener_config.num_workers = config_.num_workers;
        tls_listener_config.use_reuseport = config_.use_reuseport;

        auto tls_ctx = tls_context_;
        tls_listener_ = std::make_unique<net::TcpListener>(
            tls_listener_config,
            [tls_ctx](net::TcpSocket socket, net::EventLoop* loop) {
                handle_tls_connection(std::move(socket), loop, tls_ctx);
            }
        );

        LOG_INFO("Server", "TLS listener on %s:%d", config_.host.c_str(), config_.tls_port);
    }

    // Create cleartext listener if enabled
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

    // Create HTTP/3 listener if enabled
    if (config_.enable_http3) {
        LOG_INFO("Server", "Starting HTTP/3 (QUIC) on UDP port %d...", config_.http3_port);

        net::UdpListenerConfig quic_config;
        quic_config.host = config_.host;
        quic_config.port = config_.http3_port;
        quic_config.num_workers = config_.num_workers;
        quic_config.max_datagram_size = 65535;
        quic_config.recv_buffer_size = 2 * 1024 * 1024;
        quic_config.enable_pktinfo = true;
        quic_config.enable_tos = true;

        quic_listener_ = std::make_unique<net::UdpListener>(
            quic_config,
            on_quic_datagram
        );

        LOG_INFO("Server", "HTTP/3 listener on %s:%d (UDP)", config_.host.c_str(), config_.http3_port);
    }

    // Start TLS listener in background thread if both TCP listeners enabled
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

// ============================================================================
// Server Stop
// ============================================================================

void UnifiedServer::stop() {
    shutdown_gracefully();

    if (s_instance_ == this) {
        s_instance_ = nullptr;
    }
}

} // namespace http
} // namespace fasterapi
