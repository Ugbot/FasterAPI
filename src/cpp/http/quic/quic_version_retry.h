#pragma once

/**
 * @file quic_version_retry.h
 * @brief QUIC Version Negotiation and Retry (RFC 9000)
 *
 * - Version Negotiation: Server response to unsupported versions
 * - Retry: Server address validation mechanism
 * - Token generation and validation
 */

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <chrono>
#include <array>

#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

#include "quic_packet.h"

namespace fasterapi {
namespace quic {

/**
 * Supported QUIC versions.
 */
namespace version {
    constexpr uint32_t QUIC_V1 = 0x00000001;          // RFC 9000
    constexpr uint32_t QUIC_V2 = 0x6b3343cf;          // RFC 9369
    constexpr uint32_t VERSION_NEGOTIATION = 0x00;    // Version negotiation

    // Draft versions (for compatibility)
    constexpr uint32_t DRAFT_29 = 0xff00001d;
    constexpr uint32_t DRAFT_30 = 0xff00001e;
    constexpr uint32_t DRAFT_31 = 0xff00001f;
    constexpr uint32_t DRAFT_32 = 0xff000020;

    /**
     * Check if version is supported.
     */
    inline bool is_supported(uint32_t v) noexcept {
        return v == QUIC_V1 || v == QUIC_V2;
    }

    /**
     * Get list of supported versions.
     */
    inline std::vector<uint32_t> supported_versions() {
        return {QUIC_V1, QUIC_V2};
    }

    /**
     * Get version name for debugging.
     */
    inline const char* name(uint32_t v) noexcept {
        switch (v) {
            case QUIC_V1: return "QUICv1";
            case QUIC_V2: return "QUICv2";
            case VERSION_NEGOTIATION: return "Version Negotiation";
            case DRAFT_29: return "draft-29";
            case DRAFT_30: return "draft-30";
            case DRAFT_31: return "draft-31";
            case DRAFT_32: return "draft-32";
            default: return "Unknown";
        }
    }
}

/**
 * Version Negotiation Packet (RFC 9000 Section 17.2.1).
 *
 * Sent by server when client's version is not supported.
 *
 * Format:
 * +-+-+-+-+-+-+-+-+
 * |1|  Unused (7) |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                         Version (32) = 0                      |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | DCID Len (8)  |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |               Destination Connection ID (0..2040)            ...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | SCID Len (8)  |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                 Source Connection ID (0..2040)               ...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                    Supported Version 1 (32)                   |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                   [Supported Version 2 (32)]                  |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * ...
 */
struct VersionNegotiationPacket {
    ConnectionID dest_conn_id;    // Copied from client's SCID
    ConnectionID source_conn_id;  // Copied from client's DCID
    std::vector<uint32_t> supported_versions;

    VersionNegotiationPacket() = default;

    /**
     * Parse version negotiation packet.
     *
     * @param data Input buffer
     * @param len Buffer length
     * @return 0 on success, -1 if need more data, 1 on error
     */
    int parse(const uint8_t* data, size_t len) noexcept {
        if (len < 5) return -1;

        // First byte must have high bit set (0x80)
        if ((data[0] & 0x80) == 0) return 1;

        // Version must be 0
        uint32_t ver = (static_cast<uint32_t>(data[1]) << 24) |
                       (static_cast<uint32_t>(data[2]) << 16) |
                       (static_cast<uint32_t>(data[3]) << 8) |
                       static_cast<uint32_t>(data[4]);
        if (ver != 0) return 1;

        size_t pos = 5;

        // DCID
        if (len < pos + 1) return -1;
        uint8_t dcid_len = data[pos++];
        if (dcid_len > 20 || len < pos + dcid_len) return -1;
        dest_conn_id = ConnectionID(data + pos, dcid_len);
        pos += dcid_len;

        // SCID
        if (len < pos + 1) return -1;
        uint8_t scid_len = data[pos++];
        if (scid_len > 20 || len < pos + scid_len) return -1;
        source_conn_id = ConnectionID(data + pos, scid_len);
        pos += scid_len;

        // Supported versions
        supported_versions.clear();
        while (pos + 4 <= len) {
            uint32_t v = (static_cast<uint32_t>(data[pos]) << 24) |
                         (static_cast<uint32_t>(data[pos+1]) << 16) |
                         (static_cast<uint32_t>(data[pos+2]) << 8) |
                         static_cast<uint32_t>(data[pos+3]);
            supported_versions.push_back(v);
            pos += 4;
        }

        return 0;
    }

    /**
     * Serialize version negotiation packet.
     *
     * @param out Output buffer
     * @param max_len Maximum buffer size
     * @return Bytes written, or -1 on error
     */
    ssize_t serialize(uint8_t* out, size_t max_len) const noexcept {
        size_t needed = 5 + 1 + dest_conn_id.length + 1 + source_conn_id.length +
                        supported_versions.size() * 4;
        if (max_len < needed) return -1;

        size_t pos = 0;

        // First byte: random with high bit set
        RAND_bytes(out, 1);
        out[pos++] |= 0x80;

        // Version = 0
        out[pos++] = 0;
        out[pos++] = 0;
        out[pos++] = 0;
        out[pos++] = 0;

        // DCID (use client's SCID)
        out[pos++] = dest_conn_id.length;
        std::memcpy(out + pos, dest_conn_id.data, dest_conn_id.length);
        pos += dest_conn_id.length;

        // SCID (use client's DCID)
        out[pos++] = source_conn_id.length;
        std::memcpy(out + pos, source_conn_id.data, source_conn_id.length);
        pos += source_conn_id.length;

        // Supported versions
        for (uint32_t v : supported_versions) {
            out[pos++] = (v >> 24) & 0xFF;
            out[pos++] = (v >> 16) & 0xFF;
            out[pos++] = (v >> 8) & 0xFF;
            out[pos++] = v & 0xFF;
        }

        return static_cast<ssize_t>(pos);
    }

    /**
     * Create version negotiation response.
     *
     * @param client_dcid Client's destination connection ID
     * @param client_scid Client's source connection ID
     * @return VersionNegotiationPacket ready to serialize
     */
    static VersionNegotiationPacket create(const ConnectionID& client_dcid,
                                            const ConnectionID& client_scid) {
        VersionNegotiationPacket pkt;
        // Swap CIDs (VN uses opposite direction)
        pkt.dest_conn_id = client_scid;
        pkt.source_conn_id = client_dcid;
        pkt.supported_versions = version::supported_versions();
        return pkt;
    }
};

/**
 * Retry Packet (RFC 9000 Section 17.2.5).
 *
 * Sent by server for address validation.
 *
 * Format:
 * +-+-+-+-+-+-+-+-+
 * |1|1| 3 |X X X X|   (type = 3 for Retry)
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                         Version (32)                          |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | DCID Len (8)  |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |               Destination Connection ID (0..160)             ...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | SCID Len (8)  |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                 Source Connection ID (0..160)                ...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                        Retry Token (*)                       ...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                                                               |
 * +                   Retry Integrity Tag (128)                   +
 * |                                                               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
struct RetryPacket {
    uint32_t version;
    ConnectionID dest_conn_id;     // New server-chosen DCID
    ConnectionID source_conn_id;   // Same as client's DCID
    std::vector<uint8_t> retry_token;
    std::array<uint8_t, 16> integrity_tag;

    RetryPacket() : version(version::QUIC_V1) {}

    /**
     * Parse retry packet.
     *
     * @param data Input buffer
     * @param len Buffer length
     * @return 0 on success, -1 if need more data, 1 on error
     */
    int parse(const uint8_t* data, size_t len) noexcept {
        if (len < 5) return -1;

        // Check first byte: 1|1|11|XXXX (type 3)
        uint8_t first = data[0];
        if ((first & 0xF0) != 0xF0) return 1; // Not a retry packet

        size_t pos = 1;

        // Version
        if (len < pos + 4) return -1;
        version = (static_cast<uint32_t>(data[pos]) << 24) |
                  (static_cast<uint32_t>(data[pos+1]) << 16) |
                  (static_cast<uint32_t>(data[pos+2]) << 8) |
                  static_cast<uint32_t>(data[pos+3]);
        pos += 4;

        // DCID
        if (len < pos + 1) return -1;
        uint8_t dcid_len = data[pos++];
        if (dcid_len > 20 || len < pos + dcid_len) return -1;
        dest_conn_id = ConnectionID(data + pos, dcid_len);
        pos += dcid_len;

        // SCID
        if (len < pos + 1) return -1;
        uint8_t scid_len = data[pos++];
        if (scid_len > 20 || len < pos + scid_len) return -1;
        source_conn_id = ConnectionID(data + pos, scid_len);
        pos += scid_len;

        // Token + integrity tag (last 16 bytes are tag)
        if (len < pos + 16) return -1;
        size_t token_len = len - pos - 16;
        retry_token.assign(data + pos, data + pos + token_len);
        pos += token_len;

        std::memcpy(integrity_tag.data(), data + pos, 16);

        return 0;
    }

    /**
     * Serialize retry packet (without integrity tag - call compute_integrity_tag after).
     *
     * @param out Output buffer
     * @param max_len Maximum buffer size
     * @param original_dcid Original DCID from client's Initial (for integrity)
     * @return Bytes written, or -1 on error
     */
    ssize_t serialize(uint8_t* out, size_t max_len,
                      const ConnectionID& original_dcid) noexcept {
        size_t needed = 1 + 4 + 1 + dest_conn_id.length + 1 + source_conn_id.length +
                        retry_token.size() + 16;
        if (max_len < needed) return -1;

        size_t pos = 0;

        // First byte: 1|1|11|XXXX
        RAND_bytes(out, 1);
        out[pos] = 0xF0 | (out[pos] & 0x0F);
        pos++;

        // Version
        out[pos++] = (version >> 24) & 0xFF;
        out[pos++] = (version >> 16) & 0xFF;
        out[pos++] = (version >> 8) & 0xFF;
        out[pos++] = version & 0xFF;

        // DCID
        out[pos++] = dest_conn_id.length;
        std::memcpy(out + pos, dest_conn_id.data, dest_conn_id.length);
        pos += dest_conn_id.length;

        // SCID
        out[pos++] = source_conn_id.length;
        std::memcpy(out + pos, source_conn_id.data, source_conn_id.length);
        pos += source_conn_id.length;

        // Token
        std::memcpy(out + pos, retry_token.data(), retry_token.size());
        pos += retry_token.size();

        // Compute and append integrity tag
        compute_integrity_tag(out, pos, original_dcid);
        std::memcpy(out + pos, integrity_tag.data(), 16);
        pos += 16;

        return static_cast<ssize_t>(pos);
    }

    /**
     * Compute Retry Integrity Tag (RFC 9001 Section 5.8).
     *
     * Uses AES-128-GCM with a secret key.
     */
    void compute_integrity_tag(const uint8_t* retry_pseudo_packet, size_t len,
                               const ConnectionID& original_dcid) noexcept {
        // Retry secret key and nonce (RFC 9001 Section 5.8)
        // These are version-specific constants
        static constexpr uint8_t kRetryKeyV1[] = {
            0xbe, 0x0c, 0x69, 0x0b, 0x9f, 0x66, 0x57, 0x5a,
            0x1d, 0x76, 0x6b, 0x54, 0xe3, 0x68, 0xc8, 0x4e
        };
        static constexpr uint8_t kRetryNonceV1[] = {
            0x46, 0x15, 0x99, 0xd3, 0x5d, 0x63, 0x2b, 0xf2,
            0x23, 0x98, 0x25, 0xbb
        };

        static constexpr uint8_t kRetryKeyV2[] = {
            0x8f, 0xb4, 0xb0, 0x1b, 0x56, 0xac, 0x48, 0xe2,
            0x60, 0xfb, 0xcb, 0xce, 0xad, 0x7c, 0xcc, 0x92
        };
        static constexpr uint8_t kRetryNonceV2[] = {
            0xd8, 0x69, 0x69, 0xbc, 0x2d, 0x7c, 0x6d, 0x99,
            0x90, 0xef, 0xb0, 0x4a
        };

        const uint8_t* key;
        const uint8_t* nonce;
        if (version == version::QUIC_V2) {
            key = kRetryKeyV2;
            nonce = kRetryNonceV2;
        } else {
            key = kRetryKeyV1;
            nonce = kRetryNonceV1;
        }

        // Build Retry Pseudo-Packet:
        // ODCID Length (1) || Original DCID || Retry Packet (without tag)
        std::vector<uint8_t> pseudo;
        pseudo.push_back(original_dcid.length);
        pseudo.insert(pseudo.end(), original_dcid.data,
                      original_dcid.data + original_dcid.length);
        pseudo.insert(pseudo.end(), retry_pseudo_packet, retry_pseudo_packet + len);

        // AES-128-GCM encrypt with empty plaintext (just get the tag)
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return;

        EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, key, nonce);
        EVP_EncryptUpdate(ctx, nullptr, reinterpret_cast<int*>(&len),
                          pseudo.data(), static_cast<int>(pseudo.size()));

        int out_len;
        EVP_EncryptFinal_ex(ctx, nullptr, &out_len);
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, integrity_tag.data());

        EVP_CIPHER_CTX_free(ctx);
    }

    /**
     * Verify Retry Integrity Tag.
     *
     * @param original_dcid Original DCID from client's Initial
     * @return true if tag is valid
     */
    bool verify_integrity_tag(const ConnectionID& original_dcid) const noexcept {
        // Rebuild pseudo-packet and compute expected tag
        RetryPacket copy = *this;

        std::vector<uint8_t> packet_data(1024);
        size_t pos = 0;

        // First byte
        packet_data[pos++] = 0xF0;

        // Version
        packet_data[pos++] = (version >> 24) & 0xFF;
        packet_data[pos++] = (version >> 16) & 0xFF;
        packet_data[pos++] = (version >> 8) & 0xFF;
        packet_data[pos++] = version & 0xFF;

        // DCID
        packet_data[pos++] = dest_conn_id.length;
        std::memcpy(packet_data.data() + pos, dest_conn_id.data, dest_conn_id.length);
        pos += dest_conn_id.length;

        // SCID
        packet_data[pos++] = source_conn_id.length;
        std::memcpy(packet_data.data() + pos, source_conn_id.data, source_conn_id.length);
        pos += source_conn_id.length;

        // Token
        std::memcpy(packet_data.data() + pos, retry_token.data(), retry_token.size());
        pos += retry_token.size();

        copy.compute_integrity_tag(packet_data.data(), pos, original_dcid);

        return std::memcmp(copy.integrity_tag.data(), integrity_tag.data(), 16) == 0;
    }
};

/**
 * Token Manager for Retry and NEW_TOKEN.
 *
 * Generates and validates tokens for address validation.
 */
class TokenManager {
public:
    static constexpr size_t kTokenKeySize = 32;
    static constexpr size_t kTokenIvSize = 12;
    static constexpr size_t kMaxTokenLifetime = 24 * 60 * 60; // 24 hours

    TokenManager() {
        // Generate random key
        RAND_bytes(key_.data(), kTokenKeySize);
    }

    /**
     * Initialize with a specific key.
     */
    void set_key(const uint8_t* key) noexcept {
        std::memcpy(key_.data(), key, kTokenKeySize);
    }

    /**
     * Generate a Retry token.
     *
     * Token format (encrypted):
     * - Timestamp (8 bytes)
     * - Client IP address (16 bytes for v6, 4 for v4)
     * - Original DCID length (1 byte)
     * - Original DCID (0-20 bytes)
     *
     * @param client_addr Client IP address
     * @param addr_len Address length
     * @param original_dcid Original destination connection ID
     * @param out_token Output token buffer
     * @param max_len Maximum token length
     * @return Token length, or -1 on error
     */
    ssize_t generate_retry_token(const uint8_t* client_addr, size_t addr_len,
                                  const ConnectionID& original_dcid,
                                  uint8_t* out_token, size_t max_len) noexcept {
        // Plaintext: timestamp || addr || dcid_len || dcid
        size_t plaintext_len = 8 + addr_len + 1 + original_dcid.length;
        if (max_len < kTokenIvSize + plaintext_len + 16) {
            return -1;
        }

        std::vector<uint8_t> plaintext(plaintext_len);
        size_t pos = 0;

        // Timestamp
        uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count());
        for (int i = 7; i >= 0; i--) {
            plaintext[pos++] = (now >> (i * 8)) & 0xFF;
        }

        // Client address
        std::memcpy(plaintext.data() + pos, client_addr, addr_len);
        pos += addr_len;

        // Original DCID
        plaintext[pos++] = original_dcid.length;
        std::memcpy(plaintext.data() + pos, original_dcid.data, original_dcid.length);

        // Generate IV
        uint8_t iv[kTokenIvSize];
        RAND_bytes(iv, kTokenIvSize);

        // Output: IV || ciphertext || tag
        std::memcpy(out_token, iv, kTokenIvSize);

        // Encrypt
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return -1;

        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr,
                               key_.data(), iv) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return -1;
        }

        int out_len;
        if (EVP_EncryptUpdate(ctx, out_token + kTokenIvSize, &out_len,
                              plaintext.data(), static_cast<int>(plaintext_len)) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return -1;
        }

        int final_len;
        if (EVP_EncryptFinal_ex(ctx, out_token + kTokenIvSize + out_len, &final_len) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return -1;
        }

        // Get tag
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16,
                                out_token + kTokenIvSize + out_len + final_len) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return -1;
        }

        EVP_CIPHER_CTX_free(ctx);

        return kTokenIvSize + out_len + final_len + 16;
    }

    /**
     * Validate a Retry token.
     *
     * @param token Token to validate
     * @param token_len Token length
     * @param client_addr Expected client address
     * @param addr_len Address length
     * @param out_dcid Output: original DCID
     * @return true if token is valid
     */
    bool validate_retry_token(const uint8_t* token, size_t token_len,
                               const uint8_t* client_addr, size_t addr_len,
                               ConnectionID& out_dcid) noexcept {
        if (token_len < kTokenIvSize + 16) {
            return false;
        }

        // Extract IV
        const uint8_t* iv = token;
        const uint8_t* ciphertext = token + kTokenIvSize;
        size_t ciphertext_len = token_len - kTokenIvSize - 16;
        const uint8_t* tag = token + token_len - 16;

        // Decrypt
        std::vector<uint8_t> plaintext(ciphertext_len);

        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return false;

        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr,
                               key_.data(), iv) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16,
                                const_cast<uint8_t*>(tag)) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        int out_len;
        if (EVP_DecryptUpdate(ctx, plaintext.data(), &out_len,
                              ciphertext, static_cast<int>(ciphertext_len)) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        int final_len;
        if (EVP_DecryptFinal_ex(ctx, plaintext.data() + out_len, &final_len) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return false;
        }

        EVP_CIPHER_CTX_free(ctx);

        // Parse plaintext
        size_t pos = 0;

        // Check timestamp
        uint64_t timestamp = 0;
        for (int i = 0; i < 8; i++) {
            timestamp = (timestamp << 8) | plaintext[pos++];
        }

        uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count());

        if (now > timestamp + kMaxTokenLifetime) {
            return false; // Token expired
        }

        // Check address
        if (pos + addr_len > plaintext.size()) {
            return false;
        }
        if (std::memcmp(plaintext.data() + pos, client_addr, addr_len) != 0) {
            return false; // Address mismatch
        }
        pos += addr_len;

        // Extract original DCID
        if (pos >= plaintext.size()) {
            return false;
        }
        uint8_t dcid_len = plaintext[pos++];
        if (dcid_len > 20 || pos + dcid_len > plaintext.size()) {
            return false;
        }

        out_dcid = ConnectionID(plaintext.data() + pos, dcid_len);
        return true;
    }

private:
    std::array<uint8_t, kTokenKeySize> key_;
};

/**
 * Check if a packet is a Version Negotiation packet.
 */
inline bool is_version_negotiation(const uint8_t* data, size_t len) noexcept {
    if (len < 5) return false;
    if ((data[0] & 0x80) == 0) return false; // Must be long header

    // Version = 0
    return data[1] == 0 && data[2] == 0 && data[3] == 0 && data[4] == 0;
}

/**
 * Check if a packet is a Retry packet.
 */
inline bool is_retry_packet(const uint8_t* data, size_t len) noexcept {
    if (len < 5) return false;
    if ((data[0] & 0x80) == 0) return false; // Must be long header

    // First byte: 1|1|11|XXXX (type 3)
    // But for versions that we support, we need to check the type bits
    // For v1: type bits are bits 4-5 of first byte
    return (data[0] & 0x30) == 0x30;
}

/**
 * Create a Version Negotiation response.
 *
 * @param data Received packet data
 * @param len Packet length
 * @param out Output buffer
 * @param max_len Maximum output size
 * @return Bytes written, or -1 on error
 */
inline ssize_t create_version_negotiation_response(const uint8_t* data, size_t len,
                                                    uint8_t* out, size_t max_len) {
    if (len < 6) return -1;

    size_t pos = 5; // Skip first byte and version

    // Parse client's DCID
    if (len < pos + 1) return -1;
    uint8_t dcid_len = data[pos++];
    if (dcid_len > 20 || len < pos + dcid_len) return -1;
    ConnectionID client_dcid(data + pos, dcid_len);
    pos += dcid_len;

    // Parse client's SCID
    if (len < pos + 1) return -1;
    uint8_t scid_len = data[pos++];
    if (scid_len > 20 || len < pos + scid_len) return -1;
    ConnectionID client_scid(data + pos, scid_len);

    // Create response
    VersionNegotiationPacket vn = VersionNegotiationPacket::create(client_dcid, client_scid);
    return vn.serialize(out, max_len);
}

} // namespace quic
} // namespace fasterapi
