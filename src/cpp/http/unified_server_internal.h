/**
 * Internal declarations for unified server handlers
 * 
 * This header is shared between the split implementation files.
 * Not intended for external use.
 */

#pragma once

#include "unified_server.h"
#include "http1_connection.h"
#include "http2_connection.h"
#include "http3_connection.h"
#include "websocket.h"
#include "webtransport_connection.h"
#include "../net/tls_socket.h"
#include "../net/event_loop.h"
#include "../core/string_hash.h"
#include <unordered_map>
#include <memory>
#include <mutex>
#include <vector>

namespace fasterapi {

// Forward declaration
class App;

namespace http {

// ============================================================================
// Thread-local connection storage (shared across handler files)
// ============================================================================

// TLS sockets
extern thread_local std::unordered_map<int, std::unique_ptr<net::TlsSocket>> t_tls_sockets;

// HTTP/2 connections
extern thread_local std::unordered_map<int, std::unique_ptr<http2::Http2Connection>> t_http2_connections;

// HTTP/1.1 connections
extern thread_local std::unordered_map<int, std::unique_ptr<Http1Connection>> t_http1_connections;

// HTTP/3 connections (keyed by connection ID string)
extern thread_local std::unordered_map<std::string, std::unique_ptr<Http3Connection>,
    core::StringHash, std::equal_to<>> t_http3_connections;

// WebTransport connections (keyed by connection ID string)
extern thread_local std::unordered_map<std::string, std::unique_ptr<WebTransportConnection>,
    core::StringHash, std::equal_to<>> t_webtransport_connections;

// WebSocket connections (keyed by fd)
extern thread_local std::unordered_map<int, std::unique_ptr<WebSocketConnection>> t_websocket_connections;

// Reverse lookup: connection_id -> fd (for WebSocket dispatch)
extern thread_local std::unordered_map<uint64_t, int> t_ws_conn_id_to_fd;

// ============================================================================
// Wake pipe for cross-thread WebSocket signaling
// ============================================================================

extern thread_local int t_wake_pipe_read_fd;
extern thread_local int t_wake_pipe_write_fd;
extern thread_local bool t_wake_pipe_registered;

// Global registry of wake pipe write fds
extern std::mutex s_wake_pipes_mutex;
extern std::vector<int> s_wake_pipe_write_fds;

// ============================================================================
// Global state
// ============================================================================

// Direct App pointer for simplified Http1 handling
extern fasterapi::App* s_app_instance_;

// Ultra-fast callback for maximum performance
extern Http1Connection::UltraFastCallback s_ultra_fast_callback_;

// Global request handler
extern HttpRequestHandler s_request_handler_;

// ============================================================================
// Helper functions
// ============================================================================

// Initialize wake pipe for current thread
bool init_wake_pipe();

// Signal all worker threads to dispatch WebSocket responses
void signal_ws_response_ready();

// Register wake pipe with event loop
void register_wake_pipe_with_event_loop(net::EventLoop* event_loop);

// Helper to clean up WebSocket connection and reverse lookup
void cleanup_websocket_connection(int fd);

// Helper: Convert ConnectionID to hex string for map key
std::string connection_id_to_string(const quic::ConnectionID& conn_id) noexcept;

// Helper: Get current time in microseconds
uint64_t get_time_us() noexcept;

// Connection tracking for graceful shutdown
bool do_track_connection_open();
void do_track_connection_close();

// ============================================================================
// Handler functions (implementations in separate files)
// ============================================================================

// WebSocket connection handler
void handle_websocket_connection(
    int fd,
    net::IOEvent events,
    net::EventLoop* event_loop,
    WebSocketConnection* ws_conn
);

// Dispatch pending WebSocket responses from Python workers
void dispatch_pending_ws_responses();

} // namespace http
} // namespace fasterapi
