/**
 * QUIC Congestion Control Unit Tests
 *
 * Tests the QUIC congestion control implementation (RFC 9002):
 * - NewReno congestion control algorithm
 * - Slow start, congestion avoidance, fast recovery
 * - Pacing for burst prevention
 * - Window management
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/quic/quic_congestion.h"
#include <random>
#include <chrono>

namespace fasterapi {
namespace quic {
namespace test {

// ===========================================================================
// NewReno Congestion Control Tests
// ===========================================================================

class NewRenoCongestionControlTest : public ::testing::Test {
protected:
    std::unique_ptr<NewRenoCongestionControl> cc_;
    std::mt19937_64 rng_;

    void SetUp() override {
        cc_ = std::make_unique<NewRenoCongestionControl>();
        rng_.seed(std::random_device{}());
    }

    uint64_t now_us() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        ).count();
    }
};

TEST_F(NewRenoCongestionControlTest, InitialState) {
    EXPECT_EQ(cc_->congestion_window(), NewRenoCongestionControl::kInitialWindow);
    EXPECT_EQ(cc_->ssthresh(), UINT64_MAX);
    EXPECT_EQ(cc_->bytes_in_flight(), 0u);
    EXPECT_TRUE(cc_->in_slow_start());
    EXPECT_FALSE(cc_->in_recovery(now_us()));
}

TEST_F(NewRenoCongestionControlTest, CanSendWithinWindow) {
    EXPECT_TRUE(cc_->can_send(1000));
    EXPECT_TRUE(cc_->can_send(NewRenoCongestionControl::kInitialWindow));
    EXPECT_FALSE(cc_->can_send(NewRenoCongestionControl::kInitialWindow + 1));
}

TEST_F(NewRenoCongestionControlTest, OnPacketSent) {
    cc_->on_packet_sent(1000);
    EXPECT_EQ(cc_->bytes_in_flight(), 1000u);

    cc_->on_packet_sent(2000);
    EXPECT_EQ(cc_->bytes_in_flight(), 3000u);
}

TEST_F(NewRenoCongestionControlTest, OnPacketAcked) {
    cc_->on_packet_sent(1000);
    cc_->on_packet_sent(2000);
    EXPECT_EQ(cc_->bytes_in_flight(), 3000u);

    cc_->on_packet_acked(1000);
    EXPECT_EQ(cc_->bytes_in_flight(), 2000u);

    cc_->on_packet_acked(2000);
    EXPECT_EQ(cc_->bytes_in_flight(), 0u);
}

TEST_F(NewRenoCongestionControlTest, OnPacketLost) {
    cc_->on_packet_sent(1000);
    cc_->on_packet_sent(2000);

    cc_->on_packet_lost(1000);
    EXPECT_EQ(cc_->bytes_in_flight(), 2000u);
}

TEST_F(NewRenoCongestionControlTest, SlowStartGrowth) {
    uint64_t initial_window = cc_->congestion_window();
    EXPECT_TRUE(cc_->in_slow_start());

    // Send and ACK data - window should grow exponentially
    uint64_t now = now_us();
    cc_->on_ack_received(1000, now);

    EXPECT_GT(cc_->congestion_window(), initial_window);
    EXPECT_EQ(cc_->congestion_window(), initial_window + 1000);
}

TEST_F(NewRenoCongestionControlTest, CongestionEvent) {
    uint64_t initial_window = cc_->congestion_window();
    uint64_t now = now_us();

    cc_->on_congestion_event(now);

    // Window should be reduced to half (or minimum)
    uint64_t expected = std::max(initial_window / 2,
                                 NewRenoCongestionControl::kMinimumWindow);
    EXPECT_EQ(cc_->congestion_window(), expected);
    EXPECT_EQ(cc_->ssthresh(), expected);
    EXPECT_TRUE(cc_->in_recovery(now));
}

TEST_F(NewRenoCongestionControlTest, RecoveryPreventsWindowReduction) {
    uint64_t now = now_us();

    // First congestion event
    cc_->on_congestion_event(now);
    uint64_t window_after_first = cc_->congestion_window();

    // Second event during recovery should not reduce window further
    cc_->on_congestion_event(now + 1000);
    EXPECT_EQ(cc_->congestion_window(), window_after_first);
}

TEST_F(NewRenoCongestionControlTest, RecoveryPreventsWindowGrowth) {
    uint64_t now = now_us();

    cc_->on_congestion_event(now);
    uint64_t window_after_loss = cc_->congestion_window();

    // ACK during recovery should not increase window
    cc_->on_ack_received(1000, now + 1000);
    EXPECT_EQ(cc_->congestion_window(), window_after_loss);
}

TEST_F(NewRenoCongestionControlTest, ExitSlowStart) {
    uint64_t now = now_us();

    // Cause congestion event to set ssthresh
    cc_->on_congestion_event(now);
    EXPECT_FALSE(cc_->in_slow_start());

    // Window should be at ssthresh, not in slow start anymore
    EXPECT_EQ(cc_->congestion_window(), cc_->ssthresh());
}

TEST_F(NewRenoCongestionControlTest, CongestionAvoidanceGrowth) {
    uint64_t now = now_us();

    // Exit slow start
    cc_->on_congestion_event(now);
    now += 2000000;  // Wait for recovery to end

    uint64_t window_before = cc_->congestion_window();

    // In congestion avoidance, window grows linearly
    cc_->on_ack_received(NewRenoCongestionControl::kMaxDatagramSize, now);

    // Growth should be approximately 1 MSS per RTT
    // = (MSS * acked) / cwnd ≈ 1 byte
    EXPECT_GE(cc_->congestion_window(), window_before);
}

TEST_F(NewRenoCongestionControlTest, PersistentCongestion) {
    // First, grow the window
    uint64_t now = now_us();
    for (int i = 0; i < 10; i++) {
        cc_->on_ack_received(1000, now);
    }
    EXPECT_GT(cc_->congestion_window(), NewRenoCongestionControl::kInitialWindow);

    // Persistent congestion resets everything
    cc_->on_persistent_congestion();

    EXPECT_EQ(cc_->congestion_window(), NewRenoCongestionControl::kMinimumWindow);
    EXPECT_EQ(cc_->ssthresh(), UINT64_MAX);
    EXPECT_TRUE(cc_->in_slow_start());
}

TEST_F(NewRenoCongestionControlTest, AvailableCapacity) {
    EXPECT_EQ(cc_->available_capacity(), cc_->congestion_window());

    cc_->on_packet_sent(1000);
    EXPECT_EQ(cc_->available_capacity(), cc_->congestion_window() - 1000);

    cc_->on_packet_sent(cc_->congestion_window() - 1000);
    EXPECT_EQ(cc_->available_capacity(), 0u);
}

TEST_F(NewRenoCongestionControlTest, CanSendWithBytesInFlight) {
    cc_->on_packet_sent(cc_->congestion_window() - 100);
    EXPECT_TRUE(cc_->can_send(100));
    EXPECT_FALSE(cc_->can_send(101));
}

TEST_F(NewRenoCongestionControlTest, PacingRateWithRTT) {
    cc_->update_rtt(100000);  // 100ms RTT

    uint64_t rate = cc_->pacing_rate();
    // Rate = window / RTT (in bytes per second)
    // = 12000 bytes / 0.1s = 120000 bytes/sec
    uint64_t expected = (cc_->congestion_window() * 1000000) / 100000;
    EXPECT_EQ(rate, expected);
}

TEST_F(NewRenoCongestionControlTest, PacingRateNoRTT) {
    // Without RTT, pacing should be unlimited
    EXPECT_EQ(cc_->pacing_rate(), UINT64_MAX);
}

TEST_F(NewRenoCongestionControlTest, MinimumWindowGuard) {
    uint64_t now = now_us();

    // Multiple congestion events
    for (int i = 0; i < 10; i++) {
        cc_->on_congestion_event(now);
        now += 2000000;  // Exit recovery
    }

    // Should never go below minimum
    EXPECT_GE(cc_->congestion_window(), NewRenoCongestionControl::kMinimumWindow);
}

TEST_F(NewRenoCongestionControlTest, AckedBytesUnderflow) {
    // Test underflow protection
    cc_->on_packet_sent(1000);
    cc_->on_packet_acked(2000);  // More than sent

    EXPECT_EQ(cc_->bytes_in_flight(), 0u);
}

TEST_F(NewRenoCongestionControlTest, LostBytesUnderflow) {
    cc_->on_packet_sent(1000);
    cc_->on_packet_lost(2000);  // More than sent

    EXPECT_EQ(cc_->bytes_in_flight(), 0u);
}

// ===========================================================================
// Pacer Tests
// ===========================================================================

class PacerTest : public ::testing::Test {
protected:
    std::unique_ptr<Pacer> pacer_;

    void SetUp() override {
        pacer_ = std::make_unique<Pacer>();
    }

    uint64_t now_us() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        ).count();
    }
};

TEST_F(PacerTest, NoPacing) {
    // Without setting rate, all sends should be allowed
    uint64_t now = now_us();
    EXPECT_TRUE(pacer_->can_send(1000, now));
    EXPECT_TRUE(pacer_->can_send(10000, now));
    EXPECT_TRUE(pacer_->can_send(100000, now));
}

TEST_F(PacerTest, SetRate) {
    pacer_->set_rate(1000000);  // 1MB/sec

    uint64_t now = now_us();
    // First send should work (gets initial tokens)
    EXPECT_TRUE(pacer_->can_send(1000, now));
}

TEST_F(PacerTest, TokenAccumulation) {
    pacer_->set_rate(1000000);  // 1MB/sec = 1 byte/us

    uint64_t now = now_us();
    pacer_->can_send(0, now);  // Initialize

    // After 1000us, should have 1000 tokens
    now += 1000;
    EXPECT_TRUE(pacer_->can_send(500, now));
}

TEST_F(PacerTest, TokenDepletion) {
    pacer_->set_rate(100000);  // 100KB/sec

    uint64_t now = now_us();
    // Consume initial tokens
    while (pacer_->can_send(1000, now)) {
        // Keep consuming
    }

    // Should be blocked now
    EXPECT_FALSE(pacer_->can_send(1000, now));

    // After time passes, should be able to send again
    now += 100000;  // 100ms
    EXPECT_TRUE(pacer_->can_send(1000, now));
}

TEST_F(PacerTest, BurstPrevention) {
    pacer_->set_rate(1000000);  // 1MB/sec

    uint64_t now = now_us();
    pacer_->can_send(0, now);

    // Wait a long time
    now += 10000000;  // 10 seconds

    // Should not have accumulated more than 100ms worth (burst cap)
    // 1MB/sec * 0.1s = 100KB max
    int sends = 0;
    while (pacer_->can_send(10000, now)) {
        sends++;
        if (sends > 100) break;  // Safety
    }

    // Should have limited bursts
    EXPECT_LE(sends, 20);  // 100KB / 10KB = 10 packets, with some margin
}

// ===========================================================================
// Integration Tests
// ===========================================================================

class CongestionControlIntegrationTest : public ::testing::Test {
protected:
    std::unique_ptr<NewRenoCongestionControl> cc_;
    std::unique_ptr<Pacer> pacer_;

    void SetUp() override {
        cc_ = std::make_unique<NewRenoCongestionControl>();
        pacer_ = std::make_unique<Pacer>();
    }

    uint64_t now_us() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        ).count();
    }
};

TEST_F(CongestionControlIntegrationTest, CombinedSendCheck) {
    // Set RTT and configure pacer
    cc_->update_rtt(50000);  // 50ms
    pacer_->set_rate(cc_->pacing_rate());

    uint64_t now = now_us();
    uint64_t packet_size = 1200;

    // Both CC and pacer must allow
    bool cc_allows = cc_->can_send(packet_size);
    bool pacer_allows = pacer_->can_send(packet_size, now);

    // Initially both should allow
    EXPECT_TRUE(cc_allows);
    EXPECT_TRUE(pacer_allows);
}

TEST_F(CongestionControlIntegrationTest, SimulateTransfer) {
    cc_->update_rtt(50000);  // 50ms RTT
    uint64_t now = now_us();
    uint64_t packet_size = 1200;

    // Simulate sending packets
    int packets_sent = 0;
    while (cc_->can_send(packet_size) && packets_sent < 100) {
        cc_->on_packet_sent(packet_size);
        packets_sent++;
    }

    // Should be limited by congestion window
    EXPECT_LE(cc_->bytes_in_flight(), cc_->congestion_window());

    // Simulate ACKs
    uint64_t acked = 0;
    while (acked < cc_->bytes_in_flight()) {
        cc_->on_packet_acked(packet_size);
        cc_->on_ack_received(packet_size, now);
        acked += packet_size;
    }

    // Window should have grown
    EXPECT_GT(cc_->congestion_window(), NewRenoCongestionControl::kInitialWindow);
}

TEST_F(CongestionControlIntegrationTest, LossRecovery) {
    uint64_t now = now_us();
    uint64_t packet_size = 1200;

    // Send some packets
    for (int i = 0; i < 10; i++) {
        cc_->on_packet_sent(packet_size);
    }

    uint64_t window_before = cc_->congestion_window();

    // Simulate loss
    cc_->on_packet_lost(packet_size);
    cc_->on_congestion_event(now);

    // Window should be reduced
    EXPECT_LT(cc_->congestion_window(), window_before);
    EXPECT_TRUE(cc_->in_recovery(now));

    // More losses during recovery shouldn't further reduce window
    cc_->on_packet_lost(packet_size);
    cc_->on_congestion_event(now + 1000);

    EXPECT_EQ(cc_->congestion_window(), cc_->ssthresh());
}

// ===========================================================================
// Performance Tests
// ===========================================================================

class CongestionControlPerformanceTest : public ::testing::Test {
protected:
    std::mt19937_64 rng_;

    void SetUp() override {
        rng_.seed(std::random_device{}());
    }
};

TEST_F(CongestionControlPerformanceTest, CanSendPerformance) {
    NewRenoCongestionControl cc;
    const int iterations = 1000000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        volatile bool result = cc.can_send(1200);
        (void)result;
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "NewReno::can_send: " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 20.0);
}

TEST_F(CongestionControlPerformanceTest, OnAckReceivedPerformance) {
    NewRenoCongestionControl cc;
    const int iterations = 1000000;
    uint64_t now = 1000000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        cc.on_ack_received(1200, now);
        now += 100;
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "NewReno::on_ack_received: " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 50.0);
}

TEST_F(CongestionControlPerformanceTest, PacerPerformance) {
    Pacer pacer;
    pacer.set_rate(10000000);  // 10MB/sec
    const int iterations = 1000000;
    uint64_t now = 1000000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        pacer.can_send(1200, now);
        now += 1;  // 1us between checks
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "Pacer::can_send: " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 50.0);
}

TEST_F(CongestionControlPerformanceTest, FullSendCyclePerformance) {
    NewRenoCongestionControl cc;
    const int iterations = 100000;
    uint64_t now = 1000000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        if (cc.can_send(1200)) {
            cc.on_packet_sent(1200);
        }
        cc.on_packet_acked(1200);
        cc.on_ack_received(1200, now);
        now += 100;
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "Full send cycle: " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 100.0);
}

}  // namespace test
}  // namespace quic
}  // namespace fasterapi

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
