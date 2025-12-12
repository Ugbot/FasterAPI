/**
 * QUIC/HTTP3 Connection Handlers
 * 
 * Split from unified_server.cpp for maintainability.
 */

#include "unified_server_internal.h"
#include "unified_server.h"
#include "core/logger.h"
#include "quic/quic_packet.h"
#include <cstring>
#include <chrono>
#include <sstream>
#include <iomanip>

namespace fasterapi {
namespace http {

// ============================================================================
// Helper Functions
// ============================================================================

std::string connection_id_to_string(const quic::ConnectionID& conn_id) noexcept {
    std::ostringstream oss;
    for (uint8_t i = 0; i < conn_id.length; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(conn_id.data[i]);
    }
    return oss.str();
}

uint64_t get_time_us() noexcept {
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

// ============================================================================
// QUIC Datagram Handler
// ============================================================================

void UnifiedServer::on_quic_datagram(
    const uint8_t* data,
    size_t length,
    const struct sockaddr* addr,
    socklen_t addrlen,
    net::EventLoop* event_loop,
    int socket_fd
) {
    if (length < 5) {
        // Too short to be valid QUIC packet
        return;
    }

    uint64_t now_us = get_time_us();

    // Parse connection ID from QUIC packet
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
        // For short header, assume 8-byte connection ID
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

        // Set request callback
        if (s_request_handler_) {
            new_conn->set_request_callback(s_request_handler_);
        }

        // Set WebTransport upgrade callback
        std::string wt_conn_id = conn_id_str;
        new_conn->set_webtransport_upgrade_callback(
            [wt_conn_id](
                uint64_t stream_id,
                const std::string& path,
                const std::unordered_map<std::string, std::string>& headers,
                std::function<void()> accept,
                std::function<void(uint16_t, const char*)> reject
            ) {
                // Look up WebTransport handler for this path
                WebTransportHandler* handler = get_webtransport_handler(path);
                if (!handler) {
                    LOG_WARN("WebTransport", "No handler for path: %s", path.c_str());
                    reject(404, "WebTransport endpoint not found");
                    return;
                }

                LOG_INFO("WebTransport", "Upgrading connection %s to WebTransport at %s",
                         wt_conn_id.c_str(), path.c_str());

                // Accept the connection
                accept();

                // Get the HTTP/3 connection to extract its QUIC connection
                auto http3_it = t_http3_connections.find(wt_conn_id);
                if (http3_it == t_http3_connections.end()) {
                    LOG_ERROR("WebTransport", "HTTP/3 connection not found: %s", wt_conn_id.c_str());
                    return;
                }

                // Create WebTransportConnection using the shared QUIC connection
                quic::QUICConnection* quic_ptr = http3_it->second->quic_connection();
                auto wt_conn = std::make_unique<WebTransportConnection>(quic_ptr, true);

                // Store the WebTransport connection
                t_webtransport_connections[wt_conn_id] = std::move(wt_conn);

                // Invoke the handler
                WebTransportConnection& wt = *t_webtransport_connections[wt_conn_id];
                wt.initialize();
                wt.accept();
                (*handler)(wt);

                LOG_INFO("WebTransport", "Session established for %s", wt_conn_id.c_str());
            }
        );

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

        if (http3_conn->is_closed()) {
            LOG_INFO("HTTP3", "Connection %s closed, removing", conn_id_str.c_str());
            t_http3_connections.erase(conn_id_str);
        }
        return;
    }

    // Generate outgoing datagrams
    uint8_t output_buffer[65535];
    size_t output_len = http3_conn->generate_datagrams(output_buffer, sizeof(output_buffer), now_us);

    if (output_len > 0) {
        ssize_t sent = ::sendto(socket_fd, output_buffer, output_len, 0, addr, addrlen);
        if (sent < 0) {
            LOG_ERROR("HTTP3", "Failed to send response datagram: %s", strerror(errno));
        } else {
            LOG_DEBUG("HTTP3", "Sent %zd bytes to connection %s", sent, conn_id_str.c_str());
        }
    }

    // Clean up closed connections
    if (http3_conn->is_closed()) {
        LOG_INFO("HTTP3", "Connection %s closed, removing", conn_id_str.c_str());
        t_http3_connections.erase(conn_id_str);
    }
}

} // namespace http
} // namespace fasterapi
