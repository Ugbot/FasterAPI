/**
 * HTTP/3 and WebTransport Performance Benchmarks
 *
 * Measures performance of key HTTP/3 stack components:
 * - QUIC VarInt encoding/decoding
 * - QPACK header compression
 * - WebTransport stream operations
 * - Datagram handling
 * - Flow control
 * - Congestion control
 */

#include "src/cpp/http/quic/quic_varint.h"
#include "src/cpp/http/quic/quic_frames.h"
#include "src/cpp/http/quic/quic_connection.h"
#include "src/cpp/http/quic/quic_flow_control.h"
#include "src/cpp/http/quic/quic_congestion.h"
#include "src/cpp/http/qpack/qpack_encoder.h"
#include "src/cpp/http/qpack/qpack_decoder.h"
#include "src/cpp/http/webtransport_connection.h"
#include "src/cpp/http/http3_connection.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <random>
#include <iomanip>
#include <cstring>

using namespace fasterapi::http;
using namespace fasterapi::quic;
using namespace fasterapi::qpack;
using namespace std::chrono;

// ============================================================================
// Benchmark Utilities
// ============================================================================

template<typename Func>
struct BenchmarkResult {
    double ns_per_op;
    double ops_per_sec;
    int iterations;
};

template<typename Func>
BenchmarkResult<Func> benchmark(const char* name, Func&& func, int iterations = 1000000) {
    // Warmup
    for (int i = 0; i < 1000; ++i) {
        func();
    }

    auto start = high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        func();
    }

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<nanoseconds>(end - start).count();
    double ns_per_op = static_cast<double>(duration) / iterations;
    double ops_per_sec = 1e9 / ns_per_op;

    std::cout << std::left << std::setw(45) << name
              << std::right << std::setw(10) << std::fixed << std::setprecision(1) << ns_per_op << " ns/op"
              << std::setw(15) << std::setprecision(2) << (ops_per_sec / 1e6) << " M ops/s"
              << std::endl;

    return {ns_per_op, ops_per_sec, iterations};
}

std::vector<uint8_t> random_bytes(size_t len) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 255);

    std::vector<uint8_t> data(len);
    for (auto& b : data) {
        b = static_cast<uint8_t>(dis(gen));
    }
    return data;
}

ConnectionID make_conn_id(uint64_t seed) {
    ConnectionID id;
    id.length = 8;
    std::memcpy(id.data, &seed, 8);
    return id;
}

void establish_connection(QUICConnection& conn, const ConnectionID& local_cid) {
    uint8_t packet[100];
    ShortHeader hdr;
    hdr.dest_conn_id = local_cid;
    hdr.packet_number = 1;
    hdr.packet_number_length = 4;
    hdr.spin_bit = false;
    hdr.key_phase = false;
    size_t hdr_len = hdr.serialize(packet);
    packet[hdr_len] = 0x01;  // PING

    auto now = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
    conn.process_packet(packet, hdr_len + 1, now);
}

void print_header(const char* title) {
    std::cout << "\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "  " << title << "\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
}

// ============================================================================
// Main Benchmark Suite
// ============================================================================

int main() {
    std::cout << "\n";
    std::cout << "╔═══════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║            HTTP/3 & WebTransport Performance Benchmarks               ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════════════════╝\n";

    // ========================================================================
    // QUIC VarInt Encoding/Decoding
    // ========================================================================
    print_header("QUIC Variable-Length Integer (RFC 9000)");

    uint8_t varint_buf[8];

    benchmark("VarInt encode (1-byte: 0-63)", [&]() {
        VarInt::encode(42, varint_buf);
    });

    benchmark("VarInt encode (2-byte: 64-16383)", [&]() {
        VarInt::encode(1000, varint_buf);
    });

    benchmark("VarInt encode (4-byte: 16384-1073741823)", [&]() {
        VarInt::encode(1000000, varint_buf);
    });

    benchmark("VarInt encode (8-byte: large values)", [&]() {
        VarInt::encode(1234567890123ULL, varint_buf);
    });

    uint64_t decoded;
    uint8_t enc1[] = {42};
    benchmark("VarInt decode (1-byte)", [&]() {
        VarInt::decode(enc1, 1, decoded);
    });

    uint8_t enc2[] = {0x43, 0xE8};  // 1000
    benchmark("VarInt decode (2-byte)", [&]() {
        VarInt::decode(enc2, 2, decoded);
    });

    uint8_t enc4[] = {0x80, 0x0F, 0x42, 0x40};  // 1000000
    benchmark("VarInt decode (4-byte)", [&]() {
        VarInt::decode(enc4, 4, decoded);
    });

    benchmark("VarInt encode+decode roundtrip", [&]() {
        size_t len = VarInt::encode(999999, varint_buf);
        VarInt::decode(varint_buf, len, decoded);
    });

    // ========================================================================
    // QPACK Header Compression
    // ========================================================================
    print_header("QPACK Header Compression (RFC 9204)");

    QPACKEncoder encoder(4096, 100);
    QPACKDecoder decoder(4096);

    // Response headers
    std::vector<std::pair<std::string_view, std::string_view>> response_headers = {
        {":status", "200"},
        {"content-type", "application/json"},
        {"content-length", "1234"},
    };

    uint8_t qpack_buf[1024];
    size_t qpack_len;

    benchmark("QPACK encode (3 response headers)", [&]() {
        encoder.encode_field_section(
            response_headers.data(), response_headers.size(),
            qpack_buf, sizeof(qpack_buf), qpack_len
        );
    });

    // Pre-encode for decode benchmark
    encoder.encode_field_section(
        response_headers.data(), response_headers.size(),
        qpack_buf, sizeof(qpack_buf), qpack_len
    );

    std::pair<std::string, std::string> decoded_headers[256];
    size_t decoded_count;

    benchmark("QPACK decode (3 response headers)", [&]() {
        decoder.decode_field_section(qpack_buf, qpack_len, decoded_headers, decoded_count);
    });

    // Request headers
    std::vector<std::pair<std::string_view, std::string_view>> request_headers = {
        {":method", "GET"},
        {":scheme", "https"},
        {":authority", "api.example.com"},
        {":path", "/api/v1/users/123"},
        {"user-agent", "FasterAPI/1.0"},
        {"accept", "application/json"},
    };

    benchmark("QPACK encode (6 request headers)", [&]() {
        encoder.encode_field_section(
            request_headers.data(), request_headers.size(),
            qpack_buf, sizeof(qpack_buf), qpack_len
        );
    });

    // WebTransport CONNECT headers
    std::vector<std::pair<std::string_view, std::string_view>> wt_connect_headers = {
        {":method", "CONNECT"},
        {":scheme", "https"},
        {":authority", "localhost:8443"},
        {":path", "/webtransport"},
        {":protocol", "webtransport"},
    };

    benchmark("QPACK encode (WebTransport CONNECT)", [&]() {
        encoder.encode_field_section(
            wt_connect_headers.data(), wt_connect_headers.size(),
            qpack_buf, sizeof(qpack_buf), qpack_len
        );
    });

    // ========================================================================
    // QUIC Frame Serialization
    // ========================================================================
    print_header("QUIC Frame Serialization");

    uint8_t frame_buf[256];

    StreamFrame stream_frame;
    stream_frame.stream_id = 4;
    stream_frame.offset = 1000;
    stream_frame.length = 100;
    stream_frame.fin = false;
    auto payload = random_bytes(100);
    stream_frame.data = payload.data();

    benchmark("STREAM frame serialize", [&]() {
        stream_frame.serialize(frame_buf);
    });

    AckFrame ack_frame;
    ack_frame.largest_acked = 100;
    ack_frame.ack_delay = 1000;
    ack_frame.first_ack_range = 5;
    ack_frame.range_count = 0;

    benchmark("ACK frame serialize", [&]() {
        ack_frame.serialize(frame_buf);
    });

    DatagramFrame datagram_frame;
    auto dgram_data = random_bytes(200);
    datagram_frame.data = dgram_data.data();
    datagram_frame.length = 200;

    benchmark("DATAGRAM frame serialize", [&]() {
        datagram_frame.serialize(frame_buf);
    });

    // ========================================================================
    // Flow Control
    // ========================================================================
    print_header("QUIC Flow Control");

    FlowControl flow_control(16 * 1024 * 1024);  // 16MB window

    benchmark("Flow control: can_send check", [&]() {
        volatile bool can = flow_control.can_send(1000);
        (void)can;
    });

    benchmark("Flow control: add_sent_data (1KB)", [&]() {
        flow_control.add_sent_data(1024);
    });

    benchmark("Flow control: can_receive check", [&]() {
        volatile bool can = flow_control.can_receive(0, 1000);
        (void)can;
    });

    // ========================================================================
    // Congestion Control
    // ========================================================================
    print_header("QUIC Congestion Control (NewReno)");

    NewRenoCongestionControl congestion_control;
    auto now_us = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();

    benchmark("Congestion control: in_slow_start check", [&]() {
        volatile bool in_ss = congestion_control.in_slow_start();
        (void)in_ss;
    });

    benchmark("Congestion control: congestion_window", [&]() {
        volatile uint64_t cwnd = congestion_control.congestion_window();
        (void)cwnd;
    });

    benchmark("Congestion control: on_ack_received (slow start)", [&]() {
        congestion_control.on_ack_received(1200, now_us);
    });

    // ========================================================================
    // WebTransport Operations
    // ========================================================================
    print_header("WebTransport Operations");

    auto local_cid = make_conn_id(0x1234567890ABCDEF);
    auto peer_cid = make_conn_id(0xFEDCBA0987654321);

    auto quic_conn = std::make_unique<QUICConnection>(true, local_cid, peer_cid);
    quic_conn->initialize();
    establish_connection(*quic_conn, local_cid);

    auto wt = std::make_unique<WebTransportConnection>(std::move(quic_conn));
    wt->initialize();
    wt->accept();

    benchmark("WebTransport: open_stream", [&]() {
        uint64_t id = wt->open_stream();
        if (id > 0) {
            wt->close_stream(id);
        }
    });

    uint64_t persistent_stream = wt->open_stream();
    auto stream_data = random_bytes(100);

    benchmark("WebTransport: send_stream (100 bytes)", [&]() {
        wt->send_stream(persistent_stream, stream_data.data(), stream_data.size());
    });

    benchmark("WebTransport: open_unidirectional_stream", [&]() {
        uint64_t id = wt->open_unidirectional_stream();
        if (id > 0) {
            wt->close_unidirectional_stream(id);
        }
    });

    auto datagram_data = random_bytes(200);

    benchmark("WebTransport: send_datagram (200 bytes)", [&]() {
        wt->send_datagram(datagram_data.data(), datagram_data.size());
    });

    uint8_t output[4096];
    auto now = duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();

    benchmark("WebTransport: generate_datagrams", [&]() {
        wt->generate_datagrams(output, sizeof(output), now);
    });

    benchmark("WebTransport: get_stats", [&]() {
        auto stats = wt->get_stats();
        (void)stats;
    });

    // ========================================================================
    // HTTP/3 Connection
    // ========================================================================
    print_header("HTTP/3 Connection");

    Http3ConnectionSettings settings;
    auto h3 = std::make_unique<Http3Connection>(true, local_cid, peer_cid, settings);
    h3->initialize();

    benchmark("Http3Connection: initialize", [&]() {
        Http3Connection temp_h3(true, local_cid, peer_cid, settings);
        temp_h3.initialize();
    }, 10000);

    // ========================================================================
    // Summary
    // ========================================================================
    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════════════════════\n";
    std::cout << "  Benchmark Complete\n";
    std::cout << "═══════════════════════════════════════════════════════════════════════\n";

    return 0;
}
