#pragma once

#include <cstdint>
#include <algorithm>
#include <chrono>

namespace fasterapi {
namespace quic {

/**
 * QUIC NewReno Congestion Control (RFC 9002 Section 7.3).
 * 
 * Implements classic NewReno algorithm adapted for QUIC:
 * - Slow start: Exponential growth
 * - Congestion avoidance: Linear growth
 * - Fast recovery: After packet loss
 */
class NewRenoCongestionControl {
public:
    static constexpr uint64_t kInitialWindow = 10 * 1200;  // 10 packets * 1200 bytes
    static constexpr uint64_t kMinimumWindow = 2 * 1200;   // 2 packets
    static constexpr uint64_t kMaxDatagramSize = 1200;     // Conservative UDP MTU
    static constexpr double kLossReductionFactor = 0.5;    // Reduce window by 50% on loss
    
    NewRenoCongestionControl()
        : congestion_window_(kInitialWindow),
          ssthresh_(UINT64_MAX),  // No threshold initially (slow start)
          bytes_in_flight_(0),
          recovery_start_time_(0) {
    }
    
    /**
     * Process ACK (increases congestion window).
     * 
     * @param acked_bytes Number of newly acknowledged bytes
     * @param now Current time (microseconds)
     */
    void on_ack_received(uint64_t acked_bytes, uint64_t now) noexcept {
        // Don't increase window during recovery
        if (in_recovery(now)) {
            return;
        }
        
        if (in_slow_start()) {
            // Slow start: Exponential growth
            // Increase by acked_bytes (doubles every RTT)
            congestion_window_ += acked_bytes;
        } else {
            // Congestion avoidance: Linear growth
            // Increase by (max_datagram_size * acked_bytes) / congestion_window
            // This gives approximately +1 MSS per RTT
            uint64_t increase = (kMaxDatagramSize * acked_bytes) / congestion_window_;
            congestion_window_ += increase;
        }
    }
    
    /**
     * Process packet loss (decreases congestion window).
     * 
     * @param now Current time (microseconds)
     */
    void on_congestion_event(uint64_t now) noexcept {
        // Don't reduce window if already in recovery
        if (in_recovery(now)) {
            return;
        }
        
        // Enter recovery
        recovery_start_time_ = now;
        
        // Set ssthresh to half of current window
        ssthresh_ = std::max(congestion_window_ / 2, kMinimumWindow);
        
        // Reduce congestion window
        congestion_window_ = ssthresh_;
    }
    
    /**
     * Process persistent congestion (severe loss).
     * 
     * Resets congestion window to minimum.
     */
    void on_persistent_congestion() noexcept {
        congestion_window_ = kMinimumWindow;
        ssthresh_ = UINT64_MAX;  // Back to slow start
        recovery_start_time_ = 0;
    }
    
    /**
     * Check if we can send more data.
     * 
     * @param bytes_to_send Number of bytes we want to send
     * @return true if allowed by congestion control
     */
    bool can_send(uint64_t bytes_to_send) const noexcept {
        return bytes_in_flight_ + bytes_to_send <= congestion_window_;
    }
    
    /**
     * Record packet sent.
     * 
     * @param bytes Packet size in bytes
     */
    void on_packet_sent(uint64_t bytes) noexcept {
        bytes_in_flight_ += bytes;
    }
    
    /**
     * Record packet acknowledged.
     * 
     * @param bytes Packet size in bytes
     */
    void on_packet_acked(uint64_t bytes) noexcept {
        if (bytes_in_flight_ >= bytes) {
            bytes_in_flight_ -= bytes;
        } else {
            bytes_in_flight_ = 0;
        }
    }
    
    /**
     * Record packet lost.
     * 
     * @param bytes Packet size in bytes
     */
    void on_packet_lost(uint64_t bytes) noexcept {
        if (bytes_in_flight_ >= bytes) {
            bytes_in_flight_ -= bytes;
        } else {
            bytes_in_flight_ = 0;
        }
    }
    
    /**
     * Get current congestion window.
     */
    uint64_t congestion_window() const noexcept { return congestion_window_; }
    
    /**
     * Get slow start threshold.
     */
    uint64_t ssthresh() const noexcept { return ssthresh_; }
    
    /**
     * Get bytes in flight.
     */
    uint64_t bytes_in_flight() const noexcept { return bytes_in_flight_; }
    
    /**
     * Check if in slow start phase.
     */
    bool in_slow_start() const noexcept {
        return congestion_window_ < ssthresh_;
    }
    
    /**
     * Check if in recovery.
     * 
     * @param now Current time (microseconds)
     */
    bool in_recovery(uint64_t now) const noexcept {
        // Simple check: if we set recovery time, we're in recovery
        // More sophisticated: check if sent_time < recovery_start_time
        return recovery_start_time_ > 0 && 
               now < recovery_start_time_ + 1000000;  // 1 second max recovery
    }
    
    /**
     * Get available sending capacity.
     */
    uint64_t available_capacity() const noexcept {
        if (bytes_in_flight_ >= congestion_window_) {
            return 0;
        }
        return congestion_window_ - bytes_in_flight_;
    }
    
    /**
     * Update RTT estimate (used for pacing).
     * 
     * @param rtt_us RTT in microseconds
     */
    void update_rtt(uint64_t rtt_us) noexcept {
        smoothed_rtt_ = rtt_us;
    }
    
    /**
     * Get pacing rate (bytes per second).
     * 
     * Pacing prevents bursts.
     */
    uint64_t pacing_rate() const noexcept {
        if (smoothed_rtt_ == 0) {
            return UINT64_MAX;  // No pacing if no RTT estimate
        }
        
        // Rate = window / RTT
        // Convert to bytes/second
        return (congestion_window_ * 1000000) / smoothed_rtt_;
    }

private:
    uint64_t congestion_window_;     // Current congestion window (bytes)
    uint64_t ssthresh_;              // Slow start threshold
    uint64_t bytes_in_flight_;       // Unacknowledged bytes
    uint64_t recovery_start_time_;   // When we entered recovery (us)
    uint64_t smoothed_rtt_;          // Smoothed RTT estimate (us)
};

/**
 * Simple pacing implementation.
 * 
 * Prevents burst sending by spreading packets over time.
 */
class Pacer {
public:
    Pacer() : last_send_time_(0), tokens_(0), rate_bps_(0) {}
    
    /**
     * Update pacing rate.
     * 
     * @param rate_bps Pacing rate in bytes per second
     */
    void set_rate(uint64_t rate_bps) noexcept {
        rate_bps_ = rate_bps;
    }
    
    /**
     * Check if we can send a packet.
     * 
     * @param packet_size Packet size in bytes
     * @param now Current time (microseconds)
     * @return true if allowed to send
     */
    bool can_send(uint64_t packet_size, uint64_t now) noexcept {
        if (rate_bps_ == 0) {
            return true;  // No pacing
        }
        
        // Add tokens based on elapsed time
        if (last_send_time_ > 0) {
            uint64_t elapsed_us = now - last_send_time_;
            uint64_t new_tokens = (rate_bps_ * elapsed_us) / 1000000;
            tokens_ += new_tokens;
            
            // Cap tokens to prevent bursts
            uint64_t max_tokens = rate_bps_ / 10;  // 100ms worth
            if (tokens_ > max_tokens) {
                tokens_ = max_tokens;
            }
        } else {
            // First send
            tokens_ = rate_bps_ / 10;  // Start with 100ms worth
        }
        
        // Check if we have enough tokens
        if (tokens_ >= packet_size) {
            tokens_ -= packet_size;
            last_send_time_ = now;
            return true;
        }
        
        return false;
    }

private:
    uint64_t last_send_time_;  // Last send time (microseconds)
    uint64_t tokens_;          // Available tokens (bytes)
    uint64_t rate_bps_;        // Pacing rate (bytes per second)
};

} // namespace quic
} // namespace fasterapi
