/**
 * Binary Kwargs Encoder/Decoder
 *
 * High-performance TLV (Type-Length-Value) encoding for Python IPC.
 * Replaces JSON serialization with custom binary format for ~26x speedup.
 *
 * Binary Format:
 * +--------+----------+--------+------+--------+
 * | Magic  | ParamCnt | Param1 | ...  | ParamN |
 * +--------+----------+--------+------+--------+
 *   1 byte   2 bytes    variable
 *
 * Each parameter:
 * +----------+------+-------+--------+
 * | NameLen  | Name | Tag   | Value  |
 * +----------+------+-------+--------+
 *   1 byte    var    1 byte  variable
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <array>
#include <atomic>
#include <utility>

namespace fasterapi {
namespace python {

/**
 * Magic byte to identify binary TLV format (vs JSON or MessagePack)
 */
static constexpr uint8_t BINARY_KWARGS_MAGIC = 0xFA;

/**
 * Type tags for TLV encoding
 */
enum class KwargsTypeTag : uint8_t {
    // Null/None
    TAG_NULL = 0x00,

    // Boolean (no value bytes needed)
    TAG_BOOL_FALSE = 0x01,
    TAG_BOOL_TRUE = 0x02,

    // Integers (little-endian)
    TAG_INT8 = 0x10,
    TAG_INT16 = 0x11,
    TAG_INT32 = 0x12,
    TAG_INT64 = 0x13,

    // Unsigned integers
    TAG_UINT8 = 0x18,
    TAG_UINT16 = 0x19,
    TAG_UINT32 = 0x1A,
    TAG_UINT64 = 0x1B,

    // Floating point (IEEE 754)
    TAG_FLOAT32 = 0x20,
    TAG_FLOAT64 = 0x21,

    // Strings (UTF-8)
    TAG_STR_TINY = 0x30,     // 1 byte len (0-255)
    TAG_STR_SHORT = 0x31,    // 2 byte len (0-65535)
    TAG_STR_MEDIUM = 0x32,   // 4 byte len

    // Binary data
    TAG_BYTES_TINY = 0x40,   // 1 byte len
    TAG_BYTES_SHORT = 0x41,  // 2 byte len
    TAG_BYTES_MEDIUM = 0x42, // 4 byte len

    // Fallback: MessagePack-encoded complex value
    TAG_MSGPACK = 0x70,      // 4 byte len + msgpack data

    // Fallback: JSON-encoded complex value (legacy compatibility)
    TAG_JSON = 0x7F,         // 4 byte len + json data
};

/**
 * Kwargs format identifiers for protocol headers
 */
enum class KwargsFormat : uint8_t {
    FORMAT_JSON = 0,         // Legacy JSON format
    FORMAT_BINARY_TLV = 1,   // Custom TLV binary format
    FORMAT_MSGPACK = 2,      // MessagePack format
};

/**
 * Buffer pool for zero-allocation encoding.
 *
 * Pre-allocates buffers to avoid malloc/free in hot path.
 * Thread-safe with lock-free buffer acquisition.
 */
class KwargsBufferPool {
public:
    static constexpr size_t BUFFER_SIZE = 4096;   // Covers 99% of requests
    static constexpr size_t POOL_SIZE = 256;      // Per-pool capacity

    struct BufferSlot {
        alignas(64) uint8_t data[BUFFER_SIZE];
        std::atomic<bool> in_use{false};
    };

    /**
     * Acquire a buffer from the pool.
     * Returns nullptr if all buffers are in use (caller should allocate).
     */
    static uint8_t* acquire(size_t& capacity);

    /**
     * Release a buffer back to the pool.
     * Only call with buffers acquired from this pool.
     */
    static void release(uint8_t* buffer);

    /**
     * Check if a buffer is from this pool (for debugging).
     */
    static bool is_pool_buffer(const uint8_t* buffer);

    /**
     * Get pool statistics.
     */
    static size_t buffers_in_use();

private:
    static thread_local std::array<BufferSlot, POOL_SIZE> buffers_;
    static thread_local size_t next_slot_;
};

/**
 * RAII wrapper for pooled buffers.
 */
class PooledBuffer {
public:
    PooledBuffer();
    ~PooledBuffer();

    // Non-copyable
    PooledBuffer(const PooledBuffer&) = delete;
    PooledBuffer& operator=(const PooledBuffer&) = delete;

    // Movable
    PooledBuffer(PooledBuffer&& other) noexcept;
    PooledBuffer& operator=(PooledBuffer&& other) noexcept;

    uint8_t* data() noexcept { return data_; }
    const uint8_t* data() const noexcept { return data_; }
    size_t capacity() const noexcept { return capacity_; }
    bool is_pooled() const noexcept { return pooled_; }

    /**
     * Grow buffer if needed. May allocate new memory.
     */
    bool ensure_capacity(size_t needed);

private:
    uint8_t* data_;
    size_t capacity_;
    bool pooled_;
};

/**
 * Binary kwargs encoder.
 *
 * Encodes key-value pairs into compact TLV format.
 * Designed for zero-copy operation with pre-allocated buffers.
 */
class BinaryKwargsEncoder {
public:
    explicit BinaryKwargsEncoder(PooledBuffer& buffer);

    /**
     * Begin encoding a new kwargs dictionary.
     * Must be called before adding any parameters.
     */
    void begin();

    /**
     * Finish encoding and return the final size.
     * Updates the parameter count in the header.
     */
    size_t finish();

    /**
     * Add a null/None value.
     */
    bool add_null(std::string_view name);

    /**
     * Add a boolean value.
     */
    bool add_bool(std::string_view name, bool value);

    /**
     * Add an integer value (auto-selects smallest representation).
     */
    bool add_int(std::string_view name, int64_t value);

    /**
     * Add an unsigned integer value.
     */
    bool add_uint(std::string_view name, uint64_t value);

    /**
     * Add a floating-point value.
     */
    bool add_float(std::string_view name, double value);

    /**
     * Add a string value.
     */
    bool add_string(std::string_view name, std::string_view value);

    /**
     * Add binary data.
     */
    bool add_bytes(std::string_view name, const uint8_t* data, size_t len);

    /**
     * Add a fallback JSON-encoded value (for complex types).
     */
    bool add_json_fallback(std::string_view name, std::string_view json);

    /**
     * Add a fallback MessagePack-encoded value (for complex types).
     */
    bool add_msgpack_fallback(std::string_view name, const uint8_t* data, size_t len);

    /**
     * Get current write position (for debugging).
     */
    size_t current_size() const noexcept { return pos_; }

    /**
     * Get number of parameters written.
     */
    uint16_t param_count() const noexcept { return param_count_; }

private:
    bool ensure_space(size_t needed);
    bool write_name(std::string_view name);
    void write_u8(uint8_t value);
    void write_u16(uint16_t value);
    void write_u32(uint32_t value);
    void write_u64(uint64_t value);
    void write_i64(int64_t value);
    void write_f64(double value);
    void write_bytes(const uint8_t* data, size_t len);

    PooledBuffer& buffer_;
    size_t pos_;
    uint16_t param_count_;
    bool started_;
};

/**
 * Binary kwargs decoder (C++ side, for response parsing).
 *
 * Provides iterator-style access to encoded parameters.
 */
class BinaryKwargsDecoder {
public:
    struct Parameter {
        std::string_view name;
        KwargsTypeTag tag;
        union {
            bool bool_val;
            int64_t int_val;
            uint64_t uint_val;
            double float_val;
        };
        std::string_view str_val;  // For strings/bytes/fallbacks
        const uint8_t* bytes_ptr;  // For bytes/fallbacks
        size_t bytes_len;
    };

    /**
     * Initialize decoder with binary data.
     * Returns false if magic byte doesn't match.
     */
    bool init(const uint8_t* data, size_t len);

    /**
     * Get parameter count.
     */
    uint16_t param_count() const noexcept { return param_count_; }

    /**
     * Read next parameter.
     * Returns false when no more parameters.
     */
    bool next(Parameter& param);

    /**
     * Reset to beginning.
     */
    void reset();

    /**
     * Check if this is a binary TLV encoded buffer.
     */
    static bool is_binary_format(const uint8_t* data, size_t len) {
        return len >= 1 && data[0] == BINARY_KWARGS_MAGIC;
    }

private:
    const uint8_t* data_;
    size_t len_;
    size_t pos_;
    uint16_t param_count_;
    uint16_t params_read_;
};

/**
 * Encode simple kwargs (string key/value pairs only).
 *
 * Fast path for GET request query parameters.
 * Returns encoded size, or 0 on failure.
 */
size_t encode_simple_kwargs(
    PooledBuffer& buffer,
    const std::pair<std::string_view, std::string_view>* params,
    size_t param_count
);

}  // namespace python
}  // namespace fasterapi
