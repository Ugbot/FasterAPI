/**
 * Compression Unit Tests
 *
 * Tests multi-algorithm compression support:
 * - Accept-Encoding header parsing
 * - GZIP compression/decompression
 * - Brotli compression/decompression
 * - Zstd compression/decompression
 * - Content type filtering (compressible/excluded)
 * - Round-trip verification for all algorithms
 * - Compression levels
 * - Edge cases (empty input, small data)
 * - Performance benchmarks
 * - Legacy CompressionHandler API
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/compression.h"
#include <random>
#include <chrono>

using namespace fasterapi;

class CompressionTest : public ::testing::Test {
protected:
    std::mt19937 rng_;

    void SetUp() override {
        rng_.seed(std::random_device{}());
    }

    // Generate random string of given length
    std::string random_string(size_t length) {
        static const char charset[] =
            "abcdefghijklmnopqrstuvwxyz"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "0123456789"
            " \t\n";
        std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);
        std::string result;
        result.reserve(length);
        for (size_t i = 0; i < length; ++i) {
            result += charset[dist(rng_)];
        }
        return result;
    }

    // Generate compressible data (repeated patterns compress well)
    std::string generate_compressible_data(size_t length) {
        std::string pattern = "The quick brown fox jumps over the lazy dog. ";
        std::string result;
        result.reserve(length);
        while (result.size() < length) {
            result += pattern;
        }
        result.resize(length);
        return result;
    }

    // Generate random binary data
    std::vector<uint8_t> random_binary(size_t length) {
        std::uniform_int_distribution<> dist(0, 255);
        std::vector<uint8_t> result(length);
        for (size_t i = 0; i < length; ++i) {
            result[i] = static_cast<uint8_t>(dist(rng_));
        }
        return result;
    }
};

// ===========================================================================
// Configuration Tests
// ===========================================================================

TEST_F(CompressionTest, ConfigDefaults) {
    CompressionHandler::Config config;
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.threshold, 1024u);
    EXPECT_EQ(config.level, 3);
    EXPECT_FALSE(config.compressible_types.empty());
    EXPECT_FALSE(config.excluded_types.empty());
}

TEST_F(CompressionTest, CreateWithDefaultConfig) {
    CompressionHandler::Config config;
    CompressionHandler handler(config);

    auto retrieved = handler.get_config();
    EXPECT_EQ(retrieved.enabled, config.enabled);
    EXPECT_EQ(retrieved.threshold, config.threshold);
    EXPECT_EQ(retrieved.level, config.level);
}

TEST_F(CompressionTest, CreateWithCustomConfig) {
    CompressionHandler::Config config;
    config.enabled = false;
    config.threshold = 512;
    config.level = 9;

    CompressionHandler handler(config);

    auto retrieved = handler.get_config();
    EXPECT_FALSE(retrieved.enabled);
    EXPECT_EQ(retrieved.threshold, 512u);
    EXPECT_EQ(retrieved.level, 9);
}

TEST_F(CompressionTest, UpdateConfig) {
    CompressionHandler::Config config;
    CompressionHandler handler(config);

    CompressionHandler::Config new_config;
    new_config.enabled = false;
    new_config.threshold = 2048;
    new_config.level = 15;

    handler.update_config(new_config);

    auto retrieved = handler.get_config();
    EXPECT_FALSE(retrieved.enabled);
    EXPECT_EQ(retrieved.threshold, 2048u);
    EXPECT_EQ(retrieved.level, 15);
}

// ===========================================================================
// Content Type Filtering Tests
// ===========================================================================

TEST_F(CompressionTest, ShouldCompressTextTypes) {
    CompressionHandler::Config config;
    CompressionHandler handler(config);

    // Text types should be compressible
    EXPECT_TRUE(handler.should_compress("text/html", 2000));
    EXPECT_TRUE(handler.should_compress("text/plain", 2000));
    EXPECT_TRUE(handler.should_compress("text/css", 2000));
    EXPECT_TRUE(handler.should_compress("text/javascript", 2000));
}

TEST_F(CompressionTest, ShouldCompressJsonXml) {
    CompressionHandler::Config config;
    CompressionHandler handler(config);

    EXPECT_TRUE(handler.should_compress("application/json", 2000));
    EXPECT_TRUE(handler.should_compress("application/javascript", 2000));
    EXPECT_TRUE(handler.should_compress("application/xml", 2000));
    EXPECT_TRUE(handler.should_compress("image/svg+xml", 2000));
}

TEST_F(CompressionTest, ShouldNotCompressImages) {
    CompressionHandler::Config config;
    CompressionHandler handler(config);

    // Binary image types should not be compressed (already compressed)
    EXPECT_FALSE(handler.should_compress("image/png", 2000));
    EXPECT_FALSE(handler.should_compress("image/jpeg", 2000));
    EXPECT_FALSE(handler.should_compress("image/gif", 2000));
    EXPECT_FALSE(handler.should_compress("image/webp", 2000));
}

TEST_F(CompressionTest, ShouldNotCompressMediaTypes) {
    CompressionHandler::Config config;
    CompressionHandler handler(config);

    // Media types are already compressed
    EXPECT_FALSE(handler.should_compress("video/mp4", 2000));
    EXPECT_FALSE(handler.should_compress("audio/mpeg", 2000));
}

TEST_F(CompressionTest, ShouldNotCompressCompressedFormats) {
    CompressionHandler::Config config;
    CompressionHandler handler(config);

    // Already compressed formats
    EXPECT_FALSE(handler.should_compress("application/zip", 2000));
    EXPECT_FALSE(handler.should_compress("application/gzip", 2000));
    EXPECT_FALSE(handler.should_compress("application/x-compress", 2000));
}

TEST_F(CompressionTest, ShouldNotCompressBelowThreshold) {
    CompressionHandler::Config config;
    config.threshold = 1024;
    CompressionHandler handler(config);

    // Below threshold should not compress
    EXPECT_FALSE(handler.should_compress("text/html", 500));
    EXPECT_FALSE(handler.should_compress("application/json", 100));

    // At or above threshold should compress
    EXPECT_TRUE(handler.should_compress("text/html", 1024));
    EXPECT_TRUE(handler.should_compress("application/json", 2000));
}

TEST_F(CompressionTest, ShouldNotCompressWhenDisabled) {
    CompressionHandler::Config config;
    config.enabled = false;
    CompressionHandler handler(config);

    EXPECT_FALSE(handler.should_compress("text/html", 10000));
    EXPECT_FALSE(handler.should_compress("application/json", 10000));
}

// ===========================================================================
// Compression/Decompression Tests
// ===========================================================================

TEST_F(CompressionTest, CompressDecompressString) {
    CompressionHandler::Config config;
    CompressionHandler handler(config);

    std::string original = generate_compressible_data(10000);
    std::string compressed;
    std::string decompressed;

    int comp_result = handler.compress(original, compressed);
    EXPECT_EQ(comp_result, 0);
    EXPECT_LT(compressed.size(), original.size());  // Should be smaller

    int decomp_result = handler.decompress(compressed, decompressed);
    EXPECT_EQ(decomp_result, 0);
    EXPECT_EQ(decompressed, original);  // Should match original
}

TEST_F(CompressionTest, CompressDecompressRandomData) {
    CompressionHandler::Config config;
    CompressionHandler handler(config);

    for (int i = 0; i < 20; ++i) {
        std::uniform_int_distribution<> len_dist(100, 5000);
        std::string original = random_string(len_dist(rng_));

        std::string compressed;
        std::string decompressed;

        int comp_result = handler.compress(original, compressed);
        EXPECT_EQ(comp_result, 0);

        int decomp_result = handler.decompress(compressed, decompressed);
        EXPECT_EQ(decomp_result, 0);
        EXPECT_EQ(decompressed, original);
    }
}

TEST_F(CompressionTest, CompressionLevels) {
    CompressionHandler::Config config;
    CompressionHandler handler(config);

    std::string original = generate_compressible_data(50000);
    std::string compressed_low, compressed_high;

    // Low compression level (fast, larger output)
    int result_low = handler.compress(original, compressed_low, 1);
    EXPECT_EQ(result_low, 0);

    // High compression level (slow, smaller output)
    int result_high = handler.compress(original, compressed_high, 19);
    EXPECT_EQ(result_high, 0);

    // Higher level should produce smaller output
    EXPECT_LE(compressed_high.size(), compressed_low.size());

    // Both should decompress correctly
    std::string decompressed;
    EXPECT_EQ(handler.decompress(compressed_low, decompressed), 0);
    EXPECT_EQ(decompressed, original);

    EXPECT_EQ(handler.decompress(compressed_high, decompressed), 0);
    EXPECT_EQ(decompressed, original);
}

TEST_F(CompressionTest, DefaultCompressionLevel) {
    CompressionHandler::Config config;
    config.level = 10;
    CompressionHandler handler(config);

    std::string original = generate_compressible_data(10000);
    std::string compressed;

    // Using -1 should use config level
    int result = handler.compress(original, compressed, -1);
    EXPECT_EQ(result, 0);
    EXPECT_LT(compressed.size(), original.size());
}

// ===========================================================================
// Statistics Tests
// ===========================================================================

TEST_F(CompressionTest, InitialStatsZero) {
    CompressionHandler::Config config;
    CompressionHandler handler(config);

    auto stats = handler.get_stats();
    EXPECT_EQ(stats.total_requests, 0u);
    EXPECT_EQ(stats.compressed_requests, 0u);
    EXPECT_EQ(stats.total_bytes_in, 0u);
    EXPECT_EQ(stats.total_bytes_out, 0u);
    EXPECT_EQ(stats.bytes_saved, 0u);
}

TEST_F(CompressionTest, StatsAfterCompression) {
    CompressionHandler::Config config;
    CompressionHandler handler(config);

    std::string original = generate_compressible_data(10000);
    std::string compressed;
    handler.compress(original, compressed);

    auto stats = handler.get_stats();
    EXPECT_EQ(stats.compressed_requests, 1u);
    EXPECT_EQ(stats.total_bytes_in, original.size());
    EXPECT_EQ(stats.total_bytes_out, compressed.size());
    EXPECT_GT(stats.bytes_saved, 0u);
    // Note: avg_compression_ratio is not tracked by the simplified legacy handler
}

TEST_F(CompressionTest, StatsAccumulate) {
    CompressionHandler::Config config;
    CompressionHandler handler(config);

    for (int i = 0; i < 10; ++i) {
        std::string original = generate_compressible_data(1000 * (i + 1));
        std::string compressed;
        handler.compress(original, compressed);
    }

    auto stats = handler.get_stats();
    EXPECT_EQ(stats.compressed_requests, 10u);
    EXPECT_GT(stats.total_bytes_in, 0u);
    EXPECT_GT(stats.bytes_saved, 0u);
}

TEST_F(CompressionTest, ResetStats) {
    CompressionHandler::Config config;
    CompressionHandler handler(config);

    std::string original = generate_compressible_data(5000);
    std::string compressed;
    handler.compress(original, compressed);

    auto stats_before = handler.get_stats();
    EXPECT_GT(stats_before.compressed_requests, 0u);

    handler.reset_stats();

    auto stats_after = handler.get_stats();
    EXPECT_EQ(stats_after.compressed_requests, 0u);
    EXPECT_EQ(stats_after.total_bytes_in, 0u);
    EXPECT_EQ(stats_after.bytes_saved, 0u);
}

// ===========================================================================
// Edge Cases
// ===========================================================================

TEST_F(CompressionTest, EmptyInput) {
    CompressionHandler::Config config;
    CompressionHandler handler(config);

    std::string empty;
    std::string compressed;
    std::string decompressed;

    int comp_result = handler.compress(empty, compressed);
    EXPECT_EQ(comp_result, 0);

    int decomp_result = handler.decompress(compressed, decompressed);
    EXPECT_EQ(decomp_result, 0);
    EXPECT_TRUE(decompressed.empty());
}

TEST_F(CompressionTest, SmallInput) {
    CompressionHandler::Config config;
    CompressionHandler handler(config);

    std::string small = "x";
    std::string compressed;
    std::string decompressed;

    int comp_result = handler.compress(small, compressed);
    EXPECT_EQ(comp_result, 0);

    int decomp_result = handler.decompress(compressed, decompressed);
    EXPECT_EQ(decomp_result, 0);
    EXPECT_EQ(decompressed, small);
}

TEST_F(CompressionTest, LargeInput) {
    CompressionHandler::Config config;
    CompressionHandler handler(config);

    // 1MB of data
    std::string large = generate_compressible_data(1024 * 1024);
    std::string compressed;
    std::string decompressed;

    int comp_result = handler.compress(large, compressed);
    EXPECT_EQ(comp_result, 0);
    EXPECT_LT(compressed.size(), large.size());

    int decomp_result = handler.decompress(compressed, decompressed);
    EXPECT_EQ(decomp_result, 0);
    EXPECT_EQ(decompressed.size(), large.size());
    EXPECT_EQ(decompressed, large);
}

// ===========================================================================
// Move Semantics Tests
// ===========================================================================

TEST_F(CompressionTest, MoveConstruct) {
    CompressionHandler::Config config;
    config.level = 7;
    CompressionHandler handler1(config);

    std::string original = generate_compressible_data(5000);
    std::string compressed;
    handler1.compress(original, compressed);

    // Move construct
    CompressionHandler handler2(std::move(handler1));

    // New handler should work
    std::string decompressed;
    int result = handler2.decompress(compressed, decompressed);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(decompressed, original);
}

TEST_F(CompressionTest, MoveAssign) {
    CompressionHandler::Config config1;
    config1.level = 5;
    CompressionHandler handler1(config1);

    CompressionHandler::Config config2;
    config2.level = 10;
    CompressionHandler handler2(config2);

    std::string original = generate_compressible_data(5000);
    std::string compressed;
    handler1.compress(original, compressed);

    // Move assign
    handler2 = std::move(handler1);

    // handler2 should work with moved data
    std::string decompressed;
    int result = handler2.decompress(compressed, decompressed);
    EXPECT_EQ(result, 0);
    EXPECT_EQ(decompressed, original);
}

// ===========================================================================
// Performance Tests
// ===========================================================================

TEST_F(CompressionTest, CompressionPerformance) {
    CompressionHandler::Config config;
    config.level = 3;
    CompressionHandler handler(config);

    std::string original = generate_compressible_data(100000);  // 100KB
    std::string compressed;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 1000; ++i) {
        compressed.clear();
        handler.compress(original, compressed);
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    double us_per_op = us / 1000.0;
    double mb_per_sec = (original.size() * 1000.0 / 1024.0 / 1024.0) / (us / 1000000.0);

    printf("Compression (100KB, level 3): %.0f us/op, %.1f MB/s\n", us_per_op, mb_per_sec);
    EXPECT_LT(us_per_op, 10000);  // Should be under 10ms per 100KB
}

TEST_F(CompressionTest, DecompressionPerformance) {
    CompressionHandler::Config config;
    CompressionHandler handler(config);

    std::string original = generate_compressible_data(100000);  // 100KB
    std::string compressed;
    handler.compress(original, compressed);

    std::string decompressed;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 1000; ++i) {
        decompressed.clear();
        handler.decompress(compressed, decompressed);
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    double us_per_op = us / 1000.0;
    double mb_per_sec = (original.size() * 1000.0 / 1024.0 / 1024.0) / (us / 1000000.0);

    printf("Decompression (100KB): %.0f us/op, %.1f MB/s\n", us_per_op, mb_per_sec);
    EXPECT_LT(us_per_op, 5000);  // Decompression should be faster
}

TEST_F(CompressionTest, CompressionRatio) {
    CompressionHandler::Config config;
    CompressionHandler handler(config);

    // Highly compressible data
    std::string original = generate_compressible_data(100000);
    std::string compressed;
    handler.compress(original, compressed);

    double ratio = static_cast<double>(compressed.size()) / original.size();
    printf("Compression ratio (compressible): %.2f (%.0f%% reduction)\n",
           ratio, (1.0 - ratio) * 100);

    // Should achieve at least 50% compression for repetitive data
    EXPECT_LT(ratio, 0.5);
}

// ===========================================================================
// Accept-Encoding Parsing Tests
// ===========================================================================

TEST_F(CompressionTest, ParseAcceptEncodingSimple) {
    EXPECT_EQ(parse_accept_encoding("gzip"), CompressionAlgorithm::GZIP);
    EXPECT_EQ(parse_accept_encoding("br"), CompressionAlgorithm::BROTLI);
    EXPECT_EQ(parse_accept_encoding("deflate"), CompressionAlgorithm::DEFLATE);
    EXPECT_EQ(parse_accept_encoding("zstd"), CompressionAlgorithm::ZSTD);
}

TEST_F(CompressionTest, ParseAcceptEncodingMultiple) {
    // br has highest priority
    EXPECT_EQ(parse_accept_encoding("gzip, br"), CompressionAlgorithm::BROTLI);
    EXPECT_EQ(parse_accept_encoding("br, gzip"), CompressionAlgorithm::BROTLI);
    EXPECT_EQ(parse_accept_encoding("gzip, deflate, br"), CompressionAlgorithm::BROTLI);
    EXPECT_EQ(parse_accept_encoding("zstd, gzip, deflate, br"), CompressionAlgorithm::BROTLI);
}

TEST_F(CompressionTest, ParseAcceptEncodingWithQuality) {
    // Quality values override default priority
    EXPECT_EQ(parse_accept_encoding("gzip;q=1.0, br;q=0.5"), CompressionAlgorithm::GZIP);
    EXPECT_EQ(parse_accept_encoding("br;q=0.8, gzip;q=0.9"), CompressionAlgorithm::GZIP);
    EXPECT_EQ(parse_accept_encoding("zstd;q=1.0, br;q=0.9, gzip;q=0.8"), CompressionAlgorithm::ZSTD);
}

TEST_F(CompressionTest, ParseAcceptEncodingZeroQuality) {
    // q=0 means "not acceptable"
    EXPECT_EQ(parse_accept_encoding("br;q=0, gzip"), CompressionAlgorithm::GZIP);
    EXPECT_EQ(parse_accept_encoding("gzip;q=0, br;q=0, deflate"), CompressionAlgorithm::DEFLATE);
}

TEST_F(CompressionTest, ParseAcceptEncodingWhitespace) {
    EXPECT_EQ(parse_accept_encoding("gzip , br"), CompressionAlgorithm::BROTLI);
    EXPECT_EQ(parse_accept_encoding(" gzip, br "), CompressionAlgorithm::BROTLI);
    EXPECT_EQ(parse_accept_encoding("gzip; q=0.5, br"), CompressionAlgorithm::BROTLI);
}

TEST_F(CompressionTest, ParseAcceptEncodingEmpty) {
    EXPECT_EQ(parse_accept_encoding(""), CompressionAlgorithm::NONE);
    EXPECT_EQ(parse_accept_encoding("identity"), CompressionAlgorithm::NONE);
    EXPECT_EQ(parse_accept_encoding("unknown"), CompressionAlgorithm::NONE);
}

TEST_F(CompressionTest, ParseAcceptEncodingRealWorld) {
    // Chrome/Firefox typical header
    EXPECT_EQ(parse_accept_encoding("gzip, deflate, br"), CompressionAlgorithm::BROTLI);
    // Safari
    EXPECT_EQ(parse_accept_encoding("gzip, deflate"), CompressionAlgorithm::GZIP);
    // curl default
    EXPECT_EQ(parse_accept_encoding("gzip"), CompressionAlgorithm::GZIP);
}

// ===========================================================================
// Encoding Header Value Tests
// ===========================================================================

TEST_F(CompressionTest, EncodingHeaderValue) {
    EXPECT_STREQ(encoding_header_value(CompressionAlgorithm::GZIP), "gzip");
    EXPECT_STREQ(encoding_header_value(CompressionAlgorithm::DEFLATE), "deflate");
    EXPECT_STREQ(encoding_header_value(CompressionAlgorithm::BROTLI), "br");
    EXPECT_STREQ(encoding_header_value(CompressionAlgorithm::ZSTD), "zstd");
    EXPECT_EQ(encoding_header_value(CompressionAlgorithm::NONE), nullptr);
}

// ===========================================================================
// GZIP Compression Tests
// ===========================================================================

#ifdef FA_COMPRESSION_ENABLED

TEST_F(CompressionTest, GzipCompressDecompress) {
    std::string original = generate_compressible_data(10000);

    auto compressed = compress(original, CompressionAlgorithm::GZIP);
    EXPECT_TRUE(compressed.success);
    EXPECT_LT(compressed.compressed_size, original.size());
    EXPECT_EQ(compressed.original_size, original.size());
    EXPECT_EQ(compressed.algorithm, CompressionAlgorithm::GZIP);

    auto decompressed = decompress(
        std::string_view(compressed.data.data(), compressed.compressed_size),
        CompressionAlgorithm::GZIP
    );
    EXPECT_TRUE(decompressed.success);

    std::string result(decompressed.data.begin(), decompressed.data.end());
    EXPECT_EQ(result, original);
}

TEST_F(CompressionTest, GzipCompressionLevels) {
    std::string original = generate_compressible_data(50000);

    auto fast = compress(original, CompressionAlgorithm::GZIP, CompressionLevel::FASTEST);
    auto best = compress(original, CompressionAlgorithm::GZIP, CompressionLevel::BEST);

    EXPECT_TRUE(fast.success);
    EXPECT_TRUE(best.success);

    // Best should compress better (or equal)
    EXPECT_LE(best.compressed_size, fast.compressed_size);

    // Both should decompress correctly
    auto dec_fast = decompress(std::string_view(fast.data.data(), fast.compressed_size), CompressionAlgorithm::GZIP);
    auto dec_best = decompress(std::string_view(best.data.data(), best.compressed_size), CompressionAlgorithm::GZIP);

    EXPECT_TRUE(dec_fast.success);
    EXPECT_TRUE(dec_best.success);
    EXPECT_EQ(std::string(dec_fast.data.begin(), dec_fast.data.end()), original);
    EXPECT_EQ(std::string(dec_best.data.begin(), dec_best.data.end()), original);
}

// ===========================================================================
// Brotli Compression Tests
// ===========================================================================

TEST_F(CompressionTest, BrotliCompressDecompress) {
    std::string original = generate_compressible_data(10000);

    auto compressed = compress(original, CompressionAlgorithm::BROTLI);
    EXPECT_TRUE(compressed.success);
    EXPECT_LT(compressed.compressed_size, original.size());
    EXPECT_EQ(compressed.original_size, original.size());
    EXPECT_EQ(compressed.algorithm, CompressionAlgorithm::BROTLI);

    auto decompressed = decompress(
        std::string_view(compressed.data.data(), compressed.compressed_size),
        CompressionAlgorithm::BROTLI
    );
    EXPECT_TRUE(decompressed.success);

    std::string result(decompressed.data.begin(), decompressed.data.end());
    EXPECT_EQ(result, original);
}

TEST_F(CompressionTest, BrotliCompressionLevels) {
    std::string original = generate_compressible_data(50000);

    auto fast = compress(original, CompressionAlgorithm::BROTLI, CompressionLevel::FASTEST);
    auto best = compress(original, CompressionAlgorithm::BROTLI, CompressionLevel::BEST);

    EXPECT_TRUE(fast.success);
    EXPECT_TRUE(best.success);

    // Best should compress better
    EXPECT_LE(best.compressed_size, fast.compressed_size);

    // Both should decompress correctly
    auto dec_fast = decompress(std::string_view(fast.data.data(), fast.compressed_size), CompressionAlgorithm::BROTLI);
    auto dec_best = decompress(std::string_view(best.data.data(), best.compressed_size), CompressionAlgorithm::BROTLI);

    EXPECT_TRUE(dec_fast.success);
    EXPECT_TRUE(dec_best.success);
}

TEST_F(CompressionTest, BrotliUltraLevel) {
    std::string original = generate_compressible_data(10000);

    auto ultra = compress(original, CompressionAlgorithm::BROTLI, CompressionLevel::ULTRA);
    EXPECT_TRUE(ultra.success);

    auto decompressed = decompress(
        std::string_view(ultra.data.data(), ultra.compressed_size),
        CompressionAlgorithm::BROTLI
    );
    EXPECT_TRUE(decompressed.success);
    EXPECT_EQ(std::string(decompressed.data.begin(), decompressed.data.end()), original);
}

// ===========================================================================
// Zstd Compression Tests
// ===========================================================================

TEST_F(CompressionTest, ZstdCompressDecompress) {
    std::string original = generate_compressible_data(10000);

    auto compressed = compress(original, CompressionAlgorithm::ZSTD);
    EXPECT_TRUE(compressed.success);
    EXPECT_LT(compressed.compressed_size, original.size());
    EXPECT_EQ(compressed.original_size, original.size());
    EXPECT_EQ(compressed.algorithm, CompressionAlgorithm::ZSTD);

    auto decompressed = decompress(
        std::string_view(compressed.data.data(), compressed.compressed_size),
        CompressionAlgorithm::ZSTD
    );
    EXPECT_TRUE(decompressed.success);

    std::string result(decompressed.data.begin(), decompressed.data.end());
    EXPECT_EQ(result, original);
}

TEST_F(CompressionTest, ZstdCompressionLevels) {
    std::string original = generate_compressible_data(50000);

    auto fast = compress(original, CompressionAlgorithm::ZSTD, CompressionLevel::FASTEST);
    auto best = compress(original, CompressionAlgorithm::ZSTD, CompressionLevel::BEST);

    EXPECT_TRUE(fast.success);
    EXPECT_TRUE(best.success);

    // Best should compress better
    EXPECT_LE(best.compressed_size, fast.compressed_size);
}

// ===========================================================================
// Cross-Algorithm Tests
// ===========================================================================

TEST_F(CompressionTest, AllAlgorithmsRoundTrip) {
    std::string original = generate_compressible_data(10000);

    CompressionAlgorithm algos[] = {
        CompressionAlgorithm::GZIP,
        CompressionAlgorithm::DEFLATE,
        CompressionAlgorithm::BROTLI,
        CompressionAlgorithm::ZSTD
    };

    for (auto algo : algos) {
        auto compressed = compress(original, algo);
        ASSERT_TRUE(compressed.success) << "Compression failed for " << static_cast<int>(algo);

        auto decompressed = decompress(
            std::string_view(compressed.data.data(), compressed.compressed_size),
            algo
        );
        ASSERT_TRUE(decompressed.success) << "Decompression failed for " << static_cast<int>(algo);

        std::string result(decompressed.data.begin(), decompressed.data.end());
        EXPECT_EQ(result, original) << "Round-trip failed for " << static_cast<int>(algo);
    }
}

TEST_F(CompressionTest, CompressionRatioComparison) {
    std::string original = generate_compressible_data(100000);

    auto gzip = compress(original, CompressionAlgorithm::GZIP, CompressionLevel::DEFAULT);
    auto brotli = compress(original, CompressionAlgorithm::BROTLI, CompressionLevel::DEFAULT);
    auto zstd = compress(original, CompressionAlgorithm::ZSTD, CompressionLevel::DEFAULT);

    printf("Compression ratios (100KB, default level):\n");
    printf("  GZIP:   %.2f%% (%.0f bytes)\n", gzip.compression_ratio() * 100, (double)gzip.compressed_size);
    printf("  Brotli: %.2f%% (%.0f bytes)\n", brotli.compression_ratio() * 100, (double)brotli.compressed_size);
    printf("  Zstd:   %.2f%% (%.0f bytes)\n", zstd.compression_ratio() * 100, (double)zstd.compressed_size);

    // All should achieve reasonable compression on repetitive data
    EXPECT_LT(gzip.compression_ratio(), 0.2);
    EXPECT_LT(brotli.compression_ratio(), 0.2);
    EXPECT_LT(zstd.compression_ratio(), 0.2);
}

TEST_F(CompressionTest, AlgorithmAvailability) {
    EXPECT_TRUE(is_compression_available(CompressionAlgorithm::GZIP));
    EXPECT_TRUE(is_compression_available(CompressionAlgorithm::DEFLATE));
    EXPECT_TRUE(is_compression_available(CompressionAlgorithm::BROTLI));
    EXPECT_TRUE(is_compression_available(CompressionAlgorithm::ZSTD));
    EXPECT_FALSE(is_compression_available(CompressionAlgorithm::NONE));
}

// ===========================================================================
// Multi-Algorithm Edge Cases
// ===========================================================================

TEST_F(CompressionTest, EmptyInputAllAlgorithms) {
    std::string empty;

    CompressionAlgorithm algos[] = {
        CompressionAlgorithm::GZIP,
        CompressionAlgorithm::BROTLI,
        CompressionAlgorithm::ZSTD
    };

    for (auto algo : algos) {
        auto compressed = compress(empty, algo);
        EXPECT_TRUE(compressed.success) << "Empty compression failed for " << static_cast<int>(algo);

        auto decompressed = decompress(
            std::string_view(compressed.data.data(), compressed.compressed_size),
            algo
        );
        EXPECT_TRUE(decompressed.success) << "Empty decompression failed for " << static_cast<int>(algo);
        EXPECT_EQ(decompressed.compressed_size, 0u);
    }
}

TEST_F(CompressionTest, SmallInputAllAlgorithms) {
    std::string small = "x";

    CompressionAlgorithm algos[] = {
        CompressionAlgorithm::GZIP,
        CompressionAlgorithm::BROTLI,
        CompressionAlgorithm::ZSTD
    };

    for (auto algo : algos) {
        auto compressed = compress(small, algo);
        EXPECT_TRUE(compressed.success);

        auto decompressed = decompress(
            std::string_view(compressed.data.data(), compressed.compressed_size),
            algo
        );
        EXPECT_TRUE(decompressed.success);
        EXPECT_EQ(std::string(decompressed.data.begin(), decompressed.data.end()), small);
    }
}

TEST_F(CompressionTest, LargeInputAllAlgorithms) {
    // 1MB of data
    std::string large = generate_compressible_data(1024 * 1024);

    CompressionAlgorithm algos[] = {
        CompressionAlgorithm::GZIP,
        CompressionAlgorithm::BROTLI,
        CompressionAlgorithm::ZSTD
    };

    for (auto algo : algos) {
        auto compressed = compress(large, algo);
        ASSERT_TRUE(compressed.success) << "Large compression failed for " << static_cast<int>(algo);
        EXPECT_LT(compressed.compressed_size, large.size());

        auto decompressed = decompress(
            std::string_view(compressed.data.data(), compressed.compressed_size),
            algo
        );
        ASSERT_TRUE(decompressed.success) << "Large decompression failed for " << static_cast<int>(algo);
        EXPECT_EQ(decompressed.compressed_size, large.size());
    }
}

TEST_F(CompressionTest, RandomDataAllAlgorithms) {
    // Random data is incompressible
    std::vector<uint8_t> random = random_binary(10000);
    std::string random_str(reinterpret_cast<char*>(random.data()), random.size());

    CompressionAlgorithm algos[] = {
        CompressionAlgorithm::GZIP,
        CompressionAlgorithm::BROTLI,
        CompressionAlgorithm::ZSTD
    };

    for (auto algo : algos) {
        auto compressed = compress(random_str, algo);
        EXPECT_TRUE(compressed.success);

        // Random data may not compress well (might even expand)
        // But round-trip should still work
        auto decompressed = decompress(
            std::string_view(compressed.data.data(), compressed.compressed_size),
            algo
        );
        EXPECT_TRUE(decompressed.success);
        EXPECT_EQ(std::string(decompressed.data.begin(), decompressed.data.end()), random_str);
    }
}

// ===========================================================================
// Content Type Detection Tests (fasterapi namespace helper)
// ===========================================================================

TEST_F(CompressionTest, ContentTypeHelperTextTypes) {
    EXPECT_TRUE(should_compress_content_type("text/html"));
    EXPECT_TRUE(should_compress_content_type("text/plain"));
    EXPECT_TRUE(should_compress_content_type("text/css"));
    EXPECT_TRUE(should_compress_content_type("text/javascript"));
    EXPECT_TRUE(should_compress_content_type("text/xml"));
}

TEST_F(CompressionTest, ContentTypeHelperApplicationTypes) {
    EXPECT_TRUE(should_compress_content_type("application/json"));
    EXPECT_TRUE(should_compress_content_type("application/javascript"));
    EXPECT_TRUE(should_compress_content_type("application/xml"));
    EXPECT_TRUE(should_compress_content_type("application/ld+json"));
    EXPECT_TRUE(should_compress_content_type("application/graphql"));
    EXPECT_TRUE(should_compress_content_type("application/wasm"));
}

TEST_F(CompressionTest, ContentTypeHelperSvg) {
    EXPECT_TRUE(should_compress_content_type("image/svg+xml"));
}

TEST_F(CompressionTest, ContentTypeHelperFonts) {
    EXPECT_TRUE(should_compress_content_type("font/ttf"));
    EXPECT_TRUE(should_compress_content_type("font/otf"));
}

TEST_F(CompressionTest, ContentTypeHelperNotBinaryImages) {
    EXPECT_FALSE(should_compress_content_type("image/png"));
    EXPECT_FALSE(should_compress_content_type("image/jpeg"));
    EXPECT_FALSE(should_compress_content_type("image/gif"));
    EXPECT_FALSE(should_compress_content_type("image/webp"));
}

TEST_F(CompressionTest, ContentTypeHelperNotMedia) {
    EXPECT_FALSE(should_compress_content_type("video/mp4"));
    EXPECT_FALSE(should_compress_content_type("audio/mpeg"));
}

TEST_F(CompressionTest, ContentTypeHelperWithCharset) {
    // Should handle charset parameter
    EXPECT_TRUE(should_compress_content_type("text/html; charset=utf-8"));
    EXPECT_TRUE(should_compress_content_type("application/json; charset=utf-8"));
}

// ===========================================================================
// Multi-Algorithm Performance Tests
// ===========================================================================

TEST_F(CompressionTest, GzipPerformance) {
    std::string original = generate_compressible_data(100000);

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 100; ++i) {
        auto result = compress(original, CompressionAlgorithm::GZIP, CompressionLevel::FAST);
        ASSERT_TRUE(result.success);
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    double us_per_op = us / 100.0;
    double mb_per_sec = (original.size() * 100.0 / 1024.0 / 1024.0) / (us / 1000000.0);

    printf("GZIP compression (100KB, fast): %.0f us/op, %.1f MB/s\n", us_per_op, mb_per_sec);
}

TEST_F(CompressionTest, BrotliPerformance) {
    std::string original = generate_compressible_data(100000);

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 100; ++i) {
        auto result = compress(original, CompressionAlgorithm::BROTLI, CompressionLevel::FAST);
        ASSERT_TRUE(result.success);
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    double us_per_op = us / 100.0;
    double mb_per_sec = (original.size() * 100.0 / 1024.0 / 1024.0) / (us / 1000000.0);

    printf("Brotli compression (100KB, fast): %.0f us/op, %.1f MB/s\n", us_per_op, mb_per_sec);
}

TEST_F(CompressionTest, ZstdPerformance) {
    std::string original = generate_compressible_data(100000);

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 100; ++i) {
        auto result = compress(original, CompressionAlgorithm::ZSTD, CompressionLevel::FAST);
        ASSERT_TRUE(result.success);
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    double us_per_op = us / 100.0;
    double mb_per_sec = (original.size() * 100.0 / 1024.0 / 1024.0) / (us / 1000000.0);

    printf("Zstd compression (100KB, fast): %.0f us/op, %.1f MB/s\n", us_per_op, mb_per_sec);
}

#endif  // FA_COMPRESSION_ENABLED

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
