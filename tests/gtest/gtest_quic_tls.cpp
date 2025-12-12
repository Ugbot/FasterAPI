/**
 * @file gtest_quic_tls.cpp
 * @brief Tests for QUIC-TLS components (RFC 9001)
 *
 * Tests:
 * - Key derivation (HKDF-Expand-Label)
 * - Initial secret derivation
 * - Packet protection (AEAD)
 * - Header protection
 * - Packet number encoding/decoding
 * - CRYPTO buffer management
 */

#include <gtest/gtest.h>
#include <random>
#include <algorithm>
#include <chrono>
#include "../src/cpp/http/quic/quic_packet_protection.h"
#include "../src/cpp/http/quic/quic_crypto_buffer.h"
#include "../src/cpp/http/quic/quic_version_retry.h"
#include "../src/cpp/http/quic/quic_packet_number_space.h"

using namespace fasterapi::quic;

// Random data generator
class RandomGenerator {
public:
    RandomGenerator() : rng_(std::random_device{}()) {}

    std::vector<uint8_t> bytes(size_t len) {
        std::vector<uint8_t> data(len);
        std::uniform_int_distribution<int> dist(0, 255);
        for (auto& b : data) b = static_cast<uint8_t>(dist(rng_));
        return data;
    }

    uint64_t uint64(uint64_t max = UINT64_MAX) {
        std::uniform_int_distribution<uint64_t> dist(0, max);
        return dist(rng_);
    }

    size_t size(size_t min, size_t max) {
        std::uniform_int_distribution<size_t> dist(min, max);
        return dist(rng_);
    }

private:
    std::mt19937_64 rng_;
};

// ===========================================================================
// Key Derivation Tests
// ===========================================================================

class KeyDerivationTest : public ::testing::Test {
protected:
    RandomGenerator rng_;
};

TEST_F(KeyDerivationTest, InitialSecretFromDCID) {
    // Test initial secret derivation from DCID
    auto dcid = rng_.bytes(8);
    uint8_t initial_secret[32];

    int result = derive_initial_secret(dcid.data(), dcid.size(), initial_secret);
    EXPECT_EQ(result, 0);

    // Verify deterministic (same DCID = same secret)
    uint8_t initial_secret2[32];
    result = derive_initial_secret(dcid.data(), dcid.size(), initial_secret2);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(std::memcmp(initial_secret, initial_secret2, 32), 0);
}

TEST_F(KeyDerivationTest, DifferentDCIDsDifferentSecrets) {
    auto dcid1 = rng_.bytes(8);
    auto dcid2 = rng_.bytes(8);

    uint8_t secret1[32], secret2[32];
    derive_initial_secret(dcid1.data(), dcid1.size(), secret1);
    derive_initial_secret(dcid2.data(), dcid2.size(), secret2);

    // Different DCIDs should produce different secrets
    EXPECT_NE(std::memcmp(secret1, secret2, 32), 0);
}

TEST_F(KeyDerivationTest, ClientServerSecrets) {
    auto dcid = rng_.bytes(8);
    uint8_t initial_secret[32];
    derive_initial_secret(dcid.data(), dcid.size(), initial_secret);

    uint8_t client_secret[32], server_secret[32];
    int result = derive_initial_secrets(initial_secret, client_secret, server_secret);
    EXPECT_EQ(result, 0);

    // Client and server secrets should be different
    EXPECT_NE(std::memcmp(client_secret, server_secret, 32), 0);
}

TEST_F(KeyDerivationTest, PacketKeys) {
    auto dcid = rng_.bytes(8);
    uint8_t initial_secret[32];
    derive_initial_secret(dcid.data(), dcid.size(), initial_secret);

    uint8_t client_secret[32], server_secret[32];
    derive_initial_secrets(initial_secret, client_secret, server_secret);

    PacketProtectionKeys keys;
    int result = derive_packet_keys(client_secret, 32, AeadAlgorithm::AES_128_GCM, keys);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(keys.key_len, 16u);
    EXPECT_EQ(keys.algorithm, AeadAlgorithm::AES_128_GCM);
}

TEST_F(KeyDerivationTest, QuicV2Salt) {
    // Test that v2 uses different salt
    auto dcid = rng_.bytes(8);

    uint8_t secret_v1[32], secret_v2[32];
    derive_initial_secret(dcid.data(), dcid.size(), secret_v1, 1);
    derive_initial_secret(dcid.data(), dcid.size(), secret_v2, 2);

    // Different versions should produce different secrets
    EXPECT_NE(std::memcmp(secret_v1, secret_v2, 32), 0);
}

// ===========================================================================
// Packet Protection Tests
// ===========================================================================

class PacketProtectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        dcid_ = rng_.bytes(8);
        derive_initial_packet_protection(dcid_.data(), dcid_.size(),
                                          client_pp_, server_pp_);
    }

    RandomGenerator rng_;
    std::vector<uint8_t> dcid_;
    PacketProtection client_pp_;
    PacketProtection server_pp_;
};

TEST_F(PacketProtectionTest, Initialization) {
    EXPECT_TRUE(client_pp_.is_initialized());
    EXPECT_TRUE(server_pp_.is_initialized());
}

TEST_F(PacketProtectionTest, EncryptDecryptRoundtrip) {
    // Test encrypt/decrypt roundtrip
    for (int i = 0; i < 100; i++) {
        uint64_t pn = rng_.uint64(1000000);
        auto header = rng_.bytes(rng_.size(10, 30));
        auto plaintext = rng_.bytes(rng_.size(1, 1200));

        uint8_t ciphertext[2048];
        size_t ciphertext_len;

        // Client encrypts
        int result = client_pp_.encrypt(pn,
                                         header.data(), header.size(),
                                         plaintext.data(), plaintext.size(),
                                         ciphertext, &ciphertext_len);
        ASSERT_EQ(result, 0);
        EXPECT_EQ(ciphertext_len, plaintext.size() + kAeadTagLength);

        // Server decrypts
        uint8_t decrypted[2048];
        size_t decrypted_len;

        result = server_pp_.decrypt(pn,
                                     header.data(), header.size(),
                                     ciphertext, ciphertext_len,
                                     decrypted, &decrypted_len);
        ASSERT_EQ(result, 0);
        EXPECT_EQ(decrypted_len, plaintext.size());
        EXPECT_EQ(std::memcmp(decrypted, plaintext.data(), plaintext.size()), 0);
    }
}

TEST_F(PacketProtectionTest, TamperDetection) {
    uint64_t pn = rng_.uint64(1000);
    auto header = rng_.bytes(20);
    auto plaintext = rng_.bytes(100);

    uint8_t ciphertext[256];
    size_t ciphertext_len;

    client_pp_.encrypt(pn, header.data(), header.size(),
                       plaintext.data(), plaintext.size(),
                       ciphertext, &ciphertext_len);

    // Tamper with ciphertext
    ciphertext[50] ^= 0xFF;

    uint8_t decrypted[256];
    size_t decrypted_len;

    int result = server_pp_.decrypt(pn, header.data(), header.size(),
                                     ciphertext, ciphertext_len,
                                     decrypted, &decrypted_len);

    // Should fail authentication
    EXPECT_EQ(result, -1);
}

TEST_F(PacketProtectionTest, WrongPacketNumber) {
    uint64_t pn = rng_.uint64(1000);
    auto header = rng_.bytes(20);
    auto plaintext = rng_.bytes(100);

    uint8_t ciphertext[256];
    size_t ciphertext_len;

    client_pp_.encrypt(pn, header.data(), header.size(),
                       plaintext.data(), plaintext.size(),
                       ciphertext, &ciphertext_len);

    uint8_t decrypted[256];
    size_t decrypted_len;

    // Try to decrypt with wrong PN
    int result = server_pp_.decrypt(pn + 1, header.data(), header.size(),
                                     ciphertext, ciphertext_len,
                                     decrypted, &decrypted_len);

    // Should fail
    EXPECT_EQ(result, -1);
}

TEST_F(PacketProtectionTest, HeaderProtection) {
    // Test header protection roundtrip
    for (int i = 0; i < 100; i++) {
        // Create a realistic header
        std::vector<uint8_t> header(30);
        header[0] = 0xC0 | (rng_.uint64(15) & 0x0F); // Long header
        auto sample = rng_.bytes(16);
        size_t pn_offset = 20;
        size_t pn_length = (header[0] & 0x03) + 1;

        // Fill header with random data
        for (size_t j = 1; j < header.size(); j++) {
            header[j] = rng_.bytes(1)[0];
        }

        // Save original for comparison
        std::vector<uint8_t> original = header;

        // Apply protection
        int result = client_pp_.protect_header(header.data(), pn_offset,
                                                pn_length, sample.data());
        ASSERT_EQ(result, 0);

        // Header should be modified
        EXPECT_NE(header[0], original[0]);

        // Remove protection
        size_t recovered_pn_length;
        result = client_pp_.unprotect_header(header.data(), pn_offset,
                                              sample.data(), &recovered_pn_length);
        ASSERT_EQ(result, 0);

        // Should match original
        EXPECT_EQ(header[0], original[0]);
        EXPECT_EQ(recovered_pn_length, pn_length);
    }
}

// ===========================================================================
// Packet Number Tests
// ===========================================================================

class PacketNumberTest : public ::testing::Test {
protected:
    RandomGenerator rng_;
};

TEST_F(PacketNumberTest, EncodeDecode1Byte) {
    for (int i = 0; i < 100; i++) {
        uint64_t full_pn = rng_.uint64(127);
        int64_t largest_acked = full_pn > 0 ? full_pn - 1 : -1;

        auto [truncated, length] = encode_packet_number(full_pn, largest_acked);
        EXPECT_EQ(length, 1u);

        uint64_t decoded = decode_packet_number(truncated, length,
                                                  largest_acked >= 0 ? largest_acked : 0);
        EXPECT_EQ(decoded, full_pn);
    }
}

TEST_F(PacketNumberTest, EncodeDecode2Bytes) {
    for (int i = 0; i < 100; i++) {
        uint64_t full_pn = rng_.uint64(16383) + 128;
        int64_t largest_acked = full_pn > 100 ? full_pn - 100 : -1;

        auto [truncated, length] = encode_packet_number(full_pn, largest_acked);
        EXPECT_LE(length, 2u);

        uint64_t decoded = decode_packet_number(truncated, length,
                                                  largest_acked >= 0 ? largest_acked : 0);
        EXPECT_EQ(decoded, full_pn);
    }
}

TEST_F(PacketNumberTest, EncodeDecodeRoundtrip) {
    // Test with various packet number ranges
    std::vector<uint64_t> test_pns = {0, 1, 127, 128, 255, 256, 1000, 10000, 100000, 1000000};

    for (uint64_t pn : test_pns) {
        for (int64_t acked = -1; acked < static_cast<int64_t>(pn); acked += std::max(1LL, static_cast<int64_t>(pn / 10))) {
            auto [truncated, length] = encode_packet_number(pn, acked);
            uint64_t decoded = decode_packet_number(truncated, length,
                                                      acked >= 0 ? acked : 0);
            EXPECT_EQ(decoded, pn) << "Failed for pn=" << pn << " acked=" << acked;
        }
    }
}

// ===========================================================================
// Crypto Buffer Tests
// ===========================================================================

class CryptoBufferTest : public ::testing::Test {
protected:
    RandomGenerator rng_;
    CryptoBuffer buffer_;
};

TEST_F(CryptoBufferTest, ReceiveInOrder) {
    // Receive data in order
    for (int i = 0; i < 10; i++) {
        auto data = rng_.bytes(100);
        int result = buffer_.receive_data(i * 100, data.data(), data.size());
        EXPECT_EQ(result, 0);
    }

    EXPECT_EQ(buffer_.contiguous_available(), 1000u);
    EXPECT_FALSE(buffer_.has_gaps());
}

TEST_F(CryptoBufferTest, ReceiveOutOfOrder) {
    // Receive data out of order
    auto data2 = rng_.bytes(100);
    buffer_.receive_data(100, data2.data(), 100); // Second chunk first

    EXPECT_TRUE(buffer_.has_gaps());
    EXPECT_EQ(buffer_.contiguous_available(), 0u);

    auto data1 = rng_.bytes(100);
    buffer_.receive_data(0, data1.data(), 100); // First chunk

    EXPECT_FALSE(buffer_.has_gaps());
    EXPECT_EQ(buffer_.contiguous_available(), 200u);
}

TEST_F(CryptoBufferTest, ReadData) {
    auto data = rng_.bytes(500);
    buffer_.receive_data(0, data.data(), data.size());

    uint8_t out[256];
    size_t read = buffer_.read(out, 256);
    EXPECT_EQ(read, 256u);
    EXPECT_EQ(std::memcmp(out, data.data(), 256), 0);

    read = buffer_.read(out, 256);
    EXPECT_EQ(read, 244u); // Remaining
    EXPECT_EQ(std::memcmp(out, data.data() + 256, 244), 0);
}

TEST_F(CryptoBufferTest, DuplicateData) {
    auto data = rng_.bytes(100);
    buffer_.receive_data(0, data.data(), 100);
    buffer_.receive_data(0, data.data(), 100); // Duplicate

    EXPECT_EQ(buffer_.contiguous_available(), 100u);
}

TEST_F(CryptoBufferTest, OverlappingData) {
    auto data1 = rng_.bytes(100);
    buffer_.receive_data(0, data1.data(), 100);

    auto data2 = rng_.bytes(100);
    buffer_.receive_data(50, data2.data(), 100); // Overlaps

    EXPECT_EQ(buffer_.contiguous_available(), 150u);
}

TEST_F(CryptoBufferTest, BufferOverflow) {
    // Try to receive data beyond buffer limit
    auto data = rng_.bytes(100);
    int result = buffer_.receive_data(CryptoBuffer::kMaxBufferSize - 50,
                                       data.data(), 100);
    EXPECT_EQ(result, -1); // Should fail
}

TEST_F(CryptoBufferTest, WriteAndRead) {
    auto data = rng_.bytes(500);
    ssize_t written = buffer_.write(data.data(), data.size());
    EXPECT_EQ(written, 500);

    auto [ptr, len] = buffer_.get_send_data(0, 1000);
    EXPECT_EQ(len, 500u);
    EXPECT_EQ(std::memcmp(ptr, data.data(), 500), 0);
}

// ===========================================================================
// Version Negotiation Tests
// ===========================================================================

class VersionTest : public ::testing::Test {
protected:
    RandomGenerator rng_;
};

TEST_F(VersionTest, SupportedVersions) {
    EXPECT_TRUE(version::is_supported(version::QUIC_V1));
    EXPECT_TRUE(version::is_supported(version::QUIC_V2));
    EXPECT_FALSE(version::is_supported(0x12345678));
}

TEST_F(VersionTest, VersionNegotiationPacket) {
    ConnectionID client_dcid(rng_.bytes(8).data(), 8);
    ConnectionID client_scid(rng_.bytes(8).data(), 8);

    auto vn = VersionNegotiationPacket::create(client_dcid, client_scid);

    uint8_t buf[256];
    ssize_t len = vn.serialize(buf, sizeof(buf));
    ASSERT_GT(len, 0);

    // Parse it back
    VersionNegotiationPacket parsed;
    int result = parsed.parse(buf, len);
    EXPECT_EQ(result, 0);

    // VN packet swaps CIDs
    EXPECT_EQ(parsed.dest_conn_id, client_scid);
    EXPECT_EQ(parsed.source_conn_id, client_dcid);
    EXPECT_GE(parsed.supported_versions.size(), 2u);
}

// ===========================================================================
// Packet Number Space Tests
// ===========================================================================

class PacketNumberSpaceTest : public ::testing::Test {
protected:
    RandomGenerator rng_;
    PacketNumberSpaceManager pn_manager_;
};

TEST_F(PacketNumberSpaceTest, Initialization) {
    // All three spaces should exist
    auto& initial = pn_manager_[PacketNumberSpace::INITIAL];
    auto& handshake = pn_manager_[PacketNumberSpace::HANDSHAKE];
    auto& app = pn_manager_[PacketNumberSpace::APPLICATION];

    EXPECT_EQ(initial.space(), PacketNumberSpace::INITIAL);
    EXPECT_EQ(handshake.space(), PacketNumberSpace::HANDSHAKE);
    EXPECT_EQ(app.space(), PacketNumberSpace::APPLICATION);
}

TEST_F(PacketNumberSpaceTest, PacketNumberSequence) {
    auto& space = pn_manager_[PacketNumberSpace::APPLICATION];

    for (uint64_t i = 0; i < 100; i++) {
        EXPECT_EQ(space.next_packet_number(), i);
    }
}

TEST_F(PacketNumberSpaceTest, LevelToSpace) {
    EXPECT_EQ(level_to_space(EncryptionLevel::INITIAL), PacketNumberSpace::INITIAL);
    EXPECT_EQ(level_to_space(EncryptionLevel::ZERO_RTT), PacketNumberSpace::APPLICATION);
    EXPECT_EQ(level_to_space(EncryptionLevel::HANDSHAKE), PacketNumberSpace::HANDSHAKE);
    EXPECT_EQ(level_to_space(EncryptionLevel::ONE_RTT), PacketNumberSpace::APPLICATION);
}

TEST_F(PacketNumberSpaceTest, DiscardKeys) {
    pn_manager_[PacketNumberSpace::INITIAL].set_keys_available(true);
    EXPECT_TRUE(pn_manager_[PacketNumberSpace::INITIAL].keys_available());

    pn_manager_.discard_initial_keys();
    EXPECT_FALSE(pn_manager_[PacketNumberSpace::INITIAL].keys_available());
    EXPECT_TRUE(pn_manager_[PacketNumberSpace::INITIAL].keys_discarded());
}

// ===========================================================================
// RFC 9001 Test Vectors
// ===========================================================================

TEST(RFC9001TestVectors, InitialKeys) {
    // RFC 9001 Appendix A.1 - Client Initial
    // DCID: 0x8394c8f03e515708
    uint8_t dcid[] = {0x83, 0x94, 0xc8, 0xf0, 0x3e, 0x51, 0x57, 0x08};

    uint8_t initial_secret[32];
    int result = derive_initial_secret(dcid, sizeof(dcid), initial_secret);
    ASSERT_EQ(result, 0);

    // Expected initial_secret (from RFC 9001 A.1):
    // 7db5df06e7a69e432496adedb00851923595221596ae2ae9fb8115c1e9ed0a44
    uint8_t expected_initial_secret[] = {
        0x7d, 0xb5, 0xdf, 0x06, 0xe7, 0xa6, 0x9e, 0x43,
        0x24, 0x96, 0xad, 0xed, 0xb0, 0x08, 0x51, 0x92,
        0x35, 0x95, 0x22, 0x15, 0x96, 0xae, 0x2a, 0xe9,
        0xfb, 0x81, 0x15, 0xc1, 0xe9, 0xed, 0x0a, 0x44
    };

    EXPECT_EQ(std::memcmp(initial_secret, expected_initial_secret, 32), 0);

    // Derive client and server initial secrets
    uint8_t client_secret[32], server_secret[32];
    result = derive_initial_secrets(initial_secret, client_secret, server_secret);
    ASSERT_EQ(result, 0);

    // Expected client_initial_secret (from RFC 9001 A.1):
    // c00cf151ca5be075ed0ebfb5c80323c42d6b7db67881289af4008f1f6c357aea
    uint8_t expected_client_secret[] = {
        0xc0, 0x0c, 0xf1, 0x51, 0xca, 0x5b, 0xe0, 0x75,
        0xed, 0x0e, 0xbf, 0xb5, 0xc8, 0x03, 0x23, 0xc4,
        0x2d, 0x6b, 0x7d, 0xb6, 0x78, 0x81, 0x28, 0x9a,
        0xf4, 0x00, 0x8f, 0x1f, 0x6c, 0x35, 0x7a, 0xea
    };

    EXPECT_EQ(std::memcmp(client_secret, expected_client_secret, 32), 0);

    // Derive packet keys for client
    PacketProtectionKeys client_keys;
    result = derive_packet_keys(client_secret, 32, AeadAlgorithm::AES_128_GCM, client_keys);
    ASSERT_EQ(result, 0);

    // Expected client key (from RFC 9001 A.1):
    // 1f369613dd76d5467730efcbe3b1a22d
    uint8_t expected_client_key[] = {
        0x1f, 0x36, 0x96, 0x13, 0xdd, 0x76, 0xd5, 0x46,
        0x77, 0x30, 0xef, 0xcb, 0xe3, 0xb1, 0xa2, 0x2d
    };

    EXPECT_EQ(std::memcmp(client_keys.key.data(), expected_client_key, 16), 0);

    // Expected client IV (from RFC 9001 A.1):
    // fa044b2f42a3fd3b46fb255c
    uint8_t expected_client_iv[] = {
        0xfa, 0x04, 0x4b, 0x2f, 0x42, 0xa3, 0xfd, 0x3b,
        0x46, 0xfb, 0x25, 0x5c
    };

    EXPECT_EQ(std::memcmp(client_keys.iv.data(), expected_client_iv, 12), 0);
}

// Main
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
