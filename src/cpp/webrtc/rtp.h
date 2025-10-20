#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <array>

namespace fasterapi {
namespace webrtc {

/**
 * RTP (Real-time Transport Protocol) implementation.
 * 
 * Based on Pion's RTP implementation (pion/rtp).
 * Used for audio/video streaming in WebRTC.
 * 
 * Specs:
 * - RTP: RFC 3550
 * - SRTP: RFC 3711
 * 
 * Adaptations:
 * - Zero-copy packet parsing
 * - Stack-allocated headers
 * - Direct buffer access
 * - SIMD optimization for encryption
 * 
 * Performance targets:
 * - Parse RTP header: <20ns
 * - SRTP encrypt: <500ns per packet
 * - SRTP decrypt: <400ns per packet
 */

/**
 * RTP header (12 bytes minimum).
 */
struct RTPHeader {
    uint8_t version : 2;         // Always 2
    uint8_t padding : 1;
    uint8_t extension : 1;
    uint8_t csrc_count : 4;
    
    uint8_t marker : 1;
    uint8_t payload_type : 7;
    
    uint16_t sequence_number;
    uint32_t timestamp;
    uint32_t ssrc;               // Synchronization source
    
    // CSRC list follows (if csrc_count > 0)
    std::array<uint32_t, 15> csrc;  // Max 15 contributing sources
    
    /**
     * Parse RTP header from buffer.
     * 
     * @param data Packet buffer
     * @param len Buffer length
     * @param out_header Parsed header
     * @param out_header_len Header length in bytes
     * @return 0 on success
     */
    static int parse(
        const uint8_t* data,
        size_t len,
        RTPHeader& out_header,
        size_t& out_header_len
    ) noexcept;
    
    /**
     * Serialize RTP header to buffer.
     * 
     * @param header Header to serialize
     * @param out_buffer Output buffer (min 12 bytes)
     * @param buffer_size Buffer capacity
     * @param out_written Bytes written
     * @return 0 on success
     */
    static int serialize(
        const RTPHeader& header,
        uint8_t* out_buffer,
        size_t buffer_size,
        size_t& out_written
    ) noexcept;
};

/**
 * RTP packet (zero-copy).
 */
struct RTPPacket {
    RTPHeader header;
    std::string_view payload;  // View into original buffer
    
    /**
     * Parse RTP packet.
     * 
     * @param data Packet buffer (must remain valid)
     * @param len Buffer length
     * @param out_packet Parsed packet (payload views into data)
     * @return 0 on success
     */
    static int parse(
        const uint8_t* data,
        size_t len,
        RTPPacket& out_packet
    ) noexcept;
};

/**
 * SRTP (Secure RTP) context.
 * 
 * Handles encryption/decryption of RTP packets.
 * Based on Pion's SRTP (pion/srtp).
 */
class SRTPContext {
public:
    /**
     * SRTP profile.
     */
    enum class Profile : uint8_t {
        AES128_CM_SHA1_80,   // AES-128 Counter Mode, HMAC-SHA1 80-bit auth
        AES128_CM_SHA1_32,   // AES-128 Counter Mode, HMAC-SHA1 32-bit auth
        AEAD_AES_128_GCM,    // AES-128 GCM (modern, preferred)
        AEAD_AES_256_GCM     // AES-256 GCM
    };
    
    /**
     * Create SRTP context.
     * 
     * @param profile SRTP profile
     * @param master_key Master key (16 or 32 bytes depending on profile)
     * @param master_salt Master salt (14 bytes)
     */
    SRTPContext(
        Profile profile,
        const uint8_t* master_key,
        size_t key_len,
        const uint8_t* master_salt,
        size_t salt_len
    );
    
    ~SRTPContext();
    
    /**
     * Encrypt RTP packet to SRTP.
     * 
     * @param rtp_data RTP packet
     * @param rtp_len RTP length
     * @param out_srtp Output buffer for SRTP
     * @param out_capacity Output buffer capacity
     * @param out_srtp_len SRTP length
     * @return 0 on success
     */
    int encrypt(
        const uint8_t* rtp_data,
        size_t rtp_len,
        uint8_t* out_srtp,
        size_t out_capacity,
        size_t& out_srtp_len
    ) noexcept;
    
    /**
     * Decrypt SRTP packet to RTP.
     * 
     * @param srtp_data SRTP packet
     * @param srtp_len SRTP length
     * @param out_rtp Output buffer for RTP
     * @param out_capacity Output buffer capacity
     * @param out_rtp_len RTP length
     * @return 0 on success
     */
    int decrypt(
        const uint8_t* srtp_data,
        size_t srtp_len,
        uint8_t* out_rtp,
        size_t out_capacity,
        size_t& out_rtp_len
    ) noexcept;
    
private:
    Profile profile_;
    
    // Keys (stack-allocated)
    std::array<uint8_t, 32> master_key_;    // Up to 256 bits
    std::array<uint8_t, 14> master_salt_;
    size_t key_len_;
    size_t salt_len_;
    
    // Replay protection
    uint64_t roc_{0};  // Rollover counter
    uint16_t last_seq_{0};
    
    /**
     * Derive session keys from master key.
     */
    void derive_session_keys() noexcept;
};

/**
 * Media codec information.
 */
struct CodecInfo {
    uint8_t payload_type;        // RTP payload type
    std::string name;            // Codec name (opus, VP8, H264, etc.)
    uint32_t clock_rate;         // Sample rate
    uint8_t channels;            // Audio channels (0 for video)
    
    // Common codecs
    static const CodecInfo OPUS;       // Audio: Opus
    static const CodecInfo PCMU;       // Audio: G.711 µ-law
    static const CodecInfo VP8;        // Video: VP8
    static const CodecInfo VP9;        // Video: VP9
    static const CodecInfo H264;       // Video: H.264
    static const CodecInfo AV1;        // Video: AV1
};

} // namespace webrtc
} // namespace fasterapi

