#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <atomic>

// Forward declarations for simdjson
struct simdjson_parser;
struct simdjson_document;
struct simdjson_value;

/**
 * High-performance JSON parser using simdjson.
 * 
 * Features:
 * - SIMD-accelerated JSON parsing
 * - Zero-copy string access
 * - Streaming JSON support
 * - Error handling with detailed messages
 * - Memory-efficient parsing
 */
class JsonParser {
public:
    // JSON value types
    enum class Type {
        NULL_VALUE,
        BOOL,
        NUMBER,
        STRING,
        OBJECT,
        ARRAY
    };

    // JSON value wrapper
    class Value {
    public:
        Value() = default;
        Value(const Value& other) = default;
        Value& operator=(const Value& other) = default;
        
        Type get_type() const noexcept;
        bool is_null() const noexcept;
        bool is_bool() const noexcept;
        bool is_number() const noexcept;
        bool is_string() const noexcept;
        bool is_object() const noexcept;
        bool is_array() const noexcept;
        
        bool get_bool() const noexcept;
        double get_number() const noexcept;
        std::string get_string() const noexcept;
        
        // Object access
        bool has_field(const std::string& key) const noexcept;
        Value get_field(const std::string& key) const noexcept;
        std::vector<std::string> get_field_names() const noexcept;
        
        // Array access
        size_t get_array_size() const noexcept;
        Value get_array_element(size_t index) const noexcept;
        
        // Conversion helpers
        std::string to_string() const noexcept;
        std::unordered_map<std::string, std::string> to_string_map() const noexcept;
        std::vector<std::string> to_string_array() const noexcept;
        
    private:
        friend class JsonParser;
        std::shared_ptr<simdjson_value> value_;
        std::shared_ptr<simdjson_document> document_;
    };

    /**
     * Constructor.
     */
    JsonParser();

    /**
     * Destructor.
     */
    ~JsonParser();

    /**
     * Parse JSON from string.
     * 
     * @param json JSON string
     * @return Parsed value
     */
    Value parse(const std::string& json) noexcept;

    /**
     * Parse JSON from buffer.
     * 
     * @param data Buffer data
     * @param length Buffer length
     * @return Parsed value
     */
    Value parse(const char* data, size_t length) noexcept;

    /**
     * Parse JSON from vector.
     * 
     * @param data Vector data
     * @return Parsed value
     */
    Value parse(const std::vector<uint8_t>& data) noexcept;

    /**
     * Validate JSON without parsing.
     * 
     * @param json JSON string
     * @return true if valid, false otherwise
     */
    bool validate(const std::string& json) noexcept;

    /**
     * Get last error message.
     * 
     * @return Error message
     */
    std::string get_last_error() const noexcept;

    /**
     * Clear error state.
     */
    void clear_error() noexcept;

    /**
     * Get parser statistics.
     * 
     * @return Statistics map
     */
    std::unordered_map<std::string, uint64_t> get_stats() const noexcept;

private:
    void* parser_;  // Opaque pointer to avoid incomplete type issues
    std::string last_error_;
    
    // Statistics
    std::atomic<uint64_t> total_parses_;
    std::atomic<uint64_t> successful_parses_;
    std::atomic<uint64_t> failed_parses_;
    std::atomic<uint64_t> total_bytes_parsed_;
    
    /**
     * Set error message.
     * 
     * @param error Error message
     */
    void set_error(const std::string& error) noexcept;
    
    /**
     * Update statistics.
     * 
     * @param success Whether parse was successful
     * @param bytes Number of bytes parsed
     */
    void update_stats(bool success, size_t bytes) noexcept;
};
