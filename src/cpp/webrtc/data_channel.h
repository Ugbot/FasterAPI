#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <functional>
#include <vector>

namespace fasterapi {
namespace webrtc {

/**
 * WebRTC Data Channel.
 * 
 * Based on Pion's datachannel implementation (pion/sctp + pion/datachannel).
 * Provides reliable/unreliable bidirectional data transport.
 * 
 * Specs:
 * - Data Channel: RFC 8831
 * - SCTP: RFC 4960
 * - SCTP over DTLS: RFC 8261
 * 
 * Pion approach:
 * - Clean SCTP state machine
 * - Chunk-based processing
 * - Zero-copy where possible
 * 
 * Our adaptations:
 * - Stack-allocated buffers
 * - Zero-copy message passing
 * - Direct buffer access
 * - No dynamic allocation in hot path
 * 
 * Performance targets:
 * - Send message: <1µs
 * - Receive message: <500ns
 * - Zero allocations for small messages (<1KB)
 */

/**
 * Data channel options.
 */
struct DataChannelOptions {
    bool ordered{true};              // Ordered delivery
    uint16_t max_retransmits{0};     // Max retransmissions (0 = unlimited)
    uint16_t max_packet_lifetime_ms{0}; // Max packet lifetime
    std::string protocol;            // Sub-protocol
    bool negotiated{false};          // Pre-negotiated channel
    uint16_t id{0};                  // Channel ID (if negotiated)
};

/**
 * Data channel state.
 */
enum class DataChannelState : uint8_t {
    CONNECTING,
    OPEN,
    CLOSING,
    CLOSED
};

/**
 * SCTP Payload Protocol Identifiers for WebRTC (RFC 8831).
 */
enum class SCTPPayloadProtocolId : uint32_t {
    WEBRTC_DCEP = 50,           // Data Channel Establishment Protocol
    WEBRTC_STRING = 51,         // Text message
    WEBRTC_BINARY_PARTIAL = 52, // Binary partial (deprecated)
    WEBRTC_BINARY = 53,         // Binary message
    WEBRTC_STRING_EMPTY = 54,   // Empty text message
    WEBRTC_BINARY_PARTIAL2 = 55, // Binary partial (deprecated)
    WEBRTC_BINARY_EMPTY = 56,   // Empty binary message
};

/**
 * Data channel message.
 */
struct DataChannelMessage {
    bool binary{false};              // Binary or text
    std::string_view data;           // Message data (view into buffer)

    DataChannelMessage() = default;
    DataChannelMessage(std::string_view d, bool is_binary = false)
        : binary(is_binary), data(d) {}

    // Convenience for binary data access
    const uint8_t* binary_data() const noexcept {
        return reinterpret_cast<const uint8_t*>(data.data());
    }

    size_t size() const noexcept {
        return data.size();
    }
};

/**
 * WebRTC Data Channel.
 * 
 * Provides application-level data transport over WebRTC.
 */
class DataChannel {
public:
    /**
     * Message handler callback.
     */
    using MessageHandler = std::function<void(const DataChannelMessage&)>;
    
    /**
     * State change handler callback.
     */
    using StateHandler = std::function<void(DataChannelState)>;
    
    /**
     * Create data channel.
     * 
     * @param label Channel label
     * @param options Channel options
     */
    DataChannel(const std::string& label, const DataChannelOptions& options = {});
    
    ~DataChannel();
    
    /**
     * Send text message.
     * 
     * @param data Text data
     * @return 0 on success, error code otherwise
     */
    int send_text(std::string_view data) noexcept;
    
    /**
     * Send binary message.
     * 
     * @param data Binary data
     * @param len Data length
     * @return 0 on success
     */
    int send_binary(const uint8_t* data, size_t len) noexcept;
    
    /**
     * Set message handler.
     * 
     * @param handler Callback for received messages
     */
    void on_message(MessageHandler handler);
    
    /**
     * Set state change handler.
     * 
     * @param handler Callback for state changes
     */
    void on_state_change(StateHandler handler);
    
    /**
     * Get current state.
     */
    DataChannelState get_state() const noexcept { return state_; }
    
    /**
     * Get channel label.
     */
    const std::string& get_label() const noexcept { return label_; }
    
    /**
     * Get channel ID.
     */
    uint16_t get_id() const noexcept { return options_.id; }
    
    /**
     * Close channel.
     */
    int close() noexcept;

    /**
     * Simulate receiving data (for testing).
     *
     * Allows injecting data as if received via SCTP.
     *
     * @param data Data buffer
     * @param len Data length
     * @param ppid SCTP Payload Protocol Identifier
     * @return 0 on success
     */
    int receive_data(const uint8_t* data, size_t len, SCTPPayloadProtocolId ppid) noexcept;

    /**
     * Force channel state (for testing).
     *
     * @param state New state
     */
    void set_state(DataChannelState state) noexcept { state_ = state; }

    /**
     * Get statistics.
     */
    struct Stats {
        uint64_t messages_sent{0};
        uint64_t messages_received{0};
        uint64_t bytes_sent{0};
        uint64_t bytes_received{0};
    };
    
    Stats get_stats() const noexcept;
    
private:
    std::string label_;
    DataChannelOptions options_;
    DataChannelState state_;
    
    MessageHandler message_handler_;
    StateHandler state_handler_;
    
    // Statistics
    uint64_t messages_sent_{0};
    uint64_t messages_received_{0};
    uint64_t bytes_sent_{0};
    uint64_t bytes_received_{0};
    
    /**
     * Process incoming SCTP data.
     */
    int process_sctp_data(const uint8_t* data, size_t len) noexcept;
    
    /**
     * Send via SCTP.
     */
    int send_sctp(const uint8_t* data, size_t len, bool binary) noexcept;
};

} // namespace webrtc
} // namespace fasterapi

