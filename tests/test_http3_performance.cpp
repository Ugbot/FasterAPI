/**
 * HTTP/3 Performance and Load Tests
 *
 * Comprehensive benchmark suite for HTTP/3 implementation.
 * Measures throughput, latency, scalability, and resource usage.
 */

#include "../src/cpp/http/http3_parser.h"
#include "../src/cpp/http/qpack/qpack_encoder.h"
#include "../src/cpp/http/qpack/qpack_decoder.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <thread>
#include <iomanip>

using namespace fasterapi::http;
using namespace fasterapi::qpack;
using namespace std::chrono;

// =============================================================================
// Test Infrastructure
// =============================================================================

struct BenchmarkResult {
    std::string name;
    double throughput_rps;
    double throughput_mbps;
    double latency_p50_us;
    double latency_p95_us;
    double latency_p99_us;
    double avg_latency_us;
    size_t memory_per_op;
    size_t total_operations;
    double duration_sec;
    size_t allocation_count;
};

struct LatencyStats {
    std::vector<double> samples;

    void add(double value) {
        samples.push_back(value);
    }

    void compute(double& p50, double& p95, double& p99, double& avg) {
        if (samples.empty()) {
            p50 = p95 = p99 = avg = 0.0;
            return;
        }

        std::sort(samples.begin(), samples.end());

        size_t n = samples.size();
        p50 = samples[n * 50 / 100];
        p95 = samples[n * 95 / 100];
        p99 = samples[n * 99 / 100];

        double sum = 0.0;
        for (double s : samples) sum += s;
        avg = sum / n;
    }
};

class Timer {
public:
    Timer() : start_(high_resolution_clock::now()) {}

    double elapsed_us() const {
        auto end = high_resolution_clock::now();
        return duration_cast<microseconds>(end - start_).count();
    }

    double elapsed_ms() const {
        return elapsed_us() / 1000.0;
    }

    double elapsed_sec() const {
        return elapsed_us() / 1000000.0;
    }

    void reset() {
        start_ = high_resolution_clock::now();
    }

private:
    high_resolution_clock::time_point start_;
};

// Random data generator
class RandomGenerator {
public:
    RandomGenerator() : gen_(std::random_device{}()) {}

    void fill_random(uint8_t* data, size_t length) {
        std::uniform_int_distribution<> dist(0, 255);
        for (size_t i = 0; i < length; i++) {
            data[i] = dist(gen_);
        }
    }

    std::string random_string(size_t length) {
        std::string result(length, 0);
        std::uniform_int_distribution<> dist('a', 'z');
        for (size_t i = 0; i < length; i++) {
            result[i] = dist(gen_);
        }
        return result;
    }

    int random_int(int min, int max) {
        std::uniform_int_distribution<> dist(min, max);
        return dist(gen_);
    }

private:
    std::mt19937 gen_;
};

static RandomGenerator g_random;

// =============================================================================
// QPACK Compression Benchmarks
// =============================================================================

BenchmarkResult bench_qpack_compression() {
    BenchmarkResult result{};
    result.name = "QPACK Compression (headers)";

    QPACKEncoder encoder;
    encoder.set_huffman_encoding(false);  // Disable Huffman to avoid linker issues
    LatencyStats latency;

    // Prepare test headers
    std::vector<std::pair<std::string_view, std::string_view>> headers = {
        {":method", "GET"},
        {":scheme", "https"},
        {":authority", "example.com"},
        {":path", "/api/v1/users/12345"},
        {"user-agent", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)"},
        {"accept", "application/json, text/plain, */*"},
        {"accept-language", "en-US,en;q=0.9"},
        {"accept-encoding", "gzip, deflate, br"},
        {"authorization", "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"},
        {"content-type", "application/json"},
    };

    uint8_t output[4096];
    const size_t iterations = 100000;

    // Warm up
    for (int i = 0; i < 1000; i++) {
        size_t encoded_len;
        encoder.encode_field_section(headers.data(), headers.size(),
                                     output, sizeof(output), encoded_len);
    }

    // Benchmark
    Timer timer;
    for (size_t i = 0; i < iterations; i++) {
        Timer op_timer;

        size_t encoded_len;
        encoder.encode_field_section(headers.data(), headers.size(),
                                     output, sizeof(output), encoded_len);

        latency.add(op_timer.elapsed_us());
    }
    double duration = timer.elapsed_sec();

    // Calculate stats
    latency.compute(result.latency_p50_us, result.latency_p95_us,
                   result.latency_p99_us, result.avg_latency_us);

    result.throughput_rps = iterations / duration;
    result.total_operations = iterations;
    result.duration_sec = duration;

    return result;
}

BenchmarkResult bench_qpack_decompression() {
    BenchmarkResult result{};
    result.name = "QPACK Decompression (headers)";

    QPACKEncoder encoder;
    encoder.set_huffman_encoding(false);  // Disable Huffman to avoid linker issues
    QPACKDecoder decoder;
    LatencyStats latency;

    // Encode headers once
    std::vector<std::pair<std::string_view, std::string_view>> headers = {
        {":method", "POST"},
        {":scheme", "https"},
        {":authority", "api.example.com"},
        {":path", "/v2/data"},
        {"content-type", "application/json"},
        {"content-length", "1234"},
    };

    uint8_t encoded[4096];
    size_t encoded_len;
    encoder.encode_field_section(headers.data(), headers.size(),
                                 encoded, sizeof(encoded), encoded_len);

    std::pair<std::string, std::string> decoded_headers[QPACKDecoder::kMaxHeaders];
    const size_t iterations = 100000;

    // Warm up
    for (int i = 0; i < 1000; i++) {
        size_t count;
        decoder.decode_field_section(encoded, encoded_len, decoded_headers, count);
    }

    // Benchmark
    Timer timer;
    for (size_t i = 0; i < iterations; i++) {
        Timer op_timer;

        size_t count;
        decoder.decode_field_section(encoded, encoded_len, decoded_headers, count);

        latency.add(op_timer.elapsed_us());
    }
    double duration = timer.elapsed_sec();

    latency.compute(result.latency_p50_us, result.latency_p95_us,
                   result.latency_p99_us, result.avg_latency_us);

    result.throughput_rps = iterations / duration;
    result.total_operations = iterations;
    result.duration_sec = duration;

    return result;
}

// =============================================================================
// HTTP/3 Frame Parsing Benchmarks
// =============================================================================

BenchmarkResult bench_frame_header_parsing() {
    BenchmarkResult result{};
    result.name = "HTTP/3 Frame Header Parsing";

    HTTP3Parser parser;
    LatencyStats latency;

    // Create test frame header (HEADERS frame with length 1024)
    uint8_t frame_data[] = {0x01, 0x44, 0x00};  // Type 1, Length 1024

    const size_t iterations = 1000000;

    // Warm up
    for (int i = 0; i < 1000; i++) {
        HTTP3FrameHeader header;
        size_t consumed;
        parser.parse_frame_header(frame_data, sizeof(frame_data), header, consumed);
    }

    // Benchmark
    Timer timer;
    for (size_t i = 0; i < iterations; i++) {
        Timer op_timer;

        HTTP3FrameHeader header;
        size_t consumed;
        parser.parse_frame_header(frame_data, sizeof(frame_data), header, consumed);

        latency.add(op_timer.elapsed_us());
    }
    double duration = timer.elapsed_sec();

    latency.compute(result.latency_p50_us, result.latency_p95_us,
                   result.latency_p99_us, result.avg_latency_us);

    result.throughput_rps = iterations / duration;
    result.total_operations = iterations;
    result.duration_sec = duration;

    return result;
}

BenchmarkResult bench_settings_frame_parsing() {
    BenchmarkResult result{};
    result.name = "HTTP/3 SETTINGS Frame Parsing";

    HTTP3Parser parser;
    LatencyStats latency;

    // SETTINGS frame with multiple settings
    uint8_t settings_data[] = {
        0x01, 0x40, 0x00,        // Setting 1: 16384
        0x06, 0x40, 0x00,        // Setting 6: 16384
        0x07, 0x64,              // Setting 7: 100
    };

    const size_t iterations = 500000;

    // Warm up
    for (int i = 0; i < 1000; i++) {
        HTTP3Settings settings;
        parser.parse_settings(settings_data, sizeof(settings_data), settings);
    }

    // Benchmark
    Timer timer;
    for (size_t i = 0; i < iterations; i++) {
        Timer op_timer;

        HTTP3Settings settings;
        parser.parse_settings(settings_data, sizeof(settings_data), settings);

        latency.add(op_timer.elapsed_us());
    }
    double duration = timer.elapsed_sec();

    latency.compute(result.latency_p50_us, result.latency_p95_us,
                   result.latency_p99_us, result.avg_latency_us);

    result.throughput_rps = iterations / duration;
    result.total_operations = iterations;
    result.duration_sec = duration;

    return result;
}

// =============================================================================
// Throughput Benchmarks
// =============================================================================

BenchmarkResult bench_simple_get_throughput() {
    BenchmarkResult result{};
    result.name = "Simple GET Request (throughput)";

    QPACKEncoder encoder;
    encoder.set_huffman_encoding(false);
    QPACKDecoder decoder;
    LatencyStats latency;

    // Simple GET request headers
    std::vector<std::pair<std::string_view, std::string_view>> request_headers = {
        {":method", "GET"},
        {":scheme", "https"},
        {":authority", "localhost"},
        {":path", "/"},
    };

    // Response headers
    std::vector<std::pair<std::string_view, std::string_view>> response_headers = {
        {":status", "200"},
        {"content-type", "text/plain"},
        {"content-length", "13"},
    };

    uint8_t request_encoded[1024];
    uint8_t response_encoded[1024];
    const char* response_body = "Hello, World!";

    const size_t iterations = 100000;

    // Warm up
    for (int i = 0; i < 1000; i++) {
        size_t req_len, resp_len;
        encoder.encode_field_section(request_headers.data(), request_headers.size(),
                                     request_encoded, sizeof(request_encoded), req_len);
        encoder.encode_field_section(response_headers.data(), response_headers.size(),
                                     response_encoded, sizeof(response_encoded), resp_len);
    }

    // Benchmark
    Timer timer;
    for (size_t i = 0; i < iterations; i++) {
        Timer op_timer;

        // Encode request
        size_t req_len;
        encoder.encode_field_section(request_headers.data(), request_headers.size(),
                                     request_encoded, sizeof(request_encoded), req_len);

        // Decode request (simulating server)
        std::pair<std::string, std::string> decoded[16];
        size_t decoded_count;
        decoder.decode_field_section(request_encoded, req_len, decoded, decoded_count);

        // Encode response
        size_t resp_len;
        encoder.encode_field_section(response_headers.data(), response_headers.size(),
                                     response_encoded, sizeof(response_encoded), resp_len);

        // Decode response (simulating client)
        decoder.decode_field_section(response_encoded, resp_len, decoded, decoded_count);

        latency.add(op_timer.elapsed_us());
    }
    double duration = timer.elapsed_sec();

    latency.compute(result.latency_p50_us, result.latency_p95_us,
                   result.latency_p99_us, result.avg_latency_us);

    result.throughput_rps = iterations / duration;
    result.total_operations = iterations;
    result.duration_sec = duration;

    // Calculate throughput in MB/s
    size_t bytes_per_op = strlen(response_body) + 100;  // Approximate
    result.throughput_mbps = (iterations * bytes_per_op) / (duration * 1024 * 1024);

    return result;
}

BenchmarkResult bench_post_with_body_throughput() {
    BenchmarkResult result{};
    result.name = "POST with 1KB Body (throughput)";

    QPACKEncoder encoder;
    encoder.set_huffman_encoding(false);
    LatencyStats latency;

    // POST request headers
    std::vector<std::pair<std::string_view, std::string_view>> headers = {
        {":method", "POST"},
        {":scheme", "https"},
        {":authority", "api.example.com"},
        {":path", "/api/data"},
        {"content-type", "application/json"},
        {"content-length", "1024"},
    };

    uint8_t encoded[4096];
    uint8_t body[1024];
    g_random.fill_random(body, sizeof(body));

    const size_t iterations = 50000;

    // Warm up
    for (int i = 0; i < 1000; i++) {
        size_t encoded_len;
        encoder.encode_field_section(headers.data(), headers.size(),
                                     encoded, sizeof(encoded), encoded_len);
    }

    // Benchmark
    Timer timer;
    for (size_t i = 0; i < iterations; i++) {
        Timer op_timer;

        size_t encoded_len;
        encoder.encode_field_section(headers.data(), headers.size(),
                                     encoded, sizeof(encoded), encoded_len);

        // Simulate processing body
        volatile uint64_t sum = 0;
        for (size_t j = 0; j < sizeof(body); j++) {
            sum += body[j];
        }

        latency.add(op_timer.elapsed_us());
    }
    double duration = timer.elapsed_sec();

    latency.compute(result.latency_p50_us, result.latency_p95_us,
                   result.latency_p99_us, result.avg_latency_us);

    result.throughput_rps = iterations / duration;
    result.total_operations = iterations;
    result.duration_sec = duration;

    size_t bytes_per_op = sizeof(body) + 200;
    result.throughput_mbps = (iterations * bytes_per_op) / (duration * 1024 * 1024);

    return result;
}

BenchmarkResult bench_large_response_throughput() {
    BenchmarkResult result{};
    result.name = "Large Response (64KB body)";

    QPACKEncoder encoder;
    encoder.set_huffman_encoding(false);
    LatencyStats latency;

    std::vector<std::pair<std::string_view, std::string_view>> headers = {
        {":status", "200"},
        {"content-type", "application/octet-stream"},
        {"content-length", "65536"},
    };

    uint8_t encoded[4096];
    uint8_t body[65536];
    g_random.fill_random(body, sizeof(body));

    const size_t iterations = 10000;

    // Warm up
    for (int i = 0; i < 100; i++) {
        size_t encoded_len;
        encoder.encode_field_section(headers.data(), headers.size(),
                                     encoded, sizeof(encoded), encoded_len);
    }

    // Benchmark
    Timer timer;
    for (size_t i = 0; i < iterations; i++) {
        Timer op_timer;

        size_t encoded_len;
        encoder.encode_field_section(headers.data(), headers.size(),
                                     encoded, sizeof(encoded), encoded_len);

        // Simulate sending body
        volatile uint64_t checksum = 0;
        for (size_t j = 0; j < sizeof(body); j += 64) {
            checksum += body[j];
        }

        latency.add(op_timer.elapsed_us());
    }
    double duration = timer.elapsed_sec();

    latency.compute(result.latency_p50_us, result.latency_p95_us,
                   result.latency_p99_us, result.avg_latency_us);

    result.throughput_rps = iterations / duration;
    result.total_operations = iterations;
    result.duration_sec = duration;

    size_t bytes_per_op = sizeof(body) + 200;
    result.throughput_mbps = (iterations * bytes_per_op) / (duration * 1024 * 1024);

    return result;
}

// =============================================================================
// Load Testing Scenarios
// =============================================================================

BenchmarkResult bench_sustained_load() {
    BenchmarkResult result{};
    result.name = "Sustained Load (10k requests over 10s)";

    QPACKEncoder encoder;
    encoder.set_huffman_encoding(false);
    LatencyStats latency;

    std::vector<std::pair<std::string_view, std::string_view>> headers = {
        {":method", "GET"},
        {":scheme", "https"},
        {":authority", "localhost"},
        {":path", "/api/endpoint"},
    };

    uint8_t encoded[2048];

    const size_t target_ops = 10000;
    const double target_duration = 10.0;  // seconds

    Timer timer;
    size_t ops_completed = 0;

    while (timer.elapsed_sec() < target_duration && ops_completed < target_ops) {
        Timer op_timer;

        size_t encoded_len;
        encoder.encode_field_section(headers.data(), headers.size(),
                                     encoded, sizeof(encoded), encoded_len);

        latency.add(op_timer.elapsed_us());
        ops_completed++;

        // Rate limiting to distribute load
        if (ops_completed % 100 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    double duration = timer.elapsed_sec();

    latency.compute(result.latency_p50_us, result.latency_p95_us,
                   result.latency_p99_us, result.avg_latency_us);

    result.throughput_rps = ops_completed / duration;
    result.total_operations = ops_completed;
    result.duration_sec = duration;

    return result;
}

BenchmarkResult bench_burst_load() {
    BenchmarkResult result{};
    result.name = "Burst Load (1k requests in 100ms)";

    QPACKEncoder encoder;
    encoder.set_huffman_encoding(false);
    LatencyStats latency;

    std::vector<std::pair<std::string_view, std::string_view>> headers = {
        {":method", "POST"},
        {":scheme", "https"},
        {":authority", "api.example.com"},
        {":path", "/burst"},
    };

    uint8_t encoded[2048];
    const size_t burst_size = 1000;

    // Warm up
    for (int i = 0; i < 100; i++) {
        size_t encoded_len;
        encoder.encode_field_section(headers.data(), headers.size(),
                                     encoded, sizeof(encoded), encoded_len);
    }

    // Execute burst
    Timer timer;
    for (size_t i = 0; i < burst_size; i++) {
        Timer op_timer;

        size_t encoded_len;
        encoder.encode_field_section(headers.data(), headers.size(),
                                     encoded, sizeof(encoded), encoded_len);

        latency.add(op_timer.elapsed_us());
    }
    double duration = timer.elapsed_sec();

    latency.compute(result.latency_p50_us, result.latency_p95_us,
                   result.latency_p99_us, result.avg_latency_us);

    result.throughput_rps = burst_size / duration;
    result.total_operations = burst_size;
    result.duration_sec = duration;

    return result;
}

BenchmarkResult bench_mixed_workload() {
    BenchmarkResult result{};
    result.name = "Mixed Workload (70% GET, 20% POST, 10% large)";

    QPACKEncoder encoder;
    encoder.set_huffman_encoding(false);
    LatencyStats latency;

    // Prepare different request types
    std::vector<std::pair<std::string_view, std::string_view>> get_headers = {
        {":method", "GET"}, {":scheme", "https"}, {":authority", "api.com"}, {":path", "/data"}
    };

    std::vector<std::pair<std::string_view, std::string_view>> post_headers = {
        {":method", "POST"}, {":scheme", "https"}, {":authority", "api.com"},
        {":path", "/submit"}, {"content-length", "512"}
    };

    std::vector<std::pair<std::string_view, std::string_view>> large_headers = {
        {":status", "200"}, {"content-type", "application/json"}, {"content-length", "32768"}
    };

    uint8_t encoded[4096];
    const size_t iterations = 10000;

    // Warm up
    for (int i = 0; i < 100; i++) {
        size_t encoded_len;
        encoder.encode_field_section(get_headers.data(), get_headers.size(),
                                     encoded, sizeof(encoded), encoded_len);
    }

    // Benchmark
    Timer timer;
    for (size_t i = 0; i < iterations; i++) {
        Timer op_timer;

        int request_type = g_random.random_int(1, 100);
        size_t encoded_len;

        if (request_type <= 70) {
            // GET request (70%)
            encoder.encode_field_section(get_headers.data(), get_headers.size(),
                                         encoded, sizeof(encoded), encoded_len);
        } else if (request_type <= 90) {
            // POST request (20%)
            encoder.encode_field_section(post_headers.data(), post_headers.size(),
                                         encoded, sizeof(encoded), encoded_len);
        } else {
            // Large response (10%)
            encoder.encode_field_section(large_headers.data(), large_headers.size(),
                                         encoded, sizeof(encoded), encoded_len);
        }

        latency.add(op_timer.elapsed_us());
    }
    double duration = timer.elapsed_sec();

    latency.compute(result.latency_p50_us, result.latency_p95_us,
                   result.latency_p99_us, result.avg_latency_us);

    result.throughput_rps = iterations / duration;
    result.total_operations = iterations;
    result.duration_sec = duration;

    return result;
}

// =============================================================================
// TechEmpower-Style Benchmarks
// =============================================================================

BenchmarkResult bench_json_serialization() {
    BenchmarkResult result{};
    result.name = "JSON Response (TechEmpower-style)";

    QPACKEncoder encoder;
    encoder.set_huffman_encoding(false);
    LatencyStats latency;

    std::vector<std::pair<std::string_view, std::string_view>> headers = {
        {":status", "200"},
        {"content-type", "application/json"},
        {"content-length", "27"},
    };

    const char* json_body = "{\"message\":\"Hello, World!\"}";
    uint8_t encoded[2048];

    const size_t iterations = 100000;

    // Warm up
    for (int i = 0; i < 1000; i++) {
        size_t encoded_len;
        encoder.encode_field_section(headers.data(), headers.size(),
                                     encoded, sizeof(encoded), encoded_len);
    }

    // Benchmark
    Timer timer;
    for (size_t i = 0; i < iterations; i++) {
        Timer op_timer;

        size_t encoded_len;
        encoder.encode_field_section(headers.data(), headers.size(),
                                     encoded, sizeof(encoded), encoded_len);

        // Simulate JSON serialization
        volatile size_t len = strlen(json_body);

        latency.add(op_timer.elapsed_us());
    }
    double duration = timer.elapsed_sec();

    latency.compute(result.latency_p50_us, result.latency_p95_us,
                   result.latency_p99_us, result.avg_latency_us);

    result.throughput_rps = iterations / duration;
    result.total_operations = iterations;
    result.duration_sec = duration;

    return result;
}

BenchmarkResult bench_plaintext_response() {
    BenchmarkResult result{};
    result.name = "Plaintext Response (TechEmpower)";

    QPACKEncoder encoder;
    encoder.set_huffman_encoding(false);
    LatencyStats latency;

    std::vector<std::pair<std::string_view, std::string_view>> headers = {
        {":status", "200"},
        {"content-type", "text/plain"},
        {"content-length", "13"},
    };

    const char* body = "Hello, World!";
    uint8_t encoded[2048];

    const size_t iterations = 100000;

    // Warm up
    for (int i = 0; i < 1000; i++) {
        size_t encoded_len;
        encoder.encode_field_section(headers.data(), headers.size(),
                                     encoded, sizeof(encoded), encoded_len);
    }

    // Benchmark
    Timer timer;
    for (size_t i = 0; i < iterations; i++) {
        Timer op_timer;

        size_t encoded_len;
        encoder.encode_field_section(headers.data(), headers.size(),
                                     encoded, sizeof(encoded), encoded_len);

        volatile size_t len = strlen(body);

        latency.add(op_timer.elapsed_us());
    }
    double duration = timer.elapsed_sec();

    latency.compute(result.latency_p50_us, result.latency_p95_us,
                   result.latency_p99_us, result.avg_latency_us);

    result.throughput_rps = iterations / duration;
    result.total_operations = iterations;
    result.duration_sec = duration;

    return result;
}

// =============================================================================
// Comparison Benchmarks
// =============================================================================

BenchmarkResult bench_with_huffman_encoding() {
    BenchmarkResult result{};
    result.name = "With Huffman Encoding";

    QPACKEncoder encoder;
    encoder.set_huffman_encoding(true);
    LatencyStats latency;

    std::vector<std::pair<std::string_view, std::string_view>> headers = {
        {":method", "GET"},
        {":scheme", "https"},
        {":authority", "www.example.com"},
        {":path", "/very/long/path/with/many/segments"},
        {"user-agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36"},
    };

    uint8_t encoded[4096];
    const size_t iterations = 50000;

    Timer timer;
    for (size_t i = 0; i < iterations; i++) {
        Timer op_timer;

        size_t encoded_len;
        encoder.encode_field_section(headers.data(), headers.size(),
                                     encoded, sizeof(encoded), encoded_len);

        latency.add(op_timer.elapsed_us());
    }
    double duration = timer.elapsed_sec();

    latency.compute(result.latency_p50_us, result.latency_p95_us,
                   result.latency_p99_us, result.avg_latency_us);

    result.throughput_rps = iterations / duration;
    result.total_operations = iterations;
    result.duration_sec = duration;

    return result;
}

BenchmarkResult bench_without_huffman_encoding() {
    BenchmarkResult result{};
    result.name = "Without Huffman Encoding";

    QPACKEncoder encoder;
    encoder.set_huffman_encoding(false);
    LatencyStats latency;

    std::vector<std::pair<std::string_view, std::string_view>> headers = {
        {":method", "GET"},
        {":scheme", "https"},
        {":authority", "www.example.com"},
        {":path", "/very/long/path/with/many/segments"},
        {"user-agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36"},
    };

    uint8_t encoded[4096];
    const size_t iterations = 50000;

    Timer timer;
    for (size_t i = 0; i < iterations; i++) {
        Timer op_timer;

        size_t encoded_len;
        encoder.encode_field_section(headers.data(), headers.size(),
                                     encoded, sizeof(encoded), encoded_len);

        latency.add(op_timer.elapsed_us());
    }
    double duration = timer.elapsed_sec();

    latency.compute(result.latency_p50_us, result.latency_p95_us,
                   result.latency_p99_us, result.avg_latency_us);

    result.throughput_rps = iterations / duration;
    result.total_operations = iterations;
    result.duration_sec = duration;

    return result;
}

// =============================================================================
// Output and Reporting
// =============================================================================

void print_separator() {
    std::cout << "========================================================================" << std::endl;
}

void print_result(const BenchmarkResult& result) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\nBenchmark: " << result.name << std::endl;
    std::cout << "  Operations:       " << result.total_operations << std::endl;
    std::cout << "  Duration:         " << result.duration_sec << " sec" << std::endl;
    std::cout << "  Throughput:       " << result.throughput_rps << " ops/sec" << std::endl;

    if (result.throughput_mbps > 0) {
        std::cout << "  Throughput:       " << result.throughput_mbps << " MB/s" << std::endl;
    }

    std::cout << "  Latency (avg):    " << result.avg_latency_us << " us" << std::endl;
    std::cout << "  Latency (p50):    " << result.latency_p50_us << " us" << std::endl;
    std::cout << "  Latency (p95):    " << result.latency_p95_us << " us" << std::endl;
    std::cout << "  Latency (p99):    " << result.latency_p99_us << " us" << std::endl;
}

void print_summary(const std::vector<BenchmarkResult>& results) {
    print_separator();
    std::cout << "\nPERFORMANCE SUMMARY" << std::endl;
    print_separator();

    std::cout << std::fixed << std::setprecision(0);
    std::cout << "\n" << std::setw(45) << std::left << "Benchmark"
              << std::setw(15) << std::right << "RPS"
              << std::setw(12) << "P50 (us)"
              << std::setw(12) << "P99 (us)" << std::endl;
    std::cout << std::string(84, '-') << std::endl;

    for (const auto& result : results) {
        std::cout << std::setw(45) << std::left << result.name
                  << std::setw(15) << std::right << static_cast<int>(result.throughput_rps)
                  << std::setw(12) << static_cast<int>(result.latency_p50_us)
                  << std::setw(12) << static_cast<int>(result.latency_p99_us) << std::endl;
    }
}

void check_performance_targets(const std::vector<BenchmarkResult>& results) {
    print_separator();
    std::cout << "\nPERFORMANCE TARGET VALIDATION" << std::endl;
    print_separator();

    struct Target {
        std::string name;
        double min_rps;
        double max_p99_us;
        bool found;
        bool passed;
    };

    std::vector<Target> targets = {
        {"Simple GET", 100000, 1000, false, false},
        {"POST with 1KB Body", 50000, 1000, false, false},
        {"QPACK Compression", 1000000, 100, false, false},
    };

    for (auto& target : targets) {
        for (const auto& result : results) {
            if (result.name.find(target.name) != std::string::npos) {
                target.found = true;
                target.passed = (result.throughput_rps >= target.min_rps) &&
                               (result.latency_p99_us <= target.max_p99_us);

                std::cout << "\n" << target.name << ":" << std::endl;
                std::cout << "  Target:  >= " << target.min_rps << " RPS, "
                         << "<= " << target.max_p99_us << " us (P99)" << std::endl;
                std::cout << "  Actual:  " << static_cast<int>(result.throughput_rps) << " RPS, "
                         << static_cast<int>(result.latency_p99_us) << " us (P99)" << std::endl;
                std::cout << "  Status:  " << (target.passed ? "PASS" : "FAIL") << std::endl;
                break;
            }
        }
    }
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "╔════════════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║           HTTP/3 Performance and Load Tests                        ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;

    std::vector<BenchmarkResult> results;

    // QPACK Benchmarks
    std::cout << "\n=== QPACK Compression Benchmarks ===" << std::endl;
    results.push_back(bench_qpack_compression());
    print_result(results.back());

    results.push_back(bench_qpack_decompression());
    print_result(results.back());

    // Frame Parsing Benchmarks
    std::cout << "\n=== HTTP/3 Frame Parsing Benchmarks ===" << std::endl;
    results.push_back(bench_frame_header_parsing());
    print_result(results.back());

    results.push_back(bench_settings_frame_parsing());
    print_result(results.back());

    // Throughput Benchmarks
    std::cout << "\n=== Throughput Benchmarks ===" << std::endl;
    results.push_back(bench_simple_get_throughput());
    print_result(results.back());

    results.push_back(bench_post_with_body_throughput());
    print_result(results.back());

    results.push_back(bench_large_response_throughput());
    print_result(results.back());

    // Load Testing Scenarios
    std::cout << "\n=== Load Testing Scenarios ===" << std::endl;
    results.push_back(bench_sustained_load());
    print_result(results.back());

    results.push_back(bench_burst_load());
    print_result(results.back());

    results.push_back(bench_mixed_workload());
    print_result(results.back());

    // TechEmpower Benchmarks
    std::cout << "\n=== TechEmpower-Style Benchmarks ===" << std::endl;
    results.push_back(bench_json_serialization());
    print_result(results.back());

    results.push_back(bench_plaintext_response());
    print_result(results.back());

    // Comparison Benchmarks
    std::cout << "\n=== Comparison Benchmarks ===" << std::endl;
    results.push_back(bench_with_huffman_encoding());
    print_result(results.back());

    results.push_back(bench_without_huffman_encoding());
    print_result(results.back());

    // Final Summary
    print_summary(results);
    check_performance_targets(results);

    std::cout << "\n" << std::endl;
    print_separator();
    std::cout << "All HTTP/3 performance benchmarks completed!" << std::endl;
    print_separator();

    return 0;
}
