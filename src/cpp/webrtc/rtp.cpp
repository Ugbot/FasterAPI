#include "rtp.h"
#include <cstring>

// Network byte order functions (cross-platform)
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace fasterapi {
namespace webrtc {

// ============================================================================
// RTPHeader Implementation
// ============================================================================

int RTPHeader::parse(
    const uint8_t* data,
    size_t len,
    RTPHeader& out_header,
    size_t& out_header_len
) noexcept {
    if (!data || len < 12) {
        return 1;  // Need at least 12 bytes
    }
    
    // Parse fixed header (12 bytes)
    out_header.version = (data[0] >> 6) & 0x03;
    out_header.padding = (data[0] >> 5) & 0x01;
    out_header.extension = (data[0] >> 4) & 0x01;
    out_header.csrc_count = data[0] & 0x0F;
    
    out_header.marker = (data[1] >> 7) & 0x01;
    out_header.payload_type = data[1] & 0x7F;
    
    out_header.sequence_number = ntohs(*reinterpret_cast<const uint16_t*>(data + 2));
    out_header.timestamp = ntohl(*reinterpret_cast<const uint32_t*>(data + 4));
    out_header.ssrc = ntohl(*reinterpret_cast<const uint32_t*>(data + 8));
    
    size_t header_len = 12;
    
    // Parse CSRC list if present
    if (out_header.csrc_count > 0) {
        if (len < header_len + out_header.csrc_count * 4) {
            return 1;  // Not enough data
        }
        
        for (uint8_t i = 0; i < out_header.csrc_count; ++i) {
            out_header.csrc[i] = ntohl(*reinterpret_cast<const uint32_t*>(data + header_len));
            header_len += 4;
        }
    }
    
    out_header_len = header_len;
    return 0;
}

int RTPHeader::serialize(
    const RTPHeader& header,
    uint8_t* out_buffer,
    size_t buffer_size,
    size_t& out_written
) noexcept {
    size_t required = 12 + header.csrc_count * 4;
    if (buffer_size < required) {
        return 1;  // Buffer too small
    }
    
    // Fixed header (12 bytes)
    out_buffer[0] = (header.version << 6) | (header.padding << 5) | 
                    (header.extension << 4) | header.csrc_count;
    out_buffer[1] = (header.marker << 7) | header.payload_type;
    
    *reinterpret_cast<uint16_t*>(out_buffer + 2) = htons(header.sequence_number);
    *reinterpret_cast<uint32_t*>(out_buffer + 4) = htonl(header.timestamp);
    *reinterpret_cast<uint32_t*>(out_buffer + 8) = htonl(header.ssrc);
    
    size_t written = 12;
    
    // CSRC list
    for (uint8_t i = 0; i < header.csrc_count; ++i) {
        *reinterpret_cast<uint32_t*>(out_buffer + written) = htonl(header.csrc[i]);
        written += 4;
    }
    
    out_written = written;
    return 0;
}

// ============================================================================
// RTPPacket Implementation
// ============================================================================

int RTPPacket::parse(
    const uint8_t* data,
    size_t len,
    RTPPacket& out_packet
) noexcept {
    size_t header_len;
    
    if (RTPHeader::parse(data, len, out_packet.header, header_len) != 0) {
        return 1;
    }
    
    // Payload is everything after header (zero-copy)
    if (header_len < len) {
        out_packet.payload = std::string_view(
            reinterpret_cast<const char*>(data + header_len),
            len - header_len
        );
    }
    
    return 0;
}

// ============================================================================
// SRTPContext Implementation
// ============================================================================

SRTPContext::SRTPContext(
    Profile profile,
    const uint8_t* master_key,
    size_t key_len,
    const uint8_t* master_salt,
    size_t salt_len
) : profile_(profile), key_len_(key_len), salt_len_(salt_len) {
    
    // Copy keys to internal storage
    if (key_len <= master_key_.size()) {
        std::memcpy(master_key_.data(), master_key, key_len);
    }
    
    if (salt_len <= master_salt_.size()) {
        std::memcpy(master_salt_.data(), master_salt, salt_len);
    }
    
    derive_session_keys();
}

SRTPContext::~SRTPContext() {
    // Zero out keys
    master_key_.fill(0);
    master_salt_.fill(0);
}

int SRTPContext::encrypt(
    const uint8_t* rtp_data,
    size_t rtp_len,
    uint8_t* out_srtp,
    size_t out_capacity,
    size_t& out_srtp_len
) noexcept {
    // TODO: Implement SRTP encryption
    // For now, simplified (just copy + add auth tag)
    
    if (out_capacity < rtp_len + 10) {
        return 1;  // Need room for auth tag
    }
    
    // Copy RTP data
    std::memcpy(out_srtp, rtp_data, rtp_len);
    
    // Add authentication tag (10 bytes for SHA1-80)
    // TODO: Actual HMAC-SHA1 calculation
    std::memset(out_srtp + rtp_len, 0, 10);
    
    out_srtp_len = rtp_len + 10;
    return 0;
}

int SRTPContext::decrypt(
    const uint8_t* srtp_data,
    size_t srtp_len,
    uint8_t* out_rtp,
    size_t out_capacity,
    size_t& out_rtp_len
) noexcept {
    // TODO: Implement SRTP decryption
    // For now, simplified (just verify and remove auth tag)
    
    if (srtp_len < 10) {
        return 1;  // Too small
    }
    
    size_t rtp_len = srtp_len - 10;
    
    if (out_capacity < rtp_len) {
        return 1;
    }
    
    // Verify auth tag
    // TODO: Actual HMAC-SHA1 verification
    
    // Copy RTP data
    std::memcpy(out_rtp, srtp_data, rtp_len);
    
    out_rtp_len = rtp_len;
    return 0;
}

void SRTPContext::derive_session_keys() noexcept {
    // TODO: Implement key derivation (RFC 3711 Section 4.3)
    // Uses AES-CM for key derivation from master key
}

// ============================================================================
// Codec Definitions
// ============================================================================

const CodecInfo CodecInfo::OPUS = {111, "opus", 48000, 2};
const CodecInfo CodecInfo::PCMU = {0, "PCMU", 8000, 1};
const CodecInfo CodecInfo::VP8 = {96, "VP8", 90000, 0};
const CodecInfo CodecInfo::VP9 = {98, "VP9", 90000, 0};
const CodecInfo CodecInfo::H264 = {102, "H264", 90000, 0};
const CodecInfo CodecInfo::AV1 = {35, "AV1", 90000, 0};

} // namespace webrtc
} // namespace fasterapi

