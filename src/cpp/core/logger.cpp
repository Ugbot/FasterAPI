#include "logger.h"
#include <cstdarg>
#include <cstring>
#include <chrono>

namespace fasterapi {
namespace core {

Logger::Logger() noexcept {
    // Initialize with all tags enabled by default
    for (size_t i = 0; i < MAX_TAGS; ++i) {
        tag_filters_[i].name[0] = '\0';
        tag_filters_[i].enabled = true;
    }
}

Logger::~Logger() noexcept {
    close_output_file();
}

const char* Logger::level_to_string(LogLevel level) const noexcept {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
        default:              return "?????";
    }
}

void Logger::format_timestamp(char* buf, size_t size) const noexcept {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    struct tm tm_buf;
    localtime_r(&time_t_now, &tm_buf);

    snprintf(buf, size, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
             static_cast<int>(ms.count()));
}

void Logger::log(LogLevel level, const char* tag, const char* file, int line,
                 const char* fmt, ...) noexcept {
    // Check minimum log level
    if (static_cast<uint8_t>(level) < min_level_.load(std::memory_order_relaxed)) {
        return;
    }

    // Check if tag is enabled
    if (!is_tag_enabled(tag)) {
        return;
    }

    // Format message
    char message_buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message_buf, sizeof(message_buf), fmt, args);
    va_end(args);

    // Format timestamp
    char timestamp_buf[32];
    format_timestamp(timestamp_buf, sizeof(timestamp_buf));

    // Extract filename from path
    const char* filename = strrchr(file, '/');
    filename = filename ? filename + 1 : file;

    // Write log line
    std::lock_guard<std::mutex> lock(output_mutex_);
    fprintf(output_file_, "%s [%s] [%s] %s (%s:%d)\n",
            timestamp_buf,
            level_to_string(level),
            tag,
            message_buf,
            filename,
            line);
    fflush(output_file_);
}

void Logger::set_tag_enabled(const char* tag, bool enabled) noexcept {
    std::lock_guard<std::mutex> lock(output_mutex_);

    // Find existing tag
    for (size_t i = 0; i < tag_count_; ++i) {
        if (strcmp(tag_filters_[i].name, tag) == 0) {
            tag_filters_[i].enabled = enabled;
            return;
        }
    }

    // Add new tag if space available
    if (tag_count_ < MAX_TAGS) {
        strncpy(tag_filters_[tag_count_].name, tag, sizeof(tag_filters_[0].name) - 1);
        tag_filters_[tag_count_].name[sizeof(tag_filters_[0].name) - 1] = '\0';
        tag_filters_[tag_count_].enabled = enabled;
        ++tag_count_;
    }
}

bool Logger::is_tag_enabled(const char* tag) const noexcept {
    // If no tag filters configured, all tags are enabled
    if (tag_count_ == 0) {
        return true;
    }

    // Check if tag is in the filter list
    for (size_t i = 0; i < tag_count_; ++i) {
        if (strcmp(tag_filters_[i].name, tag) == 0) {
            return tag_filters_[i].enabled;
        }
    }

    // Tag not in list - enabled by default
    return true;
}

bool Logger::set_output_file(const char* path) noexcept {
    if (!path) {
        close_output_file();
        return true;
    }

    FILE* new_file = fopen(path, "a");
    if (!new_file) {
        return false;
    }

    std::lock_guard<std::mutex> lock(output_mutex_);

    // Close old file if we own it
    if (owns_file_ && output_file_ != stderr) {
        fclose(output_file_);
    }

    output_file_ = new_file;
    owns_file_ = true;

    return true;
}

void Logger::close_output_file() noexcept {
    std::lock_guard<std::mutex> lock(output_mutex_);

    if (owns_file_ && output_file_ != stderr) {
        fclose(output_file_);
    }

    output_file_ = stderr;
    owns_file_ = false;
}

} // namespace core
} // namespace fasterapi
