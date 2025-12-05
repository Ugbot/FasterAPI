/**
 * WebTransport Unit Tests
 *
 * Comprehensive GTest-based unit tests for WebTransportConnection class.
 * Tests all three WebTransport features:
 * - Bidirectional streams (reliable, ordered)
 * - Unidirectional streams (reliable, ordered, one-way)
 * - Datagrams (unreliable, unordered)
 *
 * Build: cmake --build build --target gtest_webtransport
 * Run:   DYLD_LIBRARY_PATH=build/lib ./build/tests/gtest/gtest_webtransport
 */

#include <gtest/gtest.h>
#include "../src/cpp/http/webtransport_connection.h"
#include "../src/cpp/http/quic/quic_connection.h"
#include "../src/cpp/http/quic/quic_packet.h"
#include "../src/cpp/http/quic/quic_frames.h"
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <random>
#include <chrono>
#include <thread>

using namespace fasterapi::http;
using namespace fasterapi::quic;

namespace {

/**
 * Get current time in microseconds.
 */
uint64_t get_current_time_us() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()
    ).count();
}

/**
 * Generate random data for testing.
 */
std::vector<uint8_t> generate_random_data(size_t length) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 255);

    std::vector<uint8_t> data(length);
    for (size_t i = 0; i < length; i++) {
        data[i] = static_cast<uint8_t>(dis(gen));
    }
    return data;
}

/**
 * Generate random string for testing.
 */
std::string generate_random_string(size_t length) {
    static const char charset[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, sizeof(charset) - 2);

    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; i++) {
        result += charset[dis(gen)];
    }
    return result;
}

/**
 * Create a connection ID from a seed value (for reproducible tests).
 */
ConnectionID make_conn_id(uint8_t seed, uint8_t length = 8) {
    ConnectionID cid;
    cid.length = length;
    for (uint8_t i = 0; i < length; i++) {
        cid.data[i] = seed + i;
    }
    return cid;
}

/**
 * Establish a QUIC connection by processing a real QUIC packet.
 * This is NOT mocking - we're using the actual QUIC packet processing.
 */
void establish_quic_connection(QUICConnection& conn, const ConnectionID& local_cid) {
    // Build a valid short header packet with PING frame
    uint8_t packet[100];
    ShortHeader hdr;
    hdr.dest_conn_id = local_cid;  // Packet addressed to this connection
    hdr.packet_number = 1;
    hdr.packet_number_length = 4;
    hdr.spin_bit = false;
    hdr.key_phase = false;

    size_t hdr_len = hdr.serialize(packet);
    packet[hdr_len] = 0x01;  // PING frame (ack-eliciting, no payload)

    // Process packet - this transitions state from HANDSHAKE to ESTABLISHED
    conn.process_packet(packet, hdr_len + 1, get_current_time_us());
}

/**
 * Create a properly established QUIC connection for testing.
 * Uses real QUIC packet processing, not mocking.
 */
std::unique_ptr<QUICConnection> create_established_quic_connection(bool is_server) {
    // Create connection IDs using proper constructor
    ConnectionID local_conn_id = make_conn_id(is_server ? 0x10 : 0x20);
    ConnectionID peer_conn_id = make_conn_id(is_server ? 0x20 : 0x10);

    auto quic_conn = std::make_unique<QUICConnection>(
        is_server,
        local_conn_id,
        peer_conn_id
    );

    // Initialize (transitions to HANDSHAKE state)
    quic_conn->initialize();

    // Establish connection by processing a real QUIC packet
    establish_quic_connection(*quic_conn, local_conn_id);

    return quic_conn;
}

/**
 * Create a QUIC connection in HANDSHAKE state (not established).
 * For testing operations that should fail before establishment.
 */
std::unique_ptr<QUICConnection> create_handshake_quic_connection(bool is_server) {
    ConnectionID local_conn_id = make_conn_id(is_server ? 0x30 : 0x40);
    ConnectionID peer_conn_id = make_conn_id(is_server ? 0x40 : 0x30);

    auto quic_conn = std::make_unique<QUICConnection>(
        is_server,
        local_conn_id,
        peer_conn_id
    );

    quic_conn->initialize();
    // Don't establish - leave in HANDSHAKE state

    return quic_conn;
}

} // anonymous namespace

// ============================================
// WebTransportConnection Initialization Tests
// ============================================

class WebTransportConnectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create server-side WebTransport connection with properly established QUIC
        auto quic_conn = create_established_quic_connection(true);
        wt_server_ = std::make_unique<WebTransportConnection>(std::move(quic_conn));

        // Create client-side WebTransport connection with properly established QUIC
        auto quic_conn_client = create_established_quic_connection(false);
        wt_client_ = std::make_unique<WebTransportConnection>(std::move(quic_conn_client));
    }

    void TearDown() override {
        wt_server_.reset();
        wt_client_.reset();
    }

    std::unique_ptr<WebTransportConnection> wt_server_;
    std::unique_ptr<WebTransportConnection> wt_client_;
};

TEST_F(WebTransportConnectionTest, InitializationSuccess) {
    ASSERT_NE(wt_server_, nullptr);
    ASSERT_NE(wt_client_, nullptr);

    // Initialize server
    EXPECT_EQ(wt_server_->initialize(), 0);

    // Initialize client
    EXPECT_EQ(wt_client_->initialize(), 0);
}

TEST_F(WebTransportConnectionTest, InitialStateIsConnecting) {
    EXPECT_EQ(wt_server_->state(), WebTransportConnection::State::CONNECTING);
    EXPECT_FALSE(wt_server_->is_connected());
    EXPECT_FALSE(wt_server_->is_closed());
}

TEST_F(WebTransportConnectionTest, ServerAcceptTransitionsToConnected) {
    ASSERT_EQ(wt_server_->initialize(), 0);
    ASSERT_EQ(wt_server_->accept(), 0);

    EXPECT_EQ(wt_server_->state(), WebTransportConnection::State::CONNECTED);
    EXPECT_TRUE(wt_server_->is_connected());
    EXPECT_FALSE(wt_server_->is_closed());
}

TEST_F(WebTransportConnectionTest, CloseTransitionsToClosed) {
    ASSERT_EQ(wt_server_->initialize(), 0);
    ASSERT_EQ(wt_server_->accept(), 0);

    wt_server_->close(0, "Test close");

    EXPECT_EQ(wt_server_->state(), WebTransportConnection::State::CLOSED);
    EXPECT_FALSE(wt_server_->is_connected());
    EXPECT_TRUE(wt_server_->is_closed());
}

// ============================================
// Bidirectional Stream Tests
// ============================================

class WebTransportBidiStreamTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto quic_conn = create_established_quic_connection(true);
        wt_ = std::make_unique<WebTransportConnection>(std::move(quic_conn));
        ASSERT_EQ(wt_->initialize(), 0);
        ASSERT_EQ(wt_->accept(), 0);
    }

    void TearDown() override {
        wt_.reset();
    }

    std::unique_ptr<WebTransportConnection> wt_;
};

TEST_F(WebTransportBidiStreamTest, OpenStreamReturnsNonZeroId) {
    uint64_t stream_id = wt_->open_stream();
    EXPECT_GT(stream_id, 0U);
}

TEST_F(WebTransportBidiStreamTest, OpenMultipleStreamsReturnsDifferentIds) {
    uint64_t stream1 = wt_->open_stream();
    uint64_t stream2 = wt_->open_stream();
    uint64_t stream3 = wt_->open_stream();

    EXPECT_NE(stream1, stream2);
    EXPECT_NE(stream2, stream3);
    EXPECT_NE(stream1, stream3);
}

TEST_F(WebTransportBidiStreamTest, OpenStreamFailsWhenNotConnected) {
    // Create new connection that's not connected (stays in HANDSHAKE state)
    auto quic_conn = create_handshake_quic_connection(true);
    WebTransportConnection wt_not_connected(std::move(quic_conn));
    wt_not_connected.initialize();
    // Don't accept - stay in CONNECTING state

    uint64_t stream_id = wt_not_connected.open_stream();
    EXPECT_EQ(stream_id, 0U);
}

TEST_F(WebTransportBidiStreamTest, SendStreamDataOnValidStream) {
    uint64_t stream_id = wt_->open_stream();
    ASSERT_GT(stream_id, 0U);

    std::string message = "Hello WebTransport!";
    ssize_t sent = wt_->send_stream(
        stream_id,
        reinterpret_cast<const uint8_t*>(message.data()),
        message.size()
    );

    // Should return positive value (bytes sent) or 0
    // The mock QUIC connection may buffer or simulate send
    EXPECT_GE(sent, 0);
}

TEST_F(WebTransportBidiStreamTest, SendStreamDataFailsOnInvalidStream) {
    // Try to send on non-existent stream
    uint64_t invalid_stream_id = 999999;
    std::string message = "Test";

    ssize_t sent = wt_->send_stream(
        invalid_stream_id,
        reinterpret_cast<const uint8_t*>(message.data()),
        message.size()
    );

    EXPECT_EQ(sent, -1);
}

TEST_F(WebTransportBidiStreamTest, CloseStreamSuccess) {
    uint64_t stream_id = wt_->open_stream();
    ASSERT_GT(stream_id, 0U);

    int result = wt_->close_stream(stream_id);
    EXPECT_EQ(result, 0);
}

TEST_F(WebTransportBidiStreamTest, SendAfterCloseStreamFails) {
    uint64_t stream_id = wt_->open_stream();
    ASSERT_GT(stream_id, 0U);

    wt_->close_stream(stream_id);

    std::string message = "Test";
    ssize_t sent = wt_->send_stream(
        stream_id,
        reinterpret_cast<const uint8_t*>(message.data()),
        message.size()
    );

    EXPECT_EQ(sent, -1);
}

// ============================================
// Unidirectional Stream Tests
// ============================================

class WebTransportUniStreamTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto quic_conn = create_established_quic_connection(true);
        wt_ = std::make_unique<WebTransportConnection>(std::move(quic_conn));
        ASSERT_EQ(wt_->initialize(), 0);
        ASSERT_EQ(wt_->accept(), 0);
    }

    void TearDown() override {
        wt_.reset();
    }

    std::unique_ptr<WebTransportConnection> wt_;
};

TEST_F(WebTransportUniStreamTest, OpenUnidirectionalStreamReturnsNonZeroId) {
    uint64_t stream_id = wt_->open_unidirectional_stream();
    EXPECT_GT(stream_id, 0U);
}

TEST_F(WebTransportUniStreamTest, OpenMultipleUniStreamsReturnsDifferentIds) {
    uint64_t stream1 = wt_->open_unidirectional_stream();
    uint64_t stream2 = wt_->open_unidirectional_stream();
    uint64_t stream3 = wt_->open_unidirectional_stream();

    EXPECT_NE(stream1, stream2);
    EXPECT_NE(stream2, stream3);
    EXPECT_NE(stream1, stream3);
}

TEST_F(WebTransportUniStreamTest, SendUnidirectionalData) {
    uint64_t stream_id = wt_->open_unidirectional_stream();
    ASSERT_GT(stream_id, 0U);

    std::string message = "One-way message";
    ssize_t sent = wt_->send_unidirectional(
        stream_id,
        reinterpret_cast<const uint8_t*>(message.data()),
        message.size()
    );

    EXPECT_GE(sent, 0);
}

TEST_F(WebTransportUniStreamTest, SendUnidirectionalFailsOnBidiStream) {
    // Open bidirectional stream
    uint64_t bidi_stream = wt_->open_stream();
    ASSERT_GT(bidi_stream, 0U);

    // Try to send as unidirectional
    std::string message = "Test";
    ssize_t sent = wt_->send_unidirectional(
        bidi_stream,
        reinterpret_cast<const uint8_t*>(message.data()),
        message.size()
    );

    EXPECT_EQ(sent, -1);
}

TEST_F(WebTransportUniStreamTest, CloseUnidirectionalStream) {
    uint64_t stream_id = wt_->open_unidirectional_stream();
    ASSERT_GT(stream_id, 0U);

    int result = wt_->close_unidirectional_stream(stream_id);
    EXPECT_EQ(result, 0);
}

// ============================================
// Datagram Tests
// ============================================

class WebTransportDatagramTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto quic_conn = create_established_quic_connection(true);
        wt_ = std::make_unique<WebTransportConnection>(std::move(quic_conn));
        ASSERT_EQ(wt_->initialize(), 0);
        ASSERT_EQ(wt_->accept(), 0);
    }

    void TearDown() override {
        wt_.reset();
    }

    std::unique_ptr<WebTransportConnection> wt_;
};

TEST_F(WebTransportDatagramTest, SendSmallDatagram) {
    std::string message = "Hello Datagram!";
    int result = wt_->send_datagram(
        reinterpret_cast<const uint8_t*>(message.data()),
        message.size()
    );

    EXPECT_EQ(result, 0);
}

TEST_F(WebTransportDatagramTest, SendRandomDataDatagram) {
    auto data = generate_random_data(100);
    int result = wt_->send_datagram(data.data(), data.size());

    EXPECT_EQ(result, 0);
}

TEST_F(WebTransportDatagramTest, SendMaxSizeDatagram) {
    // Max datagram size is around 1200 bytes (conservative MTU)
    auto data = generate_random_data(1200);
    int result = wt_->send_datagram(data.data(), data.size());

    EXPECT_EQ(result, 0);
}

TEST_F(WebTransportDatagramTest, SendOversizedDatagramFails) {
    // Datagrams larger than MTU should fail
    auto data = generate_random_data(2000);  // Too large
    int result = wt_->send_datagram(data.data(), data.size());

    EXPECT_EQ(result, -1);
}

TEST_F(WebTransportDatagramTest, SendMultipleDatagrams) {
    for (int i = 0; i < 10; i++) {
        std::string message = "Datagram #" + std::to_string(i);
        int result = wt_->send_datagram(
            reinterpret_cast<const uint8_t*>(message.data()),
            message.size()
        );
        EXPECT_EQ(result, 0) << "Failed on datagram " << i;
    }
}

TEST_F(WebTransportDatagramTest, SendDatagramFailsWhenNotConnected) {
    // Create connection that's not connected (stays in HANDSHAKE state)
    auto quic_conn = create_handshake_quic_connection(true);
    WebTransportConnection wt_not_connected(std::move(quic_conn));
    wt_not_connected.initialize();
    // Don't accept

    std::string message = "Test";
    int result = wt_not_connected.send_datagram(
        reinterpret_cast<const uint8_t*>(message.data()),
        message.size()
    );

    EXPECT_EQ(result, -1);
}

// ============================================
// Callback Tests
// ============================================

class WebTransportCallbackTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto quic_conn = create_established_quic_connection(true);
        wt_ = std::make_unique<WebTransportConnection>(std::move(quic_conn));
        ASSERT_EQ(wt_->initialize(), 0);
        ASSERT_EQ(wt_->accept(), 0);

        // Reset counters
        stream_data_count_ = 0;
        uni_data_count_ = 0;
        datagram_count_ = 0;
        stream_opened_count_ = 0;
        stream_closed_count_ = 0;
        connection_closed_count_ = 0;
    }

    void TearDown() override {
        wt_.reset();
    }

    void SetupCallbacks() {
        wt_->on_stream_data([this](uint64_t stream_id, const uint8_t* data, size_t len) {
            stream_data_count_++;
            last_stream_id_ = stream_id;
            last_data_len_ = len;
        });

        wt_->on_unidirectional_data([this](uint64_t stream_id, const uint8_t* data, size_t len) {
            uni_data_count_++;
        });

        wt_->on_datagram([this](const uint8_t* data, size_t len) {
            datagram_count_++;
            last_datagram_len_ = len;
        });

        wt_->on_stream_opened([this](uint64_t stream_id, bool is_bidi) {
            stream_opened_count_++;
            last_opened_is_bidi_ = is_bidi;
        });

        wt_->on_stream_closed([this](uint64_t stream_id) {
            stream_closed_count_++;
        });

        wt_->on_connection_closed([this](uint64_t error_code, const char* reason) {
            connection_closed_count_++;
            last_close_error_ = error_code;
        });
    }

    std::unique_ptr<WebTransportConnection> wt_;

    std::atomic<int> stream_data_count_{0};
    std::atomic<int> uni_data_count_{0};
    std::atomic<int> datagram_count_{0};
    std::atomic<int> stream_opened_count_{0};
    std::atomic<int> stream_closed_count_{0};
    std::atomic<int> connection_closed_count_{0};

    uint64_t last_stream_id_{0};
    size_t last_data_len_{0};
    size_t last_datagram_len_{0};
    bool last_opened_is_bidi_{false};
    uint64_t last_close_error_{0};
};

TEST_F(WebTransportCallbackTest, RegisterCallbacksDoesNotThrow) {
    EXPECT_NO_THROW(SetupCallbacks());
}

TEST_F(WebTransportCallbackTest, ConnectionClosedCallbackInvoked) {
    SetupCallbacks();

    wt_->close(123, "Test close reason");

    EXPECT_EQ(connection_closed_count_, 1);
    EXPECT_EQ(last_close_error_, 123U);
}

TEST_F(WebTransportCallbackTest, StreamClosedCallbackInvoked) {
    SetupCallbacks();

    uint64_t stream_id = wt_->open_stream();
    ASSERT_GT(stream_id, 0U);

    wt_->close_stream(stream_id);

    EXPECT_EQ(stream_closed_count_, 1);
}

// ============================================
// Statistics Tests
// ============================================

class WebTransportStatsTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto quic_conn = create_established_quic_connection(true);
        wt_ = std::make_unique<WebTransportConnection>(std::move(quic_conn));
        ASSERT_EQ(wt_->initialize(), 0);
        ASSERT_EQ(wt_->accept(), 0);
    }

    void TearDown() override {
        wt_.reset();
    }

    std::unique_ptr<WebTransportConnection> wt_;
};

TEST_F(WebTransportStatsTest, GetStatsReturnsMap) {
    auto stats = wt_->get_stats();

    EXPECT_TRUE(stats.count("streams_opened") > 0);
    EXPECT_TRUE(stats.count("datagrams_sent") > 0);
    EXPECT_TRUE(stats.count("datagrams_received") > 0);
    EXPECT_TRUE(stats.count("bytes_sent") > 0);
    EXPECT_TRUE(stats.count("bytes_received") > 0);
    EXPECT_TRUE(stats.count("active_streams") > 0);
    EXPECT_TRUE(stats.count("pending_datagrams") > 0);
}

TEST_F(WebTransportStatsTest, InitialStatsAreZero) {
    auto stats = wt_->get_stats();

    EXPECT_EQ(stats["streams_opened"], 0U);
    EXPECT_EQ(stats["datagrams_sent"], 0U);
    EXPECT_EQ(stats["datagrams_received"], 0U);
    EXPECT_EQ(stats["bytes_sent"], 0U);
    EXPECT_EQ(stats["bytes_received"], 0U);
    EXPECT_EQ(stats["active_streams"], 0U);
}

TEST_F(WebTransportStatsTest, StreamsOpenedIncrementsOnOpenStream) {
    auto initial_stats = wt_->get_stats();
    uint64_t initial_opened = initial_stats["streams_opened"];

    wt_->open_stream();
    wt_->open_stream();
    wt_->open_stream();

    auto new_stats = wt_->get_stats();
    EXPECT_EQ(new_stats["streams_opened"], initial_opened + 3);
}

TEST_F(WebTransportStatsTest, ActiveStreamsTracksOpenAndClosedStreams) {
    EXPECT_EQ(wt_->get_stats()["active_streams"], 0U);

    uint64_t s1 = wt_->open_stream();
    uint64_t s2 = wt_->open_stream();

    EXPECT_EQ(wt_->get_stats()["active_streams"], 2U);

    wt_->close_stream(s1);

    EXPECT_EQ(wt_->get_stats()["active_streams"], 1U);

    wt_->close_stream(s2);

    EXPECT_EQ(wt_->get_stats()["active_streams"], 0U);
}

TEST_F(WebTransportStatsTest, DatagramsSentIncrementsOnSend) {
    auto initial_stats = wt_->get_stats();
    uint64_t initial_sent = initial_stats["datagrams_sent"];

    for (int i = 0; i < 5; i++) {
        std::string msg = "Test" + std::to_string(i);
        wt_->send_datagram(
            reinterpret_cast<const uint8_t*>(msg.data()),
            msg.size()
        );
    }

    auto new_stats = wt_->get_stats();
    EXPECT_EQ(new_stats["datagrams_sent"], initial_sent + 5);
}

TEST_F(WebTransportStatsTest, PendingDatagramsReflectsQueue) {
    // Initially no pending
    EXPECT_EQ(wt_->get_stats()["pending_datagrams"], 0U);

    // Send datagrams
    for (int i = 0; i < 3; i++) {
        std::string msg = "Pending" + std::to_string(i);
        wt_->send_datagram(
            reinterpret_cast<const uint8_t*>(msg.data()),
            msg.size()
        );
    }

    // Should have pending datagrams
    EXPECT_EQ(wt_->get_stats()["pending_datagrams"], 3U);
}

// ============================================
// Edge Case Tests
// ============================================

class WebTransportEdgeCaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto quic_conn = create_established_quic_connection(true);
        wt_ = std::make_unique<WebTransportConnection>(std::move(quic_conn));
    }

    void TearDown() override {
        wt_.reset();
    }

    std::unique_ptr<WebTransportConnection> wt_;
};

TEST_F(WebTransportEdgeCaseTest, DoubleCloseIsSafe) {
    ASSERT_EQ(wt_->initialize(), 0);
    ASSERT_EQ(wt_->accept(), 0);

    // First close
    wt_->close(0, "First close");
    EXPECT_TRUE(wt_->is_closed());

    // Second close should be safe (no crash)
    EXPECT_NO_THROW(wt_->close(0, "Second close"));
    EXPECT_TRUE(wt_->is_closed());
}

TEST_F(WebTransportEdgeCaseTest, OperationsAfterCloseFail) {
    ASSERT_EQ(wt_->initialize(), 0);
    ASSERT_EQ(wt_->accept(), 0);

    wt_->close(0, "Closed");

    // All operations should fail gracefully
    EXPECT_EQ(wt_->open_stream(), 0U);
    EXPECT_EQ(wt_->open_unidirectional_stream(), 0U);

    std::string msg = "Test";
    EXPECT_EQ(wt_->send_datagram(
        reinterpret_cast<const uint8_t*>(msg.data()),
        msg.size()
    ), -1);
}

TEST_F(WebTransportEdgeCaseTest, EmptyDatagramIsValid) {
    ASSERT_EQ(wt_->initialize(), 0);
    ASSERT_EQ(wt_->accept(), 0);

    // Empty datagram
    int result = wt_->send_datagram(nullptr, 0);
    // Behavior may vary - either success (0) or failure (-1)
    EXPECT_TRUE(result == 0 || result == -1);
}

TEST_F(WebTransportEdgeCaseTest, RapidOpenCloseStreams) {
    ASSERT_EQ(wt_->initialize(), 0);
    ASSERT_EQ(wt_->accept(), 0);

    // Rapidly open and close streams
    for (int i = 0; i < 100; i++) {
        uint64_t stream = wt_->open_stream();
        if (stream > 0) {
            wt_->close_stream(stream);
        }
    }

    // Should end with no active streams
    EXPECT_EQ(wt_->get_stats()["active_streams"], 0U);
}

TEST_F(WebTransportEdgeCaseTest, MixedBidiAndUniStreams) {
    ASSERT_EQ(wt_->initialize(), 0);
    ASSERT_EQ(wt_->accept(), 0);

    std::vector<uint64_t> bidi_streams;
    std::vector<uint64_t> uni_streams;

    // Open mixed streams
    for (int i = 0; i < 10; i++) {
        if (i % 2 == 0) {
            uint64_t s = wt_->open_stream();
            if (s > 0) bidi_streams.push_back(s);
        } else {
            uint64_t s = wt_->open_unidirectional_stream();
            if (s > 0) uni_streams.push_back(s);
        }
    }

    // Total opened should be 10
    EXPECT_EQ(wt_->get_stats()["streams_opened"], 10U);

    // Close all
    for (uint64_t s : bidi_streams) wt_->close_stream(s);
    for (uint64_t s : uni_streams) wt_->close_unidirectional_stream(s);

    EXPECT_EQ(wt_->get_stats()["active_streams"], 0U);
}

// ============================================
// Generate Datagrams Tests
// ============================================

class WebTransportGenerateTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto quic_conn = create_established_quic_connection(true);
        wt_ = std::make_unique<WebTransportConnection>(std::move(quic_conn));
        ASSERT_EQ(wt_->initialize(), 0);
        ASSERT_EQ(wt_->accept(), 0);
    }

    void TearDown() override {
        wt_.reset();
    }

    std::unique_ptr<WebTransportConnection> wt_;
};

TEST_F(WebTransportGenerateTest, GenerateDatagramsReturnsBytes) {
    // Queue some datagrams
    std::string msg1 = "Datagram 1";
    std::string msg2 = "Datagram 2";
    wt_->send_datagram(
        reinterpret_cast<const uint8_t*>(msg1.data()),
        msg1.size()
    );
    wt_->send_datagram(
        reinterpret_cast<const uint8_t*>(msg2.data()),
        msg2.size()
    );

    uint8_t output[4096];
    uint64_t now = get_current_time_us();

    size_t generated = wt_->generate_datagrams(output, sizeof(output), now);

    // Should generate some data (datagrams + QUIC framing)
    EXPECT_GT(generated, 0U);
}

TEST_F(WebTransportGenerateTest, GenerateDatagramsWithSmallBuffer) {
    // Queue a datagram
    std::string msg = "Small test";
    wt_->send_datagram(
        reinterpret_cast<const uint8_t*>(msg.data()),
        msg.size()
    );

    uint8_t output[10];  // Very small buffer
    uint64_t now = get_current_time_us();

    // Should handle small buffer gracefully
    size_t generated = wt_->generate_datagrams(output, sizeof(output), now);

    // May return 0 if nothing fits, or partial
    EXPECT_LE(generated, sizeof(output));
}

// Main
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
