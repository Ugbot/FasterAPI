#pragma once

#include <cstdio>
#include <cstdint>
#include <ctime>
#include <atomic>
#include <mutex>
#include <thread>

/**
 * High-performance logging system for FasterAPI
 *
 * Features:
 * - Zero-cost when disabled (compile-time)
 * - Tagged subsystem logging (HTTP, Router, Server, etc.)
 * - Multiple log levels (DEBUG, INFO, WARN, ERROR)
 * - Thread-safe with minimal contention
 * - Runtime filtering by level and tag
 * - Redirectable output (stderr/file/custom sink)
 *
 * Usage:
 *   LOG_DEBUG("HTTP1", "Connection accepted: fd=%d", fd);
 *   LOG_INFO("Server", "Listening on %s:%d", host, port);
 *   LOG_WARN("Router", "No route found for path: %s", path.c_str());
 *   LOG_ERROR("Worker", "Failed to process request: %s", strerror(errno));
 *
 * Build-time control:
 *   Define FASTERAPI_ENABLE_LOGGING to enable logging
 *   If undefined, all LOG_* macros compile to nothing (zero overhead)
 */

namespace fasterapi {
namespace core {

enum class LogLevel : uint8_t {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3,
    NONE = 255  // Disable all logging
};

/**
 * Thread-safe logger singleton
 *
 * Designed for high-performance environments with minimal allocation.
 * Uses thread-local buffers and atomic operations where possible.
 */
class Logger {
public:
    /**
     * Get singleton instance
     */
    static Logger& instance() noexcept {
        static Logger logger;
        return logger;
    }

    /**
     * Log a message (called by macros, not meant for direct use)
     *
     * @param level Log level
     * @param tag Subsystem tag (e.g., "HTTP", "Router")
     * @param file Source file name
     * @param line Source line number
     * @param fmt Printf-style format string
     * @param ... Format arguments
     */
    void log(LogLevel level, const char* tag, const char* file, int line,
             const char* fmt, ...) noexcept __attribute__((format(printf, 6, 7)));

    /**
     * Set minimum log level (messages below this level are ignored)
     *
     * @param level Minimum log level
     */
    void set_level(LogLevel level) noexcept {
        min_level_.store(static_cast<uint8_t>(level), std::memory_order_relaxed);
    }

    /**
     * Get current minimum log level
     *
     * @return Current minimum log level
     */
    LogLevel get_level() const noexcept {
        return static_cast<LogLevel>(min_level_.load(std::memory_order_relaxed));
    }

    /**
     * Enable/disable a specific tag
     *
     * @param tag Tag to enable/disable (e.g., "HTTP", "Router")
     * @param enabled true to enable, false to disable
     */
    void set_tag_enabled(const char* tag, bool enabled) noexcept;

    /**
     * Check if a tag is enabled
     *
     * @param tag Tag to check
     * @return true if enabled, false otherwise
     */
    bool is_tag_enabled(const char* tag) const noexcept;

    /**
     * Redirect output to a file
     *
     * @param path File path (nullptr for stderr)
     * @return true on success, false on failure
     */
    bool set_output_file(const char* path) noexcept;

    /**
     * Close output file and revert to stderr
     */
    void close_output_file() noexcept;

    // Non-copyable, non-movable
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

private:
    Logger() noexcept;
    ~Logger() noexcept;

    std::atomic<uint8_t> min_level_{static_cast<uint8_t>(LogLevel::DEBUG)};
    std::mutex output_mutex_;  // Protects output operations
    FILE* output_file_{stderr};
    bool owns_file_{false};

    // Tag filtering (simple array for common tags)
    static constexpr size_t MAX_TAGS = 32;
    struct TagFilter {
        char name[16];
        bool enabled;
    };
    TagFilter tag_filters_[MAX_TAGS]{};
    size_t tag_count_{0};

    const char* level_to_string(LogLevel level) const noexcept;
    void format_timestamp(char* buf, size_t size) const noexcept;
};

} // namespace core
} // namespace fasterapi

// ============================================================================
// Logging Macros
// ============================================================================

#ifdef FASTERAPI_ENABLE_LOGGING

#define LOG_DEBUG(tag, fmt, ...) \
    ::fasterapi::core::Logger::instance().log( \
        ::fasterapi::core::LogLevel::DEBUG, tag, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_INFO(tag, fmt, ...) \
    ::fasterapi::core::Logger::instance().log( \
        ::fasterapi::core::LogLevel::INFO, tag, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_WARN(tag, fmt, ...) \
    ::fasterapi::core::Logger::instance().log( \
        ::fasterapi::core::LogLevel::WARN, tag, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_ERROR(tag, fmt, ...) \
    ::fasterapi::core::Logger::instance().log( \
        ::fasterapi::core::LogLevel::ERROR, tag, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#else

// When logging is disabled, macros compile to nothing (zero overhead)
#define LOG_DEBUG(tag, fmt, ...) ((void)0)
#define LOG_INFO(tag, fmt, ...)  ((void)0)
#define LOG_WARN(tag, fmt, ...)  ((void)0)
#define LOG_ERROR(tag, fmt, ...) ((void)0)

#endif // FASTERAPI_ENABLE_LOGGING
