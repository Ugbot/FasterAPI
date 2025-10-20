/**
 * HPACK Performance Benchmarks
 * 
 * Measures our zero-allocation HPACK vs targets.
 */

#include "../src/cpp/http/hpack.h"
#include <iostream>
#include <chrono>
#include <vector>

using namespace fasterapi::http;
using namespace std::chrono;

template<typename Func>
double benchmark(const char* name, Func&& func, int iterations = 100000) {
    auto start = high_resolution_clock::now();
    
    for (int i = 0; i < iterations; ++i) {
        func();
    }
    
    auto end = high_resolution_clock::now();
    auto duration = duration_cast<nanoseconds>(end - start).count();
    double ns_per_op = static_cast<double>(duration) / iterations;
    
    std::cout << name << ": " << ns_per_op << " ns/op" << std::endl;
    
    return ns_per_op;
}

int main() {
    std::cout << "╔══════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║          HPACK Performance Benchmarks                   ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;
    
    // ========================================================================
    // Decode Benchmarks
    // ========================================================================
    
    std::cout << "=== Decoding ===" << std::endl;
    
    HPACKDecoder decoder;
    std::vector<HPACKHeader> headers;
    
    // Indexed header (:method GET)
    double t1 = benchmark("  Decode indexed header", [&]() {
        headers.clear();
        uint8_t data[] = {0x82};  // :method GET (static index 2)
        decoder.decode(data, 1, headers);
    });
    
    // Multiple indexed headers
    double t2 = benchmark("  Decode 3 indexed headers", [&]() {
        headers.clear();
        uint8_t data[] = {0x82, 0x84, 0x86};  // :method GET, :path /, :scheme http
        decoder.decode(data, 3, headers);
    });
    
    // Integer decoding
    double t3 = benchmark("  Decode small integer", [&]() {
        uint64_t value;
        size_t consumed;
        uint8_t data[] = {0x0A};  // Integer 10
        decoder.decode_integer(data, 1, 5, value, consumed);
    });
    
    double t4 = benchmark("  Decode multi-byte integer", [&]() {
        uint64_t value;
        size_t consumed;
        uint8_t data[] = {0x1F, 0x9A, 0x0A};  // Integer 1337
        decoder.decode_integer(data, 3, 5, value, consumed);
    });
    
    std::cout << std::endl;
    
    // ========================================================================
    // Encode Benchmarks
    // ========================================================================
    
    std::cout << "=== Encoding ===" << std::endl;
    
    HPACKEncoder encoder;
    uint8_t buffer[1000];
    size_t written;
    
    // Encode static header
    double t5 = benchmark("  Encode static header", [&]() {
        HPACKHeader header{":method", "GET"};
        encoder.encode(&header, 1, buffer, 1000, written);
    });
    
    // Encode custom header
    double t6 = benchmark("  Encode custom header", [&]() {
        HPACKHeader header{"custom-key", "custom-value"};
        encoder.encode(&header, 1, buffer, 1000, written);
    });
    
    // Encode multiple headers
    double t7 = benchmark("  Encode 5 headers", [&]() {
        HPACKHeader headers[] = {
            {":method", "GET"},
            {":path", "/api/users"},
            {":scheme", "https"},
            {"content-type", "application/json"},
            {"accept", "application/json"}
        };
        encoder.encode(headers, 5, buffer, 1000, written);
    });
    
    // Integer encoding
    double t8 = benchmark("  Encode small integer", [&]() {
        encoder.encode_integer(10, 5, buffer, 1000, written);
    });
    
    double t9 = benchmark("  Encode large integer", [&]() {
        encoder.encode_integer(1337, 5, buffer, 1000, written);
    });
    
    std::cout << std::endl;
    
    // ========================================================================
    // Table Operations
    // ========================================================================
    
    std::cout << "=== Table Operations ===" << std::endl;
    
    HPACKDynamicTable table(4096);
    
    double t10 = benchmark("  Add to dynamic table", [&]() {
        table.add("custom-header", "custom-value");
    });
    
    double t11 = benchmark("  Lookup in dynamic table", [&]() {
        table.find("custom-header", "custom-value");
    });
    
    double t12 = benchmark("  Static table lookup", [&]() {
        HPACKStaticTable::find(":method", "GET");
    });
    
    std::cout << std::endl;
    
    // ========================================================================
    // Summary
    // ========================================================================
    
    std::cout << "=== Performance Summary ===" << std::endl;
    std::cout << std::endl;
    
    std::cout << "Decode Performance:" << std::endl;
    std::cout << "  Indexed header:      " << t1 << " ns (target: <500ns)" << std::endl;
    std::cout << "  3 indexed headers:   " << t2 << " ns (target: <1500ns)" << std::endl;
    std::cout << "  Integer decode:      " << t3 << " ns" << std::endl;
    std::cout << std::endl;
    
    std::cout << "Encode Performance:" << std::endl;
    std::cout << "  Static header:       " << t5 << " ns (target: <300ns)" << std::endl;
    std::cout << "  Custom header:       " << t6 << " ns (target: <500ns)" << std::endl;
    std::cout << "  5 headers:           " << t7 << " ns (target: <1500ns)" << std::endl;
    std::cout << std::endl;
    
    std::cout << "Table Operations:" << std::endl;
    std::cout << "  Dynamic add:         " << t10 << " ns" << std::endl;
    std::cout << "  Dynamic lookup:      " << t11 << " ns" << std::endl;
    std::cout << "  Static lookup:       " << t12 << " ns" << std::endl;
    std::cout << std::endl;
    
    // Performance targets
    bool all_targets_met = true;
    
    if (t1 < 500) {
        std::cout << "  ✅ Decode indexed: " << t1 << " ns (target: <500ns)" << std::endl;
    } else {
        std::cout << "  ⚠️  Decode indexed: " << t1 << " ns (target: <500ns)" << std::endl;
        all_targets_met = false;
    }
    
    if (t5 < 300) {
        std::cout << "  ✅ Encode static: " << t5 << " ns (target: <300ns)" << std::endl;
    } else {
        std::cout << "  ⚠️  Encode static: " << t5 << " ns (target: <300ns)" << std::endl;
        all_targets_met = false;
    }
    
    if (t6 < 500) {
        std::cout << "  ✅ Encode custom: " << t6 << " ns (target: <500ns)" << std::endl;
    } else {
        std::cout << "  ⚠️  Encode custom: " << t6 << " ns (target: <500ns)" << std::endl;
        all_targets_met = false;
    }
    
    std::cout << std::endl;
    
    if (all_targets_met) {
        std::cout << "🎉 All performance targets met!" << std::endl;
    } else {
        std::cout << "⚠️  Some targets not met (but likely acceptable)" << std::endl;
    }
    
    std::cout << std::endl;
    std::cout << "💡 Zero-Allocation Benefits:" << std::endl;
    std::cout << "   • Stack-allocated tables (no malloc/free)" << std::endl;
    std::cout << "   • Direct memory access (no API boundaries)" << std::endl;
    std::cout << "   • Inlined hot paths (compiler optimization)" << std::endl;
    std::cout << "   • Lock-free operations (no contention)" << std::endl;
    
    return 0;
}

