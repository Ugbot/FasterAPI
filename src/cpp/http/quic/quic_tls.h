#pragma once

/**
 * @file quic_tls.h
 * @brief QUIC-TLS Integration for quictls (OpenSSL + BoringSSL QUIC API)
 *
 * Provides TLS 1.3 support for QUIC using quictls's BoringSSL-style QUIC API.
 * This uses SSL_QUIC_METHOD callbacks for custom QUIC implementations.
 */

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <array>
#include <vector>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/rand.h>

#include "quic_crypto_buffer.h"
#include "quic_packet_protection.h"

namespace fasterapi {
namespace quic {

/**
 * TLS alert codes relevant to QUIC.
 */
enum class TlsAlert : uint8_t {
    CLOSE_NOTIFY = 0,
    UNEXPECTED_MESSAGE = 10,
    BAD_RECORD_MAC = 20,
    HANDSHAKE_FAILURE = 40,
    BAD_CERTIFICATE = 42,
    UNSUPPORTED_CERTIFICATE = 43,
    CERTIFICATE_REVOKED = 44,
    CERTIFICATE_EXPIRED = 45,
    CERTIFICATE_UNKNOWN = 46,
    ILLEGAL_PARAMETER = 47,
    UNKNOWN_CA = 48,
    ACCESS_DENIED = 49,
    DECODE_ERROR = 50,
    DECRYPT_ERROR = 51,
    PROTOCOL_VERSION = 70,
    INSUFFICIENT_SECURITY = 71,
    INTERNAL_ERROR = 80,
    INAPPROPRIATE_FALLBACK = 86,
    USER_CANCELED = 90,
    MISSING_EXTENSION = 109,
    UNSUPPORTED_EXTENSION = 110,
    CERTIFICATE_REQUIRED = 116,
    NO_APPLICATION_PROTOCOL = 120
};

/**
 * Convert TLS alert to QUIC transport error code.
 * QUIC error = 0x100 + TLS alert
 */
inline uint64_t tls_alert_to_quic_error(TlsAlert alert) noexcept {
    return 0x100 + static_cast<uint64_t>(alert);
}

/**
 * Callback types for TLS events.
 */
// Combined secret callback (receives both secrets at once)
using OnSecretCallback = std::function<void(EncryptionLevel level,
                                             const uint8_t* read_secret,
                                             const uint8_t* write_secret,
                                             size_t secret_len)>;

// Separate secret callbacks (for HandshakeManager compatibility)
using OnWriteSecretCallback = std::function<void(EncryptionLevel level,
                                                  const uint8_t* secret,
                                                  size_t secret_len,
                                                  bool is_write)>;

using OnReadSecretCallback = std::function<void(EncryptionLevel level,
                                                 const uint8_t* secret,
                                                 size_t secret_len,
                                                 bool is_write)>;

using OnCryptoDataCallback = std::function<void(EncryptionLevel level,
                                                 const uint8_t* data,
                                                 size_t data_len)>;

using OnAlertCallback = std::function<void(TlsAlert alert)>;

using OnHandshakeCompleteCallback = std::function<void()>;

using OnFlushCallback = std::function<void()>;

/**
 * Generate a self-signed certificate for testing.
 *
 * @param cert_out Output: X509 certificate
 * @param key_out Output: EVP_PKEY private key
 * @param cn Common name for the certificate
 * @param days Validity period in days
 * @return 0 on success, -1 on failure
 */
inline int generate_self_signed_cert(X509** cert_out, EVP_PKEY** key_out,
                                      const char* cn = "localhost",
                                      int days = 365) {
    EVP_PKEY* pkey = nullptr;
    X509* x509 = nullptr;
    EVP_PKEY_CTX* pctx = nullptr;
    int ret = -1;

    // Generate EC key (P-256)
    pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (!pctx) goto cleanup;

    if (EVP_PKEY_keygen_init(pctx) <= 0) goto cleanup;
    if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1) <= 0) goto cleanup;
    if (EVP_PKEY_keygen(pctx, &pkey) <= 0) goto cleanup;

    // Create X509 certificate
    x509 = X509_new();
    if (!x509) goto cleanup;

    // Set version (v3)
    X509_set_version(x509, 2);

    // Set serial number (random)
    {
        ASN1_INTEGER* serial = ASN1_INTEGER_new();
        uint64_t serial_num;
        RAND_bytes(reinterpret_cast<uint8_t*>(&serial_num), sizeof(serial_num));
        ASN1_INTEGER_set_uint64(serial, serial_num);
        X509_set_serialNumber(x509, serial);
        ASN1_INTEGER_free(serial);
    }

    // Set validity period
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 60 * 60 * 24 * days);

    // Set public key
    X509_set_pubkey(x509, pkey);

    // Set subject and issuer (self-signed)
    {
        X509_NAME* name = X509_get_subject_name(x509);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                    reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
        X509_set_issuer_name(x509, name);
    }

    // Add Subject Alternative Name extension
    {
        X509V3_CTX v3ctx;
        X509V3_set_ctx_nodb(&v3ctx);
        X509V3_set_ctx(&v3ctx, x509, x509, nullptr, nullptr, 0);

        std::string san = std::string("DNS:") + cn + ",DNS:localhost,IP:127.0.0.1,IP:::1";
        X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &v3ctx, NID_subject_alt_name, san.c_str());
        if (ext) {
            X509_add_ext(x509, ext, -1);
            X509_EXTENSION_free(ext);
        }

        // Add Basic Constraints (CA:FALSE)
        ext = X509V3_EXT_conf_nid(nullptr, &v3ctx, NID_basic_constraints, "CA:FALSE");
        if (ext) {
            X509_add_ext(x509, ext, -1);
            X509_EXTENSION_free(ext);
        }
    }

    // Sign the certificate
    if (X509_sign(x509, pkey, EVP_sha256()) == 0) goto cleanup;

    *cert_out = x509;
    *key_out = pkey;
    ret = 0;
    x509 = nullptr;
    pkey = nullptr;

cleanup:
    if (pctx) EVP_PKEY_CTX_free(pctx);
    if (x509) X509_free(x509);
    if (pkey) EVP_PKEY_free(pkey);
    return ret;
}

/**
 * QUIC TLS connection handler using quictls BoringSSL-style API.
 *
 * This class manages TLS 1.3 for QUIC, providing:
 * - Encryption secrets for each level
 * - CRYPTO frame data to send
 * - Alert handling
 *
 * The application is responsible for:
 * - QUIC packet framing
 * - Connection management
 * - Stream multiplexing
 * - Packet encryption/decryption (using secrets from callbacks)
 */
class QUICTLSConnection {
public:
    QUICTLSConnection()
        : ssl_(nullptr)
        , ctx_(nullptr)
        , is_server_(false)
        , handshake_complete_(false)
        , owns_ctx_(false) {}

    ~QUICTLSConnection() {
        if (ssl_) {
            SSL_free(ssl_);
        }
        if (owns_ctx_ && ctx_) {
            SSL_CTX_free(ctx_);
        }
    }

    // Non-copyable
    QUICTLSConnection(const QUICTLSConnection&) = delete;
    QUICTLSConnection& operator=(const QUICTLSConnection&) = delete;

    /**
     * Initialize with existing SSL_CTX.
     * This is the primary initialization method used by HandshakeManager.
     *
     * @param ssl_ctx SSL context (ownership not transferred)
     * @param is_server True for server mode, false for client mode
     * @return 0 on success, -1 on failure
     */
    int initialize(SSL_CTX* ssl_ctx, bool is_server) {
        ctx_ = ssl_ctx;
        owns_ctx_ = false;
        is_server_ = is_server;

        ssl_ = SSL_new(ctx_);
        if (!ssl_) {
            return -1;
        }

        if (is_server) {
            SSL_set_accept_state(ssl_);
        } else {
            SSL_set_connect_state(ssl_);
        }

        // Store self pointer for callbacks
        SSL_set_ex_data(ssl_, get_ex_data_index(), this);

        // Install QUIC method
        if (SSL_set_quic_method(ssl_, get_quic_method()) != 1) {
            return -1;
        }

        return 0;
    }

    /**
     * Set callback for encryption secrets.
     * Called when new secrets are available for a given encryption level.
     */
    void set_secret_callback(OnSecretCallback cb) {
        on_secret_ = std::move(cb);
    }

    /**
     * Set callback for write secrets (HandshakeManager compatibility).
     * Called when a new write secret is available.
     */
    void set_write_secret_callback(OnWriteSecretCallback cb) {
        on_write_secret_ = std::move(cb);
    }

    /**
     * Set callback for read secrets (HandshakeManager compatibility).
     * Called when a new read secret is available.
     */
    void set_read_secret_callback(OnReadSecretCallback cb) {
        on_read_secret_ = std::move(cb);
    }

    /**
     * Set callback for CRYPTO frame data.
     * Called when TLS produces handshake data to send.
     */
    void set_crypto_data_callback(OnCryptoDataCallback cb) {
        on_crypto_data_ = std::move(cb);
    }

    /**
     * Set callback for TLS alerts.
     */
    void set_alert_callback(OnAlertCallback cb) {
        on_alert_ = std::move(cb);
    }

    /**
     * Set callback for handshake completion.
     */
    void set_handshake_complete_callback(OnHandshakeCompleteCallback cb) {
        on_handshake_complete_ = std::move(cb);
    }

    /**
     * Set callback for flush signals (end of flight).
     */
    void set_flush_callback(OnFlushCallback cb) {
        on_flush_ = std::move(cb);
    }

    /**
     * Initialize as QUIC server with self-signed certificate.
     *
     * @return 0 on success, -1 on failure
     */
    int init_server_self_signed() {
        X509* cert = nullptr;
        EVP_PKEY* key = nullptr;

        if (generate_self_signed_cert(&cert, &key) != 0) {
            return -1;
        }

        ctx_ = SSL_CTX_new(TLS_server_method());
        if (!ctx_) {
            X509_free(cert);
            EVP_PKEY_free(key);
            return -1;
        }
        owns_ctx_ = true;

        // TLS 1.3 only
        SSL_CTX_set_min_proto_version(ctx_, TLS1_3_VERSION);
        SSL_CTX_set_max_proto_version(ctx_, TLS1_3_VERSION);

        if (SSL_CTX_use_certificate(ctx_, cert) != 1 ||
            SSL_CTX_use_PrivateKey(ctx_, key) != 1) {
            X509_free(cert);
            EVP_PKEY_free(key);
            return -1;
        }

        X509_free(cert);
        EVP_PKEY_free(key);

        return init_server_common();
    }

    /**
     * Initialize as QUIC server with certificate files.
     *
     * @param cert_file Path to certificate PEM file
     * @param key_file Path to private key PEM file
     * @return 0 on success, -1 on failure
     */
    int init_server(const char* cert_file, const char* key_file) {
        ctx_ = SSL_CTX_new(TLS_server_method());
        if (!ctx_) {
            return -1;
        }
        owns_ctx_ = true;

        // TLS 1.3 only
        SSL_CTX_set_min_proto_version(ctx_, TLS1_3_VERSION);
        SSL_CTX_set_max_proto_version(ctx_, TLS1_3_VERSION);

        if (SSL_CTX_use_certificate_file(ctx_, cert_file, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_use_PrivateKey_file(ctx_, key_file, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(ctx_) != 1) {
            return -1;
        }

        return init_server_common();
    }

    /**
     * Initialize as QUIC server with existing SSL_CTX.
     *
     * @param ctx SSL_CTX (ownership not transferred)
     * @return 0 on success, -1 on failure
     */
    int init_server(SSL_CTX* ctx) {
        ctx_ = ctx;
        owns_ctx_ = false;
        return init_server_common();
    }

    /**
     * Initialize as QUIC client.
     *
     * @param hostname Server hostname for SNI
     * @return 0 on success, -1 on failure
     */
    int init_client(const char* hostname = nullptr) {
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) {
            return -1;
        }
        owns_ctx_ = true;

        // TLS 1.3 only
        SSL_CTX_set_min_proto_version(ctx_, TLS1_3_VERSION);
        SSL_CTX_set_max_proto_version(ctx_, TLS1_3_VERSION);

        ssl_ = SSL_new(ctx_);
        if (!ssl_) {
            return -1;
        }

        is_server_ = false;
        SSL_set_connect_state(ssl_);

        if (hostname) {
            SSL_set_tlsext_host_name(ssl_, hostname);
        }

        // Store self pointer for callbacks
        SSL_set_ex_data(ssl_, get_ex_data_index(), this);

        // Install QUIC method
        if (SSL_set_quic_method(ssl_, get_quic_method()) != 1) {
            return -1;
        }

        // Enable keylog for debugging
        SSL_CTX_set_keylog_callback(ctx_, keylog_callback);

        return 0;
    }

    /**
     * Set ALPN protocols for negotiation.
     *
     * @param protos Wire-format ALPN list (length-prefixed strings)
     * @param protos_len Length of protos
     * @return 0 on success, -1 on failure
     */
    int set_alpn(const uint8_t* protos, size_t protos_len) {
        if (!ssl_) return -1;
        return SSL_set_alpn_protos(ssl_, protos, static_cast<unsigned int>(protos_len)) == 0 ? 0 : -1;
    }

    /**
     * Get negotiated ALPN protocol.
     */
    std::string get_alpn() const {
        if (!ssl_) return "";

        const uint8_t* data;
        unsigned int len;
        SSL_get0_alpn_selected(ssl_, &data, &len);

        if (data && len > 0) {
            return std::string(reinterpret_cast<const char*>(data), len);
        }
        return "";
    }

    /**
     * Set QUIC transport parameters to send.
     *
     * @param params Encoded transport parameters
     * @param params_len Length of params
     * @return 0 on success, -1 on failure
     */
    int set_transport_params(const uint8_t* params, size_t params_len) {
        if (!ssl_) return -1;
        return SSL_set_quic_transport_params(ssl_, params, params_len) == 1 ? 0 : -1;
    }

    /**
     * Get peer's QUIC transport parameters.
     *
     * @param out_params Output: pointer to params (valid until SSL object freed)
     * @param out_len Output: length of params
     */
    void get_peer_transport_params(const uint8_t** out_params, size_t* out_len) const {
        if (!ssl_) {
            *out_params = nullptr;
            *out_len = 0;
            return;
        }
        SSL_get_peer_quic_transport_params(ssl_, out_params, out_len);
    }

    /**
     * Provide received CRYPTO frame data to TLS.
     *
     * @param level Encryption level the data was received at
     * @param data CRYPTO frame data
     * @param data_len Length of data
     * @return 0 on success, -1 on failure
     */
    int provide_data(EncryptionLevel level, const uint8_t* data, size_t data_len) {
        if (!ssl_) return -1;

        OSSL_ENCRYPTION_LEVEL ossl_level = to_ossl_level(level);
        if (SSL_provide_quic_data(ssl_, ossl_level, data, data_len) != 1) {
            return -1;
        }
        return 0;
    }

    /**
     * Alias for provide_data (HandshakeManager compatibility).
     */
    int provide_crypto_data(EncryptionLevel level, const uint8_t* data, size_t data_len) {
        return provide_data(level, data, data_len);
    }

    /**
     * Advance the TLS handshake.
     *
     * @return 1 if handshake complete, 0 if in progress, -1 on error
     */
    int do_handshake() {
        if (!ssl_) return -1;

        int ret = SSL_do_handshake(ssl_);
        if (ret == 1) {
            if (!handshake_complete_) {
                handshake_complete_ = true;
                if (on_handshake_complete_) {
                    on_handshake_complete_();
                }
            }
            return 1;
        }

        int err = SSL_get_error(ssl_, ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            return 0;  // In progress
        }

        return -1;  // Error
    }

    /**
     * Alias for do_handshake (HandshakeManager compatibility).
     *
     * @return 1 if handshake complete, 0 if in progress, -1 on error
     */
    int advance_handshake() {
        return do_handshake();
    }

    /**
     * Process post-handshake messages (e.g., NewSessionTicket).
     *
     * @return 1 on success, 0 if would block, -1 on error
     */
    int process_post_handshake() {
        if (!ssl_) return -1;
        return SSL_process_quic_post_handshake(ssl_);
    }

    /**
     * Check if handshake is complete.
     */
    bool is_handshake_complete() const { return handshake_complete_; }

    /**
     * Check if this is a server connection.
     */
    bool is_server() const { return is_server_; }

    /**
     * Get current read encryption level.
     */
    EncryptionLevel read_level() const {
        if (!ssl_) return EncryptionLevel::INITIAL;
        return from_ossl_level(SSL_quic_read_level(ssl_));
    }

    /**
     * Get current write encryption level.
     */
    EncryptionLevel write_level() const {
        if (!ssl_) return EncryptionLevel::INITIAL;
        return from_ossl_level(SSL_quic_write_level(ssl_));
    }

    /**
     * Get underlying SSL object.
     */
    SSL* ssl() const { return ssl_; }

    /**
     * Get underlying SSL_CTX.
     */
    SSL_CTX* ctx() const { return ctx_; }

private:
    int init_server_common() {
        ssl_ = SSL_new(ctx_);
        if (!ssl_) {
            return -1;
        }

        is_server_ = true;
        SSL_set_accept_state(ssl_);

        // Store self pointer for callbacks
        SSL_set_ex_data(ssl_, get_ex_data_index(), this);

        // Install QUIC method
        if (SSL_set_quic_method(ssl_, get_quic_method()) != 1) {
            return -1;
        }

        // Enable keylog for debugging
        SSL_CTX_set_keylog_callback(ctx_, keylog_callback);

        // Set ALPN callback for server
        SSL_CTX_set_alpn_select_cb(ctx_, alpn_select_callback, this);

        return 0;
    }

    static int get_ex_data_index() {
        static int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
        return index;
    }

    static QUICTLSConnection* get_from_ssl(SSL* ssl) {
        return static_cast<QUICTLSConnection*>(SSL_get_ex_data(ssl, get_ex_data_index()));
    }

    static OSSL_ENCRYPTION_LEVEL to_ossl_level(EncryptionLevel level) {
        switch (level) {
            case EncryptionLevel::INITIAL:   return ssl_encryption_initial;
            case EncryptionLevel::ZERO_RTT:  return ssl_encryption_early_data;
            case EncryptionLevel::HANDSHAKE: return ssl_encryption_handshake;
            case EncryptionLevel::ONE_RTT:   return ssl_encryption_application;
            default:                         return ssl_encryption_initial;
        }
    }

    static EncryptionLevel from_ossl_level(OSSL_ENCRYPTION_LEVEL level) {
        switch (level) {
            case ssl_encryption_initial:     return EncryptionLevel::INITIAL;
            case ssl_encryption_early_data:  return EncryptionLevel::ZERO_RTT;
            case ssl_encryption_handshake:   return EncryptionLevel::HANDSHAKE;
            case ssl_encryption_application: return EncryptionLevel::ONE_RTT;
            default:                         return EncryptionLevel::INITIAL;
        }
    }

    // SSL_QUIC_METHOD callbacks
    static int set_encryption_secrets_cb(SSL* ssl, OSSL_ENCRYPTION_LEVEL level,
                                          const uint8_t* read_secret,
                                          const uint8_t* write_secret,
                                          size_t secret_len) {
        QUICTLSConnection* conn = get_from_ssl(ssl);
        if (!conn) return 1;

        EncryptionLevel our_level = from_ossl_level(level);

        // Call combined callback if set
        if (conn->on_secret_) {
            conn->on_secret_(our_level, read_secret, write_secret, secret_len);
        }

        // Call separate callbacks for HandshakeManager compatibility
        if (write_secret && conn->on_write_secret_) {
            conn->on_write_secret_(our_level, write_secret, secret_len, true);
        }
        if (read_secret && conn->on_read_secret_) {
            conn->on_read_secret_(our_level, read_secret, secret_len, false);
        }

        return 1;
    }

    static int add_handshake_data_cb(SSL* ssl, OSSL_ENCRYPTION_LEVEL level,
                                      const uint8_t* data, size_t len) {
        QUICTLSConnection* conn = get_from_ssl(ssl);
        if (!conn || !conn->on_crypto_data_) return 1;

        EncryptionLevel our_level = from_ossl_level(level);
        conn->on_crypto_data_(our_level, data, len);
        return 1;
    }

    static int flush_flight_cb(SSL* ssl) {
        QUICTLSConnection* conn = get_from_ssl(ssl);
        if (conn && conn->on_flush_) {
            conn->on_flush_();
        }
        return 1;
    }

    static int send_alert_cb(SSL* ssl, enum ssl_encryption_level_t level, uint8_t alert) {
        QUICTLSConnection* conn = get_from_ssl(ssl);
        if (conn && conn->on_alert_) {
            conn->on_alert_(static_cast<TlsAlert>(alert));
        }
        return 1;
    }

    static const SSL_QUIC_METHOD* get_quic_method() {
        static SSL_QUIC_METHOD method = {
            set_encryption_secrets_cb,
            add_handshake_data_cb,
            flush_flight_cb,
            send_alert_cb
        };
        return &method;
    }

    static int alpn_select_callback(SSL* ssl, const unsigned char** out,
                                     unsigned char* outlen,
                                     const unsigned char* in, unsigned int inlen,
                                     void* arg) {
        // Prefer h3, then h3-29
        static const uint8_t h3[] = {'h', '3'};
        static const uint8_t h3_29[] = {'h', '3', '-', '2', '9'};

        const unsigned char* p = in;
        const unsigned char* end = in + inlen;

        while (p < end) {
            uint8_t len = *p++;
            if (p + len > end) break;

            if (len == 2 && memcmp(p, h3, 2) == 0) {
                *out = p;
                *outlen = len;
                return SSL_TLSEXT_ERR_OK;
            }
            if (len == 5 && memcmp(p, h3_29, 5) == 0) {
                *out = p;
                *outlen = len;
                return SSL_TLSEXT_ERR_OK;
            }
            p += len;
        }

        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }

    static void keylog_callback(const SSL* ssl, const char* line) {
        const char* keylog_file = getenv("SSLKEYLOGFILE");
        if (keylog_file) {
            FILE* f = fopen(keylog_file, "a");
            if (f) {
                fprintf(f, "%s\n", line);
                fclose(f);
            }
        }
    }

    SSL* ssl_;
    SSL_CTX* ctx_;
    bool is_server_;
    bool handshake_complete_;
    bool owns_ctx_;

    OnSecretCallback on_secret_;
    OnWriteSecretCallback on_write_secret_;
    OnReadSecretCallback on_read_secret_;
    OnCryptoDataCallback on_crypto_data_;
    OnAlertCallback on_alert_;
    OnHandshakeCompleteCallback on_handshake_complete_;
    OnFlushCallback on_flush_;
};

// Backward compatibility alias
using QUICTls = QUICTLSConnection;

/**
 * Create SSL_CTX configured for QUIC server.
 *
 * @param cert_file Path to certificate file (PEM)
 * @param key_file Path to private key file (PEM)
 * @param alpn_protos ALPN protocols to advertise
 * @param alpn_protos_len Length of ALPN protocols
 * @return SSL_CTX pointer, or nullptr on failure
 */
inline SSL_CTX* create_quic_ssl_ctx(const char* cert_file, const char* key_file,
                                     const uint8_t* alpn_protos, size_t alpn_protos_len) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        return nullptr;
    }

    // TLS 1.3 only (required for QUIC)
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);

    // Load certificate
    if (cert_file && SSL_CTX_use_certificate_file(ctx, cert_file, SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx);
        return nullptr;
    }

    // Load private key
    if (key_file && SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx);
        return nullptr;
    }

    // Verify key matches certificate
    if (cert_file && key_file && SSL_CTX_check_private_key(ctx) != 1) {
        SSL_CTX_free(ctx);
        return nullptr;
    }

    // Set ALPN callback
    SSL_CTX_set_alpn_select_cb(ctx, [](SSL* ssl, const unsigned char** out,
                                        unsigned char* outlen,
                                        const unsigned char* in, unsigned int inlen,
                                        void* arg) -> int {
        // Prefer h3, then h3-29
        static const uint8_t h3[] = {'h', '3'};
        static const uint8_t h3_29[] = {'h', '3', '-', '2', '9'};

        const unsigned char* p = in;
        const unsigned char* end = in + inlen;

        while (p < end) {
            uint8_t len = *p++;
            if (p + len > end) break;

            if (len == 2 && memcmp(p, h3, 2) == 0) {
                *out = p;
                *outlen = len;
                return SSL_TLSEXT_ERR_OK;
            }
            if (len == 5 && memcmp(p, h3_29, 5) == 0) {
                *out = p;
                *outlen = len;
                return SSL_TLSEXT_ERR_OK;
            }
            p += len;
        }
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }, nullptr);

    // Disable session caching for now
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);

    // Enable keylog for debugging
    SSL_CTX_set_keylog_callback(ctx, [](const SSL* ssl, const char* line) {
        const char* keylog_file = getenv("SSLKEYLOGFILE");
        if (keylog_file) {
            FILE* f = fopen(keylog_file, "a");
            if (f) {
                fprintf(f, "%s\n", line);
                fclose(f);
            }
        }
    });

    return ctx;
}

/**
 * Create SSL_CTX for QUIC server with auto-generated self-signed certificate.
 *
 * @param alpn_protos ALPN protocols to advertise
 * @param alpn_protos_len Length of ALPN protocols
 * @return SSL_CTX pointer, or nullptr on failure
 */
inline SSL_CTX* create_quic_ssl_ctx_self_signed(const uint8_t* alpn_protos, size_t alpn_protos_len) {
    X509* cert = nullptr;
    EVP_PKEY* key = nullptr;

    if (generate_self_signed_cert(&cert, &key) != 0) {
        return nullptr;
    }

    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        X509_free(cert);
        EVP_PKEY_free(key);
        return nullptr;
    }

    // TLS 1.3 only (required for QUIC)
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);

    // Use the generated certificate
    if (SSL_CTX_use_certificate(ctx, cert) != 1) {
        SSL_CTX_free(ctx);
        X509_free(cert);
        EVP_PKEY_free(key);
        return nullptr;
    }

    if (SSL_CTX_use_PrivateKey(ctx, key) != 1) {
        SSL_CTX_free(ctx);
        X509_free(cert);
        EVP_PKEY_free(key);
        return nullptr;
    }

    X509_free(cert);
    EVP_PKEY_free(key);

    // Set ALPN callback
    SSL_CTX_set_alpn_select_cb(ctx, [](SSL* ssl, const unsigned char** out,
                                        unsigned char* outlen,
                                        const unsigned char* in, unsigned int inlen,
                                        void* arg) -> int {
        static const uint8_t h3[] = {'h', '3'};
        static const uint8_t h3_29[] = {'h', '3', '-', '2', '9'};

        const unsigned char* p = in;
        const unsigned char* end = in + inlen;

        while (p < end) {
            uint8_t len = *p++;
            if (p + len > end) break;

            if (len == 2 && memcmp(p, h3, 2) == 0) {
                *out = p;
                *outlen = len;
                return SSL_TLSEXT_ERR_OK;
            }
            if (len == 5 && memcmp(p, h3_29, 5) == 0) {
                *out = p;
                *outlen = len;
                return SSL_TLSEXT_ERR_OK;
            }
            p += len;
        }
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }, nullptr);

    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);

    SSL_CTX_set_keylog_callback(ctx, [](const SSL* ssl, const char* line) {
        const char* keylog_file = getenv("SSLKEYLOGFILE");
        if (keylog_file) {
            FILE* f = fopen(keylog_file, "a");
            if (f) {
                fprintf(f, "%s\n", line);
                fclose(f);
            }
        }
    });

    return ctx;
}

/**
 * ALPN protocol identifiers.
 */
namespace alpn {
    // HTTP/3
    static constexpr uint8_t kH3[] = {2, 'h', '3'};
    static constexpr size_t kH3Len = sizeof(kH3);

    // HTTP/3 draft-29 (for older implementations)
    static constexpr uint8_t kH3_29[] = {5, 'h', '3', '-', '2', '9'};
    static constexpr size_t kH3_29Len = sizeof(kH3_29);

    // Combined h3 + h3-29 for server
    static constexpr uint8_t kH3All[] = {2, 'h', '3', 5, 'h', '3', '-', '2', '9'};
    static constexpr size_t kH3AllLen = sizeof(kH3All);
}

} // namespace quic
} // namespace fasterapi
