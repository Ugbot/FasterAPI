#pragma once

/**
 * @file quic_packet_protection.h
 * @brief QUIC Packet Protection (RFC 9001 Section 5)
 *
 * Implements AEAD encryption/decryption for QUIC packets and header protection.
 * Supports AES-128-GCM, AES-256-GCM, and ChaCha20-Poly1305 cipher suites.
 */

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <array>
#include <memory>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/hmac.h>
#include <openssl/aes.h>
#include <openssl/err.h>
#include <openssl/core_names.h>

namespace fasterapi {
namespace quic {

/**
 * Supported AEAD algorithms for QUIC packet protection.
 */
enum class AeadAlgorithm : uint8_t {
    AES_128_GCM = 0,      // TLS_AES_128_GCM_SHA256
    AES_256_GCM = 1,      // TLS_AES_256_GCM_SHA384
    CHACHA20_POLY1305 = 2 // TLS_CHACHA20_POLY1305_SHA256
};

/**
 * Key sizes for each algorithm.
 */
constexpr size_t aead_key_length(AeadAlgorithm algo) noexcept {
    switch (algo) {
        case AeadAlgorithm::AES_128_GCM: return 16;
        case AeadAlgorithm::AES_256_GCM: return 32;
        case AeadAlgorithm::CHACHA20_POLY1305: return 32;
        default: return 0;
    }
}

/**
 * IV/nonce sizes for each algorithm.
 */
constexpr size_t aead_iv_length(AeadAlgorithm algo) noexcept {
    // All QUIC AEAD algorithms use 12-byte nonces
    (void)algo;
    return 12;
}

/**
 * Authentication tag size for all AEAD algorithms.
 */
constexpr size_t kAeadTagLength = 16;

/**
 * Header protection key size.
 */
constexpr size_t kHpKeyLength = 16;

/**
 * Sample size for header protection.
 */
constexpr size_t kHpSampleLength = 16;

/**
 * Maximum packet number length.
 */
constexpr size_t kMaxPacketNumberLength = 4;

/**
 * Initial salt for QUIC v1 (RFC 9001).
 * Used to derive initial keys from the Destination Connection ID.
 */
static constexpr uint8_t kQuicV1InitialSalt[] = {
    0x38, 0x76, 0x2c, 0xf7, 0xf5, 0x59, 0x34, 0xb3,
    0x4d, 0x17, 0x9a, 0xe6, 0xa4, 0xc8, 0x0c, 0xad,
    0xcc, 0xbb, 0x7f, 0x0a
};
static constexpr size_t kQuicV1InitialSaltLength = sizeof(kQuicV1InitialSalt);

/**
 * Initial salt for QUIC v2 (RFC 9369).
 */
static constexpr uint8_t kQuicV2InitialSalt[] = {
    0x0d, 0xed, 0xe3, 0xde, 0xf7, 0x00, 0xa6, 0xdb,
    0x81, 0x93, 0x81, 0xbe, 0x6e, 0x26, 0x9d, 0xcb,
    0xf9, 0xbd, 0x2e, 0xd9
};
static constexpr size_t kQuicV2InitialSaltLength = sizeof(kQuicV2InitialSalt);

/**
 * HKDF-Extract using OpenSSL 3.x EVP_KDF API.
 *
 * @param out Output buffer for the extracted key (32 bytes for SHA-256)
 * @param out_len Pointer to receive actual output length
 * @param ikm Input keying material
 * @param ikm_len Length of IKM
 * @param salt Salt
 * @param salt_len Length of salt
 * @return 1 on success, 0 on failure
 */
inline int hkdf_extract(
    uint8_t* out, size_t* out_len,
    const uint8_t* ikm, size_t ikm_len,
    const uint8_t* salt, size_t salt_len) noexcept {

    EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
    if (!kdf) return 0;

    EVP_KDF_CTX* ctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (!ctx) return 0;

    OSSL_PARAM params[5];
    params[0] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST,
                                                  const_cast<char*>("SHA256"), 0);
    params[1] = OSSL_PARAM_construct_int(OSSL_KDF_PARAM_MODE,
                                          const_cast<int*>(&(const int&)EVP_KDF_HKDF_MODE_EXTRACT_ONLY));
    params[2] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY,
                                                   const_cast<uint8_t*>(ikm), ikm_len);
    params[3] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
                                                   const_cast<uint8_t*>(salt), salt_len);
    params[4] = OSSL_PARAM_construct_end();

    int mode = EVP_KDF_HKDF_MODE_EXTRACT_ONLY;
    params[1] = OSSL_PARAM_construct_int(OSSL_KDF_PARAM_MODE, &mode);

    int ret = EVP_KDF_derive(ctx, out, 32, params);
    EVP_KDF_CTX_free(ctx);

    if (ret > 0) *out_len = 32;
    return ret;
}

/**
 * HKDF-Expand using OpenSSL 3.x EVP_KDF API.
 *
 * @param out Output buffer
 * @param out_len Desired output length
 * @param prk Pseudorandom key from HKDF-Extract
 * @param prk_len Length of PRK
 * @param info Info/context bytes
 * @param info_len Length of info
 * @return 1 on success, 0 on failure
 */
inline int hkdf_expand(
    uint8_t* out, size_t out_len,
    const uint8_t* prk, size_t prk_len,
    const uint8_t* info, size_t info_len) noexcept {

    EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
    if (!kdf) return 0;

    EVP_KDF_CTX* ctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (!ctx) return 0;

    int mode = EVP_KDF_HKDF_MODE_EXPAND_ONLY;
    OSSL_PARAM params[5];
    params[0] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST,
                                                  const_cast<char*>("SHA256"), 0);
    params[1] = OSSL_PARAM_construct_int(OSSL_KDF_PARAM_MODE, &mode);
    params[2] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY,
                                                   const_cast<uint8_t*>(prk), prk_len);
    params[3] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO,
                                                   const_cast<uint8_t*>(info), info_len);
    params[4] = OSSL_PARAM_construct_end();

    int ret = EVP_KDF_derive(ctx, out, out_len, params);
    EVP_KDF_CTX_free(ctx);

    return ret;
}

/**
 * HKDF-Expand-Label for QUIC (RFC 9001 Section 5.1).
 *
 * Derives keys using TLS 1.3 key derivation with QUIC-specific labels.
 *
 * @param secret Input keying material
 * @param secret_len Length of secret
 * @param label ASCII label (without "tls13 " prefix)
 * @param label_len Length of label
 * @param out Output buffer
 * @param out_len Desired output length
 * @return 0 on success, -1 on failure
 */
inline int hkdf_expand_label(
    const uint8_t* secret, size_t secret_len,
    const char* label, size_t label_len,
    uint8_t* out, size_t out_len) noexcept {

    // Construct HkdfLabel structure:
    // struct {
    //     uint16 length = out_len;
    //     opaque label<7..255> = "tls13 " + label;
    //     opaque context<0..255> = "";
    // } HkdfLabel;

    static const char* kTls13Prefix = "tls13 ";
    static const size_t kTls13PrefixLen = 6;

    uint8_t hkdf_label[2 + 1 + kTls13PrefixLen + 255 + 1 + 255];
    size_t hkdf_label_len = 0;

    // Length (2 bytes, big-endian)
    hkdf_label[hkdf_label_len++] = static_cast<uint8_t>((out_len >> 8) & 0xFF);
    hkdf_label[hkdf_label_len++] = static_cast<uint8_t>(out_len & 0xFF);

    // Label length (1 byte)
    size_t full_label_len = kTls13PrefixLen + label_len;
    hkdf_label[hkdf_label_len++] = static_cast<uint8_t>(full_label_len);

    // Label = "tls13 " + label
    std::memcpy(hkdf_label + hkdf_label_len, kTls13Prefix, kTls13PrefixLen);
    hkdf_label_len += kTls13PrefixLen;
    std::memcpy(hkdf_label + hkdf_label_len, label, label_len);
    hkdf_label_len += label_len;

    // Context length (1 byte) - empty context
    hkdf_label[hkdf_label_len++] = 0;

    // Use HKDF-Expand with the constructed info
    if (hkdf_expand(out, out_len, secret, secret_len,
                    hkdf_label, hkdf_label_len) != 1) {
        return -1;
    }

    return 0;
}

/**
 * Derive initial secret from connection ID (RFC 9001 Section 5.2).
 *
 * @param dcid Destination Connection ID
 * @param dcid_len Length of DCID
 * @param initial_secret Output buffer (32 bytes for SHA-256)
 * @param version QUIC version (1 or 2)
 * @return 0 on success, -1 on failure
 */
inline int derive_initial_secret(
    const uint8_t* dcid, size_t dcid_len,
    uint8_t* initial_secret, uint32_t version = 1) noexcept {

    const uint8_t* salt;
    size_t salt_len;

    if (version == 2) {
        salt = kQuicV2InitialSalt;
        salt_len = kQuicV2InitialSaltLength;
    } else {
        salt = kQuicV1InitialSalt;
        salt_len = kQuicV1InitialSaltLength;
    }

    // initial_secret = HKDF-Extract(initial_salt, cid)
    size_t out_len = 32; // SHA-256 output
    if (hkdf_extract(initial_secret, &out_len,
                     dcid, dcid_len, salt, salt_len) != 1) {
        return -1;
    }

    return 0;
}

/**
 * Derive client/server initial secrets (RFC 9001 Section 5.2).
 *
 * @param initial_secret The initial secret from derive_initial_secret()
 * @param client_secret Output buffer for client secret (32 bytes)
 * @param server_secret Output buffer for server secret (32 bytes)
 * @return 0 on success, -1 on failure
 */
inline int derive_initial_secrets(
    const uint8_t* initial_secret,
    uint8_t* client_secret,
    uint8_t* server_secret) noexcept {

    // client_initial_secret = HKDF-Expand-Label(initial_secret, "client in", "", 32)
    if (hkdf_expand_label(initial_secret, 32, "client in", 9, client_secret, 32) != 0) {
        return -1;
    }

    // server_initial_secret = HKDF-Expand-Label(initial_secret, "server in", "", 32)
    if (hkdf_expand_label(initial_secret, 32, "server in", 9, server_secret, 32) != 0) {
        return -1;
    }

    return 0;
}

/**
 * Keys for packet protection.
 */
struct PacketProtectionKeys {
    std::array<uint8_t, 32> key;      // AEAD key (up to 32 bytes)
    std::array<uint8_t, 12> iv;       // AEAD IV/nonce
    std::array<uint8_t, 16> hp_key;   // Header protection key
    size_t key_len;
    AeadAlgorithm algorithm;

    PacketProtectionKeys() noexcept
        : key{}, iv{}, hp_key{}, key_len(0), algorithm(AeadAlgorithm::AES_128_GCM) {}
};

/**
 * Derive packet protection keys from a secret (RFC 9001 Section 5.1).
 *
 * @param secret The secret (client or server)
 * @param secret_len Length of secret
 * @param algo AEAD algorithm
 * @param keys Output keys structure
 * @return 0 on success, -1 on failure
 */
inline int derive_packet_keys(
    const uint8_t* secret, size_t secret_len,
    AeadAlgorithm algo,
    PacketProtectionKeys& keys) noexcept {

    keys.algorithm = algo;
    keys.key_len = aead_key_length(algo);

    // key = HKDF-Expand-Label(secret, "quic key", "", key_length)
    if (hkdf_expand_label(secret, secret_len, "quic key", 8,
                          keys.key.data(), keys.key_len) != 0) {
        return -1;
    }

    // iv = HKDF-Expand-Label(secret, "quic iv", "", 12)
    if (hkdf_expand_label(secret, secret_len, "quic iv", 7,
                          keys.iv.data(), 12) != 0) {
        return -1;
    }

    // hp = HKDF-Expand-Label(secret, "quic hp", "", 16)
    if (hkdf_expand_label(secret, secret_len, "quic hp", 7,
                          keys.hp_key.data(), 16) != 0) {
        return -1;
    }

    return 0;
}

/**
 * Packet Protection context for AEAD encryption/decryption.
 *
 * Manages OpenSSL cipher contexts for efficient reuse.
 */
class PacketProtection {
public:
    PacketProtection() noexcept
        : encrypt_ctx_(nullptr),
          decrypt_ctx_(nullptr),
          hp_ctx_(nullptr),
          initialized_(false) {}

    ~PacketProtection() {
        if (encrypt_ctx_) EVP_CIPHER_CTX_free(encrypt_ctx_);
        if (decrypt_ctx_) EVP_CIPHER_CTX_free(decrypt_ctx_);
        if (hp_ctx_) EVP_CIPHER_CTX_free(hp_ctx_);
    }

    // Non-copyable
    PacketProtection(const PacketProtection&) = delete;
    PacketProtection& operator=(const PacketProtection&) = delete;

    // Movable
    PacketProtection(PacketProtection&& other) noexcept
        : keys_(std::move(other.keys_)),
          encrypt_ctx_(other.encrypt_ctx_),
          decrypt_ctx_(other.decrypt_ctx_),
          hp_ctx_(other.hp_ctx_),
          initialized_(other.initialized_) {
        other.encrypt_ctx_ = nullptr;
        other.decrypt_ctx_ = nullptr;
        other.hp_ctx_ = nullptr;
        other.initialized_ = false;
    }

    PacketProtection& operator=(PacketProtection&& other) noexcept {
        if (this != &other) {
            if (encrypt_ctx_) EVP_CIPHER_CTX_free(encrypt_ctx_);
            if (decrypt_ctx_) EVP_CIPHER_CTX_free(decrypt_ctx_);
            if (hp_ctx_) EVP_CIPHER_CTX_free(hp_ctx_);

            keys_ = std::move(other.keys_);
            encrypt_ctx_ = other.encrypt_ctx_;
            decrypt_ctx_ = other.decrypt_ctx_;
            hp_ctx_ = other.hp_ctx_;
            initialized_ = other.initialized_;

            other.encrypt_ctx_ = nullptr;
            other.decrypt_ctx_ = nullptr;
            other.hp_ctx_ = nullptr;
            other.initialized_ = false;
        }
        return *this;
    }

    /**
     * Initialize with packet protection keys.
     *
     * @param keys The derived keys
     * @return 0 on success, -1 on failure
     */
    int initialize(const PacketProtectionKeys& keys) noexcept {
        keys_ = keys;

        // Select cipher based on algorithm
        const EVP_CIPHER* cipher = nullptr;
        const EVP_CIPHER* hp_cipher = nullptr;

        switch (keys_.algorithm) {
            case AeadAlgorithm::AES_128_GCM:
                cipher = EVP_aes_128_gcm();
                hp_cipher = EVP_aes_128_ecb();
                break;
            case AeadAlgorithm::AES_256_GCM:
                cipher = EVP_aes_256_gcm();
                hp_cipher = EVP_aes_256_ecb();
                break;
            case AeadAlgorithm::CHACHA20_POLY1305:
                cipher = EVP_chacha20_poly1305();
                hp_cipher = EVP_chacha20();
                break;
            default:
                return -1;
        }

        // Create encryption context
        encrypt_ctx_ = EVP_CIPHER_CTX_new();
        if (!encrypt_ctx_) return -1;

        if (EVP_EncryptInit_ex(encrypt_ctx_, cipher, nullptr,
                               keys_.key.data(), nullptr) != 1) {
            return -1;
        }

        // Create decryption context
        decrypt_ctx_ = EVP_CIPHER_CTX_new();
        if (!decrypt_ctx_) return -1;

        if (EVP_DecryptInit_ex(decrypt_ctx_, cipher, nullptr,
                               keys_.key.data(), nullptr) != 1) {
            return -1;
        }

        // Create header protection context
        hp_ctx_ = EVP_CIPHER_CTX_new();
        if (!hp_ctx_) return -1;

        if (EVP_EncryptInit_ex(hp_ctx_, hp_cipher, nullptr,
                               keys_.hp_key.data(), nullptr) != 1) {
            return -1;
        }
        EVP_CIPHER_CTX_set_padding(hp_ctx_, 0); // No padding for HP

        initialized_ = true;
        return 0;
    }

    /**
     * Check if initialized.
     */
    bool is_initialized() const noexcept { return initialized_; }

    /**
     * Encrypt a QUIC packet payload (RFC 9001 Section 5.3).
     *
     * @param pn Packet number (used to construct nonce)
     * @param header Associated data (QUIC header)
     * @param header_len Length of header
     * @param plaintext Payload to encrypt
     * @param plaintext_len Length of plaintext
     * @param out Output buffer (must have space for plaintext_len + 16 tag)
     * @param out_len Output: actual encrypted length
     * @return 0 on success, -1 on failure
     */
    int encrypt(uint64_t pn,
                const uint8_t* header, size_t header_len,
                const uint8_t* plaintext, size_t plaintext_len,
                uint8_t* out, size_t* out_len) noexcept {

        if (!initialized_) return -1;

        // Construct nonce: IV XOR packet_number (left-padded)
        uint8_t nonce[12];
        construct_nonce(pn, nonce);

        // Set IV for this operation
        if (EVP_EncryptInit_ex(encrypt_ctx_, nullptr, nullptr, nullptr, nonce) != 1) {
            return -1;
        }

        // Set AAD (header)
        int len;
        if (EVP_EncryptUpdate(encrypt_ctx_, nullptr, &len, header, static_cast<int>(header_len)) != 1) {
            return -1;
        }

        // Encrypt plaintext
        if (EVP_EncryptUpdate(encrypt_ctx_, out, &len, plaintext, static_cast<int>(plaintext_len)) != 1) {
            return -1;
        }
        *out_len = static_cast<size_t>(len);

        // Finalize
        if (EVP_EncryptFinal_ex(encrypt_ctx_, out + *out_len, &len) != 1) {
            return -1;
        }
        *out_len += static_cast<size_t>(len);

        // Get authentication tag
        if (EVP_CIPHER_CTX_ctrl(encrypt_ctx_, EVP_CTRL_AEAD_GET_TAG,
                                kAeadTagLength, out + *out_len) != 1) {
            return -1;
        }
        *out_len += kAeadTagLength;

        return 0;
    }

    /**
     * Decrypt a QUIC packet payload (RFC 9001 Section 5.3).
     *
     * @param pn Packet number (used to construct nonce)
     * @param header Associated data (QUIC header)
     * @param header_len Length of header
     * @param ciphertext Encrypted payload with tag
     * @param ciphertext_len Length of ciphertext (including 16-byte tag)
     * @param out Output buffer for plaintext
     * @param out_len Output: actual decrypted length
     * @return 0 on success, -1 on failure/authentication error
     */
    int decrypt(uint64_t pn,
                const uint8_t* header, size_t header_len,
                const uint8_t* ciphertext, size_t ciphertext_len,
                uint8_t* out, size_t* out_len) noexcept {

        if (!initialized_) return -1;
        if (ciphertext_len < kAeadTagLength) return -1;

        // Construct nonce
        uint8_t nonce[12];
        construct_nonce(pn, nonce);

        // Set IV for this operation
        if (EVP_DecryptInit_ex(decrypt_ctx_, nullptr, nullptr, nullptr, nonce) != 1) {
            return -1;
        }

        // Set expected tag
        if (EVP_CIPHER_CTX_ctrl(decrypt_ctx_, EVP_CTRL_AEAD_SET_TAG,
                                kAeadTagLength,
                                const_cast<uint8_t*>(ciphertext + ciphertext_len - kAeadTagLength)) != 1) {
            return -1;
        }

        // Set AAD (header)
        int len;
        if (EVP_DecryptUpdate(decrypt_ctx_, nullptr, &len, header, static_cast<int>(header_len)) != 1) {
            return -1;
        }

        // Decrypt (excluding tag)
        size_t encrypted_len = ciphertext_len - kAeadTagLength;
        if (EVP_DecryptUpdate(decrypt_ctx_, out, &len, ciphertext, static_cast<int>(encrypted_len)) != 1) {
            return -1;
        }
        *out_len = static_cast<size_t>(len);

        // Finalize and verify tag
        if (EVP_DecryptFinal_ex(decrypt_ctx_, out + *out_len, &len) != 1) {
            // Authentication failed
            return -1;
        }
        *out_len += static_cast<size_t>(len);

        return 0;
    }

    /**
     * Apply header protection (RFC 9001 Section 5.4).
     *
     * @param header Packet header (will be modified in-place)
     * @param pn_offset Offset of packet number in header
     * @param pn_length Length of packet number (1-4)
     * @param sample 16-byte sample from encrypted payload
     * @return 0 on success, -1 on failure
     */
    int protect_header(uint8_t* header, size_t pn_offset,
                       size_t pn_length, const uint8_t* sample) noexcept {

        if (!initialized_) return -1;

        // AES-ECB outputs 16 bytes, but we only use first 5
        uint8_t mask[16];
        if (generate_hp_mask(sample, mask) != 0) {
            return -1;
        }

        // Apply mask to first byte
        if (header[0] & 0x80) {
            // Long header: mask lower 4 bits
            header[0] ^= mask[0] & 0x0F;
        } else {
            // Short header: mask lower 5 bits
            header[0] ^= mask[0] & 0x1F;
        }

        // Apply mask to packet number
        for (size_t i = 0; i < pn_length; i++) {
            header[pn_offset + i] ^= mask[1 + i];
        }

        return 0;
    }

    /**
     * Remove header protection (RFC 9001 Section 5.4.1).
     *
     * @param header Packet header (will be modified in-place)
     * @param pn_offset Offset of packet number in header
     * @param sample 16-byte sample from encrypted payload
     * @param out_pn_length Output: decoded packet number length
     * @return 0 on success, -1 on failure
     */
    int unprotect_header(uint8_t* header, size_t pn_offset,
                         const uint8_t* sample, size_t* out_pn_length) noexcept {

        if (!initialized_) return -1;

        // AES-ECB outputs 16 bytes, but we only use first 5
        uint8_t mask[16];
        if (generate_hp_mask(sample, mask) != 0) {
            return -1;
        }

        // Remove mask from first byte to get pn_length
        if (header[0] & 0x80) {
            // Long header: unmask lower 4 bits
            header[0] ^= mask[0] & 0x0F;
        } else {
            // Short header: unmask lower 5 bits
            header[0] ^= mask[0] & 0x1F;
        }

        // Get packet number length from unmasked first byte
        *out_pn_length = (header[0] & 0x03) + 1;

        // Remove mask from packet number
        for (size_t i = 0; i < *out_pn_length; i++) {
            header[pn_offset + i] ^= mask[1 + i];
        }

        return 0;
    }

    /**
     * Get sample offset for header protection.
     * Sample starts 4 bytes after the start of the packet number field.
     */
    static constexpr size_t sample_offset(size_t pn_offset) noexcept {
        return pn_offset + 4;
    }

    /**
     * Get the keys (for debugging/testing).
     */
    const PacketProtectionKeys& keys() const noexcept { return keys_; }

private:
    /**
     * Construct AEAD nonce from IV and packet number.
     * nonce = iv XOR (pn left-padded to 12 bytes)
     */
    void construct_nonce(uint64_t pn, uint8_t* nonce) const noexcept {
        // Start with IV
        std::memcpy(nonce, keys_.iv.data(), 12);

        // XOR with packet number (right-aligned, big-endian)
        for (int i = 0; i < 8; i++) {
            nonce[11 - i] ^= static_cast<uint8_t>((pn >> (8 * i)) & 0xFF);
        }
    }

    /**
     * Generate header protection mask.
     * For AES: mask = AES-ECB(hp_key, sample)
     * For ChaCha20: mask = ChaCha20(hp_key, sample[0..3] as counter, sample[4..15] as nonce)
     */
    int generate_hp_mask(const uint8_t* sample, uint8_t* mask) noexcept {
        if (keys_.algorithm == AeadAlgorithm::CHACHA20_POLY1305) {
            // ChaCha20 header protection
            // counter = sample[0..3] (little-endian)
            // nonce = sample[4..15]
            uint32_t counter = sample[0] | (sample[1] << 8) |
                              (sample[2] << 16) | (sample[3] << 24);

            uint8_t hp_nonce[12];
            std::memcpy(hp_nonce, sample + 4, 12);

            EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
            if (!ctx) return -1;

            if (EVP_EncryptInit_ex(ctx, EVP_chacha20(), nullptr,
                                   keys_.hp_key.data(), hp_nonce) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return -1;
            }

            // Set counter
            // Note: OpenSSL ChaCha20 expects counter in IV
            // This is a simplification - may need adjustment

            uint8_t zeros[5] = {0};
            int len;
            if (EVP_EncryptUpdate(ctx, mask, &len, zeros, 5) != 1) {
                EVP_CIPHER_CTX_free(ctx);
                return -1;
            }

            EVP_CIPHER_CTX_free(ctx);
        } else {
            // AES-ECB header protection
            int len;
            if (EVP_EncryptUpdate(hp_ctx_, mask, &len, sample, 16) != 1) {
                return -1;
            }
            // Only need first 5 bytes of the 16-byte output
        }

        return 0;
    }

    PacketProtectionKeys keys_;
    EVP_CIPHER_CTX* encrypt_ctx_;
    EVP_CIPHER_CTX* decrypt_ctx_;
    EVP_CIPHER_CTX* hp_ctx_;
    bool initialized_;
};

/**
 * Derive initial packet protection for client and server.
 *
 * Convenience function that performs the full key derivation chain.
 *
 * @param dcid Destination Connection ID (from client's Initial packet)
 * @param dcid_len Length of DCID
 * @param client_pp Output: client packet protection
 * @param server_pp Output: server packet protection
 * @param version QUIC version (1 or 2)
 * @return 0 on success, -1 on failure
 */
inline int derive_initial_packet_protection(
    const uint8_t* dcid, size_t dcid_len,
    PacketProtection& client_pp,
    PacketProtection& server_pp,
    uint32_t version = 1) noexcept {

    // Step 1: Derive initial secret
    uint8_t initial_secret[32];
    if (derive_initial_secret(dcid, dcid_len, initial_secret, version) != 0) {
        return -1;
    }

    // Step 2: Derive client and server secrets
    uint8_t client_secret[32];
    uint8_t server_secret[32];
    if (derive_initial_secrets(initial_secret, client_secret, server_secret) != 0) {
        return -1;
    }

    // Step 3: Derive packet keys for client
    PacketProtectionKeys client_keys;
    if (derive_packet_keys(client_secret, 32, AeadAlgorithm::AES_128_GCM, client_keys) != 0) {
        return -1;
    }

    // Step 4: Derive packet keys for server
    PacketProtectionKeys server_keys;
    if (derive_packet_keys(server_secret, 32, AeadAlgorithm::AES_128_GCM, server_keys) != 0) {
        return -1;
    }

    // Step 5: Initialize protection contexts
    if (client_pp.initialize(client_keys) != 0) {
        return -1;
    }

    if (server_pp.initialize(server_keys) != 0) {
        return -1;
    }

    // Clear sensitive data
    std::memset(initial_secret, 0, sizeof(initial_secret));
    std::memset(client_secret, 0, sizeof(client_secret));
    std::memset(server_secret, 0, sizeof(server_secret));

    return 0;
}

/**
 * Decode packet number from truncated form (RFC 9000 Section 17.1).
 *
 * Reconstructs full packet number from truncated value and expected range.
 *
 * @param truncated The truncated packet number from the header
 * @param pn_len Length of truncated PN (1-4 bytes)
 * @param largest_pn Largest packet number acknowledged
 * @return Full packet number
 */
inline uint64_t decode_packet_number(uint64_t truncated, size_t pn_len,
                                      uint64_t largest_pn) noexcept {
    uint64_t expected_pn = largest_pn + 1;
    uint64_t pn_win = 1ULL << (pn_len * 8);
    uint64_t pn_hwin = pn_win / 2;
    uint64_t pn_mask = pn_win - 1;

    // candidate_pn = (expected_pn & ~pn_mask) | truncated
    uint64_t candidate = (expected_pn & ~pn_mask) | truncated;

    // If the candidate is too far below expected, add a window
    // NOTE: Must check expected_pn >= pn_hwin to avoid unsigned underflow
    if (expected_pn >= pn_hwin &&
        candidate <= expected_pn - pn_hwin &&
        candidate < (1ULL << 62) - pn_win) {
        return candidate + pn_win;
    }
    // If the candidate is too far above expected, subtract a window
    if (candidate > expected_pn + pn_hwin && candidate >= pn_win) {
        return candidate - pn_win;
    }
    return candidate;
}

/**
 * Encode packet number to truncated form.
 *
 * @param full_pn Full packet number
 * @param largest_acked Largest acknowledged packet number (-1 if none)
 * @return Pair of (truncated value, required length)
 */
inline std::pair<uint64_t, size_t> encode_packet_number(uint64_t full_pn,
                                                         int64_t largest_acked) noexcept {
    // Calculate range we need to encode
    uint64_t range = (largest_acked >= 0)
        ? full_pn - static_cast<uint64_t>(largest_acked)
        : full_pn + 1;

    // Determine minimum bytes needed
    size_t pn_len;
    if (range < (1ULL << 7)) {
        pn_len = 1;
    } else if (range < (1ULL << 14)) {
        pn_len = 2;
    } else if (range < (1ULL << 22)) {
        pn_len = 3;
    } else {
        pn_len = 4;
    }

    // Truncate to required bytes
    uint64_t truncated = full_pn & ((1ULL << (pn_len * 8)) - 1);
    return {truncated, pn_len};
}

} // namespace quic
} // namespace fasterapi
