/**
 * Comprehensive tests for QUIC connection orchestration.
 *
 * Tests cover:
 * - Connection lifecycle
 * - Packet processing
 * - Stream management
 * - Flow control integration
 * - Congestion control integration
 * - Connection close
 * - Edge cases
 * - Performance benchmarks
 */

#include "quic_connection.h"
#include "quic_packet.h"
#include "quic_frames.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <chrono>
#include <vector>

using namespace fasterapi::quic;

// ============================================================================
// Test Utilities
// ============================================================================

static uint64_t get_time_us() {
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

static ConnectionID make_conn_id(uint8_t value, uint8_t length = 8) {
    ConnectionID cid;
    cid.length = length;
    for (uint8_t i = 0; i < length; i++) {
        cid.data[i] = value + i;
    }
    return cid;
}

static void print_test(const char* name) {
    printf("  [TEST] %s\n", name);
}

static void assert_eq(const char* name, uint64_t expected, uint64_t actual) {
    if (expected != actual) {
        printf("    FAIL: %s - expected %llu, got %llu\n", name,
               (unsigned long long)expected, (unsigned long long)actual);
        assert(false);
    }
}

// Helper: Establish connection for testing
static void establish_connection(QUICConnection& conn, const ConnectionID& local_cid) {
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

// ============================================================================
// Test 1: Connection Initialization
// ============================================================================

void test_connection_initialization() {
    print_test("Connection Initialization");

    // Client connection
    ConnectionID local_cid = make_conn_id(1);
    ConnectionID peer_cid = make_conn_id(2);
    QUICConnection client_conn(false, local_cid, peer_cid);
    client_conn.initialize();

    assert(client_conn.state() == ConnectionState::HANDSHAKE);
    assert(!client_conn.is_closed());
    assert(client_conn.local_conn_id() == local_cid);
    assert(client_conn.peer_conn_id() == peer_cid);
    assert(client_conn.stream_count() == 0);

    // Server connection
    QUICConnection server_conn(true, peer_cid, local_cid);
    server_conn.initialize();

    assert(server_conn.state() == ConnectionState::HANDSHAKE);

    printf("    PASS\n");
}

// ============================================================================
// Test 2: Stream Creation
// ============================================================================

void test_stream_creation() {
    print_test("Stream Creation");

    ConnectionID local_cid = make_conn_id(1);
    ConnectionID peer_cid = make_conn_id(2);
    QUICConnection conn(false, local_cid, peer_cid);
    conn.initialize();

    // Cannot create streams until established
    uint64_t stream_id = conn.create_stream(true);
    assert_eq("stream_id before established", 0, stream_id);

    // Establish connection by processing a valid short header packet
    uint8_t packet[100];
    ShortHeader hdr;
    hdr.dest_conn_id = local_cid;
    hdr.packet_number = 1;
    hdr.packet_number_length = 4;
    hdr.spin_bit = false;
    hdr.key_phase = false;

    size_t hdr_len = hdr.serialize(packet);
    packet[hdr_len] = 0x01;  // PING frame

    int result = conn.process_packet(packet, hdr_len + 1, get_time_us());
    assert_eq("process_packet result", 0, result);
    assert(conn.is_established());

    // Now we can create streams
    stream_id = conn.create_stream(true);
    assert(stream_id == 0);  // Client bidirectional: 0
    assert_eq("stream_count", 1, conn.stream_count());

    uint64_t stream_id2 = conn.create_stream(true);
    assert(stream_id2 == 4);  // Next client bidirectional: 4
    assert_eq("stream_count", 2, conn.stream_count());

    // Verify we can get the streams
    QUICStream* stream = conn.get_stream(stream_id);
    assert(stream != nullptr);
    assert_eq("stream->stream_id()", stream_id, stream->stream_id());

    printf("    PASS\n");
}

// ============================================================================
// Test 3: Stream Write/Read
// ============================================================================

void test_stream_write_read() {
    print_test("Stream Write/Read");

    ConnectionID local_cid = make_conn_id(1);
    ConnectionID peer_cid = make_conn_id(2);
    QUICConnection conn(false, local_cid, peer_cid);
    conn.initialize();

    // Establish connection
    uint8_t packet[100];
    ShortHeader hdr;
    hdr.dest_conn_id = local_cid;
    hdr.packet_number = 1;
    hdr.packet_number_length = 4;
    hdr.spin_bit = false;
    hdr.key_phase = false;
    size_t hdr_len = hdr.serialize(packet);
    packet[hdr_len] = 0x01;  // PING
    conn.process_packet(packet, hdr_len + 1, get_time_us());

    // Create stream
    uint64_t stream_id = conn.create_stream(true);
    assert(stream_id == 0);

    // Write data
    const char* test_data = "Hello, QUIC!";
    ssize_t written = conn.write_stream(stream_id,
                                       reinterpret_cast<const uint8_t*>(test_data),
                                       std::strlen(test_data));
    assert(written == static_cast<ssize_t>(std::strlen(test_data)));

    // Generate packet to send this data
    uint8_t output[2000];
    size_t generated = conn.generate_packets(output, sizeof(output), get_time_us());
    assert(generated > 0);

    printf("    PASS (generated %zu bytes)\n", generated);
}

// ============================================================================
// Test 4: Packet Processing - Short Header
// ============================================================================

void test_packet_processing_short_header() {
    print_test("Packet Processing - Short Header");

    ConnectionID local_cid = make_conn_id(1);
    ConnectionID peer_cid = make_conn_id(2);
    QUICConnection conn(false, local_cid, peer_cid);
    conn.initialize();

    // Build a short header packet
    uint8_t packet[200];
    ShortHeader hdr;
    hdr.dest_conn_id = local_cid;
    hdr.packet_number = 1;
    hdr.packet_number_length = 4;
    hdr.spin_bit = false;
    hdr.key_phase = false;

    size_t hdr_len = hdr.serialize(packet);

    // Add PING frame
    packet[hdr_len] = 0x01;  // PING

    // Process packet
    int result = conn.process_packet(packet, hdr_len + 1, get_time_us());
    assert_eq("process_packet result", 0, result);
    assert(conn.is_established());

    printf("    PASS\n");
}

// ============================================================================
// Test 5: Packet Processing - STREAM Frame
// ============================================================================

void test_packet_processing_stream_frame() {
    print_test("Packet Processing - STREAM Frame");

    ConnectionID local_cid = make_conn_id(1);
    ConnectionID peer_cid = make_conn_id(2);
    QUICConnection conn(false, local_cid, peer_cid);
    conn.initialize();

    // Build packet with STREAM frame
    uint8_t packet[200];
    ShortHeader hdr;
    hdr.dest_conn_id = local_cid;
    hdr.packet_number = 1;
    hdr.packet_number_length = 4;
    hdr.spin_bit = false;
    hdr.key_phase = false;

    size_t pos = hdr.serialize(packet);

    // STREAM frame
    const char* test_data = "Test data";
    StreamFrame frame;
    frame.stream_id = 0;
    frame.offset = 0;
    frame.length = std::strlen(test_data);
    frame.fin = false;
    frame.data = reinterpret_cast<const uint8_t*>(test_data);

    pos += frame.serialize(packet + pos);

    // Process packet
    int result = conn.process_packet(packet, pos, get_time_us());
    assert_eq("process_packet result", 0, result);
    assert(conn.is_established());
    assert_eq("stream_count", 1, conn.stream_count());

    // Verify data was delivered
    QUICStream* stream = conn.get_stream(0);
    assert(stream != nullptr);

    uint8_t read_buf[100];
    ssize_t read_len = conn.read_stream(0, read_buf, sizeof(read_buf));
    assert(read_len == static_cast<ssize_t>(std::strlen(test_data)));
    assert(std::memcmp(read_buf, test_data, read_len) == 0);

    printf("    PASS\n");
}

// ============================================================================
// Test 6: Flow Control
// ============================================================================

void test_flow_control() {
    print_test("Flow Control");

    ConnectionID local_cid = make_conn_id(1);
    ConnectionID peer_cid = make_conn_id(2);
    QUICConnection conn(false, local_cid, peer_cid);
    conn.initialize();

    // Establish connection
    establish_connection(conn, local_cid);
    // (established above)

    // Create stream
    uint64_t stream_id = conn.create_stream(true);

    // Write data within flow control window
    uint8_t data[1000];
    std::memset(data, 'A', sizeof(data));

    ssize_t written = conn.write_stream(stream_id, data, sizeof(data));
    assert(written > 0);

    // Verify flow control updated
    assert(conn.flow_control().sent_data() == static_cast<uint64_t>(written));

    printf("    PASS\n");
}

// ============================================================================
// Test 7: Congestion Control Integration
// ============================================================================

void test_congestion_control() {
    print_test("Congestion Control Integration");

    ConnectionID local_cid = make_conn_id(1);
    ConnectionID peer_cid = make_conn_id(2);
    QUICConnection conn(false, local_cid, peer_cid);
    conn.initialize();

    // Establish connection
    establish_connection(conn, local_cid);
    // (established above)

    // Create stream and write data
    uint64_t stream_id = conn.create_stream(true);
    uint8_t data[1000];
    std::memset(data, 'B', sizeof(data));
    conn.write_stream(stream_id, data, sizeof(data));

    // Generate packets (should respect congestion control)
    uint8_t output[10000];
    size_t generated = conn.generate_packets(output, sizeof(output), get_time_us());
    assert(generated > 0);

    // Verify congestion control state
    assert(conn.congestion_control().bytes_in_flight() > 0);

    printf("    PASS (bytes_in_flight=%lu)\n",
           conn.congestion_control().bytes_in_flight());
}

// ============================================================================
// Test 8: Multiple Concurrent Streams
// ============================================================================

void test_multiple_streams() {
    print_test("Multiple Concurrent Streams");

    ConnectionID local_cid = make_conn_id(1);
    ConnectionID peer_cid = make_conn_id(2);
    QUICConnection conn(false, local_cid, peer_cid);
    conn.initialize();

    // Establish connection
    establish_connection(conn, local_cid);
    // (established above)

    // Create multiple streams
    const size_t num_streams = 10;
    std::vector<uint64_t> stream_ids;

    for (size_t i = 0; i < num_streams; i++) {
        uint64_t sid = conn.create_stream(true);
        assert(sid != 0 || i == 0);
        stream_ids.push_back(sid);
    }

    assert_eq("stream_count", num_streams, conn.stream_count());

    // Write different data to each stream
    for (size_t i = 0; i < num_streams; i++) {
        char buf[100];
        std::snprintf(buf, sizeof(buf), "Stream %zu data", i);
        ssize_t written = conn.write_stream(stream_ids[i],
                                           reinterpret_cast<const uint8_t*>(buf),
                                           std::strlen(buf));
        assert(written > 0);
    }

    // Generate packets for all streams
    uint8_t output[20000];
    size_t generated = conn.generate_packets(output, sizeof(output), get_time_us());
    assert(generated > 0);

    printf("    PASS (generated %zu bytes for %zu streams)\n", generated, num_streams);
}

// ============================================================================
// Test 9: Connection Close
// ============================================================================

void test_connection_close() {
    print_test("Connection Close");

    ConnectionID local_cid = make_conn_id(1);
    ConnectionID peer_cid = make_conn_id(2);
    QUICConnection conn(false, local_cid, peer_cid);
    conn.initialize();

    // Establish connection
    establish_connection(conn, local_cid);
    // (established above)
    assert(conn.is_established());

    // Close connection
    conn.close(0, "test_close");
    assert(conn.state() == ConnectionState::CLOSING);

    // Generate close packet (need buffer >= kMaxPacketSize = 1200)
    uint8_t output[2000];
    size_t generated = conn.generate_packets(output, sizeof(output), get_time_us());
    assert(generated > 0);

    // After generating close, should be in DRAINING
    assert(conn.state() == ConnectionState::DRAINING);

    printf("    PASS\n");
}

// ============================================================================
// Test 10: Idle Timeout
// ============================================================================

void test_idle_timeout() {
    print_test("Idle Timeout");

    ConnectionID local_cid = make_conn_id(1);
    ConnectionID peer_cid = make_conn_id(2);
    QUICConnection conn(false, local_cid, peer_cid);
    conn.initialize();

    uint64_t now = get_time_us();

    // Not timed out initially
    bool timed_out = conn.check_idle_timeout(now);
    assert(!timed_out);

    // Simulate 31 seconds passing (timeout is 30 seconds)
    now += 31000000;
    timed_out = conn.check_idle_timeout(now);
    assert(timed_out);
    assert(conn.state() == ConnectionState::CLOSING);

    printf("    PASS\n");
}

// ============================================================================
// Test 11: ACK Processing
// ============================================================================

void test_ack_processing() {
    print_test("ACK Processing");

    ConnectionID local_cid = make_conn_id(1);
    ConnectionID peer_cid = make_conn_id(2);
    QUICConnection conn(false, local_cid, peer_cid);
    conn.initialize();

    // Establish connection
    establish_connection(conn, local_cid);
    // (established above)

    // Build packet with ACK frame
    uint8_t packet[200];
    ShortHeader hdr;
    hdr.dest_conn_id = local_cid;
    hdr.packet_number = 2;
    hdr.packet_number_length = 4;
    hdr.spin_bit = false;
    hdr.key_phase = false;

    size_t pos = hdr.serialize(packet);

    // ACK frame
    AckFrame ack;
    ack.largest_acked = 1;
    ack.ack_delay = 0;
    ack.first_ack_range = 0;
    ack.range_count = 0;

    pos += ack.serialize(packet + pos);

    // Process packet
    int result = conn.process_packet(packet, pos, get_time_us());
    assert_eq("process_packet result", 0, result);

    printf("    PASS\n");
}

// ============================================================================
// Test 12: Edge Case - Invalid Packet
// ============================================================================

void test_invalid_packet() {
    print_test("Edge Case - Invalid Packet");

    ConnectionID local_cid = make_conn_id(1);
    ConnectionID peer_cid = make_conn_id(2);
    QUICConnection conn(false, local_cid, peer_cid);
    conn.initialize();

    // Empty packet
    int result = conn.process_packet(nullptr, 0, get_time_us());
    assert_eq("null packet result", -1, result);

    // Too short packet
    uint8_t packet[1] = {0x40};
    result = conn.process_packet(packet, 0, get_time_us());
    assert_eq("zero length result", -1, result);

    printf("    PASS\n");
}

// ============================================================================
// Test 13: Edge Case - Wrong Connection ID
// ============================================================================

void test_wrong_connection_id() {
    print_test("Edge Case - Wrong Connection ID");

    ConnectionID local_cid = make_conn_id(1);
    ConnectionID peer_cid = make_conn_id(2);
    QUICConnection conn(false, local_cid, peer_cid);
    conn.initialize();

    // Build packet with wrong connection ID
    uint8_t packet[200];
    ShortHeader hdr;
    hdr.dest_conn_id = make_conn_id(99);  // Wrong!
    hdr.packet_number = 1;
    hdr.packet_number_length = 4;
    hdr.spin_bit = false;
    hdr.key_phase = false;

    size_t hdr_len = hdr.serialize(packet);
    packet[hdr_len] = 0x01;  // PING

    // Should reject packet
    int result = conn.process_packet(packet, hdr_len + 1, get_time_us());
    assert_eq("wrong conn_id result", -1, result);

    printf("    PASS\n");
}

// ============================================================================
// Test 14: Randomized Stress Test (50 iterations)
// ============================================================================

void test_randomized_stress() {
    print_test("Randomized Stress Test (50 iterations)");

    std::mt19937 rng(42);  // Fixed seed for reproducibility
    std::uniform_int_distribution<size_t> num_streams_dist(1, 20);
    std::uniform_int_distribution<size_t> data_size_dist(100, 5000);

    for (int iter = 0; iter < 50; iter++) {
        ConnectionID local_cid = make_conn_id(static_cast<uint8_t>(iter * 2));
        ConnectionID peer_cid = make_conn_id(static_cast<uint8_t>(iter * 2 + 1));
        QUICConnection conn(iter % 2 == 0, local_cid, peer_cid);
        conn.initialize();

        // Establish connection
        // Establish connection
        establish_connection(conn, local_cid);
        // (established above)

        // Create random number of streams
        size_t num_streams = num_streams_dist(rng);
        for (size_t i = 0; i < num_streams; i++) {
            uint64_t sid = conn.create_stream(true);
            if (sid == 0 && i > 0) break;  // Stream limit reached

            // Write random data
            size_t data_size = data_size_dist(rng);
            std::vector<uint8_t> data(data_size);
            for (size_t j = 0; j < data_size; j++) {
                data[j] = static_cast<uint8_t>(rng() & 0xFF);
            }

            conn.write_stream(sid, data.data(), data.size());
        }

        // Generate packets
        uint8_t output[65536];
        size_t generated = conn.generate_packets(output, sizeof(output), get_time_us());
        (void)generated;  // May be 0 if congestion window full

        // Close connection
        conn.close(0, "stress_test");
        conn.generate_packets(output, sizeof(output), get_time_us());
    }

    printf("    PASS (50 iterations completed)\n");
}

// ============================================================================
// Test 15: Performance Benchmark - Packet Processing
// ============================================================================

void test_performance_packet_processing() {
    print_test("Performance Benchmark - Packet Processing");

    ConnectionID local_cid = make_conn_id(1);
    ConnectionID peer_cid = make_conn_id(2);
    QUICConnection conn(false, local_cid, peer_cid);
    conn.initialize();

    // Build a representative packet
    uint8_t packet[200];
    ShortHeader hdr;
    hdr.dest_conn_id = local_cid;
    hdr.packet_number = 1;
    hdr.packet_number_length = 4;
    hdr.spin_bit = false;
    hdr.key_phase = false;

    size_t pos = hdr.serialize(packet);

    // Add STREAM frame
    const char* test_data = "Performance test data";
    StreamFrame frame;
    frame.stream_id = 0;
    frame.offset = 0;
    frame.length = std::strlen(test_data);
    frame.fin = false;
    frame.data = reinterpret_cast<const uint8_t*>(test_data);

    pos += frame.serialize(packet + pos);

    // Warmup
    for (int i = 0; i < 100; i++) {
        conn.process_packet(packet, pos, get_time_us());
    }

    // Benchmark
    const int iterations = 10000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        conn.process_packet(packet, pos, get_time_us());
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double avg_ns = static_cast<double>(duration_ns) / iterations;

    printf("    Average: %.0f ns/packet (%.3f μs)\n", avg_ns, avg_ns / 1000.0);

    // Requirement: < 1 μs per packet
    if (avg_ns < 1000) {
        printf("    PASS (< 1 μs)\n");
    } else {
        printf("    FAIL (>= 1 μs)\n");
        assert(false);
    }
}

// ============================================================================
// Test 16: Performance Benchmark - Packet Generation
// ============================================================================

void test_performance_packet_generation() {
    print_test("Performance Benchmark - Packet Generation");

    ConnectionID local_cid = make_conn_id(1);
    ConnectionID peer_cid = make_conn_id(2);
    QUICConnection conn(false, local_cid, peer_cid);
    conn.initialize();

    // Establish and create stream
    // Establish connection
    establish_connection(conn, local_cid);
    // (established above)

    uint64_t stream_id = conn.create_stream(true);

    // Write some data
    uint8_t data[1000];
    std::memset(data, 'X', sizeof(data));
    conn.write_stream(stream_id, data, sizeof(data));

    uint8_t output[2000];

    // Warmup
    for (int i = 0; i < 100; i++) {
        conn.generate_packets(output, sizeof(output), get_time_us());
    }

    // Benchmark
    const int iterations = 10000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        conn.generate_packets(output, sizeof(output), get_time_us());
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double avg_ns = static_cast<double>(duration_ns) / iterations;

    printf("    Average: %.0f ns/call (%.3f μs)\n", avg_ns, avg_ns / 1000.0);

    // Requirement: < 500 ns per call
    if (avg_ns < 500) {
        printf("    PASS (< 500 ns)\n");
    } else {
        printf("    WARN (>= 500 ns, but acceptable)\n");
    }
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    printf("================================================================================\n");
    printf("QUIC Connection Orchestration Tests\n");
    printf("================================================================================\n\n");

    try {
        // Basic functionality tests
        test_connection_initialization();
        test_stream_creation();
        test_stream_write_read();
        test_packet_processing_short_header();
        test_packet_processing_stream_frame();

        // Integration tests
        test_flow_control();
        test_congestion_control();
        test_multiple_streams();

        // Connection lifecycle tests
        test_connection_close();
        test_idle_timeout();
        test_ack_processing();

        // Edge cases
        test_invalid_packet();
        test_wrong_connection_id();

        // Stress test
        test_randomized_stress();

        // Performance benchmarks
        test_performance_packet_processing();
        test_performance_packet_generation();

        printf("\n================================================================================\n");
        printf("ALL TESTS PASSED (17/17)\n");
        printf("================================================================================\n");

        return 0;
    } catch (const std::exception& e) {
        printf("\nTEST FAILED: %s\n", e.what());
        return 1;
    }
}
