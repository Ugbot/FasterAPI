/**
 * Test QUIC Handshake - Debug test for HTTP/3 server
 *
 * Properly implements QUIC TLS handshake using quictls's QUIC API.
 * Sends a QUIC Initial packet with ClientHello and verifies the response.
 */

#include "src/cpp/http/quic/quic_packet_protection.h"
#include "src/cpp/http/quic/quic_frames.h"
#include "src/cpp/http/quic/quic_tls.h"
#include "src/cpp/http/quic/quic_packet.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <random>
#include <cassert>
#include <vector>

using namespace fasterapi::quic;

// Print hex dump
void hex_dump(const char* label, const uint8_t* data, size_t len) {
    std::cout << label << " (" << len << " bytes):\n  ";
    for (size_t i = 0; i < len && i < 128; i++) {
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(data[i]) << " ";
        if ((i + 1) % 16 == 0) std::cout << "\n  ";
    }
    if (len > 128) std::cout << "... (" << (len - 128) << " more bytes)";
    std::cout << std::dec << "\n";
}

/**
 * QUIC Test Client with proper quictls QUIC API integration.
 */
class QuicTestClient {
public:
    QuicTestClient() : ssl_ctx_(nullptr), ssl_(nullptr), socket_(-1) {}

    ~QuicTestClient() {
        if (ssl_) SSL_free(ssl_);
        if (ssl_ctx_) SSL_CTX_free(ssl_ctx_);
        if (socket_ >= 0) close(socket_);
    }

    bool init(const char* host, uint16_t port) {
        // Create UDP socket
        socket_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_ < 0) {
            std::cerr << "Failed to create socket\n";
            return false;
        }

        // Connect to server
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host, &addr.sin_addr);

        if (connect(socket_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::cerr << "Failed to connect: " << strerror(errno) << "\n";
            return false;
        }

        // Set non-blocking
        fcntl(socket_, F_SETFL, O_NONBLOCK);

        // Generate connection IDs
        RAND_bytes(local_cid_.data, 8);
        local_cid_.length = 8;
        RAND_bytes(peer_cid_.data, 8);
        peer_cid_.length = 8;
        original_dcid_ = peer_cid_;

        std::cout << "Local CID: ";
        for (size_t i = 0; i < local_cid_.length; i++) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(local_cid_.data[i]);
        }
        std::cout << std::dec << "\n";

        std::cout << "Initial DCID: ";
        for (size_t i = 0; i < peer_cid_.length; i++) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(peer_cid_.data[i]);
        }
        std::cout << std::dec << "\n";

        // Derive initial keys
        std::cout << "\nDeriving initial keys from DCID...\n";
        if (derive_initial_packet_protection(
                original_dcid_.data, original_dcid_.length,
                client_initial_pp_, server_initial_pp_) != 0) {
            std::cerr << "Failed to derive initial keys\n";
            return false;
        }
        std::cout << "Initial keys derived successfully\n";

        // Create SSL context
        ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ssl_ctx_) {
            std::cerr << "Failed to create SSL context\n";
            ERR_print_errors_fp(stderr);
            return false;
        }

        // Configure for TLS 1.3 (required for QUIC)
        SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_3_VERSION);
        SSL_CTX_set_max_proto_version(ssl_ctx_, TLS1_3_VERSION);

        // Set ALPN (h3)
        static const uint8_t alpn[] = {2, 'h', '3'};
        if (SSL_CTX_set_alpn_protos(ssl_ctx_, alpn, sizeof(alpn)) != 0) {
            std::cerr << "Failed to set ALPN\n";
            return false;
        }

        // Create SSL object
        ssl_ = SSL_new(ssl_ctx_);
        if (!ssl_) {
            std::cerr << "Failed to create SSL object\n";
            ERR_print_errors_fp(stderr);
            return false;
        }

        // Set QUIC method (this is the key for quictls!)
        static SSL_QUIC_METHOD quic_method = {
            set_encryption_secrets_cb,
            add_handshake_data_cb,
            flush_flight_cb,
            send_alert_cb
        };

        if (SSL_set_quic_method(ssl_, &quic_method) != 1) {
            std::cerr << "Failed to set QUIC method\n";
            ERR_print_errors_fp(stderr);
            return false;
        }

        // Store this pointer for callbacks
        SSL_set_app_data(ssl_, this);

        // Build QUIC transport parameters
        uint8_t tp_buf[256];
        size_t tp_len = build_transport_params(tp_buf, sizeof(tp_buf));

        if (SSL_set_quic_transport_params(ssl_, tp_buf, tp_len) != 1) {
            std::cerr << "Failed to set transport params\n";
            ERR_print_errors_fp(stderr);
            return false;
        }

        // Set connect mode
        SSL_set_connect_state(ssl_);

        return true;
    }

    bool do_handshake() {
        std::cout << "\n=== Starting QUIC Handshake ===\n";

        // Start TLS handshake - this will call our callbacks
        int ret = SSL_do_handshake(ssl_);

        if (ret != 1) {
            int err = SSL_get_error(ssl_, ret);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                std::cerr << "SSL_do_handshake failed with error: " << err << "\n";
                ERR_print_errors_fp(stderr);
                return false;
            }
            // WANT_READ/WANT_WRITE is expected - we need to send data and receive response
        }

        // Check if we have data to send
        if (crypto_send_buffer_.empty()) {
            std::cerr << "No ClientHello generated!\n";
            return false;
        }

        std::cout << "Generated " << crypto_send_buffer_.size() << " bytes of TLS data\n";

        // Send Initial packet with ClientHello
        if (!send_initial_packet()) {
            return false;
        }

        // Wait for and process response
        if (!receive_and_process_response()) {
            return false;
        }

        return true;
    }

private:
    // QUIC method callbacks
    static int set_encryption_secrets_cb(SSL* ssl, OSSL_ENCRYPTION_LEVEL level,
                                          const uint8_t* read_secret,
                                          const uint8_t* write_secret,
                                          size_t secret_len) {
        QuicTestClient* client = static_cast<QuicTestClient*>(SSL_get_app_data(ssl));
        return client->on_set_encryption_secrets(level, read_secret, write_secret, secret_len);
    }

    static int add_handshake_data_cb(SSL* ssl, OSSL_ENCRYPTION_LEVEL level,
                                      const uint8_t* data, size_t len) {
        QuicTestClient* client = static_cast<QuicTestClient*>(SSL_get_app_data(ssl));
        return client->on_add_handshake_data(level, data, len);
    }

    static int flush_flight_cb(SSL* ssl) {
        // We'll send all data at once after handshake call returns
        return 1;
    }

    static int send_alert_cb(SSL* ssl, OSSL_ENCRYPTION_LEVEL level, uint8_t alert) {
        std::cerr << "TLS Alert at level " << static_cast<int>(level)
                  << ": " << static_cast<int>(alert) << "\n";
        return 1;
    }

    int on_set_encryption_secrets(OSSL_ENCRYPTION_LEVEL level,
                                   const uint8_t* read_secret,
                                   const uint8_t* write_secret,
                                   size_t secret_len) {
        std::cout << "New secrets at level " << static_cast<int>(level)
                  << " (secret_len=" << secret_len << ")\n";

        // Derive packet protection keys from secrets
        // For now, we only handle Initial level which we derive separately
        if (level == ssl_encryption_handshake) {
            std::cout << "  -> Handshake keys available!\n";
            // TODO: Derive handshake keys
        } else if (level == ssl_encryption_application) {
            std::cout << "  -> Application keys available!\n";
            // TODO: Derive 1-RTT keys
        }

        return 1;
    }

    int on_add_handshake_data(OSSL_ENCRYPTION_LEVEL level,
                               const uint8_t* data, size_t len) {
        std::cout << "TLS handshake data at level " << static_cast<int>(level)
                  << ": " << len << " bytes\n";

        if (level == ssl_encryption_initial) {
            // This is the ClientHello
            crypto_send_buffer_.insert(crypto_send_buffer_.end(), data, data + len);
        } else if (level == ssl_encryption_handshake) {
            // Handshake level data (after we get ServerHello)
            handshake_send_buffer_.insert(handshake_send_buffer_.end(), data, data + len);
        }

        return 1;
    }

    size_t build_transport_params(uint8_t* buf, size_t buf_size) {
        size_t pos = 0;

        // initial_max_data (0x04)
        buf[pos++] = 0x04;
        buf[pos++] = 0x04; // length 4
        buf[pos++] = 0x80; // varint prefix for 4-byte value
        buf[pos++] = 0x00;
        buf[pos++] = 0x10;
        buf[pos++] = 0x00; // 1MB

        // initial_max_stream_data_bidi_local (0x05)
        buf[pos++] = 0x05;
        buf[pos++] = 0x04;
        buf[pos++] = 0x80;
        buf[pos++] = 0x00;
        buf[pos++] = 0x10;
        buf[pos++] = 0x00;

        // initial_max_stream_data_bidi_remote (0x06)
        buf[pos++] = 0x06;
        buf[pos++] = 0x04;
        buf[pos++] = 0x80;
        buf[pos++] = 0x00;
        buf[pos++] = 0x10;
        buf[pos++] = 0x00;

        // initial_max_stream_data_uni (0x07)
        buf[pos++] = 0x07;
        buf[pos++] = 0x04;
        buf[pos++] = 0x80;
        buf[pos++] = 0x00;
        buf[pos++] = 0x10;
        buf[pos++] = 0x00;

        // initial_max_streams_bidi (0x08)
        buf[pos++] = 0x08;
        buf[pos++] = 0x02;
        buf[pos++] = 0x40;
        buf[pos++] = 0x64; // 100 streams

        // initial_max_streams_uni (0x09)
        buf[pos++] = 0x09;
        buf[pos++] = 0x02;
        buf[pos++] = 0x40;
        buf[pos++] = 0x64;

        // initial_source_connection_id (0x0f)
        buf[pos++] = 0x0f;
        buf[pos++] = local_cid_.length;
        memcpy(buf + pos, local_cid_.data, local_cid_.length);
        pos += local_cid_.length;

        return pos;
    }

    bool send_initial_packet() {
        std::cout << "\n=== Sending Initial Packet ===\n";

        uint8_t packet[1200];
        memset(packet, 0xAA, sizeof(packet)); // Fill with pattern to detect unwritten areas
        size_t packet_len = 0;

        // Long header: Form=1, Fixed=1, Type=Initial(0), Reserved=0, PN_Length=3 (0-indexed)
        // First byte: 1100 0011 = 0xC3 (4-byte PN)
        // But for first packet, let's use 1-byte PN: 1100 0000 = 0xC0
        packet[packet_len++] = 0xC0;

        // Version (QUIC v1 = 0x00000001)
        packet[packet_len++] = 0x00;
        packet[packet_len++] = 0x00;
        packet[packet_len++] = 0x00;
        packet[packet_len++] = 0x01;

        std::cout << "After writing first byte and version:\n";
        std::cout << "  packet[0] = 0x" << std::hex << static_cast<int>(packet[0]) << std::dec << "\n";
        std::cout << "  packet[4] = 0x" << std::hex << static_cast<int>(packet[4]) << std::dec << "\n";
        std::cout << "  packet_len = " << packet_len << "\n";

        // DCID length and value
        packet[packet_len++] = peer_cid_.length;
        memcpy(packet + packet_len, peer_cid_.data, peer_cid_.length);
        packet_len += peer_cid_.length;

        std::cout << "After DCID:\n";
        std::cout << "  packet[5] = 0x" << std::hex << static_cast<int>(packet[5]) << " (DCID len)\n";
        std::cout << "  packet[6] = 0x" << std::hex << static_cast<int>(packet[6]) << " (DCID[0])\n" << std::dec;
        std::cout << "  packet_len = " << packet_len << "\n";

        // SCID length and value
        packet[packet_len++] = local_cid_.length;
        memcpy(packet + packet_len, local_cid_.data, local_cid_.length);
        packet_len += local_cid_.length;

        // Token length (0 for initial without retry)
        packet[packet_len++] = 0x00;

        std::cout << "After SCID and token len:\n";
        std::cout << "  packet_len = " << packet_len << "\n";
        hex_dump("Header so far", packet, packet_len);

        // Build payload: CRYPTO frame with ClientHello
        // Need enough space for CRYPTO frame (~300 bytes) + padding to reach 1200 bytes
        uint8_t payload[1200];
        size_t payload_len = 0;

        // CRYPTO frame: type=0x06, offset, length, data
        payload[payload_len++] = 0x06;

        // Offset (varint) = 0
        payload_len += VarInt::encode(0, payload + payload_len);

        // Length (varint)
        payload_len += VarInt::encode(crypto_send_buffer_.size(), payload + payload_len);

        // Data (ClientHello)
        memcpy(payload + payload_len, crypto_send_buffer_.data(), crypto_send_buffer_.size());
        payload_len += crypto_send_buffer_.size();

        // Calculate how much padding we need to reach 1200 bytes minimum
        size_t header_so_far = packet_len;
        size_t pn_len = 1; // We'll use 1-byte packet number
        size_t length_field_size = 2; // Variable-length integer for packet length
        size_t tag_size = 16; // AEAD tag

        size_t min_packet_size = 1200;
        size_t overhead = header_so_far + length_field_size + pn_len + tag_size;
        size_t padding_needed = 0;

        if (overhead + payload_len < min_packet_size) {
            padding_needed = min_packet_size - overhead - payload_len;
        }

        // Add PADDING frames
        if (padding_needed > 0) {
            memset(payload + payload_len, 0x00, padding_needed);
            payload_len += padding_needed;
        }

        // Now encode packet length (pn_len + payload_len + tag)
        size_t length_value = pn_len + payload_len + tag_size;

        // Use 2-byte varint for length (0x4000 + length)
        packet[packet_len++] = 0x40 | ((length_value >> 8) & 0x3F);
        packet[packet_len++] = length_value & 0xFF;

        size_t pn_offset = packet_len;

        // Packet number (1 byte, value 0)
        uint64_t pn = next_pn_++;
        packet[packet_len++] = pn & 0xFF;

        std::cout << "Packet number offset: " << pn_offset << "\n";
        std::cout << "Payload size: " << payload_len << " bytes\n";
        std::cout << "Header length (including PN): " << packet_len << "\n";

        // Debug: dump header before encryption
        hex_dump("Header BEFORE encryption", packet, packet_len);

        // Encrypt payload
        uint8_t encrypted[1200];
        size_t encrypted_len;

        // AAD is the header including the packet number
        if (client_initial_pp_.encrypt(pn, packet, packet_len, payload, payload_len,
                                        encrypted, &encrypted_len) != 0) {
            std::cerr << "Encryption failed\n";
            return false;
        }

        // Debug: dump header after encryption (AAD shouldn't change it)
        hex_dump("Header AFTER encryption (should be same)", packet, packet_len);

        // Copy encrypted payload
        memcpy(packet + packet_len, encrypted, encrypted_len);
        size_t total_len = packet_len + encrypted_len;

        // Debug: dump header after memcpy
        hex_dump("Header AFTER memcpy", packet, packet_len);

        // Apply header protection
        // Sample starts 4 bytes after packet number
        size_t sample_offset = pn_offset + 4;
        if (sample_offset + 16 > total_len) {
            std::cerr << "Not enough data for sample (need " << (sample_offset + 16)
                      << ", have " << total_len << ")\n";
            return false;
        }

        std::cout << "Sample offset: " << sample_offset << "\n";
        hex_dump("HP Sample", packet + sample_offset, 16);

        if (client_initial_pp_.protect_header(packet, pn_offset, pn_len,
                                               packet + sample_offset) != 0) {
            std::cerr << "Header protection failed\n";
            return false;
        }

        hex_dump("Initial Packet AFTER header protection", packet, total_len);

        // Send
        ssize_t sent = send(socket_, packet, total_len, 0);
        if (sent < 0) {
            std::cerr << "Send failed: " << strerror(errno) << "\n";
            return false;
        }
        std::cout << "Sent " << sent << " bytes\n";

        return true;
    }

    bool receive_and_process_response() {
        std::cout << "\n=== Waiting for Response ===\n";

        // Wait for response (up to 3 seconds)
        struct pollfd pfd;
        pfd.fd = socket_;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 3000);
        if (ret <= 0) {
            if (ret == 0) {
                std::cerr << "Timeout - no response received\n";
            } else {
                std::cerr << "Poll error: " << strerror(errno) << "\n";
            }
            return false;
        }

        uint8_t recv_buf[2000];
        ssize_t recv_len = recv(socket_, recv_buf, sizeof(recv_buf), 0);

        if (recv_len <= 0) {
            std::cerr << "Receive failed: " << strerror(errno) << "\n";
            return false;
        }

        std::cout << "Received " << recv_len << " bytes\n";
        hex_dump("Response", recv_buf, recv_len);

        // Parse response
        return parse_response(recv_buf, recv_len);
    }

    bool parse_response(const uint8_t* data, size_t len) {
        if (len < 6) {
            std::cerr << "Response too short\n";
            return false;
        }

        bool is_long = (data[0] & 0x80) != 0;
        std::cout << "Header form: " << (is_long ? "Long" : "Short") << "\n";

        if (!is_long) {
            std::cout << "Unexpected short header in response\n";
            return false;
        }

        // Parse version
        uint32_t version = (data[1] << 24) | (data[2] << 16) | (data[3] << 8) | data[4];
        std::cout << "Version: 0x" << std::hex << version << std::dec << "\n";

        if (version == 0) {
            // Version Negotiation
            std::cout << "Received Version Negotiation packet\n";
            return parse_version_negotiation(data, len);
        }

        // Get packet type
        uint8_t packet_type = (data[0] >> 4) & 0x03;
        std::cout << "Packet type: " << static_cast<int>(packet_type);
        switch (packet_type) {
            case 0: std::cout << " (Initial)\n"; break;
            case 1: std::cout << " (0-RTT)\n"; break;
            case 2: std::cout << " (Handshake)\n"; break;
            case 3: std::cout << " (Retry)\n"; break;
        }

        if (packet_type == 3) {
            // Retry packet - no encryption
            return parse_retry(data, len);
        }

        // Parse header
        size_t pos = 5;

        // DCID
        uint8_t dcid_len = data[pos++];
        std::cout << "DCID length: " << static_cast<int>(dcid_len) << "\n";
        if (dcid_len > 0 && dcid_len <= 20) {
            std::cout << "DCID: ";
            for (int i = 0; i < dcid_len; i++) {
                std::cout << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<int>(data[pos + i]);
            }
            std::cout << std::dec << "\n";
        }
        pos += dcid_len;

        // SCID
        uint8_t scid_len = data[pos++];
        std::cout << "SCID length: " << static_cast<int>(scid_len) << "\n";
        if (scid_len > 0 && scid_len <= 20) {
            std::cout << "SCID: ";
            for (int i = 0; i < scid_len; i++) {
                std::cout << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<int>(data[pos + i]);
            }
            std::cout << std::dec << "\n";

            // Update peer CID
            if (packet_type == 0) {
                memcpy(peer_cid_.data, data + pos, scid_len);
                peer_cid_.length = scid_len;
            }
        }
        pos += scid_len;

        // Token (Initial only)
        if (packet_type == 0) {
            uint64_t token_len;
            int token_len_bytes = VarInt::decode(data + pos, len - pos, token_len);
            if (token_len_bytes < 0) {
                std::cerr << "Failed to decode token length\n";
                return false;
            }
            pos += token_len_bytes + token_len;
            std::cout << "Token length: " << token_len << "\n";
        }

        // Packet length
        uint64_t pkt_len;
        int pkt_len_bytes = VarInt::decode(data + pos, len - pos, pkt_len);
        if (pkt_len_bytes < 0) {
            std::cerr << "Failed to decode packet length\n";
            return false;
        }
        pos += pkt_len_bytes;
        std::cout << "Packet length: " << pkt_len << "\n";

        size_t pn_offset = pos;
        std::cout << "PN offset: " << pn_offset << "\n";

        // Sample for header protection removal
        size_t sample_offset = pn_offset + 4;
        if (sample_offset + 16 > len) {
            std::cerr << "Not enough data for sample\n";
            return false;
        }

        // Copy for modification
        std::vector<uint8_t> header_copy(data, data + len);

        // Remove header protection using server Initial keys
        // (Server encrypts with server keys, we decrypt with server keys)
        size_t pn_length;
        if (server_initial_pp_.unprotect_header(header_copy.data(), pn_offset,
                                                 header_copy.data() + sample_offset,
                                                 &pn_length) != 0) {
            std::cerr << "Failed to remove header protection\n";
            return false;
        }

        std::cout << "PN length: " << pn_length << "\n";

        // Extract packet number
        uint64_t recv_pn = 0;
        for (size_t i = 0; i < pn_length; i++) {
            recv_pn = (recv_pn << 8) | header_copy[pn_offset + i];
        }
        std::cout << "Packet number: " << recv_pn << "\n";

        // Decrypt payload
        size_t header_len = pn_offset + pn_length;
        const uint8_t* ciphertext = data + header_len;
        size_t ciphertext_len = len - header_len;

        std::vector<uint8_t> plaintext(ciphertext_len);
        size_t plaintext_len;

        if (server_initial_pp_.decrypt(recv_pn, header_copy.data(), header_len,
                                        ciphertext, ciphertext_len,
                                        plaintext.data(), &plaintext_len) != 0) {
            std::cerr << "Decryption failed!\n";
            std::cerr << "This could mean:\n";
            std::cerr << "  1. Server derived keys from different DCID\n";
            std::cerr << "  2. Packet protection algorithm mismatch\n";
            std::cerr << "  3. Header protection removal failed\n";
            return false;
        }

        std::cout << "\n=== Decryption Successful! ===\n";
        std::cout << "Plaintext length: " << plaintext_len << " bytes\n";
        hex_dump("Decrypted payload", plaintext.data(), plaintext_len);

        // Parse frames
        return parse_frames(plaintext.data(), plaintext_len);
    }

    bool parse_version_negotiation(const uint8_t* data, size_t len) {
        size_t pos = 5;
        uint8_t dcid_len = data[pos++];
        pos += dcid_len;
        uint8_t scid_len = data[pos++];
        pos += scid_len;

        std::cout << "Supported versions: ";
        while (pos + 4 <= len) {
            uint32_t v = (data[pos] << 24) | (data[pos+1] << 16) |
                         (data[pos+2] << 8) | data[pos+3];
            std::cout << "0x" << std::hex << v << " ";
            pos += 4;
        }
        std::cout << std::dec << "\n";
        return true;
    }

    bool parse_retry(const uint8_t* data, size_t len) {
        std::cout << "Received Retry packet\n";
        // Retry token handling would go here
        return true;
    }

    bool parse_frames(const uint8_t* data, size_t len) {
        std::cout << "\n=== Parsing Frames ===\n";

        size_t pos = 0;
        while (pos < len) {
            uint64_t frame_type;
            int type_len = VarInt::decode(data + pos, len - pos, frame_type);
            if (type_len < 0) break;

            if (frame_type == 0x00) {
                // PADDING - skip
                pos++;
                continue;
            }

            pos += type_len;
            std::cout << "Frame type: 0x" << std::hex << frame_type << std::dec << "\n";

            if (frame_type == 0x02 || frame_type == 0x03) {
                // ACK frame
                std::cout << "  -> ACK frame\n";

                uint64_t largest_acked, ack_delay, ack_range_count, first_range;
                int n = VarInt::decode(data + pos, len - pos, largest_acked);
                if (n < 0) break;
                pos += n;

                n = VarInt::decode(data + pos, len - pos, ack_delay);
                if (n < 0) break;
                pos += n;

                n = VarInt::decode(data + pos, len - pos, ack_range_count);
                if (n < 0) break;
                pos += n;

                n = VarInt::decode(data + pos, len - pos, first_range);
                if (n < 0) break;
                pos += n;

                std::cout << "     Largest ACKed: " << largest_acked << "\n";

                // Skip additional ranges
                for (uint64_t i = 0; i < ack_range_count; i++) {
                    uint64_t gap, range_len;
                    n = VarInt::decode(data + pos, len - pos, gap);
                    if (n < 0) break;
                    pos += n;
                    n = VarInt::decode(data + pos, len - pos, range_len);
                    if (n < 0) break;
                    pos += n;
                }

            } else if (frame_type == 0x06) {
                // CRYPTO frame
                uint64_t offset, crypto_len;
                int n = VarInt::decode(data + pos, len - pos, offset);
                if (n < 0) break;
                pos += n;

                n = VarInt::decode(data + pos, len - pos, crypto_len);
                if (n < 0) break;
                pos += n;

                std::cout << "  -> CRYPTO frame: offset=" << offset
                          << ", length=" << crypto_len << "\n";

                if (pos + crypto_len > len) {
                    std::cerr << "CRYPTO frame data exceeds packet\n";
                    break;
                }

                // Parse TLS record type
                if (crypto_len > 0) {
                    uint8_t tls_type = data[pos];
                    std::cout << "     TLS record type: 0x" << std::hex
                              << static_cast<int>(tls_type) << std::dec << "\n";

                    if (tls_type == 0x02) {
                        std::cout << "     *** ServerHello received! ***\n";

                        // Provide data to TLS
                        if (SSL_provide_quic_data(ssl_, ssl_encryption_initial,
                                                  data + pos, crypto_len) != 1) {
                            std::cerr << "Failed to provide CRYPTO data to TLS\n";
                            ERR_print_errors_fp(stderr);
                        } else {
                            std::cout << "     Provided " << crypto_len << " bytes to TLS\n";

                            // Continue handshake
                            int ret = SSL_do_handshake(ssl_);
                            if (ret != 1) {
                                int err = SSL_get_error(ssl_, ret);
                                if (err == SSL_ERROR_WANT_READ) {
                                    std::cout << "     TLS wants more data (expected)\n";
                                } else if (err == SSL_ERROR_WANT_WRITE) {
                                    std::cout << "     TLS wants to write\n";
                                } else {
                                    std::cerr << "     TLS error: " << err << "\n";
                                    ERR_print_errors_fp(stderr);
                                }
                            } else {
                                std::cout << "     *** TLS Handshake Complete! ***\n";
                            }
                        }
                    }

                    hex_dump("     TLS data", data + pos, std::min(crypto_len, (uint64_t)64));
                }

                pos += crypto_len;

            } else if (frame_type == 0x1c || frame_type == 0x1d) {
                // CONNECTION_CLOSE
                std::cout << "  -> CONNECTION_CLOSE frame\n";
                uint64_t error_code;
                int n = VarInt::decode(data + pos, len - pos, error_code);
                if (n > 0) {
                    pos += n;
                    std::cout << "     Error code: 0x" << std::hex << error_code << std::dec << "\n";

                    if (frame_type == 0x1c) {
                        // QUIC error - has frame type field
                        uint64_t frame_type_field;
                        n = VarInt::decode(data + pos, len - pos, frame_type_field);
                        if (n > 0) pos += n;
                    }

                    // Reason phrase
                    uint64_t reason_len;
                    n = VarInt::decode(data + pos, len - pos, reason_len);
                    if (n > 0) {
                        pos += n;
                        if (reason_len > 0 && pos + reason_len <= len) {
                            std::string reason(reinterpret_cast<const char*>(data + pos), reason_len);
                            std::cout << "     Reason: " << reason << "\n";
                            pos += reason_len;
                        }
                    }
                }
                break;

            } else {
                std::cout << "  -> Unknown/unhandled frame type\n";
                break;
            }
        }

        return true;
    }

    SSL_CTX* ssl_ctx_;
    SSL* ssl_;
    int socket_;
    ConnectionID local_cid_;
    ConnectionID peer_cid_;
    ConnectionID original_dcid_;
    PacketProtection client_initial_pp_;
    PacketProtection server_initial_pp_;
    uint64_t next_pn_{0};

    std::vector<uint8_t> crypto_send_buffer_;
    std::vector<uint8_t> handshake_send_buffer_;
};

int main(int argc, char** argv) {
    const char* host = "127.0.0.1";
    uint16_t port = 8443;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = static_cast<uint16_t>(std::atoi(argv[2]));

    std::cout << "=== QUIC Handshake Test (with proper quictls API) ===\n";
    std::cout << "Target: " << host << ":" << port << "\n\n";

    QuicTestClient client;

    if (!client.init(host, port)) {
        std::cerr << "Failed to initialize client\n";
        return 1;
    }

    if (!client.do_handshake()) {
        std::cerr << "Handshake failed\n";
        return 1;
    }

    std::cout << "\n=== Test Complete ===\n";
    return 0;
}
