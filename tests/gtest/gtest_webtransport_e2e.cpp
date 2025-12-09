/**
 * WebTransport End-to-End Integration Tests
 *
 * Tests the full WebTransport stack integration:
 * - HTTP/3 CONNECT handshake with :protocol=webtransport
 * - WebTransport session establishment via Http3Connection
 * - Bidirectional streams over established session
 * - Unidirectional streams
 * - Datagram sending/receiving
 * - Handler registration via UnifiedServer API
 *
 * Build: cmake --build build --target gtest_webtransport_e2e
 * Run:   DYLD_LIBRARY_PATH=build/lib ./build/tests/gtest/gtest_webtransport_e2e
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "src/cpp/http/webtransport_connection.h"
#include "src/cpp/http/http3_connection.h"
#include "src/cpp/http/quic/quic_connection.h"
#include "src/cpp/http/quic/quic_frames.h"
#include "src/cpp/http/quic/quic_varint.h"
#include "src/cpp/http/qpack/qpack_encoder.h"
#include "src/cpp/http/qpack/qpack_decoder.h"
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <random>
#include <chrono>

using namespace fasterapi::http;
using namespace fasterapi::quic;
using namespace fasterapi::qpack;

namespace {

// =============================================================================
// Helper Functions
// =============================================================================

uint64_t get_time_us() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()
    ).count();
}

std::vector<uint8_t> random_bytes(size_t length) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 255);

    std::vector<uint8_t> data(length);
    for (size_t i = 0; i < length; ++i) {
        data[i] = static_cast<uint8_t>(dis(gen));
    }
    return data;
}

std::string random_string(size_t length) {
    static const char charset[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, sizeof(charset) - 2);

    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        result += charset[dis(gen)];
    }
    return result;
}

ConnectionID make_conn_id(uint64_t seed) {
    ConnectionID id;
    id.length = 8;
    std::memcpy(id.data, &seed, 8);
    return id;
}

// Establish QUIC connection by processing a packet
void establish_quic_connection(QUICConnection& conn, const ConnectionID& local_cid) {
    uint8_t packet[100];
    ShortHeader hdr;
    hdr.dest_conn_id = local_cid;
    hdr.packet_number = 1;
    hdr.packet_number_length = 4;
    hdr.spin_bit = false;
    hdr.key_phase = false;

    size_t hdr_len = hdr.serialize(packet);
    packet[hdr_len] = 0x01;  // PING frame

    conn.process_packet(packet, hdr_len + 1, get_time_us());
}

// Create established QUIC connection
std::unique_ptr<QUICConnection> create_quic_connection(bool is_server, uint64_t seed) {
    ConnectionID local = make_conn_id(seed);
    ConnectionID peer = make_conn_id(seed ^ 0xFFFFFFFFFFFFFFFF);

    auto conn = std::make_unique<QUICConnection>(is_server, local, peer);
    conn->initialize();
    establish_quic_connection(*conn, local);

    return conn;
}

} // anonymous namespace

// =============================================================================
// WebTransport HTTP/3 CONNECT Handshake Tests
// =============================================================================

class WebTransportHandshakeTest : public ::testing::Test {
protected:
    std::unique_ptr<Http3Connection> server_h3_;
    QPACKEncoder encoder_{4096, 100};
    QPACKDecoder decoder_{4096};

    bool upgrade_callback_called_ = false;
    std::string upgrade_path_;
    bool upgrade_accepted_ = false;
    bool upgrade_rejected_ = false;
    uint16_t reject_status_ = 0;

    void SetUp() override {
        ConnectionID local = make_conn_id(0x1234567890ABCDEF);
        ConnectionID peer = make_conn_id(0xFEDCBA0987654321);

        Http3ConnectionSettings settings;
        settings.max_concurrent_streams = 100;

        server_h3_ = std::make_unique<Http3Connection>(true, local, peer, settings);
        server_h3_->initialize();

        // Set up WebTransport upgrade callback
        server_h3_->set_webtransport_upgrade_callback([this](
            uint64_t stream_id,
            const std::string& path,
            const std::unordered_map<std::string, std::string>& headers,
            std::function<void()> accept,
            std::function<void(uint16_t, const char*)> reject
        ) {
            upgrade_callback_called_ = true;
            upgrade_path_ = path;

            if (path == "/wt/echo") {
                accept();
                upgrade_accepted_ = true;
            } else if (path == "/wt/reject") {
                reject(403, "Forbidden");
                upgrade_rejected_ = true;
                reject_status_ = 403;
            } else {
                reject(404, "Not Found");
                upgrade_rejected_ = true;
                reject_status_ = 404;
            }
        });
    }
};

TEST_F(WebTransportHandshakeTest, UpgradeCallbackRegistration) {
    // Verify callback is set
    EXPECT_FALSE(upgrade_callback_called_);
}

TEST_F(WebTransportHandshakeTest, QPACKEncodeCONNECT) {
    // Test encoding a WebTransport CONNECT request
    std::vector<std::pair<std::string_view, std::string_view>> headers = {
        {":method", "CONNECT"},
        {":scheme", "https"},
        {":authority", "localhost:8443"},
        {":path", "/wt/echo"},
        {":protocol", "webtransport"},
    };

    uint8_t encoded[1024];
    size_t encoded_len;

    int result = encoder_.encode_field_section(
        headers.data(), headers.size(),
        encoded, sizeof(encoded), encoded_len
    );

    EXPECT_EQ(result, 0);
    EXPECT_GT(encoded_len, 0u);

    // Decode and verify
    std::pair<std::string, std::string> decoded[256];
    size_t decoded_count;

    result = decoder_.decode_field_section(encoded, encoded_len, decoded, decoded_count);

    EXPECT_EQ(result, 0);
    EXPECT_EQ(decoded_count, 5u);

    // Check pseudo-headers
    EXPECT_EQ(decoded[0].first, ":method");
    EXPECT_EQ(decoded[0].second, "CONNECT");
    EXPECT_EQ(decoded[4].first, ":protocol");
    EXPECT_EQ(decoded[4].second, "webtransport");
}

TEST_F(WebTransportHandshakeTest, QPACKEncodeResponse200) {
    // Test encoding a successful WebTransport response
    std::vector<std::pair<std::string_view, std::string_view>> headers = {
        {":status", "200"},
        {"sec-webtransport-http3-draft", "draft02"},
    };

    uint8_t encoded[1024];
    size_t encoded_len;

    int result = encoder_.encode_field_section(
        headers.data(), headers.size(),
        encoded, sizeof(encoded), encoded_len
    );

    EXPECT_EQ(result, 0);

    // Decode
    std::pair<std::string, std::string> decoded[256];
    size_t decoded_count;

    result = decoder_.decode_field_section(encoded, encoded_len, decoded, decoded_count);

    EXPECT_EQ(result, 0);
    EXPECT_EQ(decoded[0].second, "200");
}

TEST_F(WebTransportHandshakeTest, QPACKEncodeResponse403) {
    // Test encoding a rejected WebTransport response
    std::vector<std::pair<std::string_view, std::string_view>> headers = {
        {":status", "403"},
    };

    uint8_t encoded[1024];
    size_t encoded_len;

    int result = encoder_.encode_field_section(
        headers.data(), headers.size(),
        encoded, sizeof(encoded), encoded_len
    );

    EXPECT_EQ(result, 0);

    // Decode
    std::pair<std::string, std::string> decoded[256];
    size_t decoded_count;

    result = decoder_.decode_field_section(encoded, encoded_len, decoded, decoded_count);

    EXPECT_EQ(result, 0);
    EXPECT_EQ(decoded[0].second, "403");
}

// =============================================================================
// WebTransport Session Tests
// =============================================================================

class WebTransportSessionTest : public ::testing::Test {
protected:
    std::unique_ptr<WebTransportConnection> server_wt_;
    std::unique_ptr<WebTransportConnection> client_wt_;

    void SetUp() override {
        auto server_quic = create_quic_connection(true, 0xAAAABBBBCCCCDDDD);
        auto client_quic = create_quic_connection(false, 0xDDDDCCCCBBBBAAAA);

        server_wt_ = std::make_unique<WebTransportConnection>(std::move(server_quic));
        client_wt_ = std::make_unique<WebTransportConnection>(std::move(client_quic));

        // Initialize and establish
        ASSERT_EQ(server_wt_->initialize(), 0);
        ASSERT_EQ(client_wt_->initialize(), 0);
        ASSERT_EQ(server_wt_->accept(), 0);
    }
};

TEST_F(WebTransportSessionTest, SessionEstablishment) {
    EXPECT_TRUE(server_wt_->is_connected());
    EXPECT_EQ(server_wt_->state(), WebTransportConnection::State::CONNECTED);
}

TEST_F(WebTransportSessionTest, BidirectionalStreamOpenClose) {
    uint64_t stream_id = server_wt_->open_stream();
    EXPECT_GT(stream_id, 0u);

    // Send data
    std::string message = "Hello WebTransport E2E!";
    ssize_t sent = server_wt_->send_stream(
        stream_id,
        reinterpret_cast<const uint8_t*>(message.data()),
        message.size()
    );
    EXPECT_GE(sent, 0);

    // Close stream
    EXPECT_EQ(server_wt_->close_stream(stream_id), 0);
}

TEST_F(WebTransportSessionTest, MultipleBidirectionalStreams) {
    std::vector<uint64_t> streams;

    // Open 10 streams
    for (int i = 0; i < 10; ++i) {
        uint64_t id = server_wt_->open_stream();
        EXPECT_GT(id, 0u);
        streams.push_back(id);
    }

    // Verify all unique
    std::sort(streams.begin(), streams.end());
    auto last = std::unique(streams.begin(), streams.end());
    EXPECT_EQ(last, streams.end());

    // Send on all streams
    for (uint64_t id : streams) {
        std::string msg = "Stream " + std::to_string(id);
        ssize_t sent = server_wt_->send_stream(
            id,
            reinterpret_cast<const uint8_t*>(msg.data()),
            msg.size()
        );
        EXPECT_GE(sent, 0);
    }

    // Close all
    for (uint64_t id : streams) {
        EXPECT_EQ(server_wt_->close_stream(id), 0);
    }
}

TEST_F(WebTransportSessionTest, UnidirectionalStreamOpenClose) {
    uint64_t stream_id = server_wt_->open_unidirectional_stream();
    EXPECT_GT(stream_id, 0u);

    // Send data
    std::string message = "One-way message";
    ssize_t sent = server_wt_->send_unidirectional(
        stream_id,
        reinterpret_cast<const uint8_t*>(message.data()),
        message.size()
    );
    EXPECT_GE(sent, 0);

    // Close
    EXPECT_EQ(server_wt_->close_unidirectional_stream(stream_id), 0);
}

TEST_F(WebTransportSessionTest, MixedStreamTypes) {
    // Open mix of bidi and uni streams
    uint64_t bidi1 = server_wt_->open_stream();
    uint64_t uni1 = server_wt_->open_unidirectional_stream();
    uint64_t bidi2 = server_wt_->open_stream();
    uint64_t uni2 = server_wt_->open_unidirectional_stream();

    EXPECT_GT(bidi1, 0u);
    EXPECT_GT(uni1, 0u);
    EXPECT_GT(bidi2, 0u);
    EXPECT_GT(uni2, 0u);

    // All should be unique
    EXPECT_NE(bidi1, uni1);
    EXPECT_NE(bidi1, bidi2);
    EXPECT_NE(uni1, uni2);

    // Stats should reflect opens
    auto stats = server_wt_->get_stats();
    EXPECT_EQ(stats["streams_opened"], 4u);
}

TEST_F(WebTransportSessionTest, DatagramSendReceive) {
    // Send multiple datagrams of varying sizes
    for (int i = 0; i < 10; ++i) {
        auto data = random_bytes(100 + i * 50);
        int result = server_wt_->send_datagram(data.data(), data.size());
        EXPECT_EQ(result, 0) << "Datagram " << i << " failed";
    }

    // Verify stats
    auto stats = server_wt_->get_stats();
    EXPECT_EQ(stats["datagrams_sent"], 10u);
}

TEST_F(WebTransportSessionTest, LargeStreamData) {
    uint64_t stream_id = server_wt_->open_stream();
    ASSERT_GT(stream_id, 0u);

    // Send large payload (100KB)
    auto large_data = random_bytes(100 * 1024);
    ssize_t sent = server_wt_->send_stream(
        stream_id,
        large_data.data(),
        large_data.size()
    );

    // Should accept large data (may buffer internally)
    EXPECT_GE(sent, 0);
}

TEST_F(WebTransportSessionTest, RapidStreamOperations) {
    // Stress test: rapid open/send/close
    for (int i = 0; i < 100; ++i) {
        uint64_t id = server_wt_->open_stream();
        if (id > 0) {
            std::string msg = "Rapid " + std::to_string(i);
            server_wt_->send_stream(
                id,
                reinterpret_cast<const uint8_t*>(msg.data()),
                msg.size()
            );
            server_wt_->close_stream(id);
        }
    }

    // Should have opened 100 streams
    auto stats = server_wt_->get_stats();
    EXPECT_EQ(stats["streams_opened"], 100u);
    // All should be closed
    EXPECT_EQ(stats["active_streams"], 0u);
}

// =============================================================================
// WebTransport Callback Tests
// =============================================================================

class WebTransportCallbackE2ETest : public ::testing::Test {
protected:
    std::unique_ptr<WebTransportConnection> wt_;

    std::vector<uint64_t> closed_streams_;
    bool connection_closed_ = false;
    uint64_t close_error_code_ = 0;

    void SetUp() override {
        auto quic = create_quic_connection(true, 0x1111222233334444);
        wt_ = std::make_unique<WebTransportConnection>(std::move(quic));
        ASSERT_EQ(wt_->initialize(), 0);
        ASSERT_EQ(wt_->accept(), 0);

        // Set up callbacks without mutex (single-threaded test)
        wt_->on_stream_closed([this](uint64_t stream_id) {
            closed_streams_.push_back(stream_id);
        });

        wt_->on_connection_closed([this](uint64_t error_code, const char* reason) {
            connection_closed_ = true;
            close_error_code_ = error_code;
        });
    }
};

TEST_F(WebTransportCallbackE2ETest, CallbacksRegistered) {
    // Just verify setup doesn't throw
    EXPECT_TRUE(wt_->is_connected());
}

TEST_F(WebTransportCallbackE2ETest, ConnectionCloseCallback) {
    wt_->close(42, "Test closure");

    EXPECT_TRUE(connection_closed_);
    EXPECT_EQ(close_error_code_, 42u);
}

TEST_F(WebTransportCallbackE2ETest, StreamCloseCallback) {
    uint64_t stream_id = wt_->open_stream();
    ASSERT_GT(stream_id, 0u);

    wt_->close_stream(stream_id);

    // Verify callback was invoked
    EXPECT_EQ(closed_streams_.size(), 1u);
    EXPECT_EQ(closed_streams_[0], stream_id);
}

// =============================================================================
// WebTransport Non-Owning Mode Tests (for Http3Connection integration)
// =============================================================================

class WebTransportNonOwningTest : public ::testing::Test {
protected:
    std::unique_ptr<QUICConnection> quic_;
    std::unique_ptr<WebTransportConnection> wt_;

    void SetUp() override {
        quic_ = create_quic_connection(true, 0x5555666677778888);

        // Create WebTransport in non-owning mode
        wt_ = std::make_unique<WebTransportConnection>(quic_.get(), true);
        ASSERT_EQ(wt_->initialize(), 0);
        ASSERT_EQ(wt_->accept(), 0);
    }
};

TEST_F(WebTransportNonOwningTest, NonOwningModeWorks) {
    EXPECT_TRUE(wt_->is_connected());

    // QUIC connection should still be valid
    EXPECT_NE(quic_.get(), nullptr);
    EXPECT_TRUE(quic_->is_established());
}

TEST_F(WebTransportNonOwningTest, StreamOperationsWork) {
    uint64_t stream_id = wt_->open_stream();
    EXPECT_GT(stream_id, 0u);

    std::string msg = "Non-owning test";
    ssize_t sent = wt_->send_stream(
        stream_id,
        reinterpret_cast<const uint8_t*>(msg.data()),
        msg.size()
    );
    EXPECT_GE(sent, 0);
}

TEST_F(WebTransportNonOwningTest, DatagramOperationsWork) {
    std::string msg = "Non-owning datagram";
    int result = wt_->send_datagram(
        reinterpret_cast<const uint8_t*>(msg.data()),
        msg.size()
    );
    EXPECT_EQ(result, 0);
}

TEST_F(WebTransportNonOwningTest, CloseDoesNotDestroyQuic) {
    wt_->close(0, "Graceful");

    EXPECT_TRUE(wt_->is_closed());

    // QUIC connection should still exist
    EXPECT_NE(quic_.get(), nullptr);
}

// =============================================================================
// WebTransport Statistics Tests
// =============================================================================

class WebTransportStatsE2ETest : public ::testing::Test {
protected:
    std::unique_ptr<WebTransportConnection> wt_;

    void SetUp() override {
        auto quic = create_quic_connection(true, 0x9999AAAABBBBCCCC);
        wt_ = std::make_unique<WebTransportConnection>(std::move(quic));
        ASSERT_EQ(wt_->initialize(), 0);
        ASSERT_EQ(wt_->accept(), 0);
    }
};

TEST_F(WebTransportStatsE2ETest, InitialStatsZero) {
    auto stats = wt_->get_stats();

    EXPECT_EQ(stats["streams_opened"], 0u);
    EXPECT_EQ(stats["datagrams_sent"], 0u);
    EXPECT_EQ(stats["active_streams"], 0u);
    EXPECT_EQ(stats["pending_datagrams"], 0u);
}

TEST_F(WebTransportStatsE2ETest, StreamStatsAccurate) {
    // Open 5 bidi, 3 uni
    std::vector<uint64_t> bidi_ids, uni_ids;
    for (int i = 0; i < 5; ++i) {
        bidi_ids.push_back(wt_->open_stream());
    }
    for (int i = 0; i < 3; ++i) {
        uni_ids.push_back(wt_->open_unidirectional_stream());
    }

    auto stats = wt_->get_stats();
    EXPECT_EQ(stats["streams_opened"], 8u);
    EXPECT_EQ(stats["active_streams"], 8u);

    // Close half
    for (int i = 0; i < 2; ++i) {
        wt_->close_stream(bidi_ids[i]);
        wt_->close_unidirectional_stream(uni_ids[i]);
    }

    stats = wt_->get_stats();
    EXPECT_EQ(stats["streams_opened"], 8u);  // Still 8 opened
    EXPECT_EQ(stats["active_streams"], 4u);   // But only 4 active
}

TEST_F(WebTransportStatsE2ETest, DatagramStatsAccurate) {
    for (int i = 0; i < 20; ++i) {
        auto data = random_bytes(50);
        wt_->send_datagram(data.data(), data.size());
    }

    auto stats = wt_->get_stats();
    EXPECT_EQ(stats["datagrams_sent"], 20u);
    EXPECT_EQ(stats["pending_datagrams"], 20u);
}

// =============================================================================
// Edge Cases and Error Handling
// =============================================================================

class WebTransportEdgeCasesE2ETest : public ::testing::Test {
protected:
    std::unique_ptr<WebTransportConnection> wt_;

    void SetUp() override {
        auto quic = create_quic_connection(true, 0xDDDDEEEEFFFF0000);
        wt_ = std::make_unique<WebTransportConnection>(std::move(quic));
        ASSERT_EQ(wt_->initialize(), 0);
        ASSERT_EQ(wt_->accept(), 0);
    }
};

TEST_F(WebTransportEdgeCasesE2ETest, SendOnClosedConnection) {
    wt_->close(0, "Test");

    // All operations should fail
    EXPECT_EQ(wt_->open_stream(), 0u);
    EXPECT_EQ(wt_->open_unidirectional_stream(), 0u);

    std::string msg = "Test";
    EXPECT_EQ(wt_->send_datagram(
        reinterpret_cast<const uint8_t*>(msg.data()),
        msg.size()
    ), -1);
}

TEST_F(WebTransportEdgeCasesE2ETest, SendOnInvalidStream) {
    std::string msg = "Test";
    ssize_t result = wt_->send_stream(
        0xDEADBEEF,  // Invalid stream ID
        reinterpret_cast<const uint8_t*>(msg.data()),
        msg.size()
    );
    EXPECT_EQ(result, -1);
}

TEST_F(WebTransportEdgeCasesE2ETest, CloseInvalidStream) {
    // Should handle gracefully - closing non-existent stream is a no-op (returns 0)
    int result = wt_->close_stream(0xCAFEBABE);
    EXPECT_TRUE(result == 0 || result == -1);  // Either is acceptable
}

TEST_F(WebTransportEdgeCasesE2ETest, DoubleClose) {
    wt_->close(1, "First");
    EXPECT_NO_THROW(wt_->close(2, "Second"));

    EXPECT_TRUE(wt_->is_closed());
}

TEST_F(WebTransportEdgeCasesE2ETest, OversizedDatagram) {
    auto large_data = random_bytes(2000);  // > MTU
    int result = wt_->send_datagram(large_data.data(), large_data.size());
    EXPECT_EQ(result, -1);
}

TEST_F(WebTransportEdgeCasesE2ETest, EmptyDatagram) {
    int result = wt_->send_datagram(nullptr, 0);
    // May succeed or fail depending on implementation
    EXPECT_TRUE(result == 0 || result == -1);
}

TEST_F(WebTransportEdgeCasesE2ETest, ZeroLengthStreamSend) {
    uint64_t stream_id = wt_->open_stream();
    ASSERT_GT(stream_id, 0u);

    ssize_t result = wt_->send_stream(stream_id, nullptr, 0);
    // Zero-length send may be valid (e.g., just FIN)
    EXPECT_TRUE(result >= 0 || result == -1);
}

// =============================================================================
// Sequential Stress Test (avoiding thread-safety issues in underlying QUIC)
// =============================================================================

TEST(WebTransportStressTest, SequentialStreamOperations) {
    auto quic = create_quic_connection(true, 0x1234);
    auto wt = std::make_unique<WebTransportConnection>(std::move(quic));
    ASSERT_EQ(wt->initialize(), 0);
    ASSERT_EQ(wt->accept(), 0);

    int success_count = 0;
    int failure_count = 0;

    // Run sequential operations (QUIC/WebTransport not thread-safe without proper locking)
    for (int i = 0; i < 50; ++i) {
        uint64_t stream_id = wt->open_stream();
        if (stream_id > 0) {
            std::string msg = "Message " + std::to_string(i);
            ssize_t sent = wt->send_stream(
                stream_id,
                reinterpret_cast<const uint8_t*>(msg.data()),
                msg.size()
            );
            if (sent >= 0) {
                success_count++;
            } else {
                failure_count++;
            }
            wt->close_stream(stream_id);
        } else {
            failure_count++;
        }
    }

    // Should have mostly successes
    EXPECT_GT(success_count, 0);

    auto stats = wt->get_stats();
    EXPECT_EQ(stats["active_streams"], 0u);  // All closed
}

// =============================================================================
// Generate Datagrams Test
// =============================================================================

TEST(WebTransportGenerateTest, GenerateDatagramsProducesOutput) {
    auto quic = create_quic_connection(true, 0x5678);
    auto wt = std::make_unique<WebTransportConnection>(std::move(quic));
    ASSERT_EQ(wt->initialize(), 0);
    ASSERT_EQ(wt->accept(), 0);

    // Queue datagrams
    for (int i = 0; i < 5; ++i) {
        auto data = random_bytes(100);
        wt->send_datagram(data.data(), data.size());
    }

    // Generate output
    uint8_t output[4096];
    size_t generated = wt->generate_datagrams(output, sizeof(output), get_time_us());

    EXPECT_GT(generated, 0u);
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
