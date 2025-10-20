#pragma once

#include <cstdint>
#include <string_view>

/**
 * PostgreSQL binary protocol handling.
 *
 * Phase 1: Wrapper around libpq's binary protocol
 * Phase 2+: Native protocol implementation (no libpq dependency)
 *
 * Features:
 * - Binary message framing (minimizes serialization overhead)
 * - Fast parameter encoding (integer fast-paths via template specialization)
 * - Row decoding to zero-copy views (defer materialization)
 * - Zero-allocation parameter binding
 */

namespace pg::protocol {

/**
 * Parameter encoder for binary format.
 *
 * Encodes PostgreSQL OID + binary value without allocations.
 * Fast paths for common types (int, float, bool, text).
 */
class ParamEncoder {
public:
    /**
     * Encode a parameter value.
     *
     * @param value String representation of value
     * @param pg_type PostgreSQL type OID
     * @param out_binary Output buffer (must be sized appropriately)
     * @param out_length Bytes written
     * @return Error code (0 = success)
     */
    static int encode(
        std::string_view value,
        uint32_t pg_type,
        uint8_t* out_binary,
        uint32_t* out_length
    ) noexcept;
    
    /**
     * Encode integer (OID 23 = int4).
     *
     * @param value Integer to encode
     * @param out_binary Output buffer (must be >= 4 bytes)
     * @return 4 (bytes written)
     */
    static uint32_t encode_int(int32_t value, uint8_t* out_binary) noexcept;
    
    /**
     * Encode 64-bit integer (OID 20 = int8).
     *
     * @param value Integer to encode
     * @param out_binary Output buffer (must be >= 8 bytes)
     * @return 8 (bytes written)
     */
    static uint32_t encode_int64(int64_t value, uint8_t* out_binary) noexcept;
    
    /**
     * Encode floating point (OID 701 = float8).
     *
     * @param value Float to encode
     * @param out_binary Output buffer (must be >= 8 bytes)
     * @return 8 (bytes written)
     */
    static uint32_t encode_float64(double value, uint8_t* out_binary) noexcept;
};

/**
 * Row decoder for binary format.
 *
 * Decodes PostgreSQL tuples into zero-copy views.
 * Row data is not copied; columns return pointers into the result buffer.
 */
class RowDecoder {
public:
    /**
     * Get a column value from a row.
     *
     * @param row_data Pointer to row data (from PGresult)
     * @param col_index Column index
     * @param pg_type PostgreSQL type OID
     * @param out_value Output value pointer (may point into row_data)
     * @param out_length Value length
     * @return Error code (0 = success)
     *
     * The output pointer points into row_data and is valid only while
     * the result is not freed. Use for zero-copy access.
     */
    static int decode_field(
        const uint8_t* row_data,
        uint32_t col_index,
        uint32_t pg_type,
        const uint8_t** out_value,
        uint32_t* out_length
    ) noexcept;
    
    /**
     * Decode integer from binary (OID 23 = int4).
     *
     * @param data Binary data (must be >= 4 bytes)
     * @return Decoded integer
     */
    static int32_t decode_int(const uint8_t* data) noexcept;
    
    /**
     * Decode 64-bit integer from binary (OID 20 = int8).
     *
     * @param data Binary data (must be >= 8 bytes)
     * @return Decoded integer
     */
    static int64_t decode_int64(const uint8_t* data) noexcept;
    
    /**
     * Decode floating point from binary (OID 701 = float8).
     *
     * @param data Binary data (must be >= 8 bytes)
     * @return Decoded double
     */
    static double decode_float64(const uint8_t* data) noexcept;
    
    /**
     * Decode text string (OID 25).
     *
     * @param data Binary data
     * @param length Data length
     * @return String view into data
     */
    static std::string_view decode_text(const uint8_t* data, uint32_t length) noexcept;
};

/**
 * PostgreSQL OID constants for common types.
 *
 * Used for type dispatch in encoder/decoder.
 */
namespace oid {
    constexpr uint32_t INT4 = 23;           // int4
    constexpr uint32_t INT8 = 20;           // int8
    constexpr uint32_t FLOAT8 = 701;        // float8
    constexpr uint32_t TEXT = 25;           // text
    constexpr uint32_t BYTEA = 17;          // bytea
    constexpr uint32_t BOOL = 16;           // bool
    constexpr uint32_t TIMESTAMP = 1114;    // timestamp
    constexpr uint32_t TIMESTAMPTZ = 1184;  // timestamptz
    constexpr uint32_t DATE = 1082;         // date
    constexpr uint32_t NUMERIC = 1700;      // numeric
    constexpr uint32_t UUID = 2950;         // uuid
    constexpr uint32_t JSONB = 3802;        // jsonb
}

} // namespace pg::protocol
