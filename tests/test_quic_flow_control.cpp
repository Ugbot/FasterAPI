// Test QUIC flow control implementation
// Comprehensive tests for connection-level and stream-level flow control

#include "../src/cpp/http/quic/quic_flow_control.h"
#include <cassert>
#include <iostream>
#include <cstdint>

using namespace fasterapi::quic;

// Test helper
#define TEST_ASSERT(cond, msg) \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " at line " << __LINE__ << std::endl; \
        return false; \
    }

// Test connection-level flow control
bool test_connection_flow_control() {
    std::cout << "Testing connection-level flow control..." << std::endl;

    // Test 1: Basic send flow control
    {
        FlowControl fc(1024);  // 1KB window

        TEST_ASSERT(fc.can_send(512), "Should be able to send 512 bytes");
        TEST_ASSERT(fc.can_send(1024), "Should be able to send 1024 bytes");
        TEST_ASSERT(!fc.can_send(1025), "Should NOT be able to send 1025 bytes");

        fc.add_sent_data(512);
        TEST_ASSERT(fc.sent_data() == 512, "Sent data should be 512");
        TEST_ASSERT(fc.available_window() == 512, "Available window should be 512");
        TEST_ASSERT(!fc.is_blocked(), "Should not be blocked");

        fc.add_sent_data(512);
        TEST_ASSERT(fc.sent_data() == 1024, "Sent data should be 1024");
        TEST_ASSERT(fc.available_window() == 0, "Available window should be 0");
        TEST_ASSERT(fc.is_blocked(), "Should be blocked");
    }

    // Test 2: Window update from peer
    {
        FlowControl fc(1024);
        fc.add_sent_data(1024);
        TEST_ASSERT(fc.is_blocked(), "Should be blocked");

        fc.update_peer_max_data(2048);
        TEST_ASSERT(!fc.is_blocked(), "Should not be blocked after update");
        TEST_ASSERT(fc.available_window() == 1024, "Available window should be 1024");
        TEST_ASSERT(fc.peer_max_data() == 2048, "Peer max data should be 2048");
    }

    // Test 3: Receive flow control
    {
        FlowControl fc(1024);

        TEST_ASSERT(fc.can_receive(0, 512), "Should be able to receive 512 bytes at offset 0");
        TEST_ASSERT(fc.can_receive(512, 512), "Should be able to receive 512 bytes at offset 512");
        TEST_ASSERT(!fc.can_receive(1024, 1), "Should NOT be able to receive at offset 1024");

        fc.add_recv_data(512);
        TEST_ASSERT(fc.recv_data() == 512, "Received data should be 512");
        TEST_ASSERT(fc.recv_max_data() == 1024, "Recv max data should be 1024");
    }

    // Test 4: Auto-increment window
    {
        FlowControl fc(1024);
        fc.add_recv_data(512);

        uint64_t new_max = fc.auto_increment_window(512);
        TEST_ASSERT(new_max == 1536, "New max should be 1536 (1024 + 512)");
        TEST_ASSERT(fc.recv_max_data() == 1536, "Recv max data should be 1536");
    }

    std::cout << "  ✓ Connection flow control tests passed" << std::endl;
    return true;
}

// Test stream-level flow control
bool test_stream_flow_control() {
    std::cout << "Testing stream-level flow control..." << std::endl;

    // Test 1: Basic stream send flow control
    {
        StreamFlowControl sfc(512);  // 512 byte window

        TEST_ASSERT(sfc.can_send(256), "Should be able to send 256 bytes");
        TEST_ASSERT(sfc.can_send(512), "Should be able to send 512 bytes");
        TEST_ASSERT(!sfc.can_send(513), "Should NOT be able to send 513 bytes");

        sfc.add_sent_data(256);
        TEST_ASSERT(sfc.sent_offset() == 256, "Sent offset should be 256");
        TEST_ASSERT(sfc.available_window() == 256, "Available window should be 256");
        TEST_ASSERT(!sfc.is_blocked(), "Should not be blocked");
    }

    // Test 2: Stream window update
    {
        StreamFlowControl sfc(512);
        sfc.add_sent_data(512);
        TEST_ASSERT(sfc.is_blocked(), "Should be blocked");

        sfc.update_peer_max_stream_data(1024);
        TEST_ASSERT(!sfc.is_blocked(), "Should not be blocked after update");
        TEST_ASSERT(sfc.available_window() == 512, "Available window should be 512");
    }

    // Test 3: Stream receive flow control
    {
        StreamFlowControl sfc(512);

        TEST_ASSERT(sfc.can_receive(0, 256), "Should be able to receive 256 bytes");
        TEST_ASSERT(sfc.can_receive(256, 256), "Should be able to receive 256 bytes at offset 256");
        TEST_ASSERT(!sfc.can_receive(512, 1), "Should NOT be able to receive at offset 512");

        sfc.add_recv_data(256);
        TEST_ASSERT(sfc.recv_offset() == 256, "Recv offset should be 256");
    }

    // Test 4: Stream auto-increment window
    {
        StreamFlowControl sfc(512);
        sfc.add_recv_data(256);

        uint64_t new_max = sfc.auto_increment_window(256);
        TEST_ASSERT(new_max == 768, "New max should be 768 (512 + 256)");
        TEST_ASSERT(sfc.recv_max_offset() == 768, "Recv max offset should be 768");
    }

    std::cout << "  ✓ Stream flow control tests passed" << std::endl;
    return true;
}

// Test edge cases
bool test_edge_cases() {
    std::cout << "Testing edge cases..." << std::endl;

    // Test 1: Zero window
    {
        FlowControl fc(0);
        TEST_ASSERT(!fc.can_send(1), "Should not be able to send with zero window");
        TEST_ASSERT(fc.is_blocked(), "Should be blocked");
        TEST_ASSERT(fc.available_window() == 0, "Available window should be 0");
    }

    // Test 2: Large window
    {
        FlowControl fc(UINT64_MAX / 2);
        TEST_ASSERT(fc.can_send(1024 * 1024), "Should be able to send 1MB");
        TEST_ASSERT(fc.available_window() == UINT64_MAX / 2, "Available window should be large");
    }

    // Test 3: Exact boundary
    {
        FlowControl fc(1024);
        TEST_ASSERT(fc.can_send(1024), "Should be able to send exactly 1024 bytes");

        fc.add_sent_data(1024);
        TEST_ASSERT(!fc.can_send(1), "Should not be able to send 1 more byte");
        TEST_ASSERT(fc.is_blocked(), "Should be blocked");
    }

    // Test 4: Non-decreasing window updates
    {
        FlowControl fc(1024);
        fc.update_peer_max_data(2048);
        TEST_ASSERT(fc.peer_max_data() == 2048, "Should update to 2048");

        fc.update_peer_max_data(1024);  // Try to decrease
        TEST_ASSERT(fc.peer_max_data() == 2048, "Should NOT decrease to 1024");
    }

    std::cout << "  ✓ Edge case tests passed" << std::endl;
    return true;
}

// Test realistic scenarios
bool test_realistic_scenarios() {
    std::cout << "Testing realistic scenarios..." << std::endl;

    // Scenario 1: Request/response with flow control
    {
        // Connection flow control (both peers)
        FlowControl sender_conn(10 * 1024);      // Sender can send 10KB
        FlowControl receiver_conn(10 * 1024);    // Receiver can receive 10KB

        // Stream flow control (request stream)
        StreamFlowControl sender_stream(5 * 1024);
        StreamFlowControl receiver_stream(5 * 1024);

        // Send 3KB request
        uint64_t request_size = 3 * 1024;
        TEST_ASSERT(sender_conn.can_send(request_size), "Connection should allow send");
        TEST_ASSERT(sender_stream.can_send(request_size), "Stream should allow send");

        sender_conn.add_sent_data(request_size);
        sender_stream.add_sent_data(request_size);

        // Receive 3KB request
        TEST_ASSERT(receiver_conn.can_receive(0, request_size), "Connection should allow receive");
        TEST_ASSERT(receiver_stream.can_receive(0, request_size), "Stream should allow receive");

        receiver_conn.add_recv_data(request_size);
        receiver_stream.add_recv_data(request_size);

        // Application consumes request
        receiver_conn.auto_increment_window(request_size);
        receiver_stream.auto_increment_window(request_size);

        TEST_ASSERT(receiver_conn.recv_max_data() == 13 * 1024, "Connection window extended");
        TEST_ASSERT(receiver_stream.recv_max_offset() == 8 * 1024, "Stream window extended");
    }

    // Scenario 2: Multiple streams sharing connection window
    {
        FlowControl conn(10 * 1024);  // 10KB total

        StreamFlowControl stream1(5 * 1024);
        StreamFlowControl stream2(5 * 1024);
        StreamFlowControl stream3(5 * 1024);

        // Stream 1 sends 4KB
        uint64_t s1_size = 4 * 1024;
        TEST_ASSERT(conn.can_send(s1_size), "Connection should allow stream1");
        TEST_ASSERT(stream1.can_send(s1_size), "Stream1 should allow send");
        conn.add_sent_data(s1_size);
        stream1.add_sent_data(s1_size);

        // Stream 2 sends 4KB
        uint64_t s2_size = 4 * 1024;
        TEST_ASSERT(conn.can_send(s2_size), "Connection should allow stream2");
        TEST_ASSERT(stream2.can_send(s2_size), "Stream2 should allow send");
        conn.add_sent_data(s2_size);
        stream2.add_sent_data(s2_size);

        // Stream 3 tries to send 4KB - should be blocked by connection
        uint64_t s3_size = 4 * 1024;
        TEST_ASSERT(!conn.can_send(s3_size), "Connection should block stream3");
        TEST_ASSERT(stream3.can_send(s3_size), "Stream3 has window but connection doesn't");

        // Only 2KB left in connection window
        TEST_ASSERT(conn.available_window() == 2 * 1024, "Connection has 2KB left");
    }

    std::cout << "  ✓ Realistic scenario tests passed" << std::endl;
    return true;
}

// Test randomized inputs
bool test_randomized() {
    std::cout << "Testing with randomized inputs..." << std::endl;

    // Generate pseudo-random sizes (deterministic for reproducibility)
    uint64_t seed = 42;
    auto pseudo_rand = [&seed]() -> uint64_t {
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF;
        return seed;
    };

    // Test with random window sizes
    for (int i = 0; i < 100; i++) {
        uint64_t window = (pseudo_rand() % (10 * 1024 * 1024)) + 1024;  // 1KB to 10MB
        FlowControl fc(window);

        uint64_t total_sent = 0;
        while (total_sent < window) {
            uint64_t chunk = pseudo_rand() % 1024 + 1;  // 1 to 1024 bytes
            if (total_sent + chunk > window) {
                chunk = window - total_sent;
            }

            TEST_ASSERT(fc.can_send(chunk), "Should be able to send chunk");
            fc.add_sent_data(chunk);
            total_sent += chunk;
        }

        TEST_ASSERT(fc.is_blocked(), "Should be blocked after filling window");
        TEST_ASSERT(fc.sent_data() == window, "Sent data should equal window");
    }

    std::cout << "  ✓ Randomized tests passed (100 iterations)" << std::endl;
    return true;
}

int main() {
    std::cout << "\n=== QUIC Flow Control Tests ===" << std::endl;
    std::cout << std::endl;

    bool all_passed = true;

    all_passed &= test_connection_flow_control();
    all_passed &= test_stream_flow_control();
    all_passed &= test_edge_cases();
    all_passed &= test_realistic_scenarios();
    all_passed &= test_randomized();

    std::cout << std::endl;
    if (all_passed) {
        std::cout << "✓✓✓ ALL TESTS PASSED ✓✓✓" << std::endl;
        return 0;
    } else {
        std::cout << "✗✗✗ SOME TESTS FAILED ✗✗✗" << std::endl;
        return 1;
    }
}
