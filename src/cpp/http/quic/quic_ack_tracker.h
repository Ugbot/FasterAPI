#pragma once

#include "quic_frames.h"
#include "quic_congestion.h"
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <chrono>

namespace fasterapi {
namespace quic {

/**
 * Sent packet information.
 */
struct SentPacket {
    uint64_t packet_number;
    uint64_t time_sent;      // Microseconds since epoch
    uint64_t size;           // Packet size in bytes
    bool ack_eliciting;      // Does this packet require an ACK?
    bool in_flight;          // Counted in bytes_in_flight?
    
    SentPacket() : packet_number(0), time_sent(0), size(0), 
                   ack_eliciting(false), in_flight(false) {}
};

/**
 * QUIC Loss Detection and Recovery (RFC 9002).
 * 
 * Implements:
 * - ACK processing
 * - Loss detection (time-based and packet-based)
 * - Retransmission logic
 */
class AckTracker {
public:
    // Loss detection parameters (RFC 9002)
    static constexpr uint64_t kTimeThreshold = 9;  // 9/8 = 1.125x RTT
    static constexpr uint64_t kTimeThresholdDivisor = 8;
    static constexpr uint64_t kPacketThreshold = 3;  // 3 packets
    static constexpr uint64_t kGranularity = 1000;   // 1ms in microseconds
    static constexpr uint64_t kInitialRtt = 333000;  // 333ms initial RTT
    
    AckTracker()
        : largest_acked_(0),
          latest_rtt_(0),
          smoothed_rtt_(kInitialRtt),
          rttvar_(kInitialRtt / 2),
          min_rtt_(UINT64_MAX),
          loss_time_(0),
          next_packet_number_(0) {
    }
    
    /**
     * Record packet sent.
     * 
     * @param packet_number Packet number
     * @param size Packet size
     * @param ack_eliciting Does packet require ACK?
     * @param now Current time (microseconds)
     */
    void on_packet_sent(uint64_t packet_number, uint64_t size, 
                       bool ack_eliciting, uint64_t now) noexcept {
        SentPacket pkt;
        pkt.packet_number = packet_number;
        pkt.time_sent = now;
        pkt.size = size;
        pkt.ack_eliciting = ack_eliciting;
        pkt.in_flight = true;
        
        sent_packets_[packet_number] = pkt;
        
        if (packet_number >= next_packet_number_) {
            next_packet_number_ = packet_number + 1;
        }
    }
    
    /**
     * Process ACK frame.
     * 
     * @param ack ACK frame
     * @param now Current time (microseconds)
     * @param cc Congestion control (to notify of acks/losses)
     * @return Number of newly acknowledged packets
     */
    size_t on_ack_received(const AckFrame& ack, uint64_t now, 
                          NewRenoCongestionControl& cc) noexcept {
        // Update largest acked
        if (ack.largest_acked > largest_acked_) {
            largest_acked_ = ack.largest_acked;
        }
        
        size_t newly_acked = 0;
        uint64_t acked_bytes = 0;
        
        // Process acked packets
        // First range: largest_acked - first_ack_range to largest_acked
        for (uint64_t pn = ack.largest_acked - ack.first_ack_range;
             pn <= ack.largest_acked; pn++) {
            if (mark_packet_acked(pn, now, acked_bytes)) {
                newly_acked++;
            }
        }
        
        // Additional ranges
        uint64_t smallest = ack.largest_acked - ack.first_ack_range;
        for (size_t i = 0; i < ack.range_count; i++) {
            smallest -= ack.ranges[i].gap + 2;
            uint64_t largest = smallest - 1;
            smallest -= ack.ranges[i].length;
            
            for (uint64_t pn = smallest; pn <= largest; pn++) {
                if (mark_packet_acked(pn, now, acked_bytes)) {
                    newly_acked++;
                }
            }
        }
        
        // Update congestion control
        if (acked_bytes > 0) {
            cc.on_ack_received(acked_bytes, now);
        }
        
        // Detect lost packets
        detect_and_remove_lost_packets(now, cc);
        
        return newly_acked;
    }
    
    /**
     * Detect lost packets (time-based and packet-based).
     * 
     * @param now Current time (microseconds)
     * @param cc Congestion control
     */
    void detect_and_remove_lost_packets(uint64_t now, 
                                       NewRenoCongestionControl& cc) noexcept {
        loss_time_ = 0;
        uint64_t lost_bytes = 0;
        
        // Loss detection threshold
        uint64_t loss_delay = std::max(
            (kTimeThreshold * smoothed_rtt_) / kTimeThresholdDivisor,
            kGranularity
        );
        uint64_t lost_send_time = now - loss_delay;
        
        // Find lost packets
        std::vector<uint64_t> lost_packets;
        
        for (auto& [pn, pkt] : sent_packets_) {
            if (pkt.in_flight) {
                // Packet-based detection: 3 packets acked after this one
                if (largest_acked_ >= pn + kPacketThreshold) {
                    lost_packets.push_back(pn);
                    lost_bytes += pkt.size;
                }
                // Time-based detection: sent long ago
                else if (pkt.time_sent <= lost_send_time) {
                    lost_packets.push_back(pn);
                    lost_bytes += pkt.size;
                }
                // Set loss timer for packets not yet lost
                else {
                    uint64_t pkt_loss_time = pkt.time_sent + loss_delay;
                    if (loss_time_ == 0 || pkt_loss_time < loss_time_) {
                        loss_time_ = pkt_loss_time;
                    }
                }
            }
        }
        
        // Remove lost packets
        for (uint64_t pn : lost_packets) {
            auto it = sent_packets_.find(pn);
            if (it != sent_packets_.end()) {
                cc.on_packet_lost(it->second.size);
                sent_packets_.erase(it);
            }
        }
        
        // Trigger congestion event if we lost packets
        if (!lost_packets.empty()) {
            cc.on_congestion_event(now);
        }
    }
    
    /**
     * Check if loss detection timer expired.
     * 
     * @param now Current time (microseconds)
     * @return true if timer expired
     */
    bool loss_detection_timer_expired(uint64_t now) const noexcept {
        return loss_time_ > 0 && now >= loss_time_;
    }
    
    /**
     * Get next packet number.
     */
    uint64_t next_packet_number() const noexcept { return next_packet_number_; }
    
    /**
     * Get largest acked packet number.
     */
    uint64_t largest_acked() const noexcept { return largest_acked_; }
    
    /**
     * Get smoothed RTT.
     */
    uint64_t smoothed_rtt() const noexcept { return smoothed_rtt_; }
    
    /**
     * Get latest RTT.
     */
    uint64_t latest_rtt() const noexcept { return latest_rtt_; }
    
    /**
     * Get minimum RTT.
     */
    uint64_t min_rtt() const noexcept { return min_rtt_; }
    
    /**
     * Get RTT variance.
     */
    uint64_t rttvar() const noexcept { return rttvar_; }
    
    /**
     * Get number of in-flight packets.
     */
    size_t in_flight_count() const noexcept {
        size_t count = 0;
        for (const auto& [pn, pkt] : sent_packets_) {
            if (pkt.in_flight) count++;
        }
        return count;
    }

private:
    /**
     * Mark packet as acknowledged.
     * 
     * @param packet_number Packet number
     * @param now Current time (microseconds)
     * @param out_acked_bytes Accumulator for acked bytes
     * @return true if packet was newly acked
     */
    bool mark_packet_acked(uint64_t packet_number, uint64_t now, 
                          uint64_t& out_acked_bytes) noexcept {
        auto it = sent_packets_.find(packet_number);
        if (it == sent_packets_.end()) {
            return false;  // Already acked or never sent
        }
        
        SentPacket& pkt = it->second;
        
        if (!pkt.in_flight) {
            return false;  // Already processed
        }
        
        // Update RTT
        if (packet_number == largest_acked_) {
            latest_rtt_ = now - pkt.time_sent;
            update_rtt(latest_rtt_);
        }
        
        // Mark as acked
        pkt.in_flight = false;
        out_acked_bytes += pkt.size;
        
        return true;
    }
    
    /**
     * Update RTT estimates (RFC 9002 Section 5.3).
     * 
     * @param latest_rtt Latest RTT sample
     */
    void update_rtt(uint64_t latest_rtt) noexcept {
        // Update min RTT
        if (latest_rtt < min_rtt_) {
            min_rtt_ = latest_rtt;
        }
        
        // First RTT sample
        if (smoothed_rtt_ == kInitialRtt) {
            smoothed_rtt_ = latest_rtt;
            rttvar_ = latest_rtt / 2;
            return;
        }
        
        // EWMA with alpha = 1/8, beta = 1/4
        uint64_t rtt_diff = (latest_rtt > smoothed_rtt_) ?
                           (latest_rtt - smoothed_rtt_) :
                           (smoothed_rtt_ - latest_rtt);
        
        rttvar_ = (3 * rttvar_ + rtt_diff) / 4;
        smoothed_rtt_ = (7 * smoothed_rtt_ + latest_rtt) / 8;
    }
    
    std::unordered_map<uint64_t, SentPacket> sent_packets_;
    uint64_t largest_acked_;
    uint64_t latest_rtt_;
    uint64_t smoothed_rtt_;
    uint64_t rttvar_;
    uint64_t min_rtt_;
    uint64_t loss_time_;         // When to check for lost packets
    uint64_t next_packet_number_;
};

} // namespace quic
} // namespace fasterapi
