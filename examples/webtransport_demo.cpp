/**
 * WebTransport Demo - Comprehensive Example
 *
 * Demonstrates all three WebTransport features:
 * 1. Bidirectional streams (reliable, ordered)
 * 2. Unidirectional streams (reliable, ordered, one-way)
 * 3. Datagrams (unreliable, unordered)
 *
 * Usage:
 *   Server: ./webtransport_demo server
 *   Client: ./webtransport_demo client
 *
 * Compile:
 *   g++ -std=c++20 -O3 -o webtransport_demo webtransport_demo.cpp \
 *       -I../src/cpp -L../build/lib -lfasterapi_http
 */

#include "src/cpp/http/webtransport_connection.h"
#include "src/cpp/http/quic/quic_connection.h"
#include "src/cpp/http/quic/quic_packet.h"
#include <iostream>
#include <memory>
#include <string>
#include <cstring>
#include <chrono>
#include <thread>
#include <random>

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
void generate_random_data(uint8_t* buffer, size_t length) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 255);

    for (size_t i = 0; i < length; i++) {
        buffer[i] = static_cast<uint8_t>(dis(gen));
    }
}

/**
 * WebTransport Server Example
 */
class WebTransportServer {
public:
    WebTransportServer() {
        // Create QUIC connection (server-side)
        ConnectionID local_conn_id{0x01, 0x02, 0x03, 0x04};
        ConnectionID peer_conn_id{0x05, 0x06, 0x07, 0x08};
        auto quic_conn = std::make_unique<QUICConnection>(
            true,  // is_server
            local_conn_id,
            peer_conn_id
        );

        // Initialize QUIC connection
        quic_conn->initialize();

        // Create WebTransport connection
        wt_conn_ = std::make_unique<WebTransportConnection>(std::move(quic_conn));

        // Initialize WebTransport
        if (wt_conn_->initialize() != 0) {
            std::cerr << "Failed to initialize WebTransport connection" << std::endl;
            return;
        }

        // Accept incoming session
        if (wt_conn_->accept() != 0) {
            std::cerr << "Failed to accept WebTransport session" << std::endl;
            return;
        }

        setup_callbacks();
    }

    void setup_callbacks() {
        // Callback for bidirectional stream data
        wt_conn_->on_stream_data([this](uint64_t stream_id, const uint8_t* data, size_t length) {
            std::cout << "[Server] Received " << length << " bytes on bidirectional stream "
                      << stream_id << std::endl;

            // Echo data back
            std::string response = "Echo: ";
            response.append(reinterpret_cast<const char*>(data), length);

            wt_conn_->send_stream(
                stream_id,
                reinterpret_cast<const uint8_t*>(response.data()),
                response.size()
            );

            std::cout << "[Server] Echoed back " << response.size() << " bytes" << std::endl;
        });

        // Callback for unidirectional stream data
        wt_conn_->on_unidirectional_data([](uint64_t stream_id, const uint8_t* data, size_t length) {
            std::cout << "[Server] Received " << length << " bytes on unidirectional stream "
                      << stream_id << std::endl;
            std::string msg(reinterpret_cast<const char*>(data), length);
            std::cout << "[Server] Message: " << msg << std::endl;
        });

        // Callback for datagram received
        wt_conn_->on_datagram([this](const uint8_t* data, size_t length) {
            std::cout << "[Server] Received datagram: " << length << " bytes" << std::endl;

            // Send datagram response
            std::string response = "Datagram ACK";
            wt_conn_->send_datagram(
                reinterpret_cast<const uint8_t*>(response.data()),
                response.size()
            );

            datagram_count_++;
        });

        // Callback for stream opened
        wt_conn_->on_stream_opened([](uint64_t stream_id, bool is_bidirectional) {
            std::cout << "[Server] Stream " << stream_id << " opened ("
                      << (is_bidirectional ? "bidirectional" : "unidirectional") << ")"
                      << std::endl;
        });

        // Callback for stream closed
        wt_conn_->on_stream_closed([](uint64_t stream_id) {
            std::cout << "[Server] Stream " << stream_id << " closed" << std::endl;
        });

        // Callback for connection closed
        wt_conn_->on_connection_closed([](uint64_t error_code, const char* reason) {
            std::cout << "[Server] Connection closed: error=" << error_code
                      << ", reason=" << (reason ? reason : "none") << std::endl;
        });
    }

    void run() {
        std::cout << "[Server] WebTransport server running..." << std::endl;
        std::cout << "[Server] Waiting for client connections..." << std::endl;

        uint8_t recv_buffer[4096];
        uint8_t send_buffer[4096];

        // Main event loop
        while (!wt_conn_->is_closed()) {
            uint64_t now = get_current_time_us();

            // Process incoming data (in real app, this would come from UDP socket)
            // For demo, we just call process_datagram with empty data
            // (actual data would be received from network)

            // Generate outgoing packets
            size_t bytes = wt_conn_->generate_datagrams(send_buffer, sizeof(send_buffer), now);
            if (bytes > 0) {
                // In real app, send via UDP socket
                std::cout << "[Server] Generated " << bytes << " bytes to send" << std::endl;
            }

            // Print stats every 5 seconds
            static uint64_t last_stats_time = 0;
            if (now - last_stats_time > 5000000) {  // 5 seconds
                auto stats = wt_conn_->get_stats();
                std::cout << "\n[Server] Statistics:" << std::endl;
                std::cout << "  Streams opened: " << stats["streams_opened"] << std::endl;
                std::cout << "  Datagrams sent: " << stats["datagrams_sent"] << std::endl;
                std::cout << "  Datagrams received: " << stats["datagrams_received"] << std::endl;
                std::cout << "  Bytes sent: " << stats["bytes_sent"] << std::endl;
                std::cout << "  Bytes received: " << stats["bytes_received"] << std::endl;
                std::cout << "  Active streams: " << stats["active_streams"] << std::endl;
                std::cout << std::endl;
                last_stats_time = now;
            }

            // Sleep to avoid busy-waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        std::cout << "[Server] Server stopped" << std::endl;
    }

private:
    std::unique_ptr<WebTransportConnection> wt_conn_;
    size_t datagram_count_ = 0;
};

/**
 * WebTransport Client Example
 */
class WebTransportClient {
public:
    WebTransportClient() {
        // Create QUIC connection (client-side)
        ConnectionID local_conn_id{0x05, 0x06, 0x07, 0x08};
        ConnectionID peer_conn_id{0x01, 0x02, 0x03, 0x04};
        auto quic_conn = std::make_unique<QUICConnection>(
            false,  // is_server
            local_conn_id,
            peer_conn_id
        );

        // Initialize QUIC connection
        quic_conn->initialize();

        // Create WebTransport connection
        wt_conn_ = std::make_unique<WebTransportConnection>(std::move(quic_conn));

        // Initialize WebTransport
        if (wt_conn_->initialize() != 0) {
            std::cerr << "Failed to initialize WebTransport connection" << std::endl;
            return;
        }

        setup_callbacks();
    }

    void setup_callbacks() {
        // Callback for bidirectional stream data (echo responses)
        wt_conn_->on_stream_data([](uint64_t stream_id, const uint8_t* data, size_t length) {
            std::cout << "[Client] Received echo on stream " << stream_id
                      << ": " << length << " bytes" << std::endl;
            std::string msg(reinterpret_cast<const char*>(data), length);
            std::cout << "[Client] Echo: " << msg << std::endl;
        });

        // Callback for datagram received (ACKs)
        wt_conn_->on_datagram([](const uint8_t* data, size_t length) {
            std::cout << "[Client] Received datagram ACK: " << length << " bytes" << std::endl;
            std::string msg(reinterpret_cast<const char*>(data), length);
            std::cout << "[Client] ACK: " << msg << std::endl;
        });

        // Callback for connection closed
        wt_conn_->on_connection_closed([](uint64_t error_code, const char* reason) {
            std::cout << "[Client] Connection closed: error=" << error_code
                      << ", reason=" << (reason ? reason : "none") << std::endl;
        });
    }

    void connect(const char* url) {
        std::cout << "[Client] Connecting to " << url << "..." << std::endl;

        if (wt_conn_->connect(url) != 0) {
            std::cerr << "[Client] Failed to connect" << std::endl;
            return;
        }

        std::cout << "[Client] Connection initiated" << std::endl;
    }

    void demo_bidirectional_streams() {
        std::cout << "\n=== Demo 1: Bidirectional Streams ===" << std::endl;

        // Open bidirectional stream
        uint64_t stream_id = wt_conn_->open_stream();
        if (stream_id == 0) {
            std::cerr << "[Client] Failed to open stream" << std::endl;
            return;
        }

        std::cout << "[Client] Opened bidirectional stream " << stream_id << std::endl;

        // Send data on stream
        std::string message = "Hello from bidirectional stream!";
        ssize_t sent = wt_conn_->send_stream(
            stream_id,
            reinterpret_cast<const uint8_t*>(message.data()),
            message.size()
        );

        if (sent > 0) {
            std::cout << "[Client] Sent " << sent << " bytes on stream " << stream_id << std::endl;
        } else {
            std::cerr << "[Client] Failed to send data" << std::endl;
        }

        // Wait for echo response
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // Close stream
        wt_conn_->close_stream(stream_id);
        std::cout << "[Client] Closed stream " << stream_id << std::endl;
    }

    void demo_unidirectional_streams() {
        std::cout << "\n=== Demo 2: Unidirectional Streams ===" << std::endl;

        // Open unidirectional stream
        uint64_t stream_id = wt_conn_->open_unidirectional_stream();
        if (stream_id == 0) {
            std::cerr << "[Client] Failed to open unidirectional stream" << std::endl;
            return;
        }

        std::cout << "[Client] Opened unidirectional stream " << stream_id << std::endl;

        // Send data on unidirectional stream
        std::string message = "One-way message on unidirectional stream";
        ssize_t sent = wt_conn_->send_unidirectional(
            stream_id,
            reinterpret_cast<const uint8_t*>(message.data()),
            message.size()
        );

        if (sent > 0) {
            std::cout << "[Client] Sent " << sent << " bytes on unidirectional stream "
                      << stream_id << std::endl;
        } else {
            std::cerr << "[Client] Failed to send data" << std::endl;
        }

        // Close stream
        wt_conn_->close_unidirectional_stream(stream_id);
        std::cout << "[Client] Closed unidirectional stream " << stream_id << std::endl;
    }

    void demo_datagrams() {
        std::cout << "\n=== Demo 3: Datagrams ===" << std::endl;

        // Send multiple datagrams
        for (int i = 0; i < 10; i++) {
            std::string message = "Datagram #" + std::to_string(i);

            int result = wt_conn_->send_datagram(
                reinterpret_cast<const uint8_t*>(message.data()),
                message.size()
            );

            if (result == 0) {
                std::cout << "[Client] Sent datagram " << i << " (" << message.size()
                          << " bytes)" << std::endl;
            } else {
                std::cerr << "[Client] Failed to send datagram " << i << std::endl;
            }

            // Small delay between datagrams
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "[Client] Sent all datagrams" << std::endl;
    }

    void run_demos() {
        std::cout << "[Client] Running WebTransport demos..." << std::endl;

        // Wait for connection to establish
        std::this_thread::sleep_for(std::chrono::seconds(1));

        if (!wt_conn_->is_connected()) {
            std::cerr << "[Client] Not connected, cannot run demos" << std::endl;
            return;
        }

        // Demo 1: Bidirectional streams
        demo_bidirectional_streams();
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // Demo 2: Unidirectional streams
        demo_unidirectional_streams();
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // Demo 3: Datagrams
        demo_datagrams();
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // Print final stats
        auto stats = wt_conn_->get_stats();
        std::cout << "\n[Client] Final Statistics:" << std::endl;
        std::cout << "  Streams opened: " << stats["streams_opened"] << std::endl;
        std::cout << "  Datagrams sent: " << stats["datagrams_sent"] << std::endl;
        std::cout << "  Datagrams received: " << stats["datagrams_received"] << std::endl;
        std::cout << "  Bytes sent: " << stats["bytes_sent"] << std::endl;
        std::cout << "  Bytes received: " << stats["bytes_received"] << std::endl;
        std::cout << std::endl;

        // Close connection
        wt_conn_->close(0, "Client demo completed");
        std::cout << "[Client] Connection closed" << std::endl;
    }

private:
    std::unique_ptr<WebTransportConnection> wt_conn_;
};

} // anonymous namespace

/**
 * Main entry point
 */
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <server|client>" << std::endl;
        return 1;
    }

    std::string mode(argv[1]);

    try {
        if (mode == "server") {
            WebTransportServer server;
            server.run();
        } else if (mode == "client") {
            WebTransportClient client;
            client.connect("https://localhost:4433/webtransport");
            client.run_demos();
        } else {
            std::cerr << "Invalid mode: " << mode << std::endl;
            std::cerr << "Use 'server' or 'client'" << std::endl;
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
