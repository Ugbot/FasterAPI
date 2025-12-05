// QUIC flow control implementation
// Core methods are inline in header for performance
// This file contains additional helper methods and algorithms

#include "quic_flow_control.h"
#include <algorithm>
#include <cstdint>

namespace fasterapi {
namespace quic {

// Constants for flow control auto-tuning
namespace {
    // Connection-level limits
    constexpr uint64_t MIN_CONNECTION_WINDOW = 64 * 1024;           // 64KB
    constexpr uint64_t MAX_CONNECTION_WINDOW = 64 * 1024 * 1024;    // 64MB
    constexpr uint64_t DEFAULT_CONNECTION_WINDOW = 1024 * 1024;      // 1MB

    // Stream-level limits
    constexpr uint64_t MIN_STREAM_WINDOW = 16 * 1024;               // 16KB
    constexpr uint64_t MAX_STREAM_WINDOW = 16 * 1024 * 1024;        // 16MB
    constexpr uint64_t DEFAULT_STREAM_WINDOW = 256 * 1024;          // 256KB

    // Auto-tuning thresholds
    constexpr double WINDOW_EXTEND_THRESHOLD = 0.5;  // Extend when 50% consumed
    constexpr double WINDOW_EXTEND_FACTOR = 2.0;     // Double the window
    constexpr double WINDOW_SHRINK_THRESHOLD = 0.1;  // Shrink if < 10% used
    constexpr double WINDOW_SHRINK_FACTOR = 0.5;     // Halve the window
}

// ============================================================================
// FlowControl - Connection-level Flow Control
// ============================================================================

// Calculate optimal window size based on RTT and bandwidth
uint64_t calculate_optimal_window(uint64_t rtt_us, uint64_t bandwidth_bps) {
    // BDP (Bandwidth-Delay Product) = (bandwidth * RTT) / 8
    // We want window >= BDP for full utilization
    uint64_t bdp = (bandwidth_bps * rtt_us) / (8 * 1000000);  // Convert to bytes

    // Add 20% headroom for bursts
    uint64_t optimal = bdp + (bdp / 5);

    // Clamp to reasonable limits
    return std::max(MIN_CONNECTION_WINDOW,
                    std::min(MAX_CONNECTION_WINDOW, optimal));
}

// Auto-tune receive window based on consumption patterns
uint64_t auto_tune_recv_window(uint64_t current_window,
                                uint64_t consumed_bytes,
                                uint64_t total_recv) {
    if (total_recv == 0) {
        return current_window;
    }

    double utilization = static_cast<double>(consumed_bytes) / static_cast<double>(total_recv);

    uint64_t new_window = current_window;

    // High utilization - extend window
    if (utilization >= WINDOW_EXTEND_THRESHOLD) {
        new_window = static_cast<uint64_t>(current_window * WINDOW_EXTEND_FACTOR);
        new_window = std::min(new_window, MAX_CONNECTION_WINDOW);
    }
    // Low utilization - shrink window (conserve receiver memory)
    else if (utilization <= WINDOW_SHRINK_THRESHOLD && current_window > MIN_CONNECTION_WINDOW) {
        new_window = static_cast<uint64_t>(current_window * WINDOW_SHRINK_FACTOR);
        new_window = std::max(new_window, MIN_CONNECTION_WINDOW);
    }

    return new_window;
}

// Check for flow control violations
bool validate_flow_control_send(uint64_t sent_data,
                                 uint64_t bytes_to_send,
                                 uint64_t max_data) {
    // Check for overflow
    if (sent_data > UINT64_MAX - bytes_to_send) {
        return false;  // Would overflow
    }

    // Check against limit
    return (sent_data + bytes_to_send) <= max_data;
}

bool validate_flow_control_recv(uint64_t offset,
                                 uint64_t length,
                                 uint64_t max_offset) {
    // Check for overflow
    if (offset > UINT64_MAX - length) {
        return false;  // Would overflow
    }

    // Check against limit
    return (offset + length) <= max_offset;
}

// Calculate how much credit to return to sender
uint64_t calculate_window_update(uint64_t current_max,
                                  uint64_t consumed,
                                  uint64_t received) {
    // Strategy: maintain window of at least consumed + headroom
    uint64_t desired_window = consumed + (current_max - received);

    // Only send update if significantly different (avoid spamming)
    uint64_t min_increase = current_max / 4;  // 25% increase threshold

    if (desired_window > current_max + min_increase) {
        return std::min(desired_window, MAX_CONNECTION_WINDOW);
    }

    return current_max;  // No update needed
}

// ============================================================================
// StreamFlowControl - Per-Stream Flow Control
// ============================================================================

// Auto-tune stream receive window
uint64_t auto_tune_stream_window(uint64_t current_window,
                                  uint64_t consumed_bytes,
                                  uint64_t total_recv) {
    if (total_recv == 0) {
        return current_window;
    }

    double utilization = static_cast<double>(consumed_bytes) / static_cast<double>(total_recv);

    uint64_t new_window = current_window;

    // High utilization - extend window
    if (utilization >= WINDOW_EXTEND_THRESHOLD) {
        new_window = static_cast<uint64_t>(current_window * WINDOW_EXTEND_FACTOR);
        new_window = std::min(new_window, MAX_STREAM_WINDOW);
    }
    // Low utilization - shrink window
    else if (utilization <= WINDOW_SHRINK_THRESHOLD && current_window > MIN_STREAM_WINDOW) {
        new_window = static_cast<uint64_t>(current_window * WINDOW_SHRINK_FACTOR);
        new_window = std::max(new_window, MIN_STREAM_WINDOW);
    }

    return new_window;
}

// Check if stream is significantly blocked
bool is_stream_significantly_blocked(uint64_t sent_offset,
                                      uint64_t max_stream_data,
                                      uint64_t pending_bytes) {
    // Consider blocked if we have pending data but can't send
    if (pending_bytes == 0) {
        return false;
    }

    // Blocked if we're at the limit
    if (sent_offset >= max_stream_data) {
        return true;
    }

    // Blocked if we can't send at least 25% of pending data
    uint64_t available = max_stream_data - sent_offset;
    return available < (pending_bytes / 4);
}

// Calculate stream window update
uint64_t calculate_stream_window_update(uint64_t current_max,
                                         uint64_t consumed,
                                         uint64_t received) {
    // Strategy: maintain window of at least consumed + headroom
    uint64_t desired_window = consumed + (current_max - received);

    // Only send update if significantly different
    uint64_t min_increase = current_max / 4;  // 25% increase threshold

    if (desired_window > current_max + min_increase) {
        return std::min(desired_window, MAX_STREAM_WINDOW);
    }

    return current_max;  // No update needed
}

// ============================================================================
// Flow Control Coordination
// ============================================================================

// Ensure stream flow control doesn't exceed connection flow control
uint64_t coordinate_stream_send(uint64_t stream_available,
                                 uint64_t connection_available) {
    // Can only send what both windows allow
    return std::min(stream_available, connection_available);
}

// Ensure stream window updates don't exceed connection window
uint64_t coordinate_stream_recv(uint64_t stream_window,
                                 uint64_t connection_window,
                                 uint64_t stream_recv_offset,
                                 uint64_t connection_recv_data) {
    // Stream can't receive more than connection allows
    uint64_t connection_remaining = 0;
    if (connection_window > connection_recv_data) {
        connection_remaining = connection_window - connection_recv_data;
    }

    uint64_t stream_max = stream_recv_offset + connection_remaining;

    return std::min(stream_window, stream_max);
}

// ============================================================================
// Flow Control Diagnostics and Helpers
// ============================================================================

// Calculate blocked percentage (for diagnostics)
double calculate_block_percentage(uint64_t blocked_time_us,
                                   uint64_t total_time_us) {
    if (total_time_us == 0) {
        return 0.0;
    }
    return (static_cast<double>(blocked_time_us) / static_cast<double>(total_time_us)) * 100.0;
}

// Estimate how long until unblocked (based on estimated peer consumption rate)
uint64_t estimate_unblock_time_us(uint64_t blocked_bytes,
                                   uint64_t peer_consumption_rate_bps) {
    if (peer_consumption_rate_bps == 0) {
        return UINT64_MAX;  // Unknown
    }

    // Time = bytes / (rate / 8) = (bytes * 8) / rate
    uint64_t time_us = (blocked_bytes * 8 * 1000000) / peer_consumption_rate_bps;

    return time_us;
}

// Check if window is critically low
bool is_window_critical(uint64_t available,
                        uint64_t max_window) {
    // Critical if < 10% remaining
    return available < (max_window / 10);
}

// Check if window is healthy
bool is_window_healthy(uint64_t available,
                       uint64_t max_window) {
    // Healthy if >= 50% remaining
    return available >= (max_window / 2);
}

// ============================================================================
// Advanced Flow Control Strategies
// ============================================================================

// Calculate aggressive window size (for low-latency applications)
uint64_t calculate_aggressive_window(uint64_t base_window) {
    // Use 4x base window for aggressive mode
    return std::min(base_window * 4, MAX_CONNECTION_WINDOW);
}

// Calculate conservative window size (for memory-constrained receivers)
uint64_t calculate_conservative_window(uint64_t base_window) {
    // Use 0.5x base window for conservative mode
    return std::max(base_window / 2, MIN_CONNECTION_WINDOW);
}

// Apply hysteresis to window updates (prevent oscillation)
uint64_t apply_window_hysteresis(uint64_t current_window,
                                  uint64_t proposed_window,
                                  uint64_t hysteresis_factor) {
    // Only change if difference exceeds hysteresis threshold
    uint64_t threshold = current_window / hysteresis_factor;

    if (proposed_window > current_window + threshold) {
        return proposed_window;  // Increase
    } else if (proposed_window < current_window - threshold) {
        return proposed_window;  // Decrease
    }

    return current_window;  // Keep current
}

// ============================================================================
// Exported Helper Functions (for Python bindings if needed)
// ============================================================================

// Create flow control with optimal settings
FlowControl create_connection_flow_control(uint64_t rtt_us,
                                            uint64_t bandwidth_bps) {
    uint64_t optimal_window = calculate_optimal_window(rtt_us, bandwidth_bps);
    return FlowControl(optimal_window);
}

// Create stream flow control with optimal settings
StreamFlowControl create_stream_flow_control(uint64_t connection_window) {
    // Stream window is typically 25% of connection window
    uint64_t stream_window = std::max(MIN_STREAM_WINDOW,
                                      std::min(MAX_STREAM_WINDOW,
                                              connection_window / 4));
    return StreamFlowControl(stream_window);
}

} // namespace quic
} // namespace fasterapi
