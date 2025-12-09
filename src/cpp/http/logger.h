/**
 * @file logger.h
 * @brief Structured JSON logging system
 *
 * Provides structured logging with JSON output for observability:
 * - Log levels: DEBUG, INFO, WARN, ERROR
 * - JSON format with consistent fields
 * - Request context (request_id, method, path, status, latency)
 * - Thread-safe output
 * - Configurable output destination
 *
 * Example:
 * @code
 * auto& logger = Logger::instance();
 * logger.set_level(LogLevel::INFO);
 *
 * // Simple logging
 * logger.info("Server started", {{"port", "8080"}});
 *
 * // Request logging
 * logger.request_log(RequestLogContext{
 *     .request_id = "abc-123",
 *     .method = "GET",
 *     .path = "/api/users",
 *     .status = 200,
 *     .latency_ms = 25.5,
 *     .client_ip = "192.168.1.1"
 * });
 * @endcode
 */

#pragma once

#include <string>
#include <map>
#include <sstream>
#include <iostream>
#include <fstream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <atomic>
#include <random>
#include <array>
#include <memory>

namespace fasterapi {

/**
 * Log levels.
 */
enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3,
    NONE = 4  // Disable all logging
};

/**
 * Convert log level to string.
 */
inline const char* log_level_string(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO: return "INFO";
        case LogLevel::WARN: return "WARN";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::NONE: return "NONE";
        default: return "UNKNOWN";
    }
}

/**
 * Parse log level from string.
 */
inline LogLevel parse_log_level(const std::string& s) {
    if (s == "DEBUG" || s == "debug") return LogLevel::DEBUG;
    if (s == "INFO" || s == "info") return LogLevel::INFO;
    if (s == "WARN" || s == "warn" || s == "WARNING" || s == "warning") return LogLevel::WARN;
    if (s == "ERROR" || s == "error") return LogLevel::ERROR;
    if (s == "NONE" || s == "none") return LogLevel::NONE;
    return LogLevel::INFO; // Default
}

/**
 * Request log context - all fields for an HTTP request log entry.
 */
struct RequestLogContext {
    std::string request_id;
    std::string method;
    std::string path;
    int status = 0;
    double latency_ms = 0.0;
    std::string client_ip;
    std::string user_agent;
    size_t request_size = 0;
    size_t response_size = 0;
    std::string error;
    std::map<std::string, std::string> extra;
};

/**
 * JSON builder utility.
 */
class JsonBuilder {
public:
    JsonBuilder& add(const std::string& key, const std::string& value) {
        entries_.emplace_back(key, "\"" + escape(value) + "\"");
        return *this;
    }

    JsonBuilder& add(const std::string& key, const char* value) {
        return add(key, std::string(value));
    }

    JsonBuilder& add(const std::string& key, int value) {
        entries_.emplace_back(key, std::to_string(value));
        return *this;
    }

    JsonBuilder& add(const std::string& key, int64_t value) {
        entries_.emplace_back(key, std::to_string(value));
        return *this;
    }

    JsonBuilder& add(const std::string& key, uint64_t value) {
        entries_.emplace_back(key, std::to_string(value));
        return *this;
    }

    JsonBuilder& add(const std::string& key, size_t value) {
        entries_.emplace_back(key, std::to_string(value));
        return *this;
    }

    JsonBuilder& add(const std::string& key, double value) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(3) << value;
        entries_.emplace_back(key, oss.str());
        return *this;
    }

    JsonBuilder& add(const std::string& key, bool value) {
        entries_.emplace_back(key, value ? "true" : "false");
        return *this;
    }

    // Add if value is not empty
    JsonBuilder& add_if(const std::string& key, const std::string& value) {
        if (!value.empty()) add(key, value);
        return *this;
    }

    // Add map of extra fields
    JsonBuilder& add_map(const std::map<std::string, std::string>& m) {
        for (const auto& [k, v] : m) {
            add(k, v);
        }
        return *this;
    }

    std::string build() const {
        std::ostringstream oss;
        oss << "{";
        bool first = true;
        for (const auto& [key, value] : entries_) {
            if (!first) oss << ",";
            oss << "\"" << key << "\":" << value;
            first = false;
        }
        oss << "}";
        return oss.str();
    }

private:
    std::vector<std::pair<std::string, std::string>> entries_;

    static std::string escape(const std::string& s) {
        std::string result;
        result.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '\\': result += "\\\\"; break;
                case '"': result += "\\\""; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default:
                    if (c >= 0 && c < 32) {
                        // Control character - skip
                    } else {
                        result += c;
                    }
            }
        }
        return result;
    }
};

/**
 * Request ID generator.
 */
class RequestIdGenerator {
public:
    static RequestIdGenerator& instance() {
        static RequestIdGenerator instance;
        return instance;
    }

    std::string generate() {
        // Generate UUID v4 format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
        std::array<uint8_t, 16> bytes;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& b : bytes) {
                b = static_cast<uint8_t>(dist_(rng_));
            }
        }

        // Set version 4
        bytes[6] = (bytes[6] & 0x0F) | 0x40;
        // Set variant
        bytes[8] = (bytes[8] & 0x3F) | 0x80;

        // Format as UUID string
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (size_t i = 0; i < 16; i++) {
            if (i == 4 || i == 6 || i == 8 || i == 10) oss << '-';
            oss << std::setw(2) << static_cast<int>(bytes[i]);
        }
        return oss.str();
    }

private:
    RequestIdGenerator() : rng_(std::random_device{}()), dist_(0, 255) {}

    std::mutex mutex_;
    std::mt19937 rng_;
    std::uniform_int_distribution<int> dist_;
};

/**
 * Logger - singleton for structured JSON logging.
 */
class Logger {
public:
    static Logger& instance() {
        static Logger instance;
        return instance;
    }

    // Configuration
    void set_level(LogLevel level) {
        level_.store(static_cast<int>(level), std::memory_order_relaxed);
    }

    LogLevel level() const {
        return static_cast<LogLevel>(level_.load(std::memory_order_relaxed));
    }

    void set_output(std::ostream* os) {
        std::lock_guard<std::mutex> lock(mutex_);
        output_ = os;
    }

    void set_json_format(bool json) {
        json_format_.store(json, std::memory_order_relaxed);
    }

    // Check if level is enabled
    bool is_enabled(LogLevel level) const {
        return static_cast<int>(level) >= level_.load(std::memory_order_relaxed);
    }

    // Basic logging methods
    void debug(const std::string& message,
               const std::map<std::string, std::string>& extra = {}) {
        log(LogLevel::DEBUG, message, extra);
    }

    void info(const std::string& message,
              const std::map<std::string, std::string>& extra = {}) {
        log(LogLevel::INFO, message, extra);
    }

    void warn(const std::string& message,
              const std::map<std::string, std::string>& extra = {}) {
        log(LogLevel::WARN, message, extra);
    }

    void error(const std::string& message,
               const std::map<std::string, std::string>& extra = {}) {
        log(LogLevel::ERROR, message, extra);
    }

    // Request logging
    void request_log(const RequestLogContext& ctx) {
        if (!is_enabled(LogLevel::INFO)) return;

        std::string output;
        if (json_format_.load(std::memory_order_relaxed)) {
            JsonBuilder builder;
            builder.add("timestamp", timestamp_iso8601())
                   .add("level", "INFO")
                   .add("type", "request")
                   .add_if("request_id", ctx.request_id)
                   .add("method", ctx.method)
                   .add("path", ctx.path)
                   .add("status", ctx.status)
                   .add("latency_ms", ctx.latency_ms)
                   .add_if("client_ip", ctx.client_ip)
                   .add_if("user_agent", ctx.user_agent);

            if (ctx.request_size > 0) builder.add("request_size", ctx.request_size);
            if (ctx.response_size > 0) builder.add("response_size", ctx.response_size);
            builder.add_if("error", ctx.error);
            builder.add_map(ctx.extra);

            output = builder.build();
        } else {
            // Plain text format
            std::ostringstream oss;
            oss << timestamp_iso8601() << " INFO ";
            if (!ctx.request_id.empty()) oss << "[" << ctx.request_id << "] ";
            oss << ctx.method << " " << ctx.path << " " << ctx.status
                << " " << std::fixed << std::setprecision(2) << ctx.latency_ms << "ms";
            if (!ctx.client_ip.empty()) oss << " " << ctx.client_ip;
            if (!ctx.error.empty()) oss << " error=\"" << ctx.error << "\"";
            output = oss.str();
        }

        write_line(output);
    }

    // Generate request ID
    std::string generate_request_id() {
        return RequestIdGenerator::instance().generate();
    }

private:
    Logger()
        : output_(&std::cout),
          level_(static_cast<int>(LogLevel::INFO)),
          json_format_(true) {
        // Check environment variable for log level
        if (const char* env = std::getenv("FASTERAPI_LOG_LEVEL")) {
            set_level(parse_log_level(env));
        }
        // Check for JSON format preference
        if (const char* env = std::getenv("FASTERAPI_LOG_FORMAT")) {
            std::string fmt(env);
            json_format_.store(fmt == "json" || fmt == "JSON", std::memory_order_relaxed);
        }
    }

    void log(LogLevel level, const std::string& message,
             const std::map<std::string, std::string>& extra) {
        if (!is_enabled(level)) return;

        std::string output;
        if (json_format_.load(std::memory_order_relaxed)) {
            JsonBuilder builder;
            builder.add("timestamp", timestamp_iso8601())
                   .add("level", log_level_string(level))
                   .add("message", message)
                   .add_map(extra);
            output = builder.build();
        } else {
            // Plain text format
            std::ostringstream oss;
            oss << timestamp_iso8601() << " " << log_level_string(level) << " " << message;
            for (const auto& [k, v] : extra) {
                oss << " " << k << "=" << v;
            }
            output = oss.str();
        }

        write_line(output);
    }

    void write_line(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (output_) {
            *output_ << line << "\n";
            output_->flush();
        }
    }

    static std::string timestamp_iso8601() {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::tm tm_buf;
#ifdef _WIN32
        gmtime_s(&tm_buf, &time_t_now);
#else
        gmtime_r(&time_t_now, &tm_buf);
#endif

        std::ostringstream oss;
        oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
        return oss.str();
    }

    std::mutex mutex_;
    std::ostream* output_;
    std::atomic<int> level_;
    std::atomic<bool> json_format_;
};

/**
 * Scoped request logger - logs request on destruction with timing.
 */
class ScopedRequestLogger {
public:
    ScopedRequestLogger(const std::string& method, const std::string& path,
                        const std::string& request_id = "")
        : ctx_{} {
        ctx_.method = method;
        ctx_.path = path;
        ctx_.request_id = request_id.empty() ?
            Logger::instance().generate_request_id() : request_id;
        start_ = std::chrono::steady_clock::now();
    }

    // Set additional context
    ScopedRequestLogger& client_ip(const std::string& ip) {
        ctx_.client_ip = ip;
        return *this;
    }

    ScopedRequestLogger& user_agent(const std::string& ua) {
        ctx_.user_agent = ua;
        return *this;
    }

    ScopedRequestLogger& request_size(size_t size) {
        ctx_.request_size = size;
        return *this;
    }

    ScopedRequestLogger& extra(const std::string& key, const std::string& value) {
        ctx_.extra[key] = value;
        return *this;
    }

    // Complete the request
    void complete(int status, size_t response_size = 0, const std::string& error = "") {
        if (logged_) return;
        logged_ = true;

        auto end = std::chrono::steady_clock::now();
        ctx_.latency_ms = std::chrono::duration<double, std::milli>(end - start_).count();
        ctx_.status = status;
        ctx_.response_size = response_size;
        ctx_.error = error;

        Logger::instance().request_log(ctx_);
    }

    // Get request ID for propagation
    const std::string& request_id() const { return ctx_.request_id; }

    ~ScopedRequestLogger() {
        if (!logged_) {
            complete(500, 0, "Request handler did not complete normally");
        }
    }

private:
    RequestLogContext ctx_;
    std::chrono::steady_clock::time_point start_;
    bool logged_ = false;
};

} // namespace fasterapi
