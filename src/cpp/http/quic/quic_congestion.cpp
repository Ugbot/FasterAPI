// QUIC congestion control implementation
// Core methods are inline in header for performance
// This file contains helper methods, algorithms, and utilities

#include "quic_congestion.h"
#include <algorithm>
#include <cstdint>
#include <cmath>

namespace fasterapi {
namespace quic {

// ============================================================================
// Constants for Congestion Control (RFC 9002)
// ============================================================================

namespace {
    // RTT Constants (RFC 9002 Section 6.2)
    constexpr uint64_t kDefaultInitialRtt = 333000;      // 333ms in microseconds
    constexpr uint64_t kMinRttSample = 1000;             // 1ms minimum
    constexpr uint64_t kMaxRttSample = 60000000;         // 60 seconds maximum

    // EWMA weights for RTT smoothing (RFC 9002 Section 5.3)
    constexpr double kRttAlpha = 0.125;                  // 1/8
    constexpr double kRttBeta = 0.25;                    // 1/4

    // Congestion window constants
    constexpr uint64_t kMaxCongestionWindow = 100 * 1024 * 1024;  // 100MB max
    constexpr double kSlowStartGrowthFactor = 2.0;       // Double per RTT
    constexpr double kCongestionAvoidanceGrowth = 1.0;   // +1 MSS per RTT

    // Pacing constants
    constexpr double kPacingGain = 1.25;                 // 125% of theoretical rate
    constexpr uint64_t kMinPacingRate = 100000;          // 100 KB/s minimum
    constexpr uint64_t kMaxPacingRate = 10ULL * 1000 * 1000 * 1000;  // 10 Gbps

    // Loss detection
    constexpr uint64_t kPersistentCongestionThreshold = 3;  // 3 RTTs
    constexpr double kMinLossReduction = 0.5;            // Reduce by at least 50%
}

// ============================================================================
// RTT Calculation and Smoothing (RFC 9002 Section 5)
// ============================================================================

/**
 * Calculate smoothed RTT using exponentially weighted moving average.
 *
 * From RFC 9002:
 *   smoothed_rtt = (1 - alpha) * smoothed_rtt + alpha * latest_rtt
 *   rttvar = (1 - beta) * rttvar + beta * abs(smoothed_rtt - latest_rtt)
 *
 * @param smoothed_rtt Current smoothed RTT (microseconds)
 * @param rtt_var Current RTT variance (microseconds)
 * @param latest_rtt Latest RTT sample (microseconds)
 * @param is_first_sample True if this is the first RTT sample
 */
void update_rtt_estimate(uint64_t& smoothed_rtt,
                        uint64_t& rtt_var,
                        uint64_t latest_rtt,
                        bool is_first_sample) noexcept {
    // Validate RTT sample
    if (latest_rtt < kMinRttSample || latest_rtt > kMaxRttSample) {
        return;  // Ignore invalid samples
    }

    if (is_first_sample) {
        // First sample: initialize smoothed_rtt and rttvar
        smoothed_rtt = latest_rtt;
        rtt_var = latest_rtt / 2;  // Initial variance is half of RTT
    } else {
        // Subsequent samples: EWMA smoothing
        uint64_t rtt_diff = (smoothed_rtt > latest_rtt)
                           ? (smoothed_rtt - latest_rtt)
                           : (latest_rtt - smoothed_rtt);

        // Update variance: rttvar = (1 - beta) * rttvar + beta * |smoothed_rtt - latest_rtt|
        rtt_var = static_cast<uint64_t>((1.0 - kRttBeta) * rtt_var + kRttBeta * rtt_diff);

        // Update smoothed RTT: smoothed_rtt = (1 - alpha) * smoothed_rtt + alpha * latest_rtt
        smoothed_rtt = static_cast<uint64_t>((1.0 - kRttAlpha) * smoothed_rtt + kRttAlpha * latest_rtt);
    }
}

/**
 * Calculate minimum RTT over a time window.
 *
 * Min RTT is used for detecting path changes and as baseline for loss detection.
 *
 * @param current_min Current minimum RTT
 * @param latest_rtt Latest RTT sample
 * @param window_start Start of min RTT window (microseconds)
 * @param now Current time (microseconds)
 * @param window_duration Window duration (default 10 seconds)
 * @return Updated minimum RTT
 */
uint64_t update_min_rtt(uint64_t current_min,
                       uint64_t latest_rtt,
                       uint64_t window_start,
                       uint64_t now,
                       uint64_t window_duration = 10000000) noexcept {
    // Reset min RTT if window expired
    if (now - window_start > window_duration) {
        return latest_rtt;
    }

    // Update if this is a new minimum
    if (current_min == 0 || latest_rtt < current_min) {
        return latest_rtt;
    }

    return current_min;
}

/**
 * Calculate RTO (Retransmission Timeout) from RTT estimates.
 *
 * From RFC 9002:
 *   PTO = smoothed_rtt + max(4 * rttvar, kGranularity) + max_ack_delay
 *
 * @param smoothed_rtt Smoothed RTT (microseconds)
 * @param rtt_var RTT variance (microseconds)
 * @param max_ack_delay Maximum ACK delay (microseconds)
 * @return PTO duration (microseconds)
 */
uint64_t calculate_pto(uint64_t smoothed_rtt,
                      uint64_t rtt_var,
                      uint64_t max_ack_delay) noexcept {
    constexpr uint64_t kGranularity = 1000;  // 1ms timer granularity

    uint64_t variance_component = std::max(4 * rtt_var, kGranularity);
    return smoothed_rtt + variance_component + max_ack_delay;
}

// ============================================================================
// Congestion Window Management (RFC 9002 Section 7)
// ============================================================================

/**
 * Calculate initial congestion window.
 *
 * RFC 9002 recommends:
 *   Initial cwnd = min(10 * max_datagram_size, max(2 * max_datagram_size, 14600))
 *
 * @param max_datagram_size Maximum datagram size
 * @return Initial congestion window (bytes)
 */
uint64_t calculate_initial_cwnd(uint64_t max_datagram_size) noexcept {
    uint64_t min_cwnd = std::max(2 * max_datagram_size, 14600ULL);
    return std::min(10 * max_datagram_size, min_cwnd);
}

/**
 * Calculate slow start growth.
 *
 * In slow start, cwnd increases by acked_bytes (doubles every RTT).
 *
 * @param current_cwnd Current congestion window
 * @param acked_bytes Newly acknowledged bytes
 * @param max_cwnd Maximum allowed congestion window
 * @return New congestion window
 */
uint64_t calculate_slow_start_cwnd(uint64_t current_cwnd,
                                   uint64_t acked_bytes,
                                   uint64_t max_cwnd) noexcept {
    uint64_t new_cwnd = current_cwnd + acked_bytes;
    return std::min(new_cwnd, max_cwnd);
}

/**
 * Calculate congestion avoidance growth.
 *
 * In congestion avoidance, cwnd increases by approximately 1 MSS per RTT.
 * Increase per ACK = (MSS * acked_bytes) / cwnd
 *
 * @param current_cwnd Current congestion window
 * @param acked_bytes Newly acknowledged bytes
 * @param max_datagram_size Maximum datagram size (MSS)
 * @param max_cwnd Maximum allowed congestion window
 * @return New congestion window
 */
uint64_t calculate_congestion_avoidance_cwnd(uint64_t current_cwnd,
                                             uint64_t acked_bytes,
                                             uint64_t max_datagram_size,
                                             uint64_t max_cwnd) noexcept {
    // Avoid division by zero
    if (current_cwnd == 0) {
        return max_datagram_size;
    }

    // Calculate increase: (MSS * acked_bytes) / cwnd
    uint64_t increase = (max_datagram_size * acked_bytes) / current_cwnd;
    uint64_t new_cwnd = current_cwnd + increase;

    return std::min(new_cwnd, max_cwnd);
}

/**
 * Calculate congestion window after loss event.
 *
 * NewReno reduces cwnd to 50% of current value (multiplicative decrease).
 *
 * @param current_cwnd Current congestion window
 * @param min_cwnd Minimum allowed congestion window
 * @return New congestion window after loss
 */
uint64_t calculate_loss_cwnd(uint64_t current_cwnd,
                             uint64_t min_cwnd) noexcept {
    uint64_t new_cwnd = current_cwnd / 2;
    return std::max(new_cwnd, min_cwnd);
}

/**
 * Calculate slow start threshold after loss.
 *
 * @param current_cwnd Current congestion window
 * @param min_cwnd Minimum allowed congestion window
 * @return New slow start threshold
 */
uint64_t calculate_ssthresh(uint64_t current_cwnd,
                           uint64_t min_cwnd) noexcept {
    return std::max(current_cwnd / 2, min_cwnd);
}

// ============================================================================
// Pacing Calculations (RFC 9002 Section 7.7)
// ============================================================================

/**
 * Calculate pacing rate from congestion window and RTT.
 *
 * Pacing rate = (cwnd / smoothed_rtt) * pacing_gain
 *
 * Where pacing_gain adds headroom (typically 1.25 for 25% overhead).
 *
 * @param cwnd Congestion window (bytes)
 * @param smoothed_rtt Smoothed RTT (microseconds)
 * @param pacing_gain Pacing gain multiplier (default 1.25)
 * @return Pacing rate (bytes per second)
 */
uint64_t calculate_pacing_rate(uint64_t cwnd,
                               uint64_t smoothed_rtt,
                               double pacing_gain = kPacingGain) noexcept {
    if (smoothed_rtt == 0) {
        return kMaxPacingRate;  // No limit if no RTT estimate
    }

    // Rate = (cwnd / rtt) * gain
    // Convert to bytes/second: (cwnd * 1,000,000 / rtt_us) * gain
    double rate = (static_cast<double>(cwnd) * 1000000.0 / smoothed_rtt) * pacing_gain;

    uint64_t pacing_rate = static_cast<uint64_t>(rate);

    // Clamp to reasonable range
    return std::max(kMinPacingRate, std::min(pacing_rate, kMaxPacingRate));
}

/**
 * Calculate inter-packet interval from pacing rate.
 *
 * @param packet_size Packet size (bytes)
 * @param pacing_rate Pacing rate (bytes per second)
 * @return Inter-packet interval (microseconds)
 */
uint64_t calculate_inter_packet_interval(uint64_t packet_size,
                                         uint64_t pacing_rate) noexcept {
    if (pacing_rate == 0) {
        return 0;  // No pacing
    }

    // Interval = (packet_size / pacing_rate) * 1,000,000
    return (packet_size * 1000000) / pacing_rate;
}

/**
 * Calculate burst size allowance.
 *
 * Allow limited bursts to improve throughput while maintaining pacing.
 * Typically allow 2-10 packets in burst.
 *
 * @param max_datagram_size Maximum datagram size
 * @param burst_multiplier Burst size multiplier (default 4)
 * @return Maximum burst size (bytes)
 */
uint64_t calculate_max_burst(uint64_t max_datagram_size,
                             uint32_t burst_multiplier = 4) noexcept {
    return max_datagram_size * burst_multiplier;
}

// ============================================================================
// Loss Detection and Recovery
// ============================================================================

/**
 * Detect persistent congestion.
 *
 * Persistent congestion occurs when all packets in a time period are lost.
 * Period = max(3 * PTO, PTO + 2 * smoothed_rtt)
 *
 * @param loss_period_start Start of loss period (microseconds)
 * @param loss_period_end End of loss period (microseconds)
 * @param pto Probe timeout (microseconds)
 * @param smoothed_rtt Smoothed RTT (microseconds)
 * @return True if persistent congestion detected
 */
bool detect_persistent_congestion(uint64_t loss_period_start,
                                  uint64_t loss_period_end,
                                  uint64_t pto,
                                  uint64_t smoothed_rtt) noexcept {
    uint64_t loss_duration = loss_period_end - loss_period_start;

    // Persistent congestion threshold
    uint64_t threshold = std::max(3 * pto, pto + 2 * smoothed_rtt);

    return loss_duration >= threshold;
}

/**
 * Check if in recovery period.
 *
 * Recovery period lasts until a packet sent after the loss is acknowledged.
 *
 * @param packet_sent_time When packet was sent
 * @param recovery_start_time When recovery period started
 * @return True if in recovery
 */
bool is_in_recovery(uint64_t packet_sent_time,
                   uint64_t recovery_start_time) noexcept {
    return packet_sent_time < recovery_start_time;
}

/**
 * Calculate congestion event backoff.
 *
 * After persistent congestion, use exponential backoff.
 *
 * @param base_rtt Base RTT estimate
 * @param backoff_count Number of consecutive losses
 * @return Backoff duration (microseconds)
 */
uint64_t calculate_congestion_backoff(uint64_t base_rtt,
                                      uint32_t backoff_count) noexcept {
    // Exponential backoff: rtt * 2^count, capped at 60 seconds
    uint64_t max_backoff = 60000000;  // 60 seconds

    // Prevent overflow in exponentiation
    if (backoff_count > 20) {
        return max_backoff;
    }

    uint64_t multiplier = 1ULL << backoff_count;  // 2^count
    uint64_t backoff = base_rtt * multiplier;

    return std::min(backoff, max_backoff);
}

// ============================================================================
// Bandwidth Estimation
// ============================================================================

/**
 * Estimate bandwidth from delivered bytes and elapsed time.
 *
 * This is used for more advanced congestion control (e.g., BBR).
 *
 * @param bytes_delivered Bytes delivered in interval
 * @param elapsed_time Elapsed time (microseconds)
 * @return Estimated bandwidth (bytes per second)
 */
uint64_t estimate_bandwidth(uint64_t bytes_delivered,
                           uint64_t elapsed_time) noexcept {
    if (elapsed_time == 0) {
        return 0;
    }

    // BW = bytes / time (in bytes per second)
    return (bytes_delivered * 1000000) / elapsed_time;
}

/**
 * Calculate bandwidth-delay product (BDP).
 *
 * BDP = bandwidth * RTT
 * Represents the optimal congestion window size.
 *
 * @param bandwidth Estimated bandwidth (bytes per second)
 * @param rtt Round-trip time (microseconds)
 * @return BDP (bytes)
 */
uint64_t calculate_bdp(uint64_t bandwidth,
                      uint64_t rtt) noexcept {
    // BDP = (bandwidth * rtt) / 1,000,000
    return (bandwidth * rtt) / 1000000;
}

// ============================================================================
// Congestion Control State Management
// ============================================================================

/**
 * Determine congestion control state.
 */
enum class CongestionState : uint8_t {
    SLOW_START,
    CONGESTION_AVOIDANCE,
    RECOVERY,
    PERSISTENT_CONGESTION
};

/**
 * Get string representation of congestion state.
 *
 * @param state Congestion state
 * @return String name
 */
const char* congestion_state_to_string(CongestionState state) noexcept {
    switch (state) {
        case CongestionState::SLOW_START: return "SlowStart";
        case CongestionState::CONGESTION_AVOIDANCE: return "CongestionAvoidance";
        case CongestionState::RECOVERY: return "Recovery";
        case CongestionState::PERSISTENT_CONGESTION: return "PersistentCongestion";
        default: return "Unknown";
    }
}

// ============================================================================
// Diagnostics and Statistics
// ============================================================================

/**
 * Calculate congestion window utilization.
 *
 * @param bytes_in_flight Bytes currently in flight
 * @param cwnd Congestion window size
 * @return Utilization percentage (0-100)
 */
double calculate_cwnd_utilization(uint64_t bytes_in_flight,
                                  uint64_t cwnd) noexcept {
    if (cwnd == 0) {
        return 0.0;
    }

    return (static_cast<double>(bytes_in_flight) / cwnd) * 100.0;
}

/**
 * Estimate time until window is available.
 *
 * @param bytes_in_flight Current bytes in flight
 * @param cwnd Congestion window
 * @param pacing_rate Current pacing rate (bytes per second)
 * @return Estimated time (microseconds)
 */
uint64_t estimate_time_until_available(uint64_t bytes_in_flight,
                                       uint64_t cwnd,
                                       uint64_t pacing_rate) noexcept {
    if (bytes_in_flight < cwnd) {
        return 0;  // Already available
    }

    if (pacing_rate == 0) {
        return UINT64_MAX;  // Unknown
    }

    uint64_t excess_bytes = bytes_in_flight - cwnd;
    return (excess_bytes * 1000000) / pacing_rate;
}

/**
 * Calculate loss rate from statistics.
 *
 * @param packets_lost Number of packets lost
 * @param packets_sent Number of packets sent
 * @return Loss rate (0.0 to 1.0)
 */
double calculate_loss_rate(uint64_t packets_lost,
                          uint64_t packets_sent) noexcept {
    if (packets_sent == 0) {
        return 0.0;
    }

    return static_cast<double>(packets_lost) / packets_sent;
}

// ============================================================================
// Advanced Algorithms (Optional Extensions)
// ============================================================================

/**
 * Calculate CUBIC congestion window (alternative to NewReno).
 *
 * CUBIC is a more aggressive algorithm that provides better performance
 * on high-bandwidth, long-delay networks.
 *
 * W_cubic(t) = C * (t - K)^3 + W_max
 *
 * @param time_since_loss Time since last loss event (microseconds)
 * @param w_max Maximum window before last loss
 * @param cwnd Current congestion window
 * @return CUBIC window estimate
 */
uint64_t calculate_cubic_cwnd(uint64_t time_since_loss,
                              uint64_t w_max,
                              uint64_t cwnd) noexcept {
    // CUBIC constants
    constexpr double C = 0.4;  // CUBIC scaling factor
    constexpr double beta = 0.7;  // Multiplicative decrease factor

    // Calculate K (time to reach W_max)
    // K = cube_root((W_max - cwnd) / C)
    double w_diff = static_cast<double>(w_max) - cwnd * beta;
    double K = std::cbrt(w_diff / C);

    // Convert time to seconds
    double t = time_since_loss / 1000000.0;

    // Calculate cubic window
    double t_minus_k = t - K;
    double cubic_term = C * t_minus_k * t_minus_k * t_minus_k;
    double w_cubic = cubic_term + w_max;

    return static_cast<uint64_t>(std::max(0.0, w_cubic));
}

/**
 * BBR pacing gain for different phases.
 *
 * BBR uses different pacing gains in different phases:
 * - Startup: 2.89x (aggressive growth)
 * - Drain: 1/2.89 (drain queue)
 * - ProbeBW: cycle [1.25, 0.75, 1, 1, 1, 1, 1, 1]
 * - ProbeRTT: 1.0
 */
double get_bbr_pacing_gain(uint32_t phase_index) noexcept {
    static const double probe_bw_gains[] = {
        1.25, 0.75, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0
    };

    constexpr size_t cycle_length = sizeof(probe_bw_gains) / sizeof(probe_bw_gains[0]);
    return probe_bw_gains[phase_index % cycle_length];
}

// ============================================================================
// Exported Helper Functions
// ============================================================================

/**
 * Create congestion control with optimal initial settings.
 *
 * @return Initialized NewReno congestion control
 */
NewRenoCongestionControl create_congestion_control() noexcept {
    return NewRenoCongestionControl();
}

/**
 * Create pacer with specified rate.
 *
 * @param rate_bps Initial pacing rate (bytes per second)
 * @return Initialized pacer
 */
Pacer create_pacer(uint64_t rate_bps) noexcept {
    Pacer pacer;
    pacer.set_rate(rate_bps);
    return pacer;
}

} // namespace quic
} // namespace fasterapi
