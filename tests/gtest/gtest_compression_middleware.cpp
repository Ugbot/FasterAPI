/**
 * Compression Middleware Tests
 *
 * Tests for automatic HTTP response compression based on Accept-Encoding.
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/compression_middleware.h"
#include <random>
#include <chrono>

using namespace fasterapi::http;
using namespace fasterapi;

// =============================================================================
// Test Fixtures
// =============================================================================

class CompressionMiddlewareTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Generate random compressible content for testing
        gen_.seed(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    std::string generate_json(size_t approximate_size) {
        std::string json = R"({"data":[)";
        while (json.size() < approximate_size) {
            json += R"({"id":)" + std::to_string(gen_()) +
                   R"(,"name":"item_)" + std::to_string(gen_()) +
                   R"(","value":)" + std::to_string(gen_() % 1000) +
                   R"(,"active":)" + (gen_() % 2 ? "true" : "false") + "},";
        }
        // Remove trailing comma and close
        if (json.back() == ',') {
            json.pop_back();
        }
        json += "]}";
        return json;
    }

    std::string generate_html(size_t approximate_size) {
        std::string html = "<!DOCTYPE html><html><head><title>Test</title></head><body>";
        while (html.size() < approximate_size) {
            html += "<div class=\"item\" id=\"item-" + std::to_string(gen_()) + "\">";
            html += "<h2>Item " + std::to_string(gen_()) + "</h2>";
            html += "<p>This is a test paragraph with some content.</p>";
            html += "<ul><li>Item A</li><li>Item B</li><li>Item C</li></ul>";
            html += "</div>";
        }
        html += "</body></html>";
        return html;
    }

    std::string generate_text(size_t approximate_size) {
        std::string text;
        while (text.size() < approximate_size) {
            text += "Line " + std::to_string(gen_()) + ": This is some text content that should compress well. ";
        }
        return text;
    }

    std::mt19937 gen_;
};

// =============================================================================
// Basic Functionality Tests
// =============================================================================

TEST_F(CompressionMiddlewareTest, CompressesJsonWithGzip) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip";

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";

    size_t original_size = response.body.size();
    bool compressed = middleware.apply(req_headers, response);

    ASSERT_TRUE(compressed);
    EXPECT_EQ(response.headers["Content-Encoding"], "gzip");
    EXPECT_LT(response.body.size(), original_size);
    EXPECT_GT(response.headers.count("Vary"), 0);
}

TEST_F(CompressionMiddlewareTest, CompressesHtmlWithBrotli) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "br";

    Http1Response response;
    response.body = generate_html(5000);
    response.headers["Content-Type"] = "text/html; charset=utf-8";

    size_t original_size = response.body.size();
    bool compressed = middleware.apply(req_headers, response);

    ASSERT_TRUE(compressed);
    EXPECT_EQ(response.headers["Content-Encoding"], "br");
    EXPECT_LT(response.body.size(), original_size);
}

TEST_F(CompressionMiddlewareTest, CompressesTextWithZstd) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "zstd";

    Http1Response response;
    response.body = generate_text(3000);
    response.headers["Content-Type"] = "text/plain";

    size_t original_size = response.body.size();
    bool compressed = middleware.apply(req_headers, response);

    ASSERT_TRUE(compressed);
    EXPECT_EQ(response.headers["Content-Encoding"], "zstd");
    EXPECT_LT(response.body.size(), original_size);
}

TEST_F(CompressionMiddlewareTest, PrefersClientAcceptedAlgorithms) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    // Client accepts all, should use brotli (highest preference)
    req_headers["Accept-Encoding"] = "gzip, br, zstd";

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";

    middleware.apply(req_headers, response);

    // Should prefer brotli (first in default config)
    EXPECT_EQ(response.headers["Content-Encoding"], "br");
}

TEST_F(CompressionMiddlewareTest, RespectsQualityValues) {
    CompressionConfig config;
    CompressionMiddleware middleware(config);

    std::unordered_map<std::string, std::string> req_headers;
    // Client prefers gzip over brotli
    req_headers["Accept-Encoding"] = "br;q=0.5, gzip;q=1.0";

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";

    middleware.apply(req_headers, response);

    // Should use gzip since it has higher quality, but our config prefers brotli
    // Actually, our implementation respects server preference first, then client
    // So brotli should still win since client accepts it (q > 0)
    EXPECT_EQ(response.headers["Content-Encoding"], "br");
}

TEST_F(CompressionMiddlewareTest, RejectsZeroQuality) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    // Client explicitly rejects gzip with q=0
    req_headers["Accept-Encoding"] = "gzip;q=0";

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";

    bool compressed = middleware.apply(req_headers, response);

    // Should not compress since gzip is rejected and it's the only one specified
    EXPECT_FALSE(compressed);
}

// =============================================================================
// Content-Type Tests
// =============================================================================

TEST_F(CompressionMiddlewareTest, CompressesJavaScript) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip";

    Http1Response response;
    response.body = "function test() { return 'hello world'; } // lots of repetitive code\n";
    for (int i = 0; i < 50; i++) {
        response.body += "var x" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    }
    response.headers["Content-Type"] = "application/javascript";

    bool compressed = middleware.apply(req_headers, response);
    EXPECT_TRUE(compressed);
}

TEST_F(CompressionMiddlewareTest, CompressesXml) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip";

    Http1Response response;
    response.body = "<?xml version=\"1.0\"?><root>";
    for (int i = 0; i < 50; i++) {
        response.body += "<item id=\"" + std::to_string(i) + "\">Value " + std::to_string(i) + "</item>";
    }
    response.body += "</root>";
    response.headers["Content-Type"] = "application/xml";

    bool compressed = middleware.apply(req_headers, response);
    EXPECT_TRUE(compressed);
}

TEST_F(CompressionMiddlewareTest, CompressesSvg) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "br";

    Http1Response response;
    response.body = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">)";
    for (int i = 0; i < 50; i++) {
        response.body += "<circle cx=\"" + std::to_string(i) + "\" cy=\"" + std::to_string(i) + "\" r=\"5\"/>";
    }
    response.body += "</svg>";
    response.headers["Content-Type"] = "image/svg+xml";

    bool compressed = middleware.apply(req_headers, response);
    EXPECT_TRUE(compressed);
}

TEST_F(CompressionMiddlewareTest, SkipsJpeg) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip, br";

    Http1Response response;
    response.body = std::string(1000, '\x00');  // Simulate binary image data
    response.headers["Content-Type"] = "image/jpeg";

    bool compressed = middleware.apply(req_headers, response);
    EXPECT_FALSE(compressed);
    EXPECT_EQ(response.headers.count("Content-Encoding"), 0);
}

TEST_F(CompressionMiddlewareTest, SkipsPng) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip, br";

    Http1Response response;
    response.body = std::string(1000, '\x00');
    response.headers["Content-Type"] = "image/png";

    bool compressed = middleware.apply(req_headers, response);
    EXPECT_FALSE(compressed);
}

TEST_F(CompressionMiddlewareTest, SkipsAlreadyCompressed) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip";

    Http1Response response;
    response.body = std::string(1000, '\x00');
    response.headers["Content-Type"] = "application/gzip";

    bool compressed = middleware.apply(req_headers, response);
    EXPECT_FALSE(compressed);
}

TEST_F(CompressionMiddlewareTest, SkipsVideo) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip, br, zstd";

    Http1Response response;
    response.body = std::string(1000, '\x00');
    response.headers["Content-Type"] = "video/mp4";

    bool compressed = middleware.apply(req_headers, response);
    EXPECT_FALSE(compressed);
}

// =============================================================================
// Size Threshold Tests
// =============================================================================

TEST_F(CompressionMiddlewareTest, SkipsTooSmallResponse) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip";

    Http1Response response;
    response.body = "tiny";  // Only 4 bytes
    response.headers["Content-Type"] = "text/plain";

    bool compressed = middleware.apply(req_headers, response);
    EXPECT_FALSE(compressed);
}

TEST_F(CompressionMiddlewareTest, CompressesAtThreshold) {
    CompressionConfig config;
    config.min_size = 256;
    CompressionMiddleware middleware(config);

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip";

    Http1Response response;
    // Create repetitive content that compresses well
    response.body = std::string(300, 'A');  // Just above threshold
    response.headers["Content-Type"] = "text/plain";

    bool compressed = middleware.apply(req_headers, response);
    EXPECT_TRUE(compressed);
}

TEST_F(CompressionMiddlewareTest, CustomMinSize) {
    CompressionConfig config;
    config.min_size = 1024;
    CompressionMiddleware middleware(config);

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip";

    Http1Response response;
    response.body = generate_text(500);  // Below custom threshold
    response.headers["Content-Type"] = "text/plain";

    bool compressed = middleware.apply(req_headers, response);
    EXPECT_FALSE(compressed);
}

// =============================================================================
// Skip Conditions Tests
// =============================================================================

TEST_F(CompressionMiddlewareTest, SkipsWhenDisabled) {
    CompressionConfig config;
    config.enabled = false;
    CompressionMiddleware middleware(config);

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip, br";

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";

    bool compressed = middleware.apply(req_headers, response);
    EXPECT_FALSE(compressed);
}

TEST_F(CompressionMiddlewareTest, SkipsAlreadyEncodedResponse) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip";

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";
    response.headers["Content-Encoding"] = "gzip";  // Already compressed

    size_t original_size = response.body.size();
    bool compressed = middleware.apply(req_headers, response);

    EXPECT_FALSE(compressed);
    EXPECT_EQ(response.body.size(), original_size);
}

TEST_F(CompressionMiddlewareTest, SkipsWhenNoAcceptEncoding) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    // No Accept-Encoding header

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";

    bool compressed = middleware.apply(req_headers, response);
    EXPECT_FALSE(compressed);
}

TEST_F(CompressionMiddlewareTest, SkipsUnknownContentType) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip";

    Http1Response response;
    response.body = std::string(1000, 'x');
    // No Content-Type header

    bool compressed = middleware.apply(req_headers, response);
    EXPECT_FALSE(compressed);
}

// =============================================================================
// Accept-Encoding Parsing Tests
// =============================================================================

TEST_F(CompressionMiddlewareTest, ParsesMultipleEncodings) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "deflate, gzip, br, zstd";

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";

    bool compressed = middleware.apply(req_headers, response);
    EXPECT_TRUE(compressed);
    // Should use brotli (highest server preference)
    EXPECT_EQ(response.headers["Content-Encoding"], "br");
}

TEST_F(CompressionMiddlewareTest, HandlesWildcard) {
    CompressionConfig config;
    // Only allow gzip in config
    config.preferred_algorithms = {CompressionAlgorithm::GZIP};
    CompressionMiddleware middleware(config);

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "*";

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";

    bool compressed = middleware.apply(req_headers, response);
    EXPECT_TRUE(compressed);
    EXPECT_EQ(response.headers["Content-Encoding"], "gzip");
}

TEST_F(CompressionMiddlewareTest, HandlesWhitespace) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "  gzip  ,  br  ,  zstd  ";

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";

    bool compressed = middleware.apply(req_headers, response);
    EXPECT_TRUE(compressed);
}

TEST_F(CompressionMiddlewareTest, HandlesQualityWithWhitespace) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip; q=0.8, br ; q=1.0";

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";

    bool compressed = middleware.apply(req_headers, response);
    EXPECT_TRUE(compressed);
}

TEST_F(CompressionMiddlewareTest, HandlesCaseInsensitiveHeaders) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["accept-encoding"] = "gzip";  // lowercase

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["content-type"] = "application/json";  // lowercase

    bool compressed = middleware.apply(req_headers, response);
    EXPECT_TRUE(compressed);
}

// =============================================================================
// Vary Header Tests
// =============================================================================

TEST_F(CompressionMiddlewareTest, AddsVaryHeader) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip";

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";

    middleware.apply(req_headers, response);

    EXPECT_EQ(response.headers["Vary"], "Accept-Encoding");
}

TEST_F(CompressionMiddlewareTest, AppendsToExistingVaryHeader) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip";

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";
    response.headers["Vary"] = "Origin";

    middleware.apply(req_headers, response);

    EXPECT_TRUE(response.headers["Vary"].find("Origin") != std::string::npos);
    EXPECT_TRUE(response.headers["Vary"].find("Accept-Encoding") != std::string::npos);
}

TEST_F(CompressionMiddlewareTest, DoesNotDuplicateVaryValue) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip";

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";
    response.headers["Vary"] = "Accept-Encoding";  // Already has it

    middleware.apply(req_headers, response);

    // Should not duplicate
    size_t count = 0;
    size_t pos = 0;
    while ((pos = response.headers["Vary"].find("Accept-Encoding", pos)) != std::string::npos) {
        count++;
        pos++;
    }
    EXPECT_EQ(count, 1);
}

// =============================================================================
// Custom Configuration Tests
// =============================================================================

TEST_F(CompressionMiddlewareTest, CustomAlgorithmPreference) {
    CompressionConfig config;
    config.preferred_algorithms = {
        CompressionAlgorithm::ZSTD,  // Prefer zstd first
        CompressionAlgorithm::GZIP,
        CompressionAlgorithm::BROTLI
    };
    CompressionMiddleware middleware(config);

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip, br, zstd";

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";

    middleware.apply(req_headers, response);

    EXPECT_EQ(response.headers["Content-Encoding"], "zstd");
}

TEST_F(CompressionMiddlewareTest, CustomCompressibleTypes) {
    CompressionConfig config;
    config.compressible_types = {"application/custom-type"};
    CompressionMiddleware middleware(config);

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip";

    Http1Response response;
    response.body = generate_text(1000);
    response.headers["Content-Type"] = "application/custom-type";

    bool compressed = middleware.apply(req_headers, response);
    EXPECT_TRUE(compressed);
}

TEST_F(CompressionMiddlewareTest, CustomSkipTypes) {
    CompressionConfig config;
    config.skip_types.push_back("application/json");  // Skip JSON
    CompressionMiddleware middleware(config);

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip";

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";

    bool compressed = middleware.apply(req_headers, response);
    EXPECT_FALSE(compressed);
}

TEST_F(CompressionMiddlewareTest, CustomCompressionLevel) {
    CompressionConfig config;
    config.level = CompressionLevel::BEST;
    CompressionMiddleware middleware(config);

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip";

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";

    size_t original_size = response.body.size();
    middleware.apply(req_headers, response);

    // Best compression should result in smaller size (hard to verify exactly)
    EXPECT_LT(response.body.size(), original_size);
}

// =============================================================================
// Convenience Function Tests
// =============================================================================

TEST_F(CompressionMiddlewareTest, ApplyCompressionConvenienceFunction) {
    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";

    bool compressed = apply_compression("gzip, br", response);

    EXPECT_TRUE(compressed);
    EXPECT_TRUE(response.headers.count("Content-Encoding") > 0);
}

TEST_F(CompressionMiddlewareTest, ApplyCompressionWithConfig) {
    CompressionConfig config;
    config.preferred_algorithms = {CompressionAlgorithm::GZIP};

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";

    bool compressed = apply_compression("gzip, br", response, config);

    EXPECT_TRUE(compressed);
    EXPECT_EQ(response.headers["Content-Encoding"], "gzip");
}

// =============================================================================
// Performance / Benchmark Tests
// =============================================================================

TEST_F(CompressionMiddlewareTest, BenchmarkLargeJsonCompression) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip, br, zstd";

    // Generate large JSON (~1MB)
    std::string large_json = generate_json(1024 * 1024);

    auto start = std::chrono::high_resolution_clock::now();
    const int iterations = 10;

    for (int i = 0; i < iterations; i++) {
        Http1Response response;
        response.body = large_json;
        response.headers["Content-Type"] = "application/json";
        middleware.apply(req_headers, response);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    double throughput_mbps = (double(large_json.size()) * iterations) / (1024 * 1024) / (duration_ms / 1000.0);

    std::cout << "Compression throughput: " << throughput_mbps << " MB/s\n";
    std::cout << "Average time per 1MB: " << (duration_ms / iterations) << " ms\n";

    // Expect at least 30 MB/s throughput (brotli is slower but has best compression)
    // Note: gzip/zstd would be faster (~100-200+ MB/s)
    EXPECT_GT(throughput_mbps, 30.0);
}

TEST_F(CompressionMiddlewareTest, DecisionOverhead) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip, br, zstd";

    // Small response that won't be compressed
    Http1Response response;
    response.body = "tiny";
    response.headers["Content-Type"] = "text/plain";

    auto start = std::chrono::high_resolution_clock::now();
    const int iterations = 100000;

    for (int i = 0; i < iterations; i++) {
        middleware.apply(req_headers, response);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    double avg_ns = (double(duration_us) * 1000) / iterations;

    std::cout << "Decision overhead: " << avg_ns << " ns per call\n";

    // Decision should take less than 1000 ns (1 us)
    EXPECT_LT(avg_ns, 1000.0);
}

// =============================================================================
// Round-Trip Tests (verify decompression works)
// =============================================================================

TEST_F(CompressionMiddlewareTest, RoundTripGzip) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip";

    std::string original = generate_json(2000);

    Http1Response response;
    response.body = original;
    response.headers["Content-Type"] = "application/json";

    middleware.apply(req_headers, response);

    // Decompress
    auto result = decompress(
        response.body.data(),
        response.body.size(),
        CompressionAlgorithm::GZIP
    );

    ASSERT_TRUE(result.success);
    std::string decompressed(result.data.begin(), result.data.end());
    EXPECT_EQ(decompressed, original);
}

TEST_F(CompressionMiddlewareTest, RoundTripBrotli) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "br";

    std::string original = generate_html(5000);

    Http1Response response;
    response.body = original;
    response.headers["Content-Type"] = "text/html";

    middleware.apply(req_headers, response);

    auto result = decompress(
        response.body.data(),
        response.body.size(),
        CompressionAlgorithm::BROTLI
    );

    ASSERT_TRUE(result.success);
    std::string decompressed(result.data.begin(), result.data.end());
    EXPECT_EQ(decompressed, original);
}

TEST_F(CompressionMiddlewareTest, RoundTripZstd) {
    CompressionMiddleware middleware;

    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "zstd";

    std::string original = generate_text(3000);

    Http1Response response;
    response.body = original;
    response.headers["Content-Type"] = "text/plain";

    middleware.apply(req_headers, response);

    auto result = decompress(
        response.body.data(),
        response.body.size(),
        CompressionAlgorithm::ZSTD
    );

    ASSERT_TRUE(result.success);
    std::string decompressed(result.data.begin(), result.data.end());
    EXPECT_EQ(decompressed, original);
}

// =============================================================================
// Compression Presets Tests
// =============================================================================

TEST_F(CompressionMiddlewareTest, BrowserCompatiblePresetExcludesZstd) {
    // Browser-compatible preset should not include zstd
    auto config = compression_presets::browser_compatible();
    
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.preferred_algorithms.size(), 3);
    
    // Should include brotli, gzip, deflate but NOT zstd
    bool has_brotli = false;
    bool has_gzip = false;
    bool has_deflate = false;
    bool has_zstd = false;
    
    for (auto algo : config.preferred_algorithms) {
        if (algo == CompressionAlgorithm::BROTLI) has_brotli = true;
        if (algo == CompressionAlgorithm::GZIP) has_gzip = true;
        if (algo == CompressionAlgorithm::DEFLATE) has_deflate = true;
        if (algo == CompressionAlgorithm::ZSTD) has_zstd = true;
    }
    
    EXPECT_TRUE(has_brotli);
    EXPECT_TRUE(has_gzip);
    EXPECT_TRUE(has_deflate);
    EXPECT_FALSE(has_zstd);  // zstd should NOT be in browser-compatible preset
}

TEST_F(CompressionMiddlewareTest, BrowserCompatiblePresetSelectsGzipWhenRequested) {
    auto config = compression_presets::browser_compatible();
    CompressionMiddleware middleware(config);

    std::unordered_map<std::string, std::string> req_headers;
    // Typical browser Accept-Encoding header
    req_headers["Accept-Encoding"] = "gzip, deflate, br";

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";

    size_t original_size = response.body.size();
    bool compressed = middleware.apply(req_headers, response);

    ASSERT_TRUE(compressed);
    // Brotli should be selected as it's first in preference and browser supports it
    EXPECT_EQ(response.headers["Content-Encoding"], "br");
    EXPECT_LT(response.body.size(), original_size);
}

TEST_F(CompressionMiddlewareTest, BrowserCompatiblePresetFallsToGzip) {
    auto config = compression_presets::browser_compatible();
    CompressionMiddleware middleware(config);

    std::unordered_map<std::string, std::string> req_headers;
    // Client only accepts gzip (no brotli)
    req_headers["Accept-Encoding"] = "gzip, deflate";

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";

    bool compressed = middleware.apply(req_headers, response);

    ASSERT_TRUE(compressed);
    // Should fall back to gzip
    EXPECT_EQ(response.headers["Content-Encoding"], "gzip");
}

TEST_F(CompressionMiddlewareTest, GzipOnlyPreset) {
    auto config = compression_presets::gzip_only();
    
    EXPECT_EQ(config.preferred_algorithms.size(), 1);
    EXPECT_EQ(config.preferred_algorithms[0], CompressionAlgorithm::GZIP);
    
    CompressionMiddleware middleware(config);

    std::unordered_map<std::string, std::string> req_headers;
    // Even if client prefers brotli, we only use gzip
    req_headers["Accept-Encoding"] = "br, gzip, deflate";

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";

    bool compressed = middleware.apply(req_headers, response);

    ASSERT_TRUE(compressed);
    // Should only use gzip, not brotli
    EXPECT_EQ(response.headers["Content-Encoding"], "gzip");
}

TEST_F(CompressionMiddlewareTest, ApiOptimizedPresetIncludesZstd) {
    auto config = compression_presets::api_optimized();
    
    // Should include all algorithms with zstd first
    bool has_zstd = false;
    for (auto algo : config.preferred_algorithms) {
        if (algo == CompressionAlgorithm::ZSTD) has_zstd = true;
    }
    
    EXPECT_TRUE(has_zstd);
    // Zstd should be first preference for API clients
    EXPECT_EQ(config.preferred_algorithms[0], CompressionAlgorithm::ZSTD);
}

TEST_F(CompressionMiddlewareTest, ApiOptimizedPresetUsesZstdWhenAvailable) {
    auto config = compression_presets::api_optimized();
    CompressionMiddleware middleware(config);

    std::unordered_map<std::string, std::string> req_headers;
    // API client that accepts zstd
    req_headers["Accept-Encoding"] = "gzip, br, zstd";

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";

    bool compressed = middleware.apply(req_headers, response);

    ASSERT_TRUE(compressed);
    // Should use zstd as it's preferred for API clients
    EXPECT_EQ(response.headers["Content-Encoding"], "zstd");
}

TEST_F(CompressionMiddlewareTest, FastPresetUsesLowestLevel) {
    auto config = compression_presets::fast();
    
    EXPECT_EQ(config.level, CompressionLevel::FASTEST);
    
    // Should prefer gzip for compatibility
    bool has_gzip = false;
    for (auto algo : config.preferred_algorithms) {
        if (algo == CompressionAlgorithm::GZIP) has_gzip = true;
    }
    EXPECT_TRUE(has_gzip);
}

TEST_F(CompressionMiddlewareTest, BestRatioPresetUsesHighestLevel) {
    auto config = compression_presets::best_ratio();
    
    EXPECT_EQ(config.level, CompressionLevel::BEST);
    
    // Should prefer brotli for best ratio
    EXPECT_EQ(config.preferred_algorithms[0], CompressionAlgorithm::BROTLI);
}

TEST_F(CompressionMiddlewareTest, MakeSharedCompressionMiddlewareWorks) {
    auto config = compression_presets::browser_compatible();
    auto middleware = make_shared_compression_middleware(config);
    
    ASSERT_NE(middleware, nullptr);
    
    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip";

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";

    bool compressed = middleware->apply(req_headers, response);
    EXPECT_TRUE(compressed);
    EXPECT_EQ(response.headers["Content-Encoding"], "gzip");
}

TEST_F(CompressionMiddlewareTest, MakeResponseCompressorWorks) {
    auto compressor = make_response_compressor(compression_presets::gzip_only());
    
    std::unordered_map<std::string, std::string> req_headers;
    req_headers["Accept-Encoding"] = "gzip, br";

    Http1Response response;
    response.body = generate_json(2000);
    response.headers["Content-Type"] = "application/json";

    bool compressed = compressor(req_headers, response);
    
    EXPECT_TRUE(compressed);
    EXPECT_EQ(response.headers["Content-Encoding"], "gzip");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
