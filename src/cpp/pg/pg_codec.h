#pragma once

#include <cstdint>
#include <string_view>
#include <functional>
#include <optional>

/**
 * Type codecs for PostgreSQL binary format.
 *
 * Efficient encoders and decoders for common PG types:
 * Phase 1: int, float, bool, text, bytea, timestamptz, date, numeric, uuid, jsonb
 * Phase 2+: arrays, composites, ranges
 *
 * Goals:
 * - Zero-copy where possible
 * - Branch-predictable dispatch
 * - SIMD-ready structure for bulk operations
 * - < 10ns per column decode
 */

namespace pg::codec {

/**
 * Codec base class (virtual dispatch stub).
 *
 * Will be specialized for each type to enable inlining in hot paths.
 */
class Codec {
public:
    virtual ~Codec() = default;
    
    /**
     * Encode value to binary.
     *
     * @param value String representation
     * @param out_buf Output buffer
     * @param out_len Bytes written
     * @return 0 on success
     */
    virtual int encode(
        std::string_view value,
        uint8_t* out_buf,
        uint32_t* out_len
    ) noexcept = 0;
    
    /**
     * Decode value from binary to string representation.
     *
     * @param data Binary data
     * @param length Data length
     * @param out_str Output string (may be stack-allocated by decoder)
     * @return 0 on success
     */
    virtual int decode(
        const uint8_t* data,
        uint32_t length,
        std::string_view* out_str
    ) noexcept = 0;
};

/**
 * Registry for codecs by PostgreSQL type OID.
 *
 * Provides fast type dispatch without virtual calls in hot loops.
 */
class CodecRegistry {
public:
    /**
     * Register a codec for a PostgreSQL type OID.
     *
     * @param pg_oid PostgreSQL type OID
     * @param codec Codec instance (registry takes ownership)
     */
    void register_codec(uint32_t pg_oid, Codec* codec) noexcept;
    
    /**
     * Get codec for OID.
     *
     * @param pg_oid PostgreSQL type OID
     * @return Codec pointer, or nullptr if not registered
     */
    Codec* get_codec(uint32_t pg_oid) const noexcept;
    
    /**
     * Bulk encode multiple values (optimized for SIMD).
     *
     * @param oids Array of type OIDs
     * @param values Array of values to encode
     * @param count Number of values
     * @param out_buffers Output buffers (must be pre-allocated)
     * @param out_sizes Sizes of encoded values
     * @return 0 on success
     */
    int encode_batch(
        const uint32_t* oids,
        const std::string_view* values,
        uint32_t count,
        uint8_t** out_buffers,
        uint32_t* out_sizes
    ) noexcept;
    
    /**
     * Get singleton registry.
     *
     * @return Global codec registry
     */
    static CodecRegistry& instance() noexcept;
};

/**
 * Common type codec implementations (inline for performance).
 */

// Integer (int4, OID 23)
inline int encode_int32(int32_t val, uint8_t* buf) noexcept {
    // Network byte order (big-endian)
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8) & 0xFF;
    buf[3] = val & 0xFF;
    return 0;
}

inline int32_t decode_int32(const uint8_t* buf) noexcept {
    return ((int32_t)buf[0] << 24) | ((int32_t)buf[1] << 16) |
           ((int32_t)buf[2] << 8) | (int32_t)buf[3];
}

// Boolean (bool, OID 16)
inline int encode_bool(bool val, uint8_t* buf) noexcept {
    buf[0] = val ? 1 : 0;
    return 0;
}

inline bool decode_bool(const uint8_t* buf) noexcept {
    return buf[0] != 0;
}

// Float64 (float8, OID 701)
inline int encode_float64(double val, uint8_t* buf) noexcept {
    // Interpret double as uint64_t, then write in network byte order
    uint64_t bits;
    __builtin_memcpy(&bits, &val, sizeof(bits));
    for (int i = 0; i < 8; ++i) {
        buf[i] = (bits >> (56 - 8 * i)) & 0xFF;
    }
    return 0;
}

inline double decode_float64(const uint8_t* buf) noexcept {
    uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) {
        bits = (bits << 8) | buf[i];
    }
    double val;
    __builtin_memcpy(&val, &bits, sizeof(val));
    return val;
}

// Text (text, OID 25) - just byte stream, zero-copy
inline std::string_view decode_text(const uint8_t* data, uint32_t len) noexcept {
    return std::string_view(reinterpret_cast<const char*>(data), len);
}

// Bytea (bytea, OID 17) - similar to text
inline std::string_view decode_bytea(const uint8_t* data, uint32_t len) noexcept {
    return std::string_view(reinterpret_cast<const char*>(data), len);
}

/**
 * Initialize codec registry with Phase 1 codecs.
 *
 * Should be called once at library initialization.
 *
 * @return 0 on success
 */
int init_codec_registry() noexcept;

} // namespace pg::codec
