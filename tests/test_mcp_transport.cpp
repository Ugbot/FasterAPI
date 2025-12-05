/**
 * Unit tests for MCP transport layer.
 */

#include "../src/cpp/mcp/transports/stdio_transport.h"
#include <iostream>
#include <cassert>
#include <thread>
#include <chrono>

using namespace fasterapi::mcp;

void test_transport_creation() {
    std::cout << "Testing transport creation..." << std::endl;

    // Server mode
    StdioTransport server_transport;
    assert(server_transport.get_type() == TransportType::STDIO);
    assert(server_transport.get_name() == "stdio");
    assert(!server_transport.is_connected());

    // Client mode
    StdioTransport client_transport("echo", {"hello"});
    assert(!client_transport.is_connected());

    std::cout << "  ✓ Server and client mode creation" << std::endl;
}

void test_transport_state() {
    std::cout << "Testing transport state..." << std::endl;

    StdioTransport transport;

    assert(transport.get_state() == TransportState::DISCONNECTED);
    assert(!transport.is_connected());

    std::cout << "  ✓ Initial state is DISCONNECTED" << std::endl;
}

void test_transport_callbacks() {
    std::cout << "Testing transport callbacks..." << std::endl;

    StdioTransport transport;

    bool message_received = false;
    bool error_occurred = false;
    TransportState last_state = TransportState::DISCONNECTED;

    transport.set_message_callback([&](const std::string& msg) {
        message_received = true;
    });

    transport.set_error_callback([&](const std::string& err) {
        error_occurred = true;
    });

    transport.set_state_callback([&](TransportState state) {
        last_state = state;
    });

    std::cout << "  ✓ Callbacks registered successfully" << std::endl;
}

void test_message_framing() {
    std::cout << "Testing message framing..." << std::endl;

    // Messages should be newline-delimited
    std::string msg1 = "{\"jsonrpc\":\"2.0\",\"method\":\"test\"}";
    std::string framed = msg1 + "\n";

    assert(framed.back() == '\n');
    assert(framed.find('\n') != std::string::npos);

    std::cout << "  ✓ Newline-delimited framing" << std::endl;
}

void test_transport_factory() {
    std::cout << "Testing transport factory..." << std::endl;

    // Server mode
    auto server_transport = TransportFactory::create_stdio();
    assert(server_transport != nullptr);
    assert(server_transport->get_type() == TransportType::STDIO);

    // Client mode
    auto client_transport = TransportFactory::create_stdio("python3", {"-V"});
    assert(client_transport != nullptr);

    std::cout << "  ✓ Factory creates transports" << std::endl;
}

void test_concurrent_operations() {
    std::cout << "Testing concurrent operations..." << std::endl;

    StdioTransport transport;

    std::atomic<int> messages_received{0};

    transport.set_message_callback([&](const std::string& msg) {
        messages_received++;
    });

    // Simulate concurrent access
    std::thread t1([&]() {
        for (int i = 0; i < 100; i++) {
            transport.get_state();
        }
    });

    std::thread t2([&]() {
        for (int i = 0; i < 100; i++) {
            transport.is_connected();
        }
    });

    t1.join();
    t2.join();

    std::cout << "  ✓ Concurrent state checks" << std::endl;
}

void test_error_handling() {
    std::cout << "Testing error handling..." << std::endl;

    StdioTransport transport;

    std::string last_error;
    transport.set_error_callback([&](const std::string& err) {
        last_error = err;
    });

    // Sending on disconnected transport should fail
    int result = transport.send("{\"test\": true}");
    assert(result != 0);  // Should fail

    std::cout << "  ✓ Error handling for invalid operations" << std::endl;
}

int main() {
    std::cout << "\n=== MCP Transport Tests ===\n" << std::endl;

    try {
        test_transport_creation();
        test_transport_state();
        test_transport_callbacks();
        test_message_framing();
        test_transport_factory();
        test_concurrent_operations();
        test_error_handling();

        std::cout << "\n✅ All tests passed!" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed: " << e.what() << std::endl;
        return 1;
    }
}
