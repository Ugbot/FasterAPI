/**
 * QUIC Flow Control Unit Tests
 *
 * Tests the QUIC flow control implementation (RFC 9000 Section 4):
 * - Connection-level flow control (FlowControl)
 * - Per-stream flow control (StreamFlowControl)
 * - Window management and updates
 * - Blocking detection
 * - Auto-increment behavior
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/quic/quic_flow_control.h"
#include <random>
#include <vector>

namespace fasterapi {
namespace quic {
namespace test {

// ===========================================================================
// Connection Flow Control Tests
// ===========================================================================

class FlowControlTest : public ::testing::Test {
protected:
    std::unique_ptr<FlowControl> fc_;
    std::mt19937_64 rng_;

    void SetUp() override {
        fc_ = std::make_unique<FlowControl>(1024 * 1024);  // 1MB window
        rng_.seed(std::random_device{}());
    }
};

TEST_F(FlowControlTest, InitialState) {
    EXPECT_EQ(fc_->peer_max_data(), 1024u * 1024);
    EXPECT_EQ(fc_->sent_data(), 0u);
    EXPECT_EQ(fc_->recv_data(), 0u);
    EXPECT_EQ(fc_->recv_max_data(), 1024u * 1024);
    EXPECT_FALSE(fc_->is_blocked());
    EXPECT_EQ(fc_->available_window(), 1024u * 1024);
}

TEST_F(FlowControlTest, CanSendWithinWindow) {
    EXPECT_TRUE(fc_->can_send(1000));
    EXPECT_TRUE(fc_->can_send(1024 * 1024));
    EXPECT_FALSE(fc_->can_send(1024 * 1024 + 1));
}

TEST_F(FlowControlTest, AddSentData) {
    fc_->add_sent_data(1000);
    EXPECT_EQ(fc_->sent_data(), 1000u);
    EXPECT_EQ(fc_->available_window(), 1024 * 1024 - 1000);
    EXPECT_FALSE(fc_->is_blocked());

    fc_->add_sent_data(500);
    EXPECT_EQ(fc_->sent_data(), 1500u);
}

TEST_F(FlowControlTest, CanSendUpdatesWithSentData) {
    fc_->add_sent_data(1024 * 1024 - 100);
    EXPECT_TRUE(fc_->can_send(100));
    EXPECT_FALSE(fc_->can_send(101));
}

TEST_F(FlowControlTest, BecomesBlocked) {
    fc_->add_sent_data(1024 * 1024);
    EXPECT_TRUE(fc_->is_blocked());
    EXPECT_EQ(fc_->available_window(), 0u);
    EXPECT_FALSE(fc_->can_send(1));
}

TEST_F(FlowControlTest, CanReceiveWithinWindow) {
    EXPECT_TRUE(fc_->can_receive(0, 1000));
    EXPECT_TRUE(fc_->can_receive(1000, 1000));
    EXPECT_TRUE(fc_->can_receive(0, 1024 * 1024));
    EXPECT_FALSE(fc_->can_receive(0, 1024 * 1024 + 1));
}

TEST_F(FlowControlTest, CanReceiveAtOffset) {
    // Data at offset 500000 with 100000 bytes should be allowed
    EXPECT_TRUE(fc_->can_receive(500000, 100000));
    // But offset + bytes > max should fail
    EXPECT_FALSE(fc_->can_receive(1024 * 1024 - 50, 100));
}

TEST_F(FlowControlTest, AddRecvData) {
    fc_->add_recv_data(5000);
    EXPECT_EQ(fc_->recv_data(), 5000u);
    fc_->add_recv_data(3000);
    EXPECT_EQ(fc_->recv_data(), 8000u);
}

TEST_F(FlowControlTest, UpdatePeerMaxData) {
    fc_->add_sent_data(1024 * 1024);
    EXPECT_TRUE(fc_->is_blocked());

    // Peer sends MAX_DATA frame
    fc_->update_peer_max_data(2 * 1024 * 1024);
    EXPECT_FALSE(fc_->is_blocked());
    EXPECT_EQ(fc_->peer_max_data(), 2u * 1024 * 1024);
    EXPECT_EQ(fc_->available_window(), 1024u * 1024);
}

TEST_F(FlowControlTest, UpdatePeerMaxDataIgnoresLower) {
    fc_->update_peer_max_data(2 * 1024 * 1024);
    EXPECT_EQ(fc_->peer_max_data(), 2u * 1024 * 1024);

    // Should ignore lower value
    fc_->update_peer_max_data(1024 * 1024);
    EXPECT_EQ(fc_->peer_max_data(), 2u * 1024 * 1024);
}

TEST_F(FlowControlTest, UpdateRecvMaxData) {
    fc_->update_recv_max_data(2 * 1024 * 1024);
    EXPECT_EQ(fc_->recv_max_data(), 2u * 1024 * 1024);
    EXPECT_TRUE(fc_->can_receive(0, 2 * 1024 * 1024));
}

TEST_F(FlowControlTest, AutoIncrementWindow) {
    uint64_t initial = fc_->recv_max_data();
    uint64_t new_max = fc_->auto_increment_window(50000);
    EXPECT_EQ(new_max, initial + 50000);
    EXPECT_EQ(fc_->recv_max_data(), new_max);
}

TEST_F(FlowControlTest, DefaultWindowSize) {
    FlowControl default_fc;
    EXPECT_EQ(default_fc.peer_max_data(), 1024u * 1024);  // 1MB default
}

TEST_F(FlowControlTest, SmallWindow) {
    FlowControl small_fc(1000);
    EXPECT_TRUE(small_fc.can_send(500));
    EXPECT_FALSE(small_fc.can_send(1001));
}

TEST_F(FlowControlTest, LargeWindow) {
    FlowControl large_fc(10ULL * 1024 * 1024 * 1024);  // 10GB
    EXPECT_TRUE(large_fc.can_send(1ULL * 1024 * 1024 * 1024));  // 1GB
    EXPECT_FALSE(large_fc.can_send(11ULL * 1024 * 1024 * 1024));
}

TEST_F(FlowControlTest, RandomSendReceive) {
    std::uniform_int_distribution<uint64_t> dist(1, 10000);

    uint64_t total_sent = 0;
    uint64_t total_recv = 0;

    for (int i = 0; i < 100; i++) {
        uint64_t bytes = dist(rng_);

        if (fc_->can_send(bytes)) {
            fc_->add_sent_data(bytes);
            total_sent += bytes;
        }

        if (fc_->can_receive(total_recv, bytes)) {
            fc_->add_recv_data(bytes);
            total_recv += bytes;
        }
    }

    EXPECT_EQ(fc_->sent_data(), total_sent);
    EXPECT_EQ(fc_->recv_data(), total_recv);
}

// ===========================================================================
// Stream Flow Control Tests
// ===========================================================================

class StreamFlowControlTest : public ::testing::Test {
protected:
    std::unique_ptr<StreamFlowControl> sfc_;
    std::mt19937_64 rng_;

    void SetUp() override {
        sfc_ = std::make_unique<StreamFlowControl>(256 * 1024);  // 256KB window
        rng_.seed(std::random_device{}());
    }
};

TEST_F(StreamFlowControlTest, InitialState) {
    EXPECT_EQ(sfc_->peer_max_stream_data(), 256u * 1024);
    EXPECT_EQ(sfc_->sent_offset(), 0u);
    EXPECT_EQ(sfc_->recv_offset(), 0u);
    EXPECT_EQ(sfc_->recv_max_offset(), 256u * 1024);
    EXPECT_FALSE(sfc_->is_blocked());
    EXPECT_EQ(sfc_->available_window(), 256u * 1024);
}

TEST_F(StreamFlowControlTest, CanSendWithinWindow) {
    EXPECT_TRUE(sfc_->can_send(1000));
    EXPECT_TRUE(sfc_->can_send(256 * 1024));
    EXPECT_FALSE(sfc_->can_send(256 * 1024 + 1));
}

TEST_F(StreamFlowControlTest, AddSentData) {
    sfc_->add_sent_data(1000);
    EXPECT_EQ(sfc_->sent_offset(), 1000u);
    EXPECT_EQ(sfc_->available_window(), 256 * 1024 - 1000);

    sfc_->add_sent_data(500);
    EXPECT_EQ(sfc_->sent_offset(), 1500u);
}

TEST_F(StreamFlowControlTest, BecomesBlocked) {
    sfc_->add_sent_data(256 * 1024);
    EXPECT_TRUE(sfc_->is_blocked());
    EXPECT_EQ(sfc_->available_window(), 0u);
}

TEST_F(StreamFlowControlTest, CanReceiveAtOffset) {
    EXPECT_TRUE(sfc_->can_receive(0, 1000));
    EXPECT_TRUE(sfc_->can_receive(100000, 50000));
    EXPECT_FALSE(sfc_->can_receive(256 * 1024, 1));
}

TEST_F(StreamFlowControlTest, UpdatePeerMaxStreamData) {
    sfc_->add_sent_data(256 * 1024);
    EXPECT_TRUE(sfc_->is_blocked());

    sfc_->update_peer_max_stream_data(512 * 1024);
    EXPECT_FALSE(sfc_->is_blocked());
    EXPECT_EQ(sfc_->available_window(), 256u * 1024);
}

TEST_F(StreamFlowControlTest, UpdatePeerMaxStreamDataIgnoresLower) {
    sfc_->update_peer_max_stream_data(512 * 1024);
    sfc_->update_peer_max_stream_data(256 * 1024);  // Should be ignored
    EXPECT_EQ(sfc_->peer_max_stream_data(), 512u * 1024);
}

TEST_F(StreamFlowControlTest, UpdateRecvMaxOffset) {
    sfc_->update_recv_max_offset(1024 * 1024);
    EXPECT_EQ(sfc_->recv_max_offset(), 1024u * 1024);
    EXPECT_TRUE(sfc_->can_receive(0, 1024 * 1024));
}

TEST_F(StreamFlowControlTest, AutoIncrementWindow) {
    uint64_t initial = sfc_->recv_max_offset();
    uint64_t new_max = sfc_->auto_increment_window(10000);
    EXPECT_EQ(new_max, initial + 10000);
}

TEST_F(StreamFlowControlTest, DefaultWindowSize) {
    StreamFlowControl default_sfc;
    EXPECT_EQ(default_sfc.peer_max_stream_data(), 256u * 1024);  // 256KB default
}

TEST_F(StreamFlowControlTest, MultipleStreams) {
    // Test that multiple stream controllers are independent
    StreamFlowControl stream1(100000);
    StreamFlowControl stream2(200000);

    stream1.add_sent_data(50000);
    stream2.add_sent_data(100000);

    EXPECT_EQ(stream1.sent_offset(), 50000u);
    EXPECT_EQ(stream2.sent_offset(), 100000u);

    EXPECT_EQ(stream1.available_window(), 50000u);
    EXPECT_EQ(stream2.available_window(), 100000u);
}

// ===========================================================================
// Integration Tests
// ===========================================================================

class FlowControlIntegrationTest : public ::testing::Test {
protected:
    std::unique_ptr<FlowControl> conn_fc_;
    std::vector<std::unique_ptr<StreamFlowControl>> stream_fcs_;

    void SetUp() override {
        conn_fc_ = std::make_unique<FlowControl>(1024 * 1024);
        // Create 4 streams
        for (int i = 0; i < 4; i++) {
            stream_fcs_.push_back(std::make_unique<StreamFlowControl>(256 * 1024));
        }
    }
};

TEST_F(FlowControlIntegrationTest, BothLevelsMustAllow) {
    // To send data, both connection and stream flow control must allow

    // Connection allows 1MB, each stream allows 256KB
    // Sending 300KB on stream 0 should:
    // - Fail stream check (256KB limit)
    // - Pass connection check (1MB limit)

    EXPECT_FALSE(stream_fcs_[0]->can_send(300 * 1024));
    EXPECT_TRUE(conn_fc_->can_send(300 * 1024));

    // Sending 100KB on stream 0 should pass both
    EXPECT_TRUE(stream_fcs_[0]->can_send(100 * 1024));
    EXPECT_TRUE(conn_fc_->can_send(100 * 1024));
}

TEST_F(FlowControlIntegrationTest, ConnectionBlocksAllStreams) {
    // Fill connection window
    conn_fc_->add_sent_data(1024 * 1024);
    EXPECT_TRUE(conn_fc_->is_blocked());

    // All streams should effectively be blocked (even if they have window)
    for (auto& sfc : stream_fcs_) {
        EXPECT_TRUE(sfc->can_send(100));  // Stream has window
        // But combined with connection, we can't send
        EXPECT_FALSE(conn_fc_->can_send(100));
    }
}

TEST_F(FlowControlIntegrationTest, StreamBlockDoesNotAffectOthers) {
    // Block stream 0
    stream_fcs_[0]->add_sent_data(256 * 1024);
    EXPECT_TRUE(stream_fcs_[0]->is_blocked());

    // Other streams should be fine
    EXPECT_FALSE(stream_fcs_[1]->is_blocked());
    EXPECT_FALSE(stream_fcs_[2]->is_blocked());
    EXPECT_FALSE(stream_fcs_[3]->is_blocked());
}

TEST_F(FlowControlIntegrationTest, SimulateDataTransfer) {
    const uint64_t chunk_size = 10000;
    uint64_t stream_sent[4] = {0, 0, 0, 0};
    uint64_t total_sent = 0;

    // Send data round-robin across streams
    for (int i = 0; i < 100; i++) {
        int stream_idx = i % 4;
        auto& sfc = stream_fcs_[stream_idx];

        if (conn_fc_->can_send(chunk_size) && sfc->can_send(chunk_size)) {
            conn_fc_->add_sent_data(chunk_size);
            sfc->add_sent_data(chunk_size);
            stream_sent[stream_idx] += chunk_size;
            total_sent += chunk_size;
        }
    }

    EXPECT_EQ(conn_fc_->sent_data(), total_sent);

    // Each stream should have sent 25 chunks (limited by its 256KB window)
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(stream_fcs_[i]->sent_offset(), stream_sent[i]);
    }
}

TEST_F(FlowControlIntegrationTest, WindowUpdates) {
    // Fill connection window
    conn_fc_->add_sent_data(1024 * 1024);
    EXPECT_TRUE(conn_fc_->is_blocked());

    // Simulate receiving MAX_DATA frame
    conn_fc_->update_peer_max_data(2 * 1024 * 1024);
    EXPECT_FALSE(conn_fc_->is_blocked());

    // Fill stream 0 window
    stream_fcs_[0]->add_sent_data(256 * 1024);
    EXPECT_TRUE(stream_fcs_[0]->is_blocked());

    // Simulate receiving MAX_STREAM_DATA frame
    stream_fcs_[0]->update_peer_max_stream_data(512 * 1024);
    EXPECT_FALSE(stream_fcs_[0]->is_blocked());
}

// ===========================================================================
// Performance Tests
// ===========================================================================

class FlowControlPerformanceTest : public ::testing::Test {
protected:
    std::mt19937_64 rng_;

    void SetUp() override {
        rng_.seed(std::random_device{}());
    }
};

TEST_F(FlowControlPerformanceTest, CanSendPerformance) {
    FlowControl fc(1024 * 1024);
    const int iterations = 1000000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        volatile bool result = fc.can_send(1000);
        (void)result;
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "FlowControl::can_send: " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 20.0);
}

TEST_F(FlowControlPerformanceTest, AddSentDataPerformance) {
    FlowControl fc(UINT64_MAX);  // Large window
    const int iterations = 1000000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        fc.add_sent_data(100);
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "FlowControl::add_sent_data: " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 20.0);
}

TEST_F(FlowControlPerformanceTest, StreamFlowControlPerformance) {
    StreamFlowControl sfc(UINT64_MAX);
    const int iterations = 1000000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        if (sfc.can_send(100)) {
            sfc.add_sent_data(100);
        }
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "StreamFlowControl send cycle: " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 30.0);
}

}  // namespace test
}  // namespace quic
}  // namespace fasterapi

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
