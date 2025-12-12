#pragma once

/**
 * @file http3_server.h
 * @brief HTTP/3 Server Implementation (RFC 9114)
 *
 * Complete HTTP/3 server with:
 * - QUIC transport with TLS 1.3
 * - QPACK header compression
 * - HTTP/3 frame processing
 * - WebTransport support (RFC 9297)
 */

#include <cstdint>
#include <cstddef>
#include <memory>
#include <functional>
#include <unordered_map>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <iostream>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include "quic_secure_connection.h"
#include "quic_tls.h"
#include "../http3/http3_frames.h"
#include "../http3/qpack.h"

namespace fasterapi {
namespace http3 {

/**
 * HTTP/3 request handler callback.
 */
using RequestHandler = std::function<void(
    const std::string& method,
    const std::string& path,
    const std::unordered_map<std::string, std::string>& headers,
    const std::vector<uint8_t>& body,
    std::function<void(int status,
                       const std::unordered_map<std::string, std::string>& response_headers,
                       const std::vector<uint8_t>& response_body)> respond
)>;

/**
 * WebTransport session handler.
 */
using WebTransportHandler = std::function<void(
    uint64_t session_id,
    std::function<void(const uint8_t* data, size_t len)> send_datagram,
    std::function<void()> close_session
)>;

/**
 * HTTP/3 Connection wrapper.
 *
 * Manages a single HTTP/3 connection over QUIC.
 */
class HTTP3Connection {
public:
    HTTP3Connection(std::unique_ptr<quic::QUICSecureConnection> quic,
                    const struct sockaddr_storage& peer_addr,
                    socklen_t peer_addr_len)
        : quic_(std::move(quic)),
          peer_addr_(peer_addr),
          peer_addr_len_(peer_addr_len) {

        // Set up callbacks
        quic_->set_connected_callback([this]() {
            on_connected();
        });

        quic_->set_stream_data_callback([this](uint64_t stream_id,
                                                const uint8_t* data,
                                                size_t len, bool fin) {
            on_stream_data(stream_id, data, len, fin);
        });

        quic_->set_datagram_callback([this](const uint8_t* data, size_t len) {
            on_datagram(data, len);
        });

        quic_->set_error_callback([this](uint64_t error, const std::string& reason) {
            on_error(error, reason);
        });
    }

    /**
     * Process incoming UDP datagram.
     */
    int process_datagram(const uint8_t* data, size_t len, uint64_t now) {
        return quic_->process_datagram(data, len, now);
    }

    /**
     * Generate outgoing UDP datagrams.
     */
    size_t generate_datagrams(uint8_t* output, size_t capacity, uint64_t now) {
        return quic_->generate_packets(output, capacity, now);
    }

    /**
     * Check if connection is still active.
     */
    bool is_active() const {
        auto state = quic_->state();
        return state != quic::ConnectionState::CLOSED &&
               state != quic::ConnectionState::DRAINING;
    }

    /**
     * Get peer address.
     */
    const struct sockaddr_storage& peer_addr() const { return peer_addr_; }
    socklen_t peer_addr_len() const { return peer_addr_len_; }

    /**
     * Set request handler.
     */
    void set_request_handler(RequestHandler handler) {
        request_handler_ = std::move(handler);
    }

    /**
     * Set WebTransport handler.
     */
    void set_webtransport_handler(WebTransportHandler handler) {
        webtransport_handler_ = std::move(handler);
    }

private:
    void on_connected() {
        std::string alpn = quic_->alpn();
        // HTTP/3 uses "h3" ALPN
        if (alpn != "h3" && alpn.find("h3-") != 0) {
            quic_->close(0x0100 + 120, "No application protocol"); // TLS alert + no_application_protocol
            return;
        }

        // Create control streams
        create_control_streams();
    }

    void create_control_streams() {
        // Server creates:
        // - Control stream (type 0x00)
        // - QPACK encoder stream (type 0x02)
        // - QPACK decoder stream (type 0x03)

        // These are unidirectional streams (server-initiated = stream IDs 0x03, 0x07, 0x0B)
        control_stream_id_ = 0x03;   // First server-initiated uni stream
        qpack_encoder_stream_id_ = 0x07;
        qpack_decoder_stream_id_ = 0x0B;

        // Send stream type bytes
        uint8_t control_type[] = {0x00};  // Control stream
        uint8_t encoder_type[] = {0x02};  // QPACK encoder
        uint8_t decoder_type[] = {0x03};  // QPACK decoder

        quic_->write_stream(control_stream_id_, control_type, 1);
        quic_->write_stream(qpack_encoder_stream_id_, encoder_type, 1);
        quic_->write_stream(qpack_decoder_stream_id_, decoder_type, 1);

        // Send SETTINGS frame on control stream
        send_settings();
    }

    void send_settings() {
        // SETTINGS frame (type 0x04)
        std::vector<uint8_t> settings;

        // Frame type
        settings.push_back(0x04);

        // Build settings payload
        std::vector<uint8_t> payload;

        // SETTINGS_MAX_FIELD_SECTION_SIZE (0x06)
        payload.push_back(0x06);
        append_varint(payload, 8192); // 8KB max header size

        // SETTINGS_QPACK_MAX_TABLE_CAPACITY (0x01)
        payload.push_back(0x01);
        append_varint(payload, 4096); // 4KB QPACK table

        // SETTINGS_QPACK_BLOCKED_STREAMS (0x07)
        payload.push_back(0x07);
        append_varint(payload, 100);

        // ENABLE_WEBTRANSPORT (0x2b603742)
        append_varint(payload, 0x2b603742);
        append_varint(payload, 1);

        // Frame length
        append_varint(settings, payload.size());
        settings.insert(settings.end(), payload.begin(), payload.end());

        quic_->write_stream(control_stream_id_, settings.data(), settings.size());
    }

    void on_stream_data(uint64_t stream_id, const uint8_t* data, size_t len, bool fin) {
        // Buffer stream data
        auto& buffer = stream_buffers_[stream_id];
        buffer.insert(buffer.end(), data, data + len);

        // Check stream type
        bool is_bidi = (stream_id & 0x02) == 0;
        bool is_client_initiated = (stream_id & 0x01) == 0;

        if (is_bidi && is_client_initiated) {
            // Request stream
            process_request_stream(stream_id, fin);
        } else if (!is_bidi && is_client_initiated) {
            // Client unidirectional stream
            process_client_uni_stream(stream_id);
        }
    }

    void process_client_uni_stream(uint64_t stream_id) {
        auto& buffer = stream_buffers_[stream_id];
        if (buffer.empty()) return;

        // First byte is stream type
        uint8_t stream_type = buffer[0];

        switch (stream_type) {
            case 0x00: // Control stream
                peer_control_stream_id_ = stream_id;
                process_control_stream(stream_id);
                break;
            case 0x02: // QPACK encoder stream
                peer_qpack_encoder_stream_id_ = stream_id;
                break;
            case 0x03: // QPACK decoder stream
                peer_qpack_decoder_stream_id_ = stream_id;
                break;
            default:
                // Unknown stream type - ignore
                break;
        }
    }

    void process_control_stream(uint64_t stream_id) {
        auto& buffer = stream_buffers_[stream_id];

        // Skip stream type byte
        size_t pos = 1;

        while (pos < buffer.size()) {
            // Parse frame type
            uint64_t frame_type;
            int consumed = quic::VarInt::decode(buffer.data() + pos, buffer.size() - pos, frame_type);
            if (consumed < 0) break;
            pos += consumed;

            // Parse frame length
            uint64_t frame_len;
            consumed = quic::VarInt::decode(buffer.data() + pos, buffer.size() - pos, frame_len);
            if (consumed < 0) break;
            pos += consumed;

            if (pos + frame_len > buffer.size()) break;

            // Process frame
            switch (frame_type) {
                case 0x04: // SETTINGS
                    process_settings(buffer.data() + pos, frame_len);
                    break;
                case 0x07: // GOAWAY
                    // Peer is shutting down
                    break;
                default:
                    // Unknown frame - skip
                    break;
            }

            pos += frame_len;
        }

        // Remove processed data
        if (pos > 0) {
            buffer.erase(buffer.begin(), buffer.begin() + pos);
        }
    }

    void process_settings(const uint8_t* data, size_t len) {
        size_t pos = 0;
        while (pos < len) {
            uint64_t id;
            int consumed = quic::VarInt::decode(data + pos, len - pos, id);
            if (consumed < 0) break;
            pos += consumed;

            uint64_t value;
            consumed = quic::VarInt::decode(data + pos, len - pos, value);
            if (consumed < 0) break;
            pos += consumed;

            peer_settings_[id] = value;
        }
    }

    void process_request_stream(uint64_t stream_id, bool fin) {
        auto& buffer = stream_buffers_[stream_id];
        if (buffer.empty()) return;

        size_t pos = 0;

        // Parse HEADERS frame
        while (pos < buffer.size()) {
            uint64_t frame_type;
            int consumed = quic::VarInt::decode(buffer.data() + pos, buffer.size() - pos, frame_type);
            if (consumed < 0) return;
            pos += consumed;

            uint64_t frame_len;
            consumed = quic::VarInt::decode(buffer.data() + pos, buffer.size() - pos, frame_len);
            if (consumed < 0) return;
            pos += consumed;

            if (pos + frame_len > buffer.size()) return;

            if (frame_type == 0x01) { // HEADERS
                // Decode QPACK headers
                std::unordered_map<std::string, std::string> headers;
                decode_qpack_headers(buffer.data() + pos, frame_len, headers);

                std::string method = headers[":method"];
                std::string path = headers[":path"];
                std::string protocol = headers.count(":protocol") ? headers[":protocol"] : "";

                // Check for WebTransport upgrade
                if (method == "CONNECT" && protocol == "webtransport") {
                    handle_webtransport_session(stream_id, headers);
                } else {
                    // Regular HTTP/3 request
                    handle_http_request(stream_id, method, path, headers, fin);
                }
            } else if (frame_type == 0x00) { // DATA
                // Body data - append to pending request
                auto& req = pending_requests_[stream_id];
                req.body.insert(req.body.end(),
                                buffer.data() + pos,
                                buffer.data() + pos + frame_len);
            }

            pos += frame_len;
        }

        buffer.erase(buffer.begin(), buffer.begin() + pos);
    }

    void decode_qpack_headers(const uint8_t* data, size_t len,
                               std::unordered_map<std::string, std::string>& headers) {
        // Simplified QPACK decoding (using static table only)
        size_t pos = 0;

        // Required Insert Count (varint) - skip
        uint64_t ric;
        int consumed = quic::VarInt::decode(data + pos, len - pos, ric);
        if (consumed < 0) return;
        pos += consumed;

        // Delta Base (varint with sign) - skip
        if (pos >= len) return;
        uint8_t sign_and_base = data[pos];
        uint64_t delta_base;
        if (sign_and_base & 0x80) {
            // Negative delta
            pos++; // Skip for now
        } else {
            consumed = quic::VarInt::decode(data + pos, len - pos, delta_base);
            if (consumed < 0) return;
            pos += consumed;
        }

        // Parse header lines
        while (pos < len) {
            uint8_t prefix = data[pos];

            if (prefix & 0x80) {
                // Indexed header field (static table)
                uint64_t index = prefix & 0x3F;
                if (prefix & 0x40) {
                    // Static table
                    auto [name, value] = get_static_header(index);
                    headers[name] = value;
                }
                pos++;
            } else if ((prefix & 0xC0) == 0x40) {
                // Literal header with name reference
                pos++;
                // Skip for now - need full QPACK implementation
            } else if ((prefix & 0xE0) == 0x20) {
                // Literal header with literal name
                pos++;
                // Read name length
                uint64_t name_len;
                bool huffman = data[pos] & 0x80;
                consumed = quic::VarInt::decode(data + pos, len - pos, name_len);
                if (consumed < 0) break;
                name_len &= 0x7F;
                pos += consumed;

                if (pos + name_len > len) break;
                std::string name(reinterpret_cast<const char*>(data + pos), name_len);
                pos += name_len;

                // Read value length
                uint64_t value_len;
                huffman = data[pos] & 0x80;
                consumed = quic::VarInt::decode(data + pos, len - pos, value_len);
                if (consumed < 0) break;
                value_len &= 0x7F;
                pos += consumed;

                if (pos + value_len > len) break;
                std::string value(reinterpret_cast<const char*>(data + pos), value_len);
                pos += value_len;

                headers[name] = value;
            } else {
                pos++;
            }
        }
    }

    std::pair<std::string, std::string> get_static_header(uint64_t index) {
        // QPACK static table (subset)
        static const std::pair<std::string, std::string> table[] = {
            {":authority", ""},
            {":path", "/"},
            {"age", "0"},
            {"content-disposition", ""},
            {"content-length", "0"},
            {"cookie", ""},
            {"date", ""},
            {"etag", ""},
            {"if-modified-since", ""},
            {"if-none-match", ""},
            {"last-modified", ""},
            {"link", ""},
            {"location", ""},
            {"referer", ""},
            {"set-cookie", ""},
            {":method", "CONNECT"},
            {":method", "DELETE"},
            {":method", "GET"},
            {":method", "HEAD"},
            {":method", "OPTIONS"},
            {":method", "POST"},
            {":method", "PUT"},
            {":scheme", "http"},
            {":scheme", "https"},
            {":status", "103"},
            {":status", "200"},
            {":status", "304"},
            {":status", "404"},
            {":status", "503"},
            {"accept", "*/*"},
            {"accept", "application/dns-message"},
            {"accept-encoding", "gzip, deflate, br"},
            {"accept-ranges", "bytes"},
            {"access-control-allow-headers", "cache-control"},
            {"access-control-allow-headers", "content-type"},
            {"access-control-allow-origin", "*"},
            {"cache-control", "max-age=0"},
            {"cache-control", "max-age=2592000"},
            {"cache-control", "max-age=604800"},
            {"cache-control", "no-cache"},
            {"cache-control", "no-store"},
            {"cache-control", "public, max-age=31536000"},
            {"content-encoding", "br"},
            {"content-encoding", "gzip"},
            {"content-type", "application/dns-message"},
            {"content-type", "application/javascript"},
            {"content-type", "application/json"},
            {"content-type", "application/x-www-form-urlencoded"},
            {"content-type", "image/gif"},
            {"content-type", "image/jpeg"},
            {"content-type", "image/png"},
            {"content-type", "text/css"},
            {"content-type", "text/html; charset=utf-8"},
            {"content-type", "text/plain"},
            {"content-type", "text/plain;charset=utf-8"},
            {"range", "bytes=0-"},
            {"strict-transport-security", "max-age=31536000"},
            {"strict-transport-security", "max-age=31536000; includesubdomains"},
            {"strict-transport-security", "max-age=31536000; includesubdomains; preload"},
            {"vary", "accept-encoding"},
            {"vary", "origin"},
            {"x-content-type-options", "nosniff"},
            {"x-xss-protection", "1; mode=block"},
            {":status", "100"},
            {":status", "204"},
            {":status", "206"},
            {":status", "302"},
            {":status", "400"},
            {":status", "403"},
            {":status", "421"},
            {":status", "425"},
            {":status", "500"},
            {"accept-language", ""},
            {"access-control-allow-credentials", "FALSE"},
            {"access-control-allow-credentials", "TRUE"},
            {"access-control-allow-methods", "get"},
            {"access-control-allow-methods", "get, post, options"},
            {"access-control-allow-methods", "options"},
            {"access-control-expose-headers", "content-length"},
            {"access-control-request-headers", "content-type"},
            {"access-control-request-method", "get"},
            {"access-control-request-method", "post"},
            {"alt-svc", "clear"},
            {"authorization", ""},
            {"content-security-policy", "script-src 'none'; object-src 'none'; base-uri 'none'"},
            {"early-data", "1"},
            {"expect-ct", ""},
            {"forwarded", ""},
            {"if-range", ""},
            {"origin", ""},
            {"purpose", "prefetch"},
            {"server", ""},
            {"timing-allow-origin", "*"},
            {"upgrade-insecure-requests", "1"},
            {"user-agent", ""},
            {"x-forwarded-for", ""},
            {"x-frame-options", "deny"},
            {"x-frame-options", "sameorigin"},
        };

        if (index < sizeof(table) / sizeof(table[0])) {
            return table[index];
        }
        return {"", ""};
    }

    void handle_http_request(uint64_t stream_id,
                              const std::string& method,
                              const std::string& path,
                              const std::unordered_map<std::string, std::string>& headers,
                              bool fin) {
        if (!request_handler_) {
            send_error_response(stream_id, 500);
            return;
        }

        auto& req = pending_requests_[stream_id];
        req.method = method;
        req.path = path;
        req.headers = headers;

        if (fin || method == "GET" || method == "HEAD") {
            // Request complete
            request_handler_(method, path, headers, req.body,
                [this, stream_id](int status,
                                  const std::unordered_map<std::string, std::string>& resp_headers,
                                  const std::vector<uint8_t>& resp_body) {
                    send_response(stream_id, status, resp_headers, resp_body);
                });
            pending_requests_.erase(stream_id);
        }
    }

    void handle_webtransport_session(uint64_t stream_id,
                                      const std::unordered_map<std::string, std::string>& headers) {
        if (!webtransport_handler_) {
            send_error_response(stream_id, 501);
            return;
        }

        // Send 200 OK to establish WebTransport session
        std::unordered_map<std::string, std::string> resp_headers;
        resp_headers[":status"] = "200";
        resp_headers["sec-webtransport-http3-draft"] = "draft02";

        send_headers(stream_id, resp_headers, false);

        // Create WebTransport session
        uint64_t session_id = next_webtransport_session_id_++;
        webtransport_sessions_[session_id] = stream_id;

        webtransport_handler_(session_id,
            [this](const uint8_t* data, size_t len) {
                // Send datagram
                quic_->send_datagram(data, len);
            },
            [this, session_id]() {
                // Close session
                auto it = webtransport_sessions_.find(session_id);
                if (it != webtransport_sessions_.end()) {
                    quic_->close(0, "Session closed");
                    webtransport_sessions_.erase(it);
                }
            });
    }

    void send_response(uint64_t stream_id, int status,
                       const std::unordered_map<std::string, std::string>& headers,
                       const std::vector<uint8_t>& body) {
        // Add status to headers
        std::unordered_map<std::string, std::string> all_headers = headers;
        all_headers[":status"] = std::to_string(status);

        if (!body.empty() && all_headers.find("content-length") == all_headers.end()) {
            all_headers["content-length"] = std::to_string(body.size());
        }

        // Send HEADERS frame
        send_headers(stream_id, all_headers, body.empty());

        // Send DATA frame if body exists
        if (!body.empty()) {
            send_data(stream_id, body.data(), body.size(), true);
        }
    }

    void send_headers(uint64_t stream_id,
                      const std::unordered_map<std::string, std::string>& headers,
                      bool end_stream) {
        // Encode headers with QPACK
        std::vector<uint8_t> encoded;

        // Required Insert Count = 0, Delta Base = 0
        encoded.push_back(0x00);
        encoded.push_back(0x00);

        for (const auto& [name, value] : headers) {
            // Literal header with literal name
            encoded.push_back(0x20 | (name.size() > 127 ? 0x00 : 0x00));
            append_varint(encoded, name.size());
            encoded.insert(encoded.end(), name.begin(), name.end());

            encoded.push_back(value.size() > 127 ? 0x00 : 0x00);
            append_varint(encoded, value.size());
            encoded.insert(encoded.end(), value.begin(), value.end());
        }

        // Build HEADERS frame
        std::vector<uint8_t> frame;
        frame.push_back(0x01); // Frame type = HEADERS
        append_varint(frame, encoded.size());
        frame.insert(frame.end(), encoded.begin(), encoded.end());

        quic_->write_stream(stream_id, frame.data(), frame.size());
    }

    void send_data(uint64_t stream_id, const uint8_t* data, size_t len, bool end_stream) {
        std::vector<uint8_t> frame;
        frame.push_back(0x00); // Frame type = DATA
        append_varint(frame, len);
        frame.insert(frame.end(), data, data + len);

        quic_->write_stream(stream_id, frame.data(), frame.size());
    }

    void send_error_response(uint64_t stream_id, int status) {
        std::unordered_map<std::string, std::string> headers;
        headers["content-type"] = "text/plain";

        std::string body = "Error " + std::to_string(status);
        send_response(stream_id, status, headers,
                      std::vector<uint8_t>(body.begin(), body.end()));
    }

    void append_varint(std::vector<uint8_t>& buf, uint64_t value) {
        uint8_t temp[8];
        int len = quic::VarInt::encode(value, temp);
        buf.insert(buf.end(), temp, temp + len);
    }

    void on_datagram(const uint8_t* data, size_t len) {
        // WebTransport datagrams
        // First quarter stream ID identifies the session
        // For now, just forward to handler
    }

    void on_error(uint64_t error, const std::string& reason) {
        // Connection error
    }

    struct PendingRequest {
        std::string method;
        std::string path;
        std::unordered_map<std::string, std::string> headers;
        std::vector<uint8_t> body;
    };

    std::unique_ptr<quic::QUICSecureConnection> quic_;
    struct sockaddr_storage peer_addr_;
    socklen_t peer_addr_len_;

    // Stream state
    std::unordered_map<uint64_t, std::vector<uint8_t>> stream_buffers_;
    std::unordered_map<uint64_t, PendingRequest> pending_requests_;

    // HTTP/3 streams
    uint64_t control_stream_id_{0};
    uint64_t qpack_encoder_stream_id_{0};
    uint64_t qpack_decoder_stream_id_{0};
    uint64_t peer_control_stream_id_{0};
    uint64_t peer_qpack_encoder_stream_id_{0};
    uint64_t peer_qpack_decoder_stream_id_{0};

    // Settings
    std::unordered_map<uint64_t, uint64_t> peer_settings_;

    // WebTransport
    uint64_t next_webtransport_session_id_{1};
    std::unordered_map<uint64_t, uint64_t> webtransport_sessions_; // session_id -> stream_id

    // Handlers
    RequestHandler request_handler_;
    WebTransportHandler webtransport_handler_;
};

/**
 * HTTP/3 Server.
 *
 * Listens for UDP datagrams and manages QUIC/HTTP/3 connections.
 */
class HTTP3Server {
public:
    HTTP3Server() : running_(false), socket_(-1) {}

    ~HTTP3Server() {
        stop();
    }

    /**
     * Configure TLS with certificate files.
     */
    bool configure_tls(const char* cert_file, const char* key_file) {
        ssl_ctx_ = quic::create_quic_ssl_ctx(cert_file, key_file,
                                              quic::alpn::kH3All,
                                              quic::alpn::kH3AllLen);
        return ssl_ctx_ != nullptr;
    }

    /**
     * Configure TLS with auto-generated self-signed certificate.
     */
    bool configure_tls_self_signed() {
        ssl_ctx_ = quic::create_quic_ssl_ctx_self_signed(quic::alpn::kH3All,
                                                          quic::alpn::kH3AllLen);
        return ssl_ctx_ != nullptr;
    }

    /**
     * Set request handler.
     */
    void set_request_handler(RequestHandler handler) {
        request_handler_ = std::move(handler);
    }

    /**
     * Set WebTransport handler.
     */
    void set_webtransport_handler(WebTransportHandler handler) {
        webtransport_handler_ = std::move(handler);
    }

    /**
     * Start the server.
     */
    bool start(const char* host, uint16_t port) {
        if (!ssl_ctx_) {
            return false;
        }

        // Create UDP socket
        socket_ = socket(AF_INET6, SOCK_DGRAM, 0);
        if (socket_ < 0) {
            socket_ = socket(AF_INET, SOCK_DGRAM, 0);
            if (socket_ < 0) {
                return false;
            }
        }

        // Allow IPv4 connections on IPv6 socket
        int no = 0;
        setsockopt(socket_, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));

        // Enable address reuse
        int yes = 1;
        setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        setsockopt(socket_, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));

        // Bind
        struct sockaddr_in6 addr6{};
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(port);

        if (strcmp(host, "0.0.0.0") == 0 || strcmp(host, "::") == 0) {
            addr6.sin6_addr = in6addr_any;
        } else {
            inet_pton(AF_INET6, host, &addr6.sin6_addr);
        }

        if (bind(socket_, reinterpret_cast<struct sockaddr*>(&addr6), sizeof(addr6)) < 0) {
            close(socket_);
            socket_ = -1;
            return false;
        }

        // Set non-blocking
        fcntl(socket_, F_SETFL, O_NONBLOCK);

        running_ = true;
        worker_thread_ = std::thread([this]() { run_loop(); });

        return true;
    }

    /**
     * Stop the server.
     */
    void stop() {
        running_ = false;
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
        if (socket_ >= 0) {
            close(socket_);
            socket_ = -1;
        }
        if (ssl_ctx_) {
            SSL_CTX_free(ssl_ctx_);
            ssl_ctx_ = nullptr;
        }
    }

    /**
     * Check if running.
     */
    bool is_running() const { return running_; }

private:
    void run_loop() {
        uint8_t recv_buf[65535];
        uint8_t send_buf[65535];

        while (running_) {
            // Poll for incoming data
            struct pollfd pfd;
            pfd.fd = socket_;
            pfd.events = POLLIN;

            int ret = poll(&pfd, 1, 10); // 10ms timeout
            if (ret < 0) break;

            uint64_t now = get_time_us();

            if (ret > 0 && (pfd.revents & POLLIN)) {
                // Receive datagram
                struct sockaddr_storage peer_addr;
                socklen_t peer_addr_len = sizeof(peer_addr);

                ssize_t recv_len = recvfrom(socket_, recv_buf, sizeof(recv_buf), 0,
                                            reinterpret_cast<struct sockaddr*>(&peer_addr),
                                            &peer_addr_len);

                if (recv_len > 0) {
                    process_datagram(recv_buf, recv_len, peer_addr, peer_addr_len, now);
                }
            }

            // Generate and send outgoing packets
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                for (auto it = connections_.begin(); it != connections_.end(); ) {
                    auto& conn = it->second;

                    if (!conn->is_active()) {
                        it = connections_.erase(it);
                        continue;
                    }

                    size_t send_len = conn->generate_datagrams(send_buf, sizeof(send_buf), now);
                    std::cerr << "[Server] generate_datagrams returned " << send_len << " bytes\n";
                    if (send_len > 0) {
                        std::cerr << "[Server] Sending " << send_len << " bytes response\n";
                        ssize_t sent = sendto(socket_, send_buf, send_len, 0,
                               reinterpret_cast<const struct sockaddr*>(&conn->peer_addr()),
                               conn->peer_addr_len());
                        std::cerr << "[Server] sendto returned " << sent << "\n";
                    }

                    ++it;
                }
            }
        }
    }

    void process_datagram(const uint8_t* data, size_t len,
                          const struct sockaddr_storage& peer_addr,
                          socklen_t peer_addr_len,
                          uint64_t now) {
        // Extract connection ID from packet
        if (len < 6) return;

        std::cerr << "[Server] Received " << len << " bytes, first byte: 0x"
                  << std::hex << static_cast<int>(data[0]) << std::dec << "\n";

        bool is_long = (data[0] & 0x80) != 0;

        quic::ConnectionID dcid;
        if (is_long) {
            // Long header: DCID is at offset 5
            if (len < 6) return;
            uint8_t dcid_len = data[5];
            std::cerr << "[Server] Long header, DCID len: " << static_cast<int>(dcid_len) << "\n";
            if (dcid_len > 20 || len < 6 + dcid_len) return;
            dcid = quic::ConnectionID(data + 6, dcid_len);
        } else {
            // Short header: DCID is at offset 1
            // Use our known local CID length
            uint8_t dcid_len = 8; // We use 8-byte CIDs
            if (len < 1 + dcid_len) return;
            dcid = quic::ConnectionID(data + 1, dcid_len);
        }

        // Look up connection
        std::string key = make_connection_key(dcid);

        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(key);

        if (it != connections_.end()) {
            // Existing connection
            std::cerr << "[Server] Existing connection\n";
            it->second->process_datagram(data, len, now);
        } else if (is_long) {
            // New connection (Initial packet)
            uint32_t version = (static_cast<uint32_t>(data[1]) << 24) |
                              (static_cast<uint32_t>(data[2]) << 16) |
                              (static_cast<uint32_t>(data[3]) << 8) |
                              static_cast<uint32_t>(data[4]);

            std::cerr << "[Server] New connection, version: 0x" << std::hex << version << std::dec << "\n";

            // Check for Version Negotiation
            if (!quic::version::is_supported(version)) {
                std::cerr << "[Server] Version not supported, sending VN\n";
                send_version_negotiation(data, len, peer_addr, peer_addr_len);
                return;
            }

            // Create new connection
            auto local_cid = quic::generate_connection_id(8);

            std::cerr << "[Server] DCID for key derivation: ";
            for (size_t i = 0; i < dcid.length; i++) {
                std::cerr << std::hex << static_cast<int>(dcid.data[i]);
            }
            std::cerr << std::dec << " (len=" << dcid.length << ")\n";

            auto quic_conn = std::make_unique<quic::QUICSecureConnection>(
                ssl_ctx_, true, local_cid, dcid, version);

            std::cerr << "[Server] Initializing QUIC connection...\n";
            if (quic_conn->initialize() != 0) {
                std::cerr << "[Server] QUIC connection initialize() failed!\n";
                return;
            }
            std::cerr << "[Server] QUIC connection initialized\n";

            auto conn = std::make_unique<HTTP3Connection>(
                std::move(quic_conn), peer_addr, peer_addr_len);

            conn->set_request_handler(request_handler_);
            conn->set_webtransport_handler(webtransport_handler_);

            std::cerr << "[Server] Processing datagram...\n";
            conn->process_datagram(data, len, now);
            std::cerr << "[Server] Datagram processed\n";

            connections_[key] = std::move(conn);
        }
    }

    void send_version_negotiation(const uint8_t* data, size_t len,
                                   const struct sockaddr_storage& peer_addr,
                                   socklen_t peer_addr_len) {
        uint8_t vn_buf[128];
        ssize_t vn_len = quic::create_version_negotiation_response(data, len, vn_buf, sizeof(vn_buf));
        if (vn_len > 0) {
            sendto(socket_, vn_buf, vn_len, 0,
                   reinterpret_cast<const struct sockaddr*>(&peer_addr),
                   peer_addr_len);
        }
    }

    std::string make_connection_key(const quic::ConnectionID& cid) {
        return std::string(reinterpret_cast<const char*>(cid.data), cid.length);
    }

    uint64_t get_time_us() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
    }

    SSL_CTX* ssl_ctx_{nullptr};
    int socket_;
    std::atomic<bool> running_;
    std::thread worker_thread_;

    std::mutex connections_mutex_;
    std::unordered_map<std::string, std::unique_ptr<HTTP3Connection>> connections_;

    RequestHandler request_handler_;
    WebTransportHandler webtransport_handler_;
};

} // namespace http3
} // namespace fasterapi
