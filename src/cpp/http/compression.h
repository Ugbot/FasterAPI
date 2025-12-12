#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstring>
#include <algorithm>
#include <array>
#include <cstdint>

#ifdef FA_COMPRESSION_ENABLED
#include <zstd.h>
#include <brotli/encode.h>
#include <brotli/decode.h>
#include <zlib.h>
#endif

namespace fasterapi {

// Compression algorithm enumeration
enum class CompressionAlgorithm {
    NONE = 0,
    GZIP,
    DEFLATE,
    BROTLI,
    ZSTD
};

// Quality levels for compression (1-11, where 11 is best but slowest)
enum class CompressionLevel {
    FASTEST = 1,
    FAST = 3,
    DEFAULT = 6,
    BEST = 9,
    ULTRA = 11  // Brotli only supports up to 11
};

// Result of compression operation
struct CompressionResult {
    bool success{false};
    std::vector<char> data;
    size_t original_size{0};
    size_t compressed_size{0};
    CompressionAlgorithm algorithm{CompressionAlgorithm::NONE};

    double compression_ratio() const noexcept {
        return original_size > 0 ? static_cast<double>(compressed_size) / original_size : 1.0;
    }
};

// Parse Accept-Encoding header and return preferred algorithm
inline CompressionAlgorithm parse_accept_encoding(std::string_view accept_encoding) noexcept {
    // Priority: br > gzip > deflate > zstd (zstd less widely supported in browsers)
    // Parse quality values if present

    struct EncodingQuality {
        CompressionAlgorithm algo;
        float quality;
    };

    std::array<EncodingQuality, 4> encodings = {{
        {CompressionAlgorithm::BROTLI, 0.0f},
        {CompressionAlgorithm::GZIP, 0.0f},
        {CompressionAlgorithm::DEFLATE, 0.0f},
        {CompressionAlgorithm::ZSTD, 0.0f}
    }};

    // Parse the header
    size_t pos = 0;
    while (pos < accept_encoding.size()) {
        // Skip whitespace
        while (pos < accept_encoding.size() && (accept_encoding[pos] == ' ' || accept_encoding[pos] == ',')) {
            ++pos;
        }
        if (pos >= accept_encoding.size()) break;

        // Find encoding name
        size_t name_start = pos;
        while (pos < accept_encoding.size() && accept_encoding[pos] != ',' &&
               accept_encoding[pos] != ';' && accept_encoding[pos] != ' ') {
            ++pos;
        }
        std::string_view name = accept_encoding.substr(name_start, pos - name_start);

        // Default quality is 1.0
        float quality = 1.0f;

        // Check for quality parameter
        if (pos < accept_encoding.size() && accept_encoding[pos] == ';') {
            ++pos;
            // Skip whitespace
            while (pos < accept_encoding.size() && accept_encoding[pos] == ' ') ++pos;

            // Look for q=
            if (pos + 2 < accept_encoding.size() && accept_encoding[pos] == 'q' && accept_encoding[pos + 1] == '=') {
                pos += 2;
                size_t q_start = pos;
                while (pos < accept_encoding.size() && accept_encoding[pos] != ',' && accept_encoding[pos] != ' ') {
                    ++pos;
                }
                std::string_view q_str = accept_encoding.substr(q_start, pos - q_start);
                // Simple float parse
                quality = 0.0f;
                float decimal = 0.1f;
                bool after_dot = false;
                for (char c : q_str) {
                    if (c == '.') {
                        after_dot = true;
                    } else if (c >= '0' && c <= '9') {
                        if (after_dot) {
                            quality += (c - '0') * decimal;
                            decimal *= 0.1f;
                        } else {
                            quality = quality * 10 + (c - '0');
                        }
                    }
                }
            }
        }

        // Map encoding name to algorithm
        if (name == "br") {
            encodings[0].quality = quality;
        } else if (name == "gzip") {
            encodings[1].quality = quality;
        } else if (name == "deflate") {
            encodings[2].quality = quality;
        } else if (name == "zstd") {
            encodings[3].quality = quality;
        }
    }

    // Find highest quality encoding
    CompressionAlgorithm best = CompressionAlgorithm::NONE;
    float best_quality = 0.0f;

    // Priority order when qualities are equal: br > gzip > deflate > zstd
    const int priority[] = {4, 3, 2, 1};  // br, gzip, deflate, zstd
    int best_priority = 0;

    for (size_t i = 0; i < encodings.size(); ++i) {
        if (encodings[i].quality > best_quality ||
            (encodings[i].quality == best_quality && priority[i] > best_priority)) {
            best_quality = encodings[i].quality;
            best_priority = priority[i];
            best = encodings[i].algo;
        }
    }

    return best_quality > 0.0f ? best : CompressionAlgorithm::NONE;
}

// Get Content-Encoding header value for algorithm
inline const char* encoding_header_value(CompressionAlgorithm algo) noexcept {
    switch (algo) {
        case CompressionAlgorithm::GZIP: return "gzip";
        case CompressionAlgorithm::DEFLATE: return "deflate";
        case CompressionAlgorithm::BROTLI: return "br";
        case CompressionAlgorithm::ZSTD: return "zstd";
        default: return nullptr;
    }
}

#ifdef FA_COMPRESSION_ENABLED

// GZIP compression
inline CompressionResult compress_gzip(const char* data, size_t len, int level = 6) noexcept {
    CompressionResult result;
    result.original_size = len;
    result.algorithm = CompressionAlgorithm::GZIP;

    // gzip header (10 bytes) + deflate data + trailer (8 bytes)
    uLongf compressed_bound = compressBound(static_cast<uLong>(len)) + 18;
    result.data.resize(compressed_bound);

    // Initialize zlib stream for gzip
    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));

    // windowBits = 15 + 16 for gzip format
    int ret = deflateInit2(&strm, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
        return result;
    }

    strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data));
    strm.avail_in = static_cast<uInt>(len);
    strm.next_out = reinterpret_cast<Bytef*>(result.data.data());
    strm.avail_out = static_cast<uInt>(compressed_bound);

    ret = deflate(&strm, Z_FINISH);
    deflateEnd(&strm);

    if (ret != Z_STREAM_END) {
        return result;
    }

    result.compressed_size = strm.total_out;
    result.data.resize(result.compressed_size);
    result.success = true;
    return result;
}

// GZIP decompression
inline CompressionResult decompress_gzip(const char* data, size_t len) noexcept {
    CompressionResult result;
    result.original_size = len;
    result.algorithm = CompressionAlgorithm::GZIP;

    // Estimate decompressed size (start with 4x, grow if needed)
    size_t output_size = len * 4;
    result.data.resize(output_size);

    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));

    // windowBits = 15 + 16 for gzip format
    int ret = inflateInit2(&strm, 15 + 16);
    if (ret != Z_OK) {
        return result;
    }

    strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data));
    strm.avail_in = static_cast<uInt>(len);

    size_t total_out = 0;
    do {
        if (total_out >= result.data.size()) {
            result.data.resize(result.data.size() * 2);
        }
        strm.next_out = reinterpret_cast<Bytef*>(result.data.data() + total_out);
        strm.avail_out = static_cast<uInt>(result.data.size() - total_out);

        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            inflateEnd(&strm);
            return result;
        }
        total_out = strm.total_out;
    } while (ret != Z_STREAM_END);

    inflateEnd(&strm);

    result.compressed_size = total_out;
    result.data.resize(total_out);
    result.success = true;
    return result;
}

// Brotli compression
inline CompressionResult compress_brotli(const char* data, size_t len, int quality = 6) noexcept {
    CompressionResult result;
    result.original_size = len;
    result.algorithm = CompressionAlgorithm::BROTLI;

    // Clamp quality to valid range (0-11)
    quality = std::min(11, std::max(0, quality));

    size_t compressed_bound = BrotliEncoderMaxCompressedSize(len);
    if (compressed_bound == 0) {
        compressed_bound = len + 1024;  // Fallback for very large inputs
    }
    result.data.resize(compressed_bound);

    size_t encoded_size = compressed_bound;
    BROTLI_BOOL success = BrotliEncoderCompress(
        quality,                              // quality (0-11)
        BROTLI_DEFAULT_WINDOW,               // window size
        BROTLI_DEFAULT_MODE,                 // mode (generic)
        len,                                  // input size
        reinterpret_cast<const uint8_t*>(data),  // input
        &encoded_size,                        // output size (in/out)
        reinterpret_cast<uint8_t*>(result.data.data())  // output
    );

    if (!success) {
        return result;
    }

    result.compressed_size = encoded_size;
    result.data.resize(encoded_size);
    result.success = true;
    return result;
}

// Brotli decompression
inline CompressionResult decompress_brotli(const char* data, size_t len) noexcept {
    CompressionResult result;
    result.original_size = len;
    result.algorithm = CompressionAlgorithm::BROTLI;

    // Estimate decompressed size (start with 4x, grow if needed)
    size_t output_size = len * 4;
    result.data.resize(output_size);

    BrotliDecoderState* state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (!state) {
        return result;
    }

    size_t available_in = len;
    const uint8_t* next_in = reinterpret_cast<const uint8_t*>(data);
    size_t available_out = output_size;
    uint8_t* next_out = reinterpret_cast<uint8_t*>(result.data.data());
    size_t total_out = 0;

    BrotliDecoderResult decode_result;
    do {
        decode_result = BrotliDecoderDecompressStream(
            state,
            &available_in, &next_in,
            &available_out, &next_out,
            &total_out
        );

        if (decode_result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
            size_t used = result.data.size() - available_out;
            result.data.resize(result.data.size() * 2);
            available_out = result.data.size() - used;
            next_out = reinterpret_cast<uint8_t*>(result.data.data() + used);
        }
    } while (decode_result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT);

    BrotliDecoderDestroyInstance(state);

    if (decode_result != BROTLI_DECODER_RESULT_SUCCESS) {
        return result;
    }

    result.compressed_size = total_out;
    result.data.resize(total_out);
    result.success = true;
    return result;
}

// Zstd compression
inline CompressionResult compress_zstd(const char* data, size_t len, int level = 3) noexcept {
    CompressionResult result;
    result.original_size = len;
    result.algorithm = CompressionAlgorithm::ZSTD;

    // Clamp level to valid range
    level = std::min(ZSTD_maxCLevel(), std::max(1, level));

    size_t compressed_bound = ZSTD_compressBound(len);
    result.data.resize(compressed_bound);

    size_t compressed_size = ZSTD_compress(
        result.data.data(),
        compressed_bound,
        data,
        len,
        level
    );

    if (ZSTD_isError(compressed_size)) {
        return result;
    }

    result.compressed_size = compressed_size;
    result.data.resize(compressed_size);
    result.success = true;
    return result;
}

// Zstd decompression
inline CompressionResult decompress_zstd(const char* data, size_t len) noexcept {
    CompressionResult result;
    result.original_size = len;
    result.algorithm = CompressionAlgorithm::ZSTD;

    // Get decompressed size if available in frame header
    unsigned long long decompressed_size = ZSTD_getFrameContentSize(data, len);
    size_t output_size;

    if (decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN || decompressed_size == ZSTD_CONTENTSIZE_ERROR) {
        output_size = len * 4;  // Estimate
    } else {
        output_size = static_cast<size_t>(decompressed_size);
    }

    result.data.resize(output_size);

    size_t actual_size = ZSTD_decompress(
        result.data.data(),
        output_size,
        data,
        len
    );

    if (ZSTD_isError(actual_size)) {
        return result;
    }

    result.compressed_size = actual_size;
    result.data.resize(actual_size);
    result.success = true;
    return result;
}

// Unified compression interface
inline CompressionResult compress(const char* data, size_t len, CompressionAlgorithm algo,
                                  CompressionLevel level = CompressionLevel::DEFAULT) noexcept {
    int int_level = static_cast<int>(level);

    switch (algo) {
        case CompressionAlgorithm::GZIP:
        case CompressionAlgorithm::DEFLATE:  // Use gzip for both
            return compress_gzip(data, len, std::min(9, int_level));
        case CompressionAlgorithm::BROTLI:
            return compress_brotli(data, len, std::min(11, int_level));
        case CompressionAlgorithm::ZSTD:
            return compress_zstd(data, len, int_level);
        default: {
            CompressionResult result;
            result.original_size = len;
            return result;
        }
    }
}

// Unified decompression interface
inline CompressionResult decompress(const char* data, size_t len, CompressionAlgorithm algo) noexcept {
    switch (algo) {
        case CompressionAlgorithm::GZIP:
        case CompressionAlgorithm::DEFLATE:
            return decompress_gzip(data, len);
        case CompressionAlgorithm::BROTLI:
            return decompress_brotli(data, len);
        case CompressionAlgorithm::ZSTD:
            return decompress_zstd(data, len);
        default: {
            CompressionResult result;
            result.original_size = len;
            return result;
        }
    }
}

// Convenience overloads for std::string_view
inline CompressionResult compress(std::string_view data, CompressionAlgorithm algo,
                                  CompressionLevel level = CompressionLevel::DEFAULT) noexcept {
    return compress(data.data(), data.size(), algo, level);
}

inline CompressionResult decompress(std::string_view data, CompressionAlgorithm algo) noexcept {
    return decompress(data.data(), data.size(), algo);
}

#else  // FA_COMPRESSION_ENABLED not defined

// Stub implementations when compression is disabled
inline CompressionResult compress(const char*, size_t len, CompressionAlgorithm,
                                  CompressionLevel = CompressionLevel::DEFAULT) noexcept {
    CompressionResult result;
    result.original_size = len;
    return result;
}

inline CompressionResult decompress(const char*, size_t len, CompressionAlgorithm) noexcept {
    CompressionResult result;
    result.original_size = len;
    return result;
}

inline CompressionResult compress(std::string_view data, CompressionAlgorithm algo,
                                  CompressionLevel level = CompressionLevel::DEFAULT) noexcept {
    return compress(data.data(), data.size(), algo, level);
}

inline CompressionResult decompress(std::string_view data, CompressionAlgorithm algo) noexcept {
    return decompress(data.data(), data.size(), algo);
}

#endif  // FA_COMPRESSION_ENABLED

// Check if algorithm is available at runtime
inline bool is_compression_available(CompressionAlgorithm algo) noexcept {
#ifdef FA_COMPRESSION_ENABLED
    switch (algo) {
        case CompressionAlgorithm::GZIP:
        case CompressionAlgorithm::DEFLATE:
        case CompressionAlgorithm::BROTLI:
        case CompressionAlgorithm::ZSTD:
            return true;
        default:
            return false;
    }
#else
    (void)algo;
    return false;
#endif
}

// MIME types that should be compressed
inline bool should_compress_content_type(std::string_view content_type) noexcept {
    // Extract media type (ignore parameters like charset)
    size_t semicolon = content_type.find(';');
    if (semicolon != std::string_view::npos) {
        content_type = content_type.substr(0, semicolon);
    }

    // Trim whitespace
    while (!content_type.empty() && content_type.back() == ' ') {
        content_type.remove_suffix(1);
    }

    // Text types
    if (content_type.size() >= 5 && content_type.substr(0, 5) == "text/") {
        return true;
    }

    // Application types that compress well
    if (content_type == "application/json" ||
        content_type == "application/javascript" ||
        content_type == "application/xml" ||
        content_type == "application/xhtml+xml" ||
        content_type == "application/rss+xml" ||
        content_type == "application/atom+xml" ||
        content_type == "application/x-javascript" ||
        content_type == "application/ld+json" ||
        content_type == "application/manifest+json" ||
        content_type == "application/vnd.api+json" ||
        content_type == "application/graphql" ||
        content_type == "application/wasm") {
        return true;
    }

    // SVG images
    if (content_type == "image/svg+xml") {
        return true;
    }

    // Font types (woff/woff2 are already compressed)
    if (content_type == "font/ttf" ||
        content_type == "font/otf" ||
        content_type == "application/x-font-ttf" ||
        content_type == "application/x-font-opentype") {
        return true;
    }

    return false;
}

// Minimum size worth compressing (small responses have overhead)
constexpr size_t MIN_COMPRESSION_SIZE = 256;

}  // namespace fasterapi

// Legacy class-based API for backwards compatibility
class CompressionHandler {
public:
    struct Config {
        bool enabled{true};
        uint32_t threshold{1024};
        int level{3};
        std::vector<std::string> compressible_types{
            "text/",
            "application/json",
            "application/javascript",
            "application/xml",
            "image/svg+xml"
        };
        std::vector<std::string> excluded_types{
            "image/",
            "video/",
            "audio/",
            "application/zip",
            "application/gzip",
            "application/x-compress"
        };

        Config() = default;
    };

    struct Stats {
        uint64_t total_requests{0};
        uint64_t compressed_requests{0};
        uint64_t total_bytes_in{0};
        uint64_t total_bytes_out{0};
        uint64_t bytes_saved{0};
        double avg_compression_ratio{0.0};
        uint64_t compression_time_ns{0};
    };

    CompressionHandler() : config_() {}
    explicit CompressionHandler(const Config& config) : config_(config) {}
    ~CompressionHandler() = default;

    CompressionHandler(const CompressionHandler&) = delete;
    CompressionHandler& operator=(const CompressionHandler&) = delete;
    CompressionHandler(CompressionHandler&&) noexcept = default;
    CompressionHandler& operator=(CompressionHandler&&) noexcept = default;

    bool should_compress(const std::string& content_type, uint64_t content_size) const noexcept {
        if (!config_.enabled || content_size < config_.threshold) {
            return false;
        }
        return fasterapi::should_compress_content_type(content_type);
    }

    int compress(const std::string& input, std::string& output, int level = -1) noexcept {
#ifdef FA_COMPRESSION_ENABLED
        int use_level = (level >= 0) ? level : config_.level;
        auto result = fasterapi::compress_zstd(input.data(), input.size(), use_level);
        if (result.success) {
            output.assign(result.data.begin(), result.data.end());
            stats_.total_requests++;
            stats_.compressed_requests++;
            stats_.total_bytes_in += input.size();
            stats_.total_bytes_out += result.compressed_size;
            stats_.bytes_saved += input.size() - result.compressed_size;
            return 0;
        }
        return -1;
#else
        (void)input; (void)output; (void)level;
        return -1;
#endif
    }

    int decompress(const std::string& input, std::string& output) noexcept {
#ifdef FA_COMPRESSION_ENABLED
        auto result = fasterapi::decompress_zstd(input.data(), input.size());
        if (result.success) {
            output.assign(result.data.begin(), result.data.end());
            return 0;
        }
        return -1;
#else
        (void)input; (void)output;
        return -1;
#endif
    }

    Stats get_stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_ = Stats{}; }
    void update_config(const Config& config) noexcept { config_ = config; }
    const Config& get_config() const noexcept { return config_; }

private:
    Config config_;
    mutable Stats stats_;
};
