#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

/**
 * zstd compression middleware for HTTP responses.
 * 
 * Features:
 * - Automatic compression based on content type and size
 * - Configurable compression levels
 * - Compression statistics
 * - Zero-copy compression when possible
 * - Content-Encoding header management
 */
class CompressionHandler {
public:
    // Compression configuration
    struct Config {
        bool enabled = true;
        uint32_t threshold = 1024;  // Minimum size to compress (1KB)
        int level = 3;  // zstd compression level (1-22)
        std::vector<std::string> compressible_types = {
            "text/",
            "application/json",
            "application/javascript",
            "application/xml",
            "image/svg+xml"
        };
        std::vector<std::string> excluded_types = {
            "image/",
            "video/",
            "audio/",
            "application/zip",
            "application/gzip",
            "application/x-compress"
        };
    };

    // Compression statistics
    struct Stats {
        uint64_t total_requests{0};
        uint64_t compressed_requests{0};
        uint64_t total_bytes_in{0};
        uint64_t total_bytes_out{0};
        uint64_t bytes_saved{0};
        double avg_compression_ratio{0.0};
        uint64_t compression_time_ns{0};
    };

    /**
     * Create compression handler.
     * 
     * @param config Compression configuration
     */
    explicit CompressionHandler(const Config& config);
    
    ~CompressionHandler();

    // Non-copyable, movable
    CompressionHandler(const CompressionHandler&) = delete;
    CompressionHandler& operator=(const CompressionHandler&) = delete;
    CompressionHandler(CompressionHandler&&) noexcept;
    CompressionHandler& operator=(CompressionHandler&&) noexcept;

    /**
     * Check if content should be compressed.
     * 
     * @param content_type Content type
     * @param content_size Content size in bytes
     * @return true if should compress, false otherwise
     */
    bool should_compress(const std::string& content_type, uint64_t content_size) const noexcept;

    /**
     * Compress content.
     * 
     * @param input Input data
     * @param output Output buffer (will be resized)
     * @param level Compression level (overrides config)
     * @return Error code (0 = success)
     */
    int compress(const std::string& input, std::string& output, int level = -1) noexcept;

    /**
     * Compress binary content.
     * 
     * @param input Input data
     * @param output Output buffer (will be resized)
     * @param level Compression level (overrides config)
     * @return Error code (0 = success)
     */
    int compress(const std::vector<uint8_t>& input, std::vector<uint8_t>& output, int level = -1) noexcept;

    /**
     * Decompress content.
     * 
     * @param input Compressed data
     * @param output Output buffer (will be resized)
     * @return Error code (0 = success)
     */
    int decompress(const std::string& input, std::string& output) noexcept;

    /**
     * Decompress binary content.
     * 
     * @param input Compressed data
     * @param output Output buffer (will be resized)
     * @return Error code (0 = success)
     */
    int decompress(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) noexcept;

    /**
     * Get compression statistics.
     * 
     * @return Current statistics
     */
    Stats get_stats() const noexcept;

    /**
     * Reset statistics.
     */
    void reset_stats() noexcept;

    /**
     * Update configuration.
     * 
     * @param config New configuration
     */
    void update_config(const Config& config) noexcept;

    /**
     * Get current configuration.
     * 
     * @return Current configuration
     */
    const Config& get_config() const noexcept;

private:
    Config config_;
    mutable Stats stats_;
    
    // zstd context (reused for performance)
    void* compress_ctx_;
    void* decompress_ctx_;

    /**
     * Initialize zstd contexts.
     * 
     * @return Error code (0 = success)
     */
    int init_contexts() noexcept;

    /**
     * Cleanup zstd contexts.
     */
    void cleanup_contexts() noexcept;

    /**
     * Check if content type is compressible.
     * 
     * @param content_type Content type to check
     * @return true if compressible, false otherwise
     */
    bool is_compressible_type(const std::string& content_type) const noexcept;

    /**
     * Check if content type is excluded.
     * 
     * @param content_type Content type to check
     * @return true if excluded, false otherwise
     */
    bool is_excluded_type(const std::string& content_type) const noexcept;

    /**
     * Update statistics after compression.
     * 
     * @param input_size Input size
     * @param output_size Output size
     * @param compression_time_ns Compression time in nanoseconds
     */
    void update_stats(uint64_t input_size, uint64_t output_size, uint64_t compression_time_ns) noexcept;

    /**
     * Get current time in nanoseconds.
     * 
     * @return Current time in nanoseconds
     */
    uint64_t get_time_ns() const noexcept;
};