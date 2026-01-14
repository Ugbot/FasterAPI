#pragma once

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <atomic>
#include <chrono>
#include <unistd.h>

/**
 * High-performance lock-free logging system for FasterAPI
 *
 * Features:
 * - Lock-free design using SPSC ring buffers per thread
 * - Zero-cost when disabled (compile-time)
 * - Tagged subsystem logging (HTTP, Router, Server, etc.)
 * - Multiple log levels (DEBUG, INFO, WARN, ERROR)
 * - No mutex contention in hot path
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
 * Lock-free logger using direct writes
 * 
 * For high-performance, we use:
 * 1. Atomic level check (no lock)
 * 2. Thread-local formatting buffer
 * 3. Single write() syscall (atomic for small messages on most systems)
 * 4. No fflush() - let the OS buffer
 */
class Logger {
public:
    static Logger& instance() noexcept {
        static Logger logger;
        return logger;
    }

    void set_level(LogLevel level) noexcept {
        min_level_.store(static_cast<uint8_t>(level), std::memory_order_relaxed);
    }

    LogLevel get_level() const noexcept {
        return static_cast<LogLevel>(min_level_.load(std::memory_order_relaxed));
    }

    void set_output_fd(int fd) noexcept {
        output_fd_.store(fd, std::memory_order_relaxed);
    }

    // Lock-free log function - formats to thread-local buffer and writes
    void log(LogLevel level, const char* tag, const char* file, int line,
             const char* fmt, ...) noexcept __attribute__((format(printf, 6, 7))) {
        // Level already checked by macro, but double-check for direct calls
        if (static_cast<uint8_t>(level) < min_level_.load(std::memory_order_relaxed)) {
            return;
        }

        // Thread-local buffer - no allocation, no contention
        thread_local char buffer[4096];
        
        // Format timestamp
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        
        struct tm tm_buf;
        localtime_r(&time_t_now, &tm_buf);
        
        // Extract filename from path
        const char* filename = strrchr(file, '/');
        filename = filename ? filename + 1 : file;
        
        // Format header
        int header_len = snprintf(buffer, sizeof(buffer),
            "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] [%s] ",
            tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
            tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
            static_cast<int>(ms.count()),
            level_string(level),
            tag);
        
        if (header_len < 0 || header_len >= static_cast<int>(sizeof(buffer))) {
            return;
        }
        
        // Format message
        va_list args;
        va_start(args, fmt);
        int msg_len = vsnprintf(buffer + header_len, sizeof(buffer) - header_len - 32, fmt, args);
        va_end(args);
        
        if (msg_len < 0) {
            return;
        }
        
        int total_len = header_len + msg_len;
        if (total_len >= static_cast<int>(sizeof(buffer) - 32)) {
            total_len = sizeof(buffer) - 32;
        }
        
        // Append file:line and newline
        total_len += snprintf(buffer + total_len, sizeof(buffer) - total_len,
                              " (%s:%d)\n", filename, line);
        
        // Single write syscall - atomic for messages < PIPE_BUF (usually 4096)
        // This avoids interleaving on most systems
        int fd = output_fd_.load(std::memory_order_relaxed);
        [[maybe_unused]] ssize_t written = ::write(fd, buffer, total_len);
    }

    // Non-copyable
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    Logger() noexcept : min_level_(static_cast<uint8_t>(LogLevel::WARN)), output_fd_(2) {}
    
    static const char* level_string(LogLevel level) noexcept {
        switch (level) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::ERROR: return "ERROR";
            default:              return "?????";
        }
    }

    std::atomic<uint8_t> min_level_;
    std::atomic<int> output_fd_;  // File descriptor for output (default stderr=2)
};

} // namespace core
} // namespace fasterapi

// ============================================================================
// Keyed Debug Logging (compile-time controlled)
// ============================================================================

/**
 * Keyed debug logging - enable specific debug categories via compile defines.
 * 
 * Usage:
 *   DEBUG_LOG(HTTP1, "Connection fd=%d", fd);
 *   DEBUG_LOG(PARAMS, "Query param: %s=%s", key, value);
 *   DEBUG_LOG(ROUTING, "Matched route: %s", pattern);
 * 
 * Enable at compile time by defining:
 *   -DDEBUG_HTTP1=1      Enable HTTP/1.1 connection debugging
 *   -DDEBUG_PARAMS=1     Enable parameter extraction debugging  
 *   -DDEBUG_ROUTING=1    Enable route matching debugging
 *   -DDEBUG_BODY=1       Enable request body debugging
 *   -DDEBUG_ZMQ=1        Enable ZeroMQ IPC debugging
 *   -DDEBUG_CALLBACK=1   Enable Python callback debugging
 *   -DDEBUG_ALL=1        Enable ALL debug categories
 * 
 * When not defined, DEBUG_LOG compiles to nothing (zero overhead).
 * Output goes directly to stderr with file:line info.
 */

// Helper macro for conditional debug output
#define _DEBUG_LOG_IMPL(key, fmt, ...) \
    do { \
        fprintf(stderr, "[DEBUG:%s] %s:%d: " fmt "\n", \
                #key, __FILE__, __LINE__, ##__VA_ARGS__); \
        fflush(stderr); \
    } while(0)

// Enable all debug keys if DEBUG_ALL is set
#ifdef DEBUG_ALL
    #ifndef DEBUG_HTTP1
        #define DEBUG_HTTP1 1
    #endif
    #ifndef DEBUG_PARAMS
        #define DEBUG_PARAMS 1
    #endif
    #ifndef DEBUG_ROUTING
        #define DEBUG_ROUTING 1
    #endif
    #ifndef DEBUG_BODY
        #define DEBUG_BODY 1
    #endif
    #ifndef DEBUG_ZMQ
        #define DEBUG_ZMQ 1
    #endif
    #ifndef DEBUG_CALLBACK
        #define DEBUG_CALLBACK 1
    #endif
    #ifndef DEBUG_SERVER
        #define DEBUG_SERVER 1
    #endif
    #ifndef DEBUG_CONN
        #define DEBUG_CONN 1
    #endif
#endif

// Per-key debug macros - compile to nothing when not enabled
#if DEBUG_HTTP1
    #define DEBUG_LOG_HTTP1(fmt, ...) _DEBUG_LOG_IMPL(HTTP1, fmt, ##__VA_ARGS__)
#else
    #define DEBUG_LOG_HTTP1(fmt, ...) ((void)0)
#endif

#if DEBUG_PARAMS
    #define DEBUG_LOG_PARAMS(fmt, ...) _DEBUG_LOG_IMPL(PARAMS, fmt, ##__VA_ARGS__)
#else
    #define DEBUG_LOG_PARAMS(fmt, ...) ((void)0)
#endif

#if DEBUG_ROUTING
    #define DEBUG_LOG_ROUTING(fmt, ...) _DEBUG_LOG_IMPL(ROUTING, fmt, ##__VA_ARGS__)
#else
    #define DEBUG_LOG_ROUTING(fmt, ...) ((void)0)
#endif

#if DEBUG_BODY
    #define DEBUG_LOG_BODY(fmt, ...) _DEBUG_LOG_IMPL(BODY, fmt, ##__VA_ARGS__)
#else
    #define DEBUG_LOG_BODY(fmt, ...) ((void)0)
#endif

#if DEBUG_ZMQ
    #define DEBUG_LOG_ZMQ(fmt, ...) _DEBUG_LOG_IMPL(ZMQ, fmt, ##__VA_ARGS__)
#else
    #define DEBUG_LOG_ZMQ(fmt, ...) ((void)0)
#endif

#if DEBUG_CALLBACK
    #define DEBUG_LOG_CALLBACK(fmt, ...) _DEBUG_LOG_IMPL(CALLBACK, fmt, ##__VA_ARGS__)
#else
    #define DEBUG_LOG_CALLBACK(fmt, ...) ((void)0)
#endif

#if DEBUG_SERVER
    #define DEBUG_LOG_SERVER(fmt, ...) _DEBUG_LOG_IMPL(SERVER, fmt, ##__VA_ARGS__)
#else
    #define DEBUG_LOG_SERVER(fmt, ...) ((void)0)
#endif

#if DEBUG_CONN
    #define DEBUG_LOG_CONN(fmt, ...) _DEBUG_LOG_IMPL(CONN, fmt, ##__VA_ARGS__)
#else
    #define DEBUG_LOG_CONN(fmt, ...) ((void)0)
#endif

// Generic DEBUG_LOG macro that takes key as first argument
// Usage: DEBUG_LOG(PARAMS, "value=%s", val)
#define DEBUG_LOG(key, fmt, ...) DEBUG_LOG_##key(fmt, ##__VA_ARGS__)

// ============================================================================
// Logging Macros
// ============================================================================

#ifdef FASTERAPI_ENABLE_LOGGING

// Check log level BEFORE calling log() to avoid argument evaluation overhead
#define LOG_DEBUG(tag, fmt, ...) \
    do { if (::fasterapi::core::Logger::instance().get_level() <= ::fasterapi::core::LogLevel::DEBUG) \
        ::fasterapi::core::Logger::instance().log( \
            ::fasterapi::core::LogLevel::DEBUG, tag, __FILE__, __LINE__, fmt, ##__VA_ARGS__); \
    } while(0)

#define LOG_INFO(tag, fmt, ...) \
    do { if (::fasterapi::core::Logger::instance().get_level() <= ::fasterapi::core::LogLevel::INFO) \
        ::fasterapi::core::Logger::instance().log( \
            ::fasterapi::core::LogLevel::INFO, tag, __FILE__, __LINE__, fmt, ##__VA_ARGS__); \
    } while(0)

#define LOG_WARN(tag, fmt, ...) \
    do { if (::fasterapi::core::Logger::instance().get_level() <= ::fasterapi::core::LogLevel::WARN) \
        ::fasterapi::core::Logger::instance().log( \
            ::fasterapi::core::LogLevel::WARN, tag, __FILE__, __LINE__, fmt, ##__VA_ARGS__); \
    } while(0)

#define LOG_ERROR(tag, fmt, ...) \
    do { if (::fasterapi::core::Logger::instance().get_level() <= ::fasterapi::core::LogLevel::ERROR) \
        ::fasterapi::core::Logger::instance().log( \
            ::fasterapi::core::LogLevel::ERROR, tag, __FILE__, __LINE__, fmt, ##__VA_ARGS__); \
    } while(0)

#else

// When logging is disabled, macros compile to nothing (zero overhead)
#define LOG_DEBUG(tag, fmt, ...) ((void)0)
#define LOG_INFO(tag, fmt, ...)  ((void)0)
#define LOG_WARN(tag, fmt, ...)  ((void)0)
#define LOG_ERROR(tag, fmt, ...) ((void)0)

#endif // FASTERAPI_ENABLE_LOGGING
