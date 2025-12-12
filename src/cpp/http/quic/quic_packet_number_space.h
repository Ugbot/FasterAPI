#pragma once

/**
 * @file quic_packet_number_space.h
 * @brief QUIC Packet Number Spaces (RFC 9000 Section 12.3)
 *
 * Each packet number space has independent:
 * - Packet number sequence
 * - ACK tracking
 * - Loss detection
 * - CRYPTO buffer
 * - Packet protection keys
 */

#include <cstdint>
#include <array>
#include <memory>

#include "quic_crypto_buffer.h"
#include "quic_packet_protection.h"
#include "quic_ack_tracker.h"
#include "quic_congestion.h"

namespace fasterapi {
namespace quic {

/**
 * QUIC packet number spaces.
 * QUIC uses three distinct packet number spaces (RFC 9000 Section 12.3).
 */
enum class PacketNumberSpace : uint8_t {
    INITIAL = 0,      // Initial and Retry packets
    HANDSHAKE = 1,    // Handshake packets
    APPLICATION = 2,  // 0-RTT and 1-RTT packets
    NUM_SPACES = 3
};

/**
 * Convert packet number space to string for debugging.
 */
inline const char* packet_space_name(PacketNumberSpace space) noexcept {
    switch (space) {
        case PacketNumberSpace::INITIAL: return "Initial";
        case PacketNumberSpace::HANDSHAKE: return "Handshake";
        case PacketNumberSpace::APPLICATION: return "Application";
        default: return "Unknown";
    }
}

/**
 * Map encryption level to packet number space.
 */
inline PacketNumberSpace level_to_space(EncryptionLevel level) noexcept {
    switch (level) {
        case EncryptionLevel::INITIAL: return PacketNumberSpace::INITIAL;
        case EncryptionLevel::ZERO_RTT: return PacketNumberSpace::APPLICATION;
        case EncryptionLevel::HANDSHAKE: return PacketNumberSpace::HANDSHAKE;
        case EncryptionLevel::ONE_RTT: return PacketNumberSpace::APPLICATION;
        default: return PacketNumberSpace::INITIAL;
    }
}

/**
 * State for a single packet number space.
 */
class PacketNumberSpaceState {
public:
    PacketNumberSpaceState() noexcept
        : space_(PacketNumberSpace::INITIAL),
          next_pn_(0),
          largest_received_pn_(-1),
          largest_acked_pn_(-1),
          keys_available_(false),
          keys_discarded_(false),
          needs_ack_(false),
          ack_eliciting_in_flight_(0) {}

    /**
     * Initialize for a specific space.
     */
    void initialize(PacketNumberSpace space) noexcept {
        space_ = space;
    }

    /**
     * Get the packet number space.
     */
    PacketNumberSpace space() const noexcept { return space_; }

    /**
     * Get and increment next packet number.
     */
    uint64_t next_packet_number() noexcept {
        return next_pn_++;
    }

    /**
     * Peek at next packet number (don't increment).
     */
    uint64_t peek_next_packet_number() const noexcept {
        return next_pn_;
    }

    /**
     * Record that we received a packet.
     */
    void on_packet_received(uint64_t pn, bool ack_eliciting) noexcept {
        if (static_cast<int64_t>(pn) > largest_received_pn_) {
            largest_received_pn_ = static_cast<int64_t>(pn);
        }
        if (ack_eliciting) {
            needs_ack_ = true;
        }
        recv_tracker_.record_received(pn);
    }

    /**
     * Record that we sent a packet.
     */
    void on_packet_sent(uint64_t pn, uint64_t size, bool ack_eliciting, uint64_t now) noexcept {
        ack_tracker_.on_packet_sent(pn, size, ack_eliciting, now);
        if (ack_eliciting) {
            ack_eliciting_in_flight_++;
        }
    }

    /**
     * Process received ACK frame.
     */
    size_t on_ack_received(const AckFrame& ack, uint64_t now, NewRenoCongestionControl& cc) noexcept {
        if (static_cast<int64_t>(ack.largest_acked) > largest_acked_pn_) {
            largest_acked_pn_ = static_cast<int64_t>(ack.largest_acked);
        }
        return ack_tracker_.on_ack_received(ack, now, cc);
    }

    /**
     * Build an ACK frame for packets received in this space.
     */
    bool build_ack_frame(AckFrame& ack, uint64_t now) noexcept {
        if (!needs_ack_ || largest_received_pn_ < 0) {
            return false;
        }
        return recv_tracker_.build_ack_frame(ack, static_cast<uint64_t>(largest_received_pn_), now);
    }

    /**
     * Mark ACK as sent.
     */
    void on_ack_sent() noexcept {
        needs_ack_ = false;
    }

    /**
     * Check if we need to send an ACK.
     */
    bool needs_ack() const noexcept { return needs_ack_; }

    /**
     * Get largest received packet number.
     */
    int64_t largest_received() const noexcept { return largest_received_pn_; }

    /**
     * Get largest acknowledged packet number.
     */
    int64_t largest_acked() const noexcept { return largest_acked_pn_; }

    /**
     * Set keys available.
     */
    void set_keys_available(bool available) noexcept { keys_available_ = available; }

    /**
     * Check if keys are available.
     */
    bool keys_available() const noexcept { return keys_available_; }

    /**
     * Discard keys (e.g., after handshake confirmed).
     */
    void discard_keys() noexcept {
        keys_discarded_ = true;
        keys_available_ = false;
    }

    /**
     * Check if keys have been discarded.
     */
    bool keys_discarded() const noexcept { return keys_discarded_; }

    /**
     * Check if there are ACK-eliciting packets in flight.
     */
    bool has_ack_eliciting_in_flight() const noexcept {
        return ack_eliciting_in_flight_ > 0;
    }

    /**
     * Get the ACK tracker.
     */
    AckTracker& ack_tracker() noexcept { return ack_tracker_; }
    const AckTracker& ack_tracker() const noexcept { return ack_tracker_; }

    /**
     * Get the CRYPTO buffer.
     */
    CryptoBuffer& crypto_buffer() noexcept { return crypto_buffer_; }
    const CryptoBuffer& crypto_buffer() const noexcept { return crypto_buffer_; }

private:
    /**
     * Simple received packet tracker for building ACK frames.
     */
    class RecvTracker {
    public:
        RecvTracker() noexcept : first_recv_time_(0), last_recv_time_(0) {}

        void record_received(uint64_t pn) noexcept {
            uint64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count();

            if (received_pns_.empty()) {
                first_recv_time_ = now;
            }
            last_recv_time_ = now;

            // Insert in sorted order
            auto it = std::lower_bound(received_pns_.begin(), received_pns_.end(), pn);
            if (it == received_pns_.end() || *it != pn) {
                received_pns_.insert(it, pn);
            }

            // Limit size (keep most recent)
            while (received_pns_.size() > kMaxTrackedPackets) {
                received_pns_.erase(received_pns_.begin());
            }
        }

        bool build_ack_frame(AckFrame& ack, uint64_t largest, uint64_t now) noexcept {
            if (received_pns_.empty()) {
                return false;
            }

            ack.largest_acked = largest;
            ack.ack_delay = (now - last_recv_time_) / 8; // in 8us units (ack_delay_exponent=3)

            // Find the largest actually received
            auto it = received_pns_.rbegin();
            while (it != received_pns_.rend() && *it > largest) {
                ++it;
            }
            if (it == received_pns_.rend()) {
                return false;
            }

            ack.largest_acked = *it;
            uint64_t prev = *it;

            // Count first contiguous range
            ++it;
            ack.first_ack_range = 0;
            while (it != received_pns_.rend() && *it == prev - 1) {
                ack.first_ack_range++;
                prev = *it;
                ++it;
            }

            // Build additional ranges (up to limit)
            ack.range_count = 0;
            while (it != received_pns_.rend() && ack.range_count < AckFrame::MAX_ACK_RANGES) {
                // Gap: number of missing packets - 2
                uint64_t gap = prev - *it - 2;
                if (gap > prev - 2) {
                    break; // Overflow protection
                }

                // Range: contiguous packets
                prev = *it;
                uint64_t range_len = 0;
                ++it;
                while (it != received_pns_.rend() && *it == prev - 1) {
                    range_len++;
                    prev = *it;
                    ++it;
                }

                ack.ranges[ack.range_count].gap = gap;
                ack.ranges[ack.range_count].length = range_len;
                ack.range_count++;
            }

            return true;
        }

    private:
        static constexpr size_t kMaxTrackedPackets = 256;
        std::vector<uint64_t> received_pns_;
        uint64_t first_recv_time_;
        uint64_t last_recv_time_;
    };

    PacketNumberSpace space_;
    uint64_t next_pn_;
    int64_t largest_received_pn_;
    int64_t largest_acked_pn_;
    bool keys_available_;
    bool keys_discarded_;
    bool needs_ack_;
    uint32_t ack_eliciting_in_flight_;

    AckTracker ack_tracker_;
    CryptoBuffer crypto_buffer_;
    RecvTracker recv_tracker_;
};

/**
 * Manager for all packet number spaces.
 */
class PacketNumberSpaceManager {
public:
    PacketNumberSpaceManager() noexcept {
        spaces_[0].initialize(PacketNumberSpace::INITIAL);
        spaces_[1].initialize(PacketNumberSpace::HANDSHAKE);
        spaces_[2].initialize(PacketNumberSpace::APPLICATION);
    }

    /**
     * Get state for a packet number space.
     */
    PacketNumberSpaceState& operator[](PacketNumberSpace space) noexcept {
        return spaces_[static_cast<size_t>(space)];
    }

    const PacketNumberSpaceState& operator[](PacketNumberSpace space) const noexcept {
        return spaces_[static_cast<size_t>(space)];
    }

    /**
     * Get state for an encryption level.
     */
    PacketNumberSpaceState& for_level(EncryptionLevel level) noexcept {
        return spaces_[static_cast<size_t>(level_to_space(level))];
    }

    const PacketNumberSpaceState& for_level(EncryptionLevel level) const noexcept {
        return spaces_[static_cast<size_t>(level_to_space(level))];
    }

    /**
     * Discard Initial keys after Handshake keys are available.
     */
    void discard_initial_keys() noexcept {
        spaces_[static_cast<size_t>(PacketNumberSpace::INITIAL)].discard_keys();
    }

    /**
     * Discard Handshake keys after handshake is confirmed.
     */
    void discard_handshake_keys() noexcept {
        spaces_[static_cast<size_t>(PacketNumberSpace::HANDSHAKE)].discard_keys();
    }

    /**
     * Check if any space needs to send an ACK.
     */
    bool needs_ack() const noexcept {
        for (const auto& space : spaces_) {
            if (space.keys_available() && space.needs_ack()) {
                return true;
            }
        }
        return false;
    }

    /**
     * Iterator support.
     */
    auto begin() noexcept { return spaces_.begin(); }
    auto end() noexcept { return spaces_.end(); }
    auto begin() const noexcept { return spaces_.begin(); }
    auto end() const noexcept { return spaces_.end(); }

private:
    std::array<PacketNumberSpaceState, 3> spaces_;
};

} // namespace quic
} // namespace fasterapi
