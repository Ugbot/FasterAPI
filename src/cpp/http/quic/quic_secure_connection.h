#pragma once

/**
 * @file quic_secure_connection.h
 * @brief QUIC Connection with TLS Integration (RFC 9001)
 *
 * Extends QUICConnection with:
 * - TLS 1.3 handshake via CRYPTO frames
 * - Packet protection per encryption level
 * - Packet number spaces
 * - Version negotiation and Retry handling
 */

#include <cstdint>
#include <cstddef>
#include <memory>
#include <functional>
#include <array>
#include <vector>
#include <iostream>

#include "quic_connection.h"
#include "quic_handshake.h"
#include "quic_packet_number_space.h"
#include "quic_version_retry.h"
#include "quic_frames.h"

namespace fasterapi {
namespace quic {

/**
 * Transport parameters (RFC 9000 Section 18).
 */
struct TransportParameters {
    // Required parameters
    ConnectionID original_destination_connection_id;
    uint64_t max_idle_timeout{30000};                    // 30 seconds
    uint64_t max_udp_payload_size{65527};
    uint64_t initial_max_data{10 * 1024 * 1024};         // 10 MB
    uint64_t initial_max_stream_data_bidi_local{1 * 1024 * 1024};
    uint64_t initial_max_stream_data_bidi_remote{1 * 1024 * 1024};
    uint64_t initial_max_stream_data_uni{1 * 1024 * 1024};
    uint64_t initial_max_streams_bidi{100};
    uint64_t initial_max_streams_uni{100};
    uint64_t ack_delay_exponent{3};
    uint64_t max_ack_delay{25};                          // 25 ms
    bool disable_active_migration{false};

    // Server-only
    ConnectionID retry_source_connection_id;
    std::vector<uint8_t> stateless_reset_token;

    // Optional extensions
    uint64_t active_connection_id_limit{2};
    uint64_t max_datagram_frame_size{65535};             // For DATAGRAM frames

    /**
     * Encode transport parameters to wire format.
     *
     * @param out Output buffer
     * @param max_len Maximum output length
     * @param is_server True if encoding server parameters
     * @return Bytes written, or -1 on error
     */
    ssize_t encode(uint8_t* out, size_t max_len, bool is_server) const noexcept {
        size_t pos = 0;

        auto write_varint_param = [&](uint64_t id, uint64_t value) -> bool {
            size_t param_start = pos;

            // Parameter ID
            int id_len = VarInt::encode(id, out + pos);
            if (id_len < 0 || pos + id_len > max_len) return false;
            pos += id_len;

            // Value length (we need to know the encoded length first)
            uint8_t temp[8];
            int val_len = VarInt::encode(value, temp);
            if (val_len < 0) return false;

            int len_len = VarInt::encode(static_cast<uint64_t>(val_len), out + pos);
            if (len_len < 0 || pos + len_len + val_len > max_len) return false;
            pos += len_len;

            // Value
            std::memcpy(out + pos, temp, val_len);
            pos += val_len;

            return true;
        };

        auto write_cid_param = [&](uint64_t id, const ConnectionID& cid) -> bool {
            if (cid.length == 0) return true; // Skip empty

            // Parameter ID
            int id_len = VarInt::encode(id, out + pos);
            if (id_len < 0 || pos + id_len > max_len) return false;
            pos += id_len;

            // Length
            int len_len = VarInt::encode(cid.length, out + pos);
            if (len_len < 0 || pos + len_len + cid.length > max_len) return false;
            pos += len_len;

            // Value
            std::memcpy(out + pos, cid.data, cid.length);
            pos += cid.length;

            return true;
        };

        // Encode parameters (parameter IDs from RFC 9000 Section 18.2)
        if (is_server) {
            if (!write_cid_param(0x00, original_destination_connection_id)) return -1;
        }

        if (!write_varint_param(0x01, max_idle_timeout)) return -1;
        if (!write_varint_param(0x03, max_udp_payload_size)) return -1;
        if (!write_varint_param(0x04, initial_max_data)) return -1;
        if (!write_varint_param(0x05, initial_max_stream_data_bidi_local)) return -1;
        if (!write_varint_param(0x06, initial_max_stream_data_bidi_remote)) return -1;
        if (!write_varint_param(0x07, initial_max_stream_data_uni)) return -1;
        if (!write_varint_param(0x08, initial_max_streams_bidi)) return -1;
        if (!write_varint_param(0x09, initial_max_streams_uni)) return -1;
        if (!write_varint_param(0x0a, ack_delay_exponent)) return -1;
        if (!write_varint_param(0x0b, max_ack_delay)) return -1;
        if (!write_varint_param(0x0e, active_connection_id_limit)) return -1;

        if (disable_active_migration) {
            // Parameter 0x0c with empty value
            int id_len = VarInt::encode(0x0c, out + pos);
            if (id_len < 0 || pos + id_len + 1 > max_len) return -1;
            pos += id_len;
            out[pos++] = 0; // Length = 0
        }

        if (max_datagram_frame_size > 0) {
            if (!write_varint_param(0x20, max_datagram_frame_size)) return -1;
        }

        return static_cast<ssize_t>(pos);
    }

    /**
     * Decode transport parameters from wire format.
     */
    int decode(const uint8_t* data, size_t len) noexcept {
        size_t pos = 0;

        while (pos < len) {
            // Parameter ID
            uint64_t param_id;
            int id_len = VarInt::decode(data + pos, len - pos, param_id);
            if (id_len < 0) return -1;
            pos += id_len;

            // Parameter length
            uint64_t param_len;
            int len_len = VarInt::decode(data + pos, len - pos, param_len);
            if (len_len < 0) return -1;
            pos += len_len;

            if (pos + param_len > len) return -1;

            // Parse value
            const uint8_t* val = data + pos;
            size_t val_len = static_cast<size_t>(param_len);

            switch (param_id) {
                case 0x00: // original_destination_connection_id
                    if (val_len > 20) return -1;
                    original_destination_connection_id = ConnectionID(val, static_cast<uint8_t>(val_len));
                    break;
                case 0x01: { // max_idle_timeout
                    uint64_t v;
                    if (VarInt::decode(val, val_len, v) < 0) return -1;
                    max_idle_timeout = v;
                    break;
                }
                case 0x03: { // max_udp_payload_size
                    uint64_t v;
                    if (VarInt::decode(val, val_len, v) < 0) return -1;
                    max_udp_payload_size = v;
                    break;
                }
                case 0x04: { // initial_max_data
                    uint64_t v;
                    if (VarInt::decode(val, val_len, v) < 0) return -1;
                    initial_max_data = v;
                    break;
                }
                case 0x05: {
                    uint64_t v;
                    if (VarInt::decode(val, val_len, v) < 0) return -1;
                    initial_max_stream_data_bidi_local = v;
                    break;
                }
                case 0x06: {
                    uint64_t v;
                    if (VarInt::decode(val, val_len, v) < 0) return -1;
                    initial_max_stream_data_bidi_remote = v;
                    break;
                }
                case 0x07: {
                    uint64_t v;
                    if (VarInt::decode(val, val_len, v) < 0) return -1;
                    initial_max_stream_data_uni = v;
                    break;
                }
                case 0x08: {
                    uint64_t v;
                    if (VarInt::decode(val, val_len, v) < 0) return -1;
                    initial_max_streams_bidi = v;
                    break;
                }
                case 0x09: {
                    uint64_t v;
                    if (VarInt::decode(val, val_len, v) < 0) return -1;
                    initial_max_streams_uni = v;
                    break;
                }
                case 0x0a: {
                    uint64_t v;
                    if (VarInt::decode(val, val_len, v) < 0) return -1;
                    ack_delay_exponent = v;
                    break;
                }
                case 0x0b: {
                    uint64_t v;
                    if (VarInt::decode(val, val_len, v) < 0) return -1;
                    max_ack_delay = v;
                    break;
                }
                case 0x0c: // disable_active_migration
                    disable_active_migration = true;
                    break;
                case 0x0e: {
                    uint64_t v;
                    if (VarInt::decode(val, val_len, v) < 0) return -1;
                    active_connection_id_limit = v;
                    break;
                }
                case 0x20: {
                    uint64_t v;
                    if (VarInt::decode(val, val_len, v) < 0) return -1;
                    max_datagram_frame_size = v;
                    break;
                }
                default:
                    // Unknown parameter - ignore (MUST for forward compatibility)
                    break;
            }

            pos += param_len;
        }

        return 0;
    }
};

/**
 * Callback types.
 */
using OnConnectedCallback = std::function<void()>;
using OnStreamDataCallback = std::function<void(uint64_t stream_id,
                                                 const uint8_t* data,
                                                 size_t len,
                                                 bool fin)>;
using OnDatagramCallback = std::function<void(const uint8_t* data, size_t len)>;
using OnConnectionErrorCallback = std::function<void(uint64_t error_code,
                                                      const std::string& reason)>;

/**
 * QUIC Secure Connection.
 *
 * Full QUIC connection with TLS 1.3 integration.
 */
class QUICSecureConnection {
public:
    /**
     * Constructor.
     *
     * @param ssl_ctx SSL context for TLS
     * @param is_server True if server-side
     * @param local_cid Local connection ID
     * @param peer_cid Peer connection ID (original DCID for initial key derivation)
     * @param version QUIC version
     */
    QUICSecureConnection(SSL_CTX* ssl_ctx, bool is_server,
                          const ConnectionID& local_cid,
                          const ConnectionID& peer_cid,
                          uint32_t version = version::QUIC_V1)
        : ssl_ctx_(ssl_ctx),
          is_server_(is_server),
          version_(version),
          local_cid_(local_cid),
          peer_cid_(peer_cid),
          original_dcid_(peer_cid),
          state_(ConnectionState::IDLE),
          current_send_level_(EncryptionLevel::INITIAL),
          current_recv_level_(EncryptionLevel::INITIAL) {}

    /**
     * Initialize the connection.
     *
     * @return 0 on success, -1 on failure
     */
    int initialize() noexcept {
        // Initialize handshake manager
        if (handshake_.initialize(ssl_ctx_, is_server_,
                                   original_dcid_.data, original_dcid_.length) != 0) {
            return -1;
        }

        // Set up transport parameters
        uint8_t tp_buf[512];
        ssize_t tp_len = local_params_.encode(tp_buf, sizeof(tp_buf), is_server_);
        if (tp_len < 0) {
            return -1;
        }
        if (handshake_.set_transport_params(tp_buf, static_cast<size_t>(tp_len)) != 0) {
            return -1;
        }

        // Set ALPN
        if (handshake_.set_alpn(alpn::kH3All, alpn::kH3AllLen) != 0) {
            return -1;
        }

        // Set up handshake callbacks
        handshake_.set_handshake_complete_callback([this]() {
            on_handshake_complete();
        });

        handshake_.set_error_callback([this](uint64_t error, const std::string& reason) {
            on_error(error, reason);
        });

        // Initialize packet number spaces
        pn_spaces_[PacketNumberSpace::INITIAL].set_keys_available(true);

        state_ = ConnectionState::HANDSHAKE;

        // Client starts handshake immediately
        if (!is_server_) {
            if (handshake_.start_handshake() != 0) {
                return -1;
            }
        }

        return 0;
    }

    /**
     * Process received UDP datagram.
     *
     * @param data Datagram data
     * @param len Datagram length
     * @param now Current time (microseconds)
     * @return 0 on success, -1 on error
     */
    int process_datagram(const uint8_t* data, size_t len, uint64_t now) noexcept {
        last_activity_time_ = now;
        std::cerr << "[Conn] process_datagram called, len=" << len << "\n";

        // Check for Version Negotiation
        if (is_version_negotiation(data, len)) {
            return process_version_negotiation(data, len);
        }

        // Check for Retry
        if (is_retry_packet(data, len)) {
            return process_retry(data, len);
        }

        // Parse header to determine encryption level
        if (len < 1) return -1;
        uint8_t first_byte = data[0];
        bool is_long = (first_byte & 0x80) != 0;
        std::cerr << "[Conn] first_byte=0x" << std::hex << (int)first_byte << std::dec
                  << " is_long=" << is_long << "\n";

        EncryptionLevel level;
        size_t header_len;
        uint64_t packet_number;
        size_t pn_offset;

        if (is_long) {
            LongHeader hdr;
            size_t consumed;
            if (hdr.parse(data, len, consumed) != 0) {
                std::cerr << "[Conn] Long header parse failed\n";
                return -1;
            }
            std::cerr << "[Conn] Long header: type=" << static_cast<int>(hdr.type)
                      << " version=0x" << std::hex << hdr.version << std::dec << "\n";

            // Check version
            if (!version::is_supported(hdr.version)) {
                std::cerr << "[Conn] Unsupported version\n";
                // Should send Version Negotiation
                return -1;
            }

            level = packet_type_to_level(hdr.type);
            header_len = consumed;
            pn_offset = consumed; // PN is at end of header
        } else {
            // Short header (1-RTT)
            level = EncryptionLevel::ONE_RTT;
            header_len = 1 + local_cid_.length; // First byte + DCID
            pn_offset = header_len;
        }

        std::cerr << "[Conn] level=" << static_cast<int>(level)
                  << " pn_offset=" << pn_offset << "\n";

        // Get packet protection
        PacketProtection* pp = handshake_.get_protection(level, false);
        if (!pp) {
            std::cerr << "[Conn] No protection keys for level\n";
            // Keys not yet available
            return 0;
        }

        // Remove header protection
        uint8_t decrypted[2048];
        std::memcpy(decrypted, data, len);

        size_t sample_offset = PacketProtection::sample_offset(pn_offset);
        if (sample_offset + kHpSampleLength > len) {
            std::cerr << "[Conn] Sample offset out of range\n";
            return -1;
        }

        size_t pn_length;
        if (pp->unprotect_header(decrypted, pn_offset,
                                  data + sample_offset, &pn_length) != 0) {
            std::cerr << "[Conn] unprotect_header failed\n";
            return -1;
        }
        std::cerr << "[Conn] Header unprotected, pn_length=" << pn_length << "\n";

        // Decode packet number
        uint64_t truncated_pn = 0;
        for (size_t i = 0; i < pn_length; i++) {
            truncated_pn = (truncated_pn << 8) | decrypted[pn_offset + i];
        }

        auto& space = pn_spaces_[level_to_space(level)];
        // If no packets received yet, just use the truncated PN directly
        // (largest_received() returns -1 when no packets received)
        int64_t largest = space.largest_received();
        if (largest < 0) {
            packet_number = truncated_pn;
        } else {
            packet_number = decode_packet_number(truncated_pn, pn_length,
                                                  static_cast<uint64_t>(largest));
        }
        std::cerr << "[Conn] truncated_pn=" << truncated_pn << " largest=" << largest
                  << " -> packet_number=" << packet_number << "\n";

        // Decrypt payload
        size_t payload_offset = pn_offset + pn_length;
        size_t ciphertext_len = len - payload_offset;

        uint8_t plaintext[2048];
        size_t plaintext_len;

        if (pp->decrypt(packet_number,
                        decrypted, pn_offset + pn_length, // Full header as AAD
                        data + payload_offset, ciphertext_len,
                        plaintext, &plaintext_len) != 0) {
            std::cerr << "[Conn] Decrypt failed\n";
            // Authentication failed
            return -1;
        }
        std::cerr << "[Conn] Decrypted " << plaintext_len << " bytes of payload\n";

        // Record receipt
        space.on_packet_received(packet_number, true);

        // Process frames
        int result = process_frames(level, plaintext, plaintext_len, now);
        std::cerr << "[Conn] process_frames returned " << result << "\n";
        return result;
    }

    /**
     * Generate packets to send.
     *
     * @param output Output buffer
     * @param capacity Buffer capacity
     * @param now Current time (microseconds)
     * @return Bytes written
     */
    size_t generate_packets(uint8_t* output, size_t capacity, uint64_t now) noexcept {
        size_t total_written = 0;
        std::cerr << "[Conn] generate_packets called\n";

        // Generate packets for each encryption level that has data
        for (int level_idx = 0; level_idx < 4; level_idx++) {
            EncryptionLevel level = static_cast<EncryptionLevel>(level_idx);

            bool has_keys = handshake_.keys_available(level);
            bool has_data = handshake_.has_crypto_data_to_send(level);
            std::cerr << "[Conn] Level " << level_idx << ": keys=" << has_keys
                      << " has_crypto_data=" << has_data << "\n";

            if (!has_keys) {
                continue;
            }

            size_t written = generate_packet_at_level(level,
                                                       output + total_written,
                                                       capacity - total_written,
                                                       now);
            std::cerr << "[Conn] Level " << level_idx << " generated " << written << " bytes\n";
            total_written += written;
        }

        std::cerr << "[Conn] generate_packets returning " << total_written << " bytes\n";
        return total_written;
    }

    /**
     * Write data to a stream.
     */
    ssize_t write_stream(uint64_t stream_id, const uint8_t* data, size_t len) noexcept {
        if (!is_connected()) return -1;

        auto it = streams_.find(stream_id);
        if (it == streams_.end()) {
            // Create stream if needed
            auto stream = std::make_unique<QUICStream>(stream_id, is_server_);
            it = streams_.emplace(stream_id, std::move(stream)).first;
        }

        return it->second->write(data, len);
    }

    /**
     * Send a datagram (unreliable).
     */
    int send_datagram(const uint8_t* data, size_t len) noexcept {
        if (!is_connected()) return -1;

        // Queue datagram for sending
        pending_datagrams_.emplace_back(data, data + len);
        return 0;
    }

    /**
     * Close the connection.
     */
    void close(uint64_t error_code = 0, const char* reason = nullptr) noexcept {
        if (state_ == ConnectionState::CLOSED) return;

        close_error_code_ = error_code;
        if (reason) {
            close_reason_ = reason;
        }
        state_ = ConnectionState::CLOSING;
    }

    /**
     * Check if connected (handshake complete).
     */
    bool is_connected() const noexcept {
        return state_ == ConnectionState::ESTABLISHED;
    }

    /**
     * Get connection state.
     */
    ConnectionState state() const noexcept { return state_; }

    /**
     * Get negotiated ALPN.
     */
    std::string alpn() const noexcept {
        return handshake_.get_alpn();
    }

    /**
     * Get local connection ID.
     */
    const ConnectionID& local_cid() const noexcept { return local_cid_; }

    /**
     * Get peer connection ID.
     */
    const ConnectionID& peer_cid() const noexcept { return peer_cid_; }

    /**
     * Set callbacks.
     */
    void set_connected_callback(OnConnectedCallback cb) {
        on_connected_ = std::move(cb);
    }

    void set_stream_data_callback(OnStreamDataCallback cb) {
        on_stream_data_ = std::move(cb);
    }

    void set_datagram_callback(OnDatagramCallback cb) {
        on_datagram_ = std::move(cb);
    }

    void set_error_callback(OnConnectionErrorCallback cb) {
        on_connection_error_ = std::move(cb);
    }

    /**
     * Set local transport parameters.
     */
    void set_transport_parameters(const TransportParameters& params) {
        local_params_ = params;
    }

    /**
     * Get peer transport parameters.
     */
    const TransportParameters& peer_transport_parameters() const noexcept {
        return peer_params_;
    }

private:
    /**
     * Map packet type to encryption level.
     */
    EncryptionLevel packet_type_to_level(PacketType type) const noexcept {
        switch (type) {
            case PacketType::INITIAL: return EncryptionLevel::INITIAL;
            case PacketType::ZERO_RTT: return EncryptionLevel::ZERO_RTT;
            case PacketType::HANDSHAKE: return EncryptionLevel::HANDSHAKE;
            default: return EncryptionLevel::ONE_RTT;
        }
    }

    /**
     * Process Version Negotiation.
     */
    int process_version_negotiation(const uint8_t* data, size_t len) noexcept {
        if (is_server_) return -1; // Server shouldn't receive VN

        VersionNegotiationPacket vn;
        if (vn.parse(data, len) != 0) {
            return -1;
        }

        // Check if any supported version
        for (uint32_t v : vn.supported_versions) {
            if (version::is_supported(v)) {
                // Could retry with this version
                return 0;
            }
        }

        // No compatible version
        return -1;
    }

    /**
     * Process Retry packet.
     */
    int process_retry(const uint8_t* data, size_t len) noexcept {
        if (is_server_) return -1; // Server shouldn't receive Retry

        RetryPacket retry;
        if (retry.parse(data, len) != 0) {
            return -1;
        }

        // Verify integrity tag
        if (!retry.verify_integrity_tag(original_dcid_)) {
            return -1;
        }

        // Update connection IDs
        peer_cid_ = retry.source_conn_id;
        retry_token_ = retry.retry_token;
        retry_received_ = true;

        // Need to restart handshake with new DCID
        // Re-derive initial keys with new DCID
        return 0;
    }

    /**
     * Process frames in a decrypted packet.
     */
    int process_frames(EncryptionLevel level, const uint8_t* data,
                       size_t len, uint64_t now) noexcept {
        size_t pos = 0;

        while (pos < len) {
            // Get frame type
            uint64_t frame_type;
            int type_len = VarInt::decode(data + pos, len - pos, frame_type);
            if (type_len < 0) return -1;
            pos += type_len;

            // Process frame based on type
            size_t consumed = 0;
            int result = process_frame(level, frame_type, data + pos, len - pos, now, consumed);
            if (result < 0) return -1;
            pos += consumed;
        }

        return 0;
    }

    /**
     * Process a single frame.
     */
    int process_frame(EncryptionLevel level, uint64_t frame_type,
                      const uint8_t* data, size_t len,
                      uint64_t now, size_t& consumed) noexcept {
        consumed = 0;

        switch (frame_type) {
            case 0x00: // PADDING
                consumed = 0;
                return 0;

            case 0x01: // PING
                consumed = 0;
                return 0;

            case 0x02: // ACK (no ECN)
            case 0x03: { // ACK (with ECN)
                AckFrame ack;
                size_t ack_consumed;
                if (ack.parse(data, len, ack_consumed) != 0) return -1;
                consumed = ack_consumed;

                auto& space = pn_spaces_[level_to_space(level)];
                space.on_ack_received(ack, now, congestion_control_);
                return 0;
            }

            case 0x06: { // CRYPTO
                CryptoFrame crypto;
                size_t crypto_consumed;
                if (crypto.parse(data, len, crypto_consumed) != 0) return -1;
                consumed = crypto_consumed;

                // Provide to handshake manager
                if (handshake_.receive_crypto_data(level, crypto.offset,
                                                    crypto.data, crypto.length) != 0) {
                    return -1;
                }
                return 0;
            }

            case 0x08 ... 0x0f: { // STREAM
                StreamFrame stream;
                size_t stream_consumed;
                if (stream.parse(static_cast<uint8_t>(frame_type), data, len, stream_consumed) != 0) return -1;
                consumed = stream_consumed;

                // Deliver to application
                if (on_stream_data_) {
                    on_stream_data_(stream.stream_id, stream.data, stream.length, stream.fin);
                }
                return 0;
            }

            case 0x1c: // CONNECTION_CLOSE (QUIC)
            case 0x1d: { // CONNECTION_CLOSE (Application)
                ConnectionCloseFrame close_frame;
                size_t close_consumed;
                bool is_app_error = (frame_type == 0x1d);
                if (close_frame.parse(data, len, is_app_error, close_consumed) != 0) return -1;
                consumed = close_consumed;

                state_ = ConnectionState::DRAINING;
                if (on_connection_error_) {
                    on_connection_error_(close_frame.error_code,
                                          std::string(close_frame.reason_phrase,
                                                      close_frame.reason_length));
                }
                return 0;
            }

            case 0x1e: { // HANDSHAKE_DONE
                if (!is_server_) {
                    // Client received HANDSHAKE_DONE
                    handshake_.confirm_handshake();
                    pn_spaces_.discard_handshake_keys();
                }
                consumed = 0;
                return 0;
            }

            case 0x30: // DATAGRAM (no length)
            case 0x31: { // DATAGRAM (with length)
                DatagramFrame dgram;
                size_t dgram_consumed;
                if (dgram.parse(static_cast<uint8_t>(frame_type), data, len, dgram_consumed) != 0) return -1;
                consumed = dgram_consumed;

                if (on_datagram_) {
                    on_datagram_(dgram.data, dgram.length);
                }
                return 0;
            }

            default:
                // Unknown frame - skip
                // For variable-length frames, we'd need to parse length
                return -1;
        }
    }

    /**
     * Generate a packet at specific encryption level.
     */
    size_t generate_packet_at_level(EncryptionLevel level,
                                     uint8_t* output, size_t capacity,
                                     uint64_t now) noexcept {
        PacketProtection* pp = handshake_.get_protection(level, true);
        if (!pp) return 0;

        auto& space = pn_spaces_.for_level(level);

        // Build payload (CRYPTO frames, STREAM frames, ACKs)
        uint8_t payload[1200];
        size_t payload_len = 0;

        // Add CRYPTO data if available
        if (handshake_.has_crypto_data_to_send(level)) {
            uint64_t send_offset = handshake_.get_crypto_send_offset(level);
            auto [crypto_data, crypto_len] = handshake_.get_crypto_data_to_send(
                level, send_offset, 1000);

            if (crypto_len > 0) {
                CryptoFrame crypto;
                crypto.offset = send_offset;
                crypto.length = crypto_len;
                crypto.data = crypto_data;

                payload_len += crypto.serialize(payload + payload_len);
                handshake_.on_crypto_data_sent(level, crypto_len);
            }
        }

        // Add ACK if needed
        if (space.needs_ack()) {
            AckFrame ack;
            if (space.build_ack_frame(ack, now)) {
                payload_len += ack.serialize(payload + payload_len);
                space.on_ack_sent();
            }
        }

        // Add STREAM frames (1-RTT only)
        if (level == EncryptionLevel::ONE_RTT && is_connected()) {
            for (auto& [stream_id, stream] : streams_) {
                if (stream->has_data_to_send()) {
                    uint8_t stream_data[1000];
                    size_t data_len = stream->peek_send_data(stream_data, 1000);

                    if (data_len > 0) {
                        StreamFrame sf;
                        sf.stream_id = stream_id;
                        sf.offset = stream->send_offset();
                        sf.length = data_len;
                        sf.data = stream_data;
                        sf.fin = false;

                        payload_len += sf.serialize(payload + payload_len);
                        stream->advance_send_offset(data_len);
                    }
                }
            }

            // Add HANDSHAKE_DONE (server sends once)
            if (is_server_ && handshake_.is_handshake_complete() && !handshake_done_sent_) {
                payload[payload_len++] = 0x1e; // HANDSHAKE_DONE frame
                handshake_done_sent_ = true;
            }
        }

        if (payload_len == 0) return 0;

        // Build header
        uint64_t pn = space.next_packet_number();
        auto [truncated_pn, pn_length] = encode_packet_number(pn, space.largest_acked());

        uint8_t header[64];
        size_t header_len = 0;

        if (level == EncryptionLevel::ONE_RTT) {
            // Short header
            ShortHeader sh;
            sh.dest_conn_id = peer_cid_;
            sh.packet_number = truncated_pn;
            sh.packet_number_length = static_cast<uint8_t>(pn_length);
            sh.spin_bit = false;
            sh.key_phase = false;
            header_len = sh.serialize(header);
        } else {
            // Long header
            LongHeader lh;
            lh.type = (level == EncryptionLevel::INITIAL) ? PacketType::INITIAL :
                      (level == EncryptionLevel::HANDSHAKE) ? PacketType::HANDSHAKE :
                      PacketType::ZERO_RTT;
            lh.version = version_;
            lh.dest_conn_id = peer_cid_;
            lh.source_conn_id = local_cid_;
            lh.token = (level == EncryptionLevel::INITIAL && !retry_token_.empty())
                       ? retry_token_.data() : nullptr;
            lh.token_length = (level == EncryptionLevel::INITIAL)
                              ? retry_token_.size() : 0;
            lh.packet_length = pn_length + payload_len + kAeadTagLength;
            header_len = lh.serialize(header);
        }

        // Encrypt
        uint8_t encrypted_payload[2048];
        size_t encrypted_len;

        if (pp->encrypt(pn, header, header_len + pn_length,
                        payload, payload_len,
                        encrypted_payload, &encrypted_len) != 0) {
            return 0;
        }

        // Copy header to output
        std::memcpy(output, header, header_len);
        size_t pn_offset = header_len;

        // Add packet number
        for (size_t i = 0; i < pn_length; i++) {
            output[header_len + i] = (truncated_pn >> ((pn_length - 1 - i) * 8)) & 0xFF;
        }

        // Add encrypted payload
        std::memcpy(output + header_len + pn_length, encrypted_payload, encrypted_len);
        size_t total_len = header_len + pn_length + encrypted_len;

        // Apply header protection
        size_t sample_offset = PacketProtection::sample_offset(pn_offset);
        pp->protect_header(output, pn_offset, pn_length, output + sample_offset);

        // Record sent
        space.on_packet_sent(pn, total_len, true, now);

        return total_len;
    }

    /**
     * Called when TLS handshake completes.
     */
    void on_handshake_complete() noexcept {
        // Get peer's transport parameters
        const uint8_t* peer_tp;
        size_t peer_tp_len;
        handshake_.get_peer_transport_params(&peer_tp, &peer_tp_len);

        if (peer_tp && peer_tp_len > 0) {
            peer_params_.decode(peer_tp, peer_tp_len);
        }

        // Update state
        state_ = ConnectionState::ESTABLISHED;

        // Discard Initial keys
        pn_spaces_.discard_initial_keys();
        handshake_.discard_keys(EncryptionLevel::INITIAL);

        // Notify application
        if (on_connected_) {
            on_connected_();
        }
    }

    /**
     * Called on error.
     */
    void on_error(uint64_t error_code, const std::string& reason) noexcept {
        close_error_code_ = error_code;
        close_reason_ = reason;
        state_ = ConnectionState::CLOSING;

        if (on_connection_error_) {
            on_connection_error_(error_code, reason);
        }
    }

    // TLS
    SSL_CTX* ssl_ctx_;
    HandshakeManager handshake_;

    // Connection info
    bool is_server_;
    uint32_t version_;
    ConnectionID local_cid_;
    ConnectionID peer_cid_;
    ConnectionID original_dcid_;
    std::vector<uint8_t> retry_token_;
    bool retry_received_{false};

    // State
    ConnectionState state_;
    EncryptionLevel current_send_level_;
    EncryptionLevel current_recv_level_;

    // Packet number spaces
    PacketNumberSpaceManager pn_spaces_;

    // Congestion control
    NewRenoCongestionControl congestion_control_;

    // Transport parameters
    TransportParameters local_params_;
    TransportParameters peer_params_;

    // Streams
    std::unordered_map<uint64_t, std::unique_ptr<QUICStream>> streams_;

    // Datagrams
    std::vector<std::vector<uint8_t>> pending_datagrams_;

    // Close state
    uint64_t close_error_code_{0};
    std::string close_reason_;

    // Flags
    bool handshake_done_sent_{false};

    // Timing
    uint64_t last_activity_time_{0};

    // Callbacks
    OnConnectedCallback on_connected_;
    OnStreamDataCallback on_stream_data_;
    OnDatagramCallback on_datagram_;
    OnConnectionErrorCallback on_connection_error_;
};

} // namespace quic
} // namespace fasterapi
