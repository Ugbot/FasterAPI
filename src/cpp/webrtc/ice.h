#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <array>

namespace fasterapi {
namespace webrtc {

/**
 * ICE (Interactive Connectivity Establishment) support.
 * 
 * Based on Pion's ICE implementation (pion/ice) - excellent Go WebRTC library.
 * Adapted for zero-allocation, C++ performance.
 * 
 * Specs:
 * - ICE: RFC 8445
 * - STUN: RFC 8489
 * - TURN: RFC 8656
 * 
 * Pion approach:
 * - Simple, readable state machines
 * - Minimal dependencies
 * - Well-tested algorithms
 * 
 * Our adaptations:
 * - Zero heap allocations
 * - Stack-allocated candidates
 * - simdjson for JSON parsing
 * - Direct buffer access
 */

/**
 * ICE candidate type (RFC 8445).
 */
enum class ICECandidateType : uint8_t {
    HOST,       // Local interface
    SRFLX,      // Server reflexive (via STUN)
    PRFLX,      // Peer reflexive
    RELAY       // Relayed (via TURN)
};

/**
 * ICE transport protocol.
 */
enum class ICEProtocol : uint8_t {
    UDP,
    TCP
};

/**
 * ICE candidate.
 * 
 * Represents a potential connection endpoint.
 */
struct ICECandidate {
    ICECandidateType type;
    ICEProtocol protocol;
    
    std::string foundation;      // Candidate identifier
    uint32_t priority;           // Candidate priority
    std::string address;         // IP address
    uint16_t port;               // Port number
    std::string related_address; // Related address (for reflexive/relay)
    uint16_t related_port;       // Related port
    
    // Component (1 = RTP, 2 = RTCP)
    uint8_t component;
    
    /**
     * Generate ICE candidate string (for SDP).
     * 
     * Format: candidate:<foundation> <component> <protocol> <priority> <address> <port> typ <type>
     */
    std::string to_string() const;
    
    /**
     * Parse ICE candidate from string.
     * 
     * @param candidate_str Candidate string
     * @param out_candidate Parsed candidate
     * @return 0 on success
     */
    static int from_string(std::string_view candidate_str, ICECandidate& out_candidate) noexcept;
};

/**
 * STUN message (simplified).
 * 
 * Based on Pion's STUN implementation.
 * Used for NAT traversal and connectivity checks.
 */
struct STUNMessage {
    // STUN message types
    enum class Type : uint16_t {
        BINDING_REQUEST = 0x0001,
        BINDING_RESPONSE = 0x0101,
        BINDING_ERROR = 0x0111,
    };
    
    Type type;
    uint32_t transaction_id[3];  // 96-bit transaction ID
    
    // Attributes
    std::string username;
    std::string password;
    std::string mapped_address;  // Public IP:port
    uint16_t mapped_port;
    
    /**
     * Parse STUN message from buffer.
     * 
     * @param data Buffer
     * @param len Buffer length
     * @param out_message Parsed message
     * @return 0 on success
     */
    static int parse(const uint8_t* data, size_t len, STUNMessage& out_message) noexcept;
    
    /**
     * Generate STUN message.
     * 
     * @param message Message to serialize
     * @param out_buffer Output buffer
     * @param buffer_size Buffer capacity
     * @param out_written Bytes written
     * @return 0 on success
     */
    static int generate(
        const STUNMessage& message,
        uint8_t* out_buffer,
        size_t buffer_size,
        size_t& out_written
    ) noexcept;
};

/**
 * ICE agent (simplified).
 * 
 * Manages ICE candidate gathering and connectivity checks.
 * Based on Pion's ice.Agent but adapted for our needs.
 */
class ICEAgent {
public:
    /**
     * ICE configuration.
     */
    struct Config {
        std::vector<std::string> stun_servers;   // STUN server URLs
        std::vector<std::string> turn_servers;   // TURN server URLs
        bool gather_host_candidates;
        bool gather_srflx_candidates;
        bool gather_relay_candidates;
        
        Config()
            : gather_host_candidates(true),
              gather_srflx_candidates(true),
              gather_relay_candidates(false) {}
    };
    
    explicit ICEAgent(const Config& config);
    ~ICEAgent();
    
    /**
     * Gather ICE candidates.
     * 
     * Discovers local network interfaces and queries STUN/TURN servers.
     * 
     * @param out_candidates Vector to populate with candidates
     * @return 0 on success
     */
    int gather_candidates(std::vector<ICECandidate>& out_candidates) noexcept;
    
    /**
     * Add remote candidate.
     * 
     * @param candidate Remote candidate
     * @return 0 on success
     */
    int add_remote_candidate(const ICECandidate& candidate) noexcept;
    
    /**
     * Start connectivity checks.
     * 
     * @return 0 on success
     */
    int start_connectivity_checks() noexcept;
    
    /**
     * Get selected candidate pair (after checks complete).
     * 
     * @param out_local Local candidate
     * @param out_remote Remote candidate
     * @return 0 on success, 1 if no pair selected
     */
    int get_selected_pair(
        ICECandidate& out_local,
        ICECandidate& out_remote
    ) const noexcept;
    
private:
    Config config_;
    std::vector<ICECandidate> local_candidates_;
    std::vector<ICECandidate> remote_candidates_;
    
    /**
     * Gather host candidates (local interfaces).
     */
    int gather_host_candidates() noexcept;
    
    /**
     * Gather server reflexive candidates (via STUN).
     */
    int gather_srflx_candidates() noexcept;
};

} // namespace webrtc
} // namespace fasterapi

