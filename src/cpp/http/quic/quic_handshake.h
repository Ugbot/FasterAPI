#pragma once

/**
 * @file quic_handshake.h
 * @brief QUIC Handshake State Machine (RFC 9001)
 *
 * Coordinates the TLS 1.3 handshake with QUIC packet protection,
 * managing encryption level transitions and key updates.
 */

#include <cstdint>
#include <cstddef>
#include <memory>
#include <functional>
#include <array>
#include <iostream>

#include "quic_crypto_buffer.h"
#include "quic_packet_protection.h"
#include "quic_packet_number_space.h"
#include "quic_tls.h"

namespace fasterapi {
namespace quic {

/**
 * Handshake states.
 */
enum class HandshakeState : uint8_t {
    IDLE = 0,                    // Not started
    INITIAL_SENT = 1,            // Client: Initial sent, Server: Initial sent
    INITIAL_RECEIVED = 2,        // Initial packet received
    HANDSHAKE_KEYS_AVAILABLE = 3,// Handshake encryption keys ready
    HANDSHAKE_SENT = 4,          // Handshake packets exchanged
    HANDSHAKE_RECEIVED = 5,      // Handshake data received
    HANDSHAKE_COMPLETE = 6,      // TLS handshake complete (1-RTT keys available)
    CONFIRMED = 7,               // Handshake confirmed (for server: received HANDSHAKE_DONE or ACK of 1-RTT)
    FAILED = 8                   // Handshake failed
};

/**
 * Convert handshake state to string for debugging.
 */
inline const char* handshake_state_name(HandshakeState state) noexcept {
    switch (state) {
        case HandshakeState::IDLE: return "IDLE";
        case HandshakeState::INITIAL_SENT: return "INITIAL_SENT";
        case HandshakeState::INITIAL_RECEIVED: return "INITIAL_RECEIVED";
        case HandshakeState::HANDSHAKE_KEYS_AVAILABLE: return "HANDSHAKE_KEYS_AVAILABLE";
        case HandshakeState::HANDSHAKE_SENT: return "HANDSHAKE_SENT";
        case HandshakeState::HANDSHAKE_RECEIVED: return "HANDSHAKE_RECEIVED";
        case HandshakeState::HANDSHAKE_COMPLETE: return "HANDSHAKE_COMPLETE";
        case HandshakeState::CONFIRMED: return "CONFIRMED";
        case HandshakeState::FAILED: return "FAILED";
        default: return "UNKNOWN";
    }
}

/**
 * QUIC Handshake Manager.
 *
 * Coordinates:
 * - TLS 1.3 handshake via QUICTls
 * - CRYPTO frame buffering per encryption level
 * - Packet protection key installation
 * - Encryption level transitions
 */
class HandshakeManager {
public:
    // Callbacks
    using OnCryptoDataCallback = std::function<void(EncryptionLevel level,
                                                     const uint8_t* data,
                                                     size_t len)>;
    using OnKeysAvailableCallback = std::function<void(EncryptionLevel level,
                                                        PacketProtection& protection)>;
    using OnHandshakeCompleteCallback = std::function<void()>;
    using OnHandshakeConfirmedCallback = std::function<void()>;
    using OnErrorCallback = std::function<void(uint64_t error_code,
                                                const std::string& reason)>;

    HandshakeManager() noexcept
        : state_(HandshakeState::IDLE),
          is_server_(false),
          handshake_complete_(false),
          handshake_confirmed_(false),
          error_code_(0) {}

    /**
     * Initialize the handshake manager.
     *
     * @param ssl_ctx SSL context configured for QUIC
     * @param is_server True for server, false for client
     * @param dcid Destination Connection ID (for initial key derivation)
     * @param dcid_len Length of DCID
     * @return 0 on success, -1 on failure
     */
    int initialize(SSL_CTX* ssl_ctx, bool is_server,
                   const uint8_t* dcid, size_t dcid_len) noexcept {
        is_server_ = is_server;

        // Initialize TLS
        if (tls_.initialize(ssl_ctx, is_server) != 0) {
            state_ = HandshakeState::FAILED;
            return -1;
        }

        // Set up TLS callbacks
        tls_.set_write_secret_callback([this](EncryptionLevel level,
                                               const uint8_t* secret,
                                               size_t secret_len,
                                               bool is_write) {
            on_secret_available(level, secret, secret_len, true);
        });

        tls_.set_read_secret_callback([this](EncryptionLevel level,
                                              const uint8_t* secret,
                                              size_t secret_len,
                                              bool is_write) {
            on_secret_available(level, secret, secret_len, false);
        });

        tls_.set_crypto_data_callback([this](EncryptionLevel level,
                                              const uint8_t* data,
                                              size_t len) {
            on_crypto_data_generated(level, data, len);
        });

        tls_.set_alert_callback([this](TlsAlert alert) {
            on_tls_alert(alert);
        });

        tls_.set_handshake_complete_callback([this]() {
            on_tls_handshake_complete();
        });

        // Derive initial keys from DCID
        if (derive_initial_packet_protection(dcid, dcid_len,
                                              protectors_[static_cast<size_t>(EncryptionLevel::INITIAL)][0],
                                              protectors_[static_cast<size_t>(EncryptionLevel::INITIAL)][1]) != 0) {
            state_ = HandshakeState::FAILED;
            return -1;
        }

        // Initial keys are now available
        // Index 0 = client protection, Index 1 = server protection
        // For sending: server uses index 1, client uses index 0
        // For receiving: server uses index 0, client uses index 1
        keys_available_[static_cast<size_t>(EncryptionLevel::INITIAL)] = true;

        return 0;
    }

    /**
     * Set ALPN protocols.
     */
    int set_alpn(const uint8_t* protos, size_t protos_len) noexcept {
        return tls_.set_alpn(protos, protos_len);
    }

    /**
     * Set transport parameters.
     */
    int set_transport_params(const uint8_t* params, size_t params_len) noexcept {
        return tls_.set_transport_params(params, params_len);
    }

    /**
     * Start the handshake (client only).
     *
     * For server, handshake is started when first Initial packet arrives.
     */
    int start_handshake() noexcept {
        if (is_server_) {
            // Server waits for client's Initial
            return 0;
        }

        // Client starts TLS handshake (generates ClientHello)
        int ret = tls_.advance_handshake();
        if (ret < 0) {
            state_ = HandshakeState::FAILED;
            return -1;
        }

        state_ = HandshakeState::INITIAL_SENT;
        return 0;
    }

    /**
     * Provide received CRYPTO frame data.
     *
     * @param level Encryption level the data was received at
     * @param offset Offset in the CRYPTO stream
     * @param data CRYPTO frame data
     * @param len Length of data
     * @return 0 on success, -1 on failure
     */
    int receive_crypto_data(EncryptionLevel level,
                            uint64_t offset,
                            const uint8_t* data, size_t len) noexcept {
        std::cerr << "[Handshake] receive_crypto_data: level=" << static_cast<int>(level)
                  << " offset=" << offset << " len=" << len << "\n";

        if (state_ == HandshakeState::FAILED) {
            std::cerr << "[Handshake] State is FAILED\n";
            return -1;
        }

        // Buffer the data
        auto& buffer = crypto_buffers_[static_cast<size_t>(level)];
        if (buffer.receive_data(offset, data, len) != 0) {
            std::cerr << "[Handshake] buffer.receive_data failed\n";
            return -1;
        }

        // Try to read contiguous data and provide to TLS
        uint8_t read_buf[4096];
        size_t read_len;

        while ((read_len = buffer.read(read_buf, sizeof(read_buf))) > 0) {
            std::cerr << "[Handshake] Providing " << read_len << " bytes to TLS\n";
            if (tls_.provide_crypto_data(level, read_buf, read_len) != 0) {
                std::cerr << "[Handshake] tls_.provide_crypto_data failed\n";
                state_ = HandshakeState::FAILED;
                return -1;
            }
        }

        // Advance TLS handshake
        std::cerr << "[Handshake] Advancing TLS handshake\n";
        int ret = tls_.advance_handshake();
        std::cerr << "[Handshake] advance_handshake returned " << ret << "\n";
        if (ret < 0) {
            std::cerr << "[Handshake] TLS handshake failed\n";
            state_ = HandshakeState::FAILED;
            return -1;
        }

        // Update state
        update_state_after_receive(level);
        std::cerr << "[Handshake] New state: " << handshake_state_name(state_) << "\n";

        // Check if any data was generated for sending
        for (int i = 0; i < 4; i++) {
            bool has_send = send_buffers_[i].has_pending_send();
            if (has_send) {
                std::cerr << "[Handshake] Level " << i << " has data to send\n";
            }
        }

        return 0;
    }

    /**
     * Check if there's CRYPTO data to send.
     */
    bool has_crypto_data_to_send(EncryptionLevel level) const noexcept {
        return send_buffers_[static_cast<size_t>(level)].has_pending_send();
    }

    /**
     * Get CRYPTO data to send.
     *
     * @param level Encryption level to send at
     * @param offset Starting offset
     * @param max_len Maximum bytes to return
     * @return Pair of (data pointer, length)
     */
    std::pair<const uint8_t*, size_t> get_crypto_data_to_send(
        EncryptionLevel level, uint64_t offset, size_t max_len) const noexcept {
        return send_buffers_[static_cast<size_t>(level)].get_send_data(offset, max_len);
    }

    /**
     * Mark CRYPTO data as sent.
     */
    void on_crypto_data_sent(EncryptionLevel level, size_t bytes) noexcept {
        send_buffers_[static_cast<size_t>(level)].advance_send_offset(bytes);
    }

    /**
     * Get the next send offset for CRYPTO data at a level.
     */
    uint64_t get_crypto_send_offset(EncryptionLevel level) const noexcept {
        return send_buffers_[static_cast<size_t>(level)].next_send_offset();
    }

    /**
     * Get packet protection for an encryption level.
     *
     * @param level Encryption level
     * @param for_sending True for sending, false for receiving
     * @return Pointer to packet protection, or nullptr if not available
     */
    PacketProtection* get_protection(EncryptionLevel level, bool for_sending) noexcept {
        size_t idx = static_cast<size_t>(level);
        if (!keys_available_[idx]) {
            return nullptr;
        }

        // For initial: index 0 = client, index 1 = server
        // For sending: server uses index 1, client uses index 0
        // For receiving: server uses index 0, client uses index 1
        size_t key_idx;
        if (level == EncryptionLevel::INITIAL) {
            if (for_sending) {
                key_idx = is_server_ ? 1 : 0;
            } else {
                key_idx = is_server_ ? 0 : 1;
            }
        } else {
            // For handshake/1-RTT: index 0 = write keys, index 1 = read keys
            key_idx = for_sending ? 0 : 1;
        }

        auto& prot = protectors_[idx][key_idx];
        return prot.is_initialized() ? &prot : nullptr;
    }

    /**
     * Check if keys are available for an encryption level.
     */
    bool keys_available(EncryptionLevel level) const noexcept {
        return keys_available_[static_cast<size_t>(level)];
    }

    /**
     * Discard keys for an encryption level.
     */
    void discard_keys(EncryptionLevel level) noexcept {
        keys_available_[static_cast<size_t>(level)] = false;
    }

    /**
     * Get current handshake state.
     */
    HandshakeState state() const noexcept { return state_; }

    /**
     * Check if handshake is complete (1-RTT keys available).
     */
    bool is_handshake_complete() const noexcept { return handshake_complete_; }

    /**
     * Check if handshake is confirmed.
     */
    bool is_handshake_confirmed() const noexcept { return handshake_confirmed_; }

    /**
     * Confirm the handshake (server: received HANDSHAKE_DONE ACK).
     */
    void confirm_handshake() noexcept {
        if (handshake_complete_ && !handshake_confirmed_) {
            handshake_confirmed_ = true;
            state_ = HandshakeState::CONFIRMED;

            // Discard Initial and Handshake keys
            discard_keys(EncryptionLevel::INITIAL);
            discard_keys(EncryptionLevel::HANDSHAKE);

            if (on_handshake_confirmed_) {
                on_handshake_confirmed_();
            }
        }
    }

    /**
     * Get negotiated ALPN protocol.
     */
    std::string get_alpn() const noexcept {
        return tls_.get_alpn();
    }

    /**
     * Get peer's transport parameters.
     */
    void get_peer_transport_params(const uint8_t** params, size_t* len) const noexcept {
        tls_.get_peer_transport_params(params, len);
    }

    /**
     * Get error code if handshake failed.
     */
    uint64_t error_code() const noexcept { return error_code_; }

    /**
     * Get error reason if handshake failed.
     */
    const std::string& error_reason() const noexcept { return error_reason_; }

    /**
     * Set callback for outgoing CRYPTO data.
     */
    void set_crypto_data_callback(OnCryptoDataCallback cb) {
        on_crypto_data_ = std::move(cb);
    }

    /**
     * Set callback for when keys become available.
     */
    void set_keys_available_callback(OnKeysAvailableCallback cb) {
        on_keys_available_ = std::move(cb);
    }

    /**
     * Set callback for handshake completion.
     */
    void set_handshake_complete_callback(OnHandshakeCompleteCallback cb) {
        on_handshake_complete_ = std::move(cb);
    }

    /**
     * Set callback for handshake confirmation.
     */
    void set_handshake_confirmed_callback(OnHandshakeConfirmedCallback cb) {
        on_handshake_confirmed_ = std::move(cb);
    }

    /**
     * Set callback for errors.
     */
    void set_error_callback(OnErrorCallback cb) {
        on_error_ = std::move(cb);
    }

    /**
     * Get the underlying QUICTls object.
     */
    QUICTls& tls() noexcept { return tls_; }
    const QUICTls& tls() const noexcept { return tls_; }

private:
    /**
     * Called when TLS generates CRYPTO data to send.
     */
    void on_crypto_data_generated(EncryptionLevel level,
                                   const uint8_t* data, size_t len) noexcept {
        // Buffer for sending
        send_buffers_[static_cast<size_t>(level)].write(data, len);

        // Notify connection
        if (on_crypto_data_) {
            on_crypto_data_(level, data, len);
        }
    }

    /**
     * Called when TLS provides a secret.
     */
    void on_secret_available(EncryptionLevel level,
                              const uint8_t* secret, size_t secret_len,
                              bool is_write) noexcept {
        size_t idx = static_cast<size_t>(level);

        // Derive packet keys from secret
        PacketProtectionKeys keys;
        if (derive_packet_keys(secret, secret_len, AeadAlgorithm::AES_128_GCM, keys) != 0) {
            state_ = HandshakeState::FAILED;
            return;
        }

        // Initialize protection
        // Index: 0 = write keys, 1 = read keys
        size_t key_idx = is_write ? 0 : 1;
        if (protectors_[idx][key_idx].initialize(keys) != 0) {
            state_ = HandshakeState::FAILED;
            return;
        }

        // Mark keys as available when we have both read and write
        if (protectors_[idx][0].is_initialized() && protectors_[idx][1].is_initialized()) {
            keys_available_[idx] = true;

            // Notify connection
            if (on_keys_available_) {
                on_keys_available_(level, protectors_[idx][is_server_ ? 0 : 1]);
            }

            // Update state
            if (level == EncryptionLevel::HANDSHAKE) {
                state_ = HandshakeState::HANDSHAKE_KEYS_AVAILABLE;
            }
        }
    }

    /**
     * Called when TLS handshake completes.
     */
    void on_tls_handshake_complete() noexcept {
        handshake_complete_ = true;
        state_ = HandshakeState::HANDSHAKE_COMPLETE;

        if (on_handshake_complete_) {
            on_handshake_complete_();
        }
    }

    /**
     * Called when TLS generates an alert.
     */
    void on_tls_alert(TlsAlert alert) noexcept {
        error_code_ = tls_alert_to_quic_error(alert);
        error_reason_ = "TLS alert: " + std::to_string(static_cast<int>(alert));
        state_ = HandshakeState::FAILED;

        if (on_error_) {
            on_error_(error_code_, error_reason_);
        }
    }

    /**
     * Update state after receiving CRYPTO data.
     */
    void update_state_after_receive(EncryptionLevel level) noexcept {
        switch (state_) {
            case HandshakeState::IDLE:
            case HandshakeState::INITIAL_SENT:
                if (level == EncryptionLevel::INITIAL) {
                    state_ = HandshakeState::INITIAL_RECEIVED;
                }
                break;

            case HandshakeState::INITIAL_RECEIVED:
            case HandshakeState::HANDSHAKE_KEYS_AVAILABLE:
                if (level == EncryptionLevel::HANDSHAKE) {
                    state_ = HandshakeState::HANDSHAKE_RECEIVED;
                }
                break;

            default:
                break;
        }
    }

    HandshakeState state_;
    bool is_server_;
    bool handshake_complete_;
    bool handshake_confirmed_;
    uint64_t error_code_;
    std::string error_reason_;

    QUICTls tls_;

    // Crypto buffers for receiving (one per encryption level)
    std::array<CryptoBuffer, static_cast<size_t>(EncryptionLevel::NUM_LEVELS)> crypto_buffers_;

    // Send buffers for outgoing CRYPTO data
    std::array<CryptoBuffer, static_cast<size_t>(EncryptionLevel::NUM_LEVELS)> send_buffers_;

    // Packet protection (2 per level: [0]=write, [1]=read, except Initial: [0]=client, [1]=server)
    std::array<std::array<PacketProtection, 2>, static_cast<size_t>(EncryptionLevel::NUM_LEVELS)> protectors_;

    // Keys available flags
    std::array<bool, static_cast<size_t>(EncryptionLevel::NUM_LEVELS)> keys_available_{};

    // Callbacks
    OnCryptoDataCallback on_crypto_data_;
    OnKeysAvailableCallback on_keys_available_;
    OnHandshakeCompleteCallback on_handshake_complete_;
    OnHandshakeConfirmedCallback on_handshake_confirmed_;
    OnErrorCallback on_error_;
};

} // namespace quic
} // namespace fasterapi
