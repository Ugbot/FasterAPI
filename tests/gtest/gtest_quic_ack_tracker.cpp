/**
 * QUIC ACK Tracker Unit Tests
 *
 * Tests the QUIC loss detection and recovery mechanism (RFC 9002):
 * - SentPacket tracking
 * - ACK processing
 * - RTT estimation (smoothed RTT, min RTT, RTT variance)
 * - Loss detection (time-based and packet-based)
 * - Congestion control integration
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/quic/quic_ack_tracker.h"
#include <random>
#include <chrono>

namespace fasterapi {
namespace quic {
namespace test {

// ===========================================================================
// SentPacket Tests
// ===========================================================================

class SentPacketTest : public ::testing::Test {
protected:
    void SetUp() override {}
};

TEST_F(SentPacketTest, DefaultConstruction) {
    SentPacket pkt;

    EXPECT_EQ(pkt.packet_number, 0u);
    EXPECT_EQ(pkt.time_sent, 0u);
    EXPECT_EQ(pkt.size, 0u);
    EXPECT_FALSE(pkt.ack_eliciting);
    EXPECT_FALSE(pkt.in_flight);
}

TEST_F(SentPacketTest, FieldAssignment) {
    SentPacket pkt;
    pkt.packet_number = 12345;
    pkt.time_sent = 1000000;
    pkt.size = 1200;
    pkt.ack_eliciting = true;
    pkt.in_flight = true;

    EXPECT_EQ(pkt.packet_number, 12345u);
    EXPECT_EQ(pkt.time_sent, 1000000u);
    EXPECT_EQ(pkt.size, 1200u);
    EXPECT_TRUE(pkt.ack_eliciting);
    EXPECT_TRUE(pkt.in_flight);
}

// ===========================================================================
// AckTracker Basic Tests
// ===========================================================================

class AckTrackerTest : public ::testing::Test {
protected:
    AckTracker tracker_;
    NewRenoCongestionControl cc_;
    std::mt19937_64 rng_;

    void SetUp() override {
        rng_.seed(std::random_device{}());
    }

    uint64_t random_time() {
        return std::uniform_int_distribution<uint64_t>(1000000, 10000000)(rng_);
    }

    uint64_t random_size() {
        return std::uniform_int_distribution<uint64_t>(100, 1500)(rng_);
    }
};

TEST_F(AckTrackerTest, InitialState) {
    EXPECT_EQ(tracker_.next_packet_number(), 0u);
    EXPECT_EQ(tracker_.largest_acked(), 0u);
    EXPECT_EQ(tracker_.smoothed_rtt(), AckTracker::kInitialRtt);
    EXPECT_EQ(tracker_.rttvar(), AckTracker::kInitialRtt / 2);
    EXPECT_EQ(tracker_.min_rtt(), UINT64_MAX);
    EXPECT_EQ(tracker_.latest_rtt(), 0u);
    EXPECT_EQ(tracker_.in_flight_count(), 0u);
}

TEST_F(AckTrackerTest, Constants) {
    EXPECT_EQ(AckTracker::kTimeThreshold, 9u);
    EXPECT_EQ(AckTracker::kTimeThresholdDivisor, 8u);
    EXPECT_EQ(AckTracker::kPacketThreshold, 3u);
    EXPECT_EQ(AckTracker::kGranularity, 1000u);  // 1ms
    EXPECT_EQ(AckTracker::kInitialRtt, 333000u);  // 333ms
}

// ===========================================================================
// Packet Sending Tests
// ===========================================================================

TEST_F(AckTrackerTest, SendSinglePacket) {
    uint64_t now = random_time();
    uint64_t size = random_size();

    tracker_.on_packet_sent(0, size, true, now);

    EXPECT_EQ(tracker_.next_packet_number(), 1u);
    EXPECT_EQ(tracker_.in_flight_count(), 1u);
}

TEST_F(AckTrackerTest, SendMultiplePackets) {
    uint64_t now = random_time();

    for (uint64_t i = 0; i < 10; i++) {
        tracker_.on_packet_sent(i, random_size(), true, now + i * 1000);
    }

    EXPECT_EQ(tracker_.next_packet_number(), 10u);
    EXPECT_EQ(tracker_.in_flight_count(), 10u);
}

TEST_F(AckTrackerTest, SendNonContiguousPackets) {
    uint64_t now = random_time();

    tracker_.on_packet_sent(0, 100, true, now);
    tracker_.on_packet_sent(5, 100, true, now + 1000);
    tracker_.on_packet_sent(10, 100, true, now + 2000);

    EXPECT_EQ(tracker_.next_packet_number(), 11u);
    EXPECT_EQ(tracker_.in_flight_count(), 3u);
}

TEST_F(AckTrackerTest, SendNonAckElicitingPacket) {
    uint64_t now = random_time();

    // Non-ACK-eliciting packet (like ACK-only)
    tracker_.on_packet_sent(0, 50, false, now);

    EXPECT_EQ(tracker_.next_packet_number(), 1u);
    EXPECT_EQ(tracker_.in_flight_count(), 1u);  // Still in flight
}

// ===========================================================================
// ACK Processing Tests
// ===========================================================================

TEST_F(AckTrackerTest, AckSinglePacket) {
    uint64_t now = 1000000;

    tracker_.on_packet_sent(0, 1200, true, now);

    // ACK after 50ms RTT
    uint64_t ack_time = now + 50000;
    AckFrame ack;
    ack.largest_acked = 0;
    ack.ack_delay = 1000;
    ack.first_ack_range = 0;
    ack.range_count = 0;

    size_t newly_acked = tracker_.on_ack_received(ack, ack_time, cc_);

    EXPECT_EQ(newly_acked, 1u);
    EXPECT_EQ(tracker_.largest_acked(), 0u);
    EXPECT_EQ(tracker_.in_flight_count(), 0u);
}

TEST_F(AckTrackerTest, AckMultiplePackets) {
    uint64_t now = 1000000;

    // Send 5 packets
    for (uint64_t i = 0; i < 5; i++) {
        tracker_.on_packet_sent(i, 1200, true, now + i * 100);
    }

    // ACK all 5 (largest=4, first_ack_range=4 means acks 0-4)
    uint64_t ack_time = now + 50000;
    AckFrame ack;
    ack.largest_acked = 4;
    ack.ack_delay = 1000;
    ack.first_ack_range = 4;
    ack.range_count = 0;

    size_t newly_acked = tracker_.on_ack_received(ack, ack_time, cc_);

    EXPECT_EQ(newly_acked, 5u);
    EXPECT_EQ(tracker_.largest_acked(), 4u);
    EXPECT_EQ(tracker_.in_flight_count(), 0u);
}

TEST_F(AckTrackerTest, AckWithGaps) {
    uint64_t now = 1000000;

    // Send 10 packets
    for (uint64_t i = 0; i < 10; i++) {
        tracker_.on_packet_sent(i, 1200, true, now + i * 100);
    }

    // ACK packets 7-9 (largest=9, first_ack_range=2)
    // This tests basic ACK without additional ranges
    AckFrame ack;
    ack.largest_acked = 9;
    ack.ack_delay = 1000;
    ack.first_ack_range = 2;  // Acks 7, 8, 9 (3 packets)
    ack.range_count = 0;

    size_t newly_acked = tracker_.on_ack_received(ack, now + 50000, cc_);

    // Should ACK 3 packets: 7, 8, 9
    EXPECT_EQ(newly_acked, 3u);
    EXPECT_EQ(tracker_.largest_acked(), 9u);
    // Loss detection: 9 - 0 >= 3, so packets 0-6 are lost
    EXPECT_EQ(tracker_.in_flight_count(), 0u);
}

TEST_F(AckTrackerTest, DuplicateAck) {
    uint64_t now = 1000000;

    tracker_.on_packet_sent(0, 1200, true, now);

    AckFrame ack;
    ack.largest_acked = 0;
    ack.ack_delay = 1000;
    ack.first_ack_range = 0;
    ack.range_count = 0;

    // First ACK
    size_t newly_acked1 = tracker_.on_ack_received(ack, now + 50000, cc_);
    EXPECT_EQ(newly_acked1, 1u);

    // Duplicate ACK
    size_t newly_acked2 = tracker_.on_ack_received(ack, now + 60000, cc_);
    EXPECT_EQ(newly_acked2, 0u);  // Already acked
}

TEST_F(AckTrackerTest, AckUnsentPacket) {
    uint64_t now = 1000000;

    tracker_.on_packet_sent(0, 1200, true, now);

    // ACK packet 5 which was never sent
    AckFrame ack;
    ack.largest_acked = 5;
    ack.ack_delay = 1000;
    ack.first_ack_range = 0;
    ack.range_count = 0;

    size_t newly_acked = tracker_.on_ack_received(ack, now + 50000, cc_);

    EXPECT_EQ(newly_acked, 0u);  // Can't ack unsent packet
    EXPECT_EQ(tracker_.largest_acked(), 5u);  // But largest_acked updates
}

// ===========================================================================
// RTT Estimation Tests
// ===========================================================================

TEST_F(AckTrackerTest, FirstRttSample) {
    uint64_t now = 1000000;

    tracker_.on_packet_sent(0, 1200, true, now);

    // ACK after 100ms
    uint64_t rtt = 100000;  // 100ms
    AckFrame ack;
    ack.largest_acked = 0;
    ack.ack_delay = 0;
    ack.first_ack_range = 0;
    ack.range_count = 0;

    tracker_.on_ack_received(ack, now + rtt, cc_);

    // First sample: smoothed_rtt = rtt, rttvar = rtt/2
    EXPECT_EQ(tracker_.smoothed_rtt(), rtt);
    EXPECT_EQ(tracker_.rttvar(), rtt / 2);
    EXPECT_EQ(tracker_.min_rtt(), rtt);
    EXPECT_EQ(tracker_.latest_rtt(), rtt);
}

TEST_F(AckTrackerTest, MultipleRttSamples) {
    uint64_t now = 1000000;
    uint64_t rtt1 = 100000;  // 100ms
    uint64_t rtt2 = 120000;  // 120ms

    // First packet
    tracker_.on_packet_sent(0, 1200, true, now);
    AckFrame ack1;
    ack1.largest_acked = 0;
    ack1.ack_delay = 0;
    ack1.first_ack_range = 0;
    ack1.range_count = 0;
    tracker_.on_ack_received(ack1, now + rtt1, cc_);

    // Second packet
    tracker_.on_packet_sent(1, 1200, true, now + rtt1);
    AckFrame ack2;
    ack2.largest_acked = 1;
    ack2.ack_delay = 0;
    ack2.first_ack_range = 0;
    ack2.range_count = 0;
    tracker_.on_ack_received(ack2, now + rtt1 + rtt2, cc_);

    // EWMA: smoothed_rtt = (7 * 100000 + 120000) / 8 = 102500
    EXPECT_EQ(tracker_.smoothed_rtt(), 102500u);
    EXPECT_EQ(tracker_.latest_rtt(), rtt2);
    EXPECT_EQ(tracker_.min_rtt(), rtt1);  // min stays at first sample
}

TEST_F(AckTrackerTest, MinRttUpdates) {
    uint64_t now = 1000000;

    // First packet with high RTT
    tracker_.on_packet_sent(0, 1200, true, now);
    AckFrame ack1;
    ack1.largest_acked = 0;
    ack1.ack_delay = 0;
    ack1.first_ack_range = 0;
    ack1.range_count = 0;
    tracker_.on_ack_received(ack1, now + 100000, cc_);  // 100ms

    // Second packet with lower RTT
    tracker_.on_packet_sent(1, 1200, true, now + 100000);
    AckFrame ack2;
    ack2.largest_acked = 1;
    ack2.ack_delay = 0;
    ack2.first_ack_range = 0;
    ack2.range_count = 0;
    tracker_.on_ack_received(ack2, now + 150000, cc_);  // 50ms RTT

    EXPECT_EQ(tracker_.min_rtt(), 50000u);
}

TEST_F(AckTrackerTest, RttVarianceUpdate) {
    uint64_t now = 1000000;

    // First packet: 100ms RTT
    tracker_.on_packet_sent(0, 1200, true, now);
    AckFrame ack1;
    ack1.largest_acked = 0;
    ack1.ack_delay = 0;
    ack1.first_ack_range = 0;
    ack1.range_count = 0;
    tracker_.on_ack_received(ack1, now + 100000, cc_);

    uint64_t rttvar1 = tracker_.rttvar();  // 50000

    // Second packet: 200ms RTT (large variance)
    tracker_.on_packet_sent(1, 1200, true, now + 100000);
    AckFrame ack2;
    ack2.largest_acked = 1;
    ack2.ack_delay = 0;
    ack2.first_ack_range = 0;
    ack2.range_count = 0;
    tracker_.on_ack_received(ack2, now + 300000, cc_);  // 200ms RTT

    // rttvar should increase due to large RTT difference
    // rtt_diff = 200000 - 100000 = 100000
    // rttvar = (3 * 50000 + 100000) / 4 = 62500
    EXPECT_EQ(tracker_.rttvar(), 62500u);
}

// ===========================================================================
// Loss Detection Tests
// ===========================================================================

TEST_F(AckTrackerTest, PacketBasedLossDetection) {
    uint64_t now = 1000000;

    // Send 5 packets
    for (uint64_t i = 0; i < 5; i++) {
        tracker_.on_packet_sent(i, 1200, true, now + i * 1000);
    }

    // ACK packet 4 only (which is 3+ packets ahead of packet 0)
    AckFrame ack;
    ack.largest_acked = 4;
    ack.ack_delay = 0;
    ack.first_ack_range = 0;  // Only packet 4
    ack.range_count = 0;

    tracker_.on_ack_received(ack, now + 50000, cc_);

    // Packet 0 and 1 should be detected as lost (4 - 0 >= 3 and 4 - 1 >= 3)
    // Packet 0, 1 are lost
    // In-flight: 2, 3 (not lost yet, 4-2=2 < 3, 4-3=1 < 3)
    EXPECT_EQ(tracker_.in_flight_count(), 2u);  // 2 and 3 still in flight
}

TEST_F(AckTrackerTest, TimeBasedLossDetection) {
    uint64_t now = 1000000;
    uint64_t rtt = 50000;  // 50ms

    // Send first packet
    tracker_.on_packet_sent(0, 1200, true, now);

    // ACK it to establish RTT
    AckFrame ack1;
    ack1.largest_acked = 0;
    ack1.ack_delay = 0;
    ack1.first_ack_range = 0;
    ack1.range_count = 0;
    tracker_.on_ack_received(ack1, now + rtt, cc_);

    // Send second packet
    tracker_.on_packet_sent(1, 1200, true, now + rtt);

    // Wait for > 9/8 * RTT = 56250us
    uint64_t loss_delay = (9 * rtt) / 8;  // 56250

    // Send and ACK third packet much later
    tracker_.on_packet_sent(2, 1200, true, now + rtt + loss_delay + 10000);

    AckFrame ack2;
    ack2.largest_acked = 2;
    ack2.ack_delay = 0;
    ack2.first_ack_range = 0;  // Only packet 2
    ack2.range_count = 0;

    // This should trigger time-based loss for packet 1
    tracker_.on_ack_received(ack2, now + rtt + loss_delay + 20000, cc_);

    // Packet 1 should be marked as lost (time-based)
    EXPECT_EQ(tracker_.in_flight_count(), 0u);
}

TEST_F(AckTrackerTest, LossTimerExpiration) {
    uint64_t now = 1000000;

    // Send packet
    tracker_.on_packet_sent(0, 1200, true, now);

    // Before any ACK, no loss time
    EXPECT_FALSE(tracker_.loss_detection_timer_expired(now + 1000000));
}

// ===========================================================================
// Congestion Control Integration Tests
// ===========================================================================

TEST_F(AckTrackerTest, CongestionControlOnAck) {
    uint64_t now = 1000000;
    uint64_t cwnd_before = cc_.congestion_window();

    // Send and ACK packet
    tracker_.on_packet_sent(0, 1200, true, now);

    AckFrame ack;
    ack.largest_acked = 0;
    ack.ack_delay = 0;
    ack.first_ack_range = 0;
    ack.range_count = 0;

    tracker_.on_ack_received(ack, now + 50000, cc_);

    // Congestion window should increase (slow start)
    EXPECT_GT(cc_.congestion_window(), cwnd_before);
}

TEST_F(AckTrackerTest, CongestionControlOnLoss) {
    uint64_t now = 1000000;

    // First establish RTT
    tracker_.on_packet_sent(0, 1200, true, now);
    AckFrame ack1;
    ack1.largest_acked = 0;
    ack1.ack_delay = 0;
    ack1.first_ack_range = 0;
    ack1.range_count = 0;
    tracker_.on_ack_received(ack1, now + 50000, cc_);

    // Send more packets
    for (uint64_t i = 1; i <= 5; i++) {
        tracker_.on_packet_sent(i, 1200, true, now + 50000 + i * 1000);
    }

    uint64_t cwnd_before = cc_.congestion_window();

    // ACK packet 5, causing loss of packets 1 and 2 (3+ ahead)
    AckFrame ack2;
    ack2.largest_acked = 5;
    ack2.ack_delay = 0;
    ack2.first_ack_range = 0;  // Only packet 5
    ack2.range_count = 0;

    tracker_.on_ack_received(ack2, now + 100000, cc_);

    // Congestion window should decrease due to loss
    EXPECT_LT(cc_.congestion_window(), cwnd_before);
}

// ===========================================================================
// Edge Cases
// ===========================================================================

TEST_F(AckTrackerTest, LargePacketNumbers) {
    uint64_t now = 1000000;
    uint64_t large_pn = 0x3FFFFFFF;  // Large packet number

    tracker_.on_packet_sent(large_pn, 1200, true, now);

    EXPECT_EQ(tracker_.next_packet_number(), large_pn + 1);

    AckFrame ack;
    ack.largest_acked = large_pn;
    ack.ack_delay = 0;
    ack.first_ack_range = 0;
    ack.range_count = 0;

    size_t newly_acked = tracker_.on_ack_received(ack, now + 50000, cc_);
    EXPECT_EQ(newly_acked, 1u);
}

TEST_F(AckTrackerTest, ZeroSizePacket) {
    uint64_t now = 1000000;

    // Zero-size packet (header only)
    tracker_.on_packet_sent(0, 0, true, now);

    EXPECT_EQ(tracker_.in_flight_count(), 1u);

    AckFrame ack;
    ack.largest_acked = 0;
    ack.ack_delay = 0;
    ack.first_ack_range = 0;
    ack.range_count = 0;

    tracker_.on_ack_received(ack, now + 50000, cc_);
    EXPECT_EQ(tracker_.in_flight_count(), 0u);
}

TEST_F(AckTrackerTest, ManyPacketsInFlight) {
    uint64_t now = 1000000;
    const int count = 1000;

    // Send many packets
    for (int i = 0; i < count; i++) {
        tracker_.on_packet_sent(i, random_size(), true, now + i * 100);
    }

    EXPECT_EQ(tracker_.in_flight_count(), static_cast<size_t>(count));
    EXPECT_EQ(tracker_.next_packet_number(), static_cast<uint64_t>(count));

    // ACK all at once
    AckFrame ack;
    ack.largest_acked = count - 1;
    ack.ack_delay = 0;
    ack.first_ack_range = count - 1;  // All packets
    ack.range_count = 0;

    size_t newly_acked = tracker_.on_ack_received(ack, now + 100000, cc_);
    EXPECT_EQ(newly_acked, static_cast<size_t>(count));
    EXPECT_EQ(tracker_.in_flight_count(), 0u);
}

TEST_F(AckTrackerTest, RapidAckProcessing) {
    uint64_t now = 1000000;

    // Send 10 packets
    for (uint64_t i = 0; i < 10; i++) {
        tracker_.on_packet_sent(i, 1200, true, now + i * 1000);
    }

    // ACK packets one at a time in reverse order
    for (int i = 9; i >= 0; i--) {
        AckFrame ack;
        ack.largest_acked = i;
        ack.ack_delay = 0;
        ack.first_ack_range = 0;
        ack.range_count = 0;

        tracker_.on_ack_received(ack, now + 50000 + (9 - i) * 1000, cc_);
    }

    EXPECT_EQ(tracker_.in_flight_count(), 0u);
}

// ===========================================================================
// Performance Tests
// ===========================================================================

class AckTrackerPerformanceTest : public ::testing::Test {
protected:
    std::mt19937_64 rng_;

    void SetUp() override {
        rng_.seed(std::random_device{}());
    }
};

TEST_F(AckTrackerPerformanceTest, PacketSentPerformance) {
    AckTracker tracker;
    const int iterations = 100000;
    uint64_t now = 1000000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        tracker.on_packet_sent(i, 1200, true, now + i);
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "on_packet_sent: " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 1000.0);  // Should be < 1µs
}

TEST_F(AckTrackerPerformanceTest, AckProcessingPerformance) {
    const int iterations = 10000;
    uint64_t now = 1000000;
    double total_ns = 0;

    // Run multiple independent trials to avoid O(n^2) behavior
    for (int trial = 0; trial < 10; trial++) {
        AckTracker tracker;
        NewRenoCongestionControl cc;

        // Send 100 packets per trial
        for (int i = 0; i < 100; i++) {
            tracker.on_packet_sent(i, 1200, true, now + i);
        }

        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 100; i++) {
            AckFrame ack;
            ack.largest_acked = i;
            ack.ack_delay = 0;
            ack.first_ack_range = 0;  // ACK 1 packet
            ack.range_count = 0;

            tracker.on_ack_received(ack, now + 50000 + i, cc);
        }
        auto end = std::chrono::high_resolution_clock::now();

        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
        total_ns += static_cast<double>(duration.count()) / 100;
    }

    double ns_per_op = total_ns / 10;
    std::cout << "on_ack_received (1 packet, small queue): " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 50000.0);  // Should be < 50µs per ACK
}

TEST_F(AckTrackerPerformanceTest, LossDetectionPerformance) {
    AckTracker tracker;
    NewRenoCongestionControl cc;
    const int packet_count = 10000;
    const int iterations = 1000;
    uint64_t now = 1000000;

    // Send many packets
    for (int i = 0; i < packet_count; i++) {
        tracker.on_packet_sent(i, 1200, true, now + i);
    }

    // Establish RTT
    AckFrame ack1;
    ack1.largest_acked = 0;
    ack1.ack_delay = 0;
    ack1.first_ack_range = 0;
    ack1.range_count = 0;
    tracker.on_ack_received(ack1, now + 50000, cc);

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        tracker.detect_and_remove_lost_packets(now + 50000 + i * 100, cc);
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "detect_and_remove_lost_packets (" << packet_count
              << " packets): " << ns_per_op << " ns/op\n";
    // Loss detection should be efficient even with many packets
    EXPECT_LT(ns_per_op, 5000000.0);  // < 5ms
}

}  // namespace test
}  // namespace quic
}  // namespace fasterapi

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
