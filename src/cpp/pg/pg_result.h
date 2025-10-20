#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Forward declarations
struct pg_result;

/**
 * PostgreSQL query result with zero-copy row access.
 * 
 * Features:
 * - Zero-copy row decoding
 * - Binary protocol support
 * - Streaming result support
 * - Type-safe column access
 */
class PgResult {
public:
    /**
     * Create result from libpq result.
     * 
     * @param result libpq result
     */
    explicit PgResult(pg_result* result) noexcept;
    
    /**
     * Destructor.
     */
    ~PgResult();

    // Non-copyable, movable
    PgResult(const PgResult&) = delete;
    PgResult& operator=(const PgResult&) = delete;
    PgResult(PgResult&&) noexcept;
    PgResult& operator=(PgResult&&) noexcept;

    /**
     * Get number of rows in result.
     * 
     * @return Number of rows
     */
    int64_t row_count() const noexcept;

    /**
     * Get number of columns in result.
     * 
     * @return Number of columns
     */
    int32_t field_count() const noexcept;

    /**
     * Get column name.
     * 
     * @param col_index Column index (0-based)
     * @return Column name, or nullptr if invalid
     */
    const char* field_name(int32_t col_index) const noexcept;

    /**
     * Get column type OID.
     * 
     * @param col_index Column index (0-based)
     * @return Type OID, or 0 if invalid
     */
    uint32_t field_type(int32_t col_index) const noexcept;

    /**
     * Get value as string.
     * 
     * @param row_index Row index (0-based)
     * @param col_index Column index (0-based)
     * @return Value string, or nullptr if NULL/invalid
     */
    const char* get_value(int64_t row_index, int32_t col_index) const noexcept;

    /**
     * Check if value is NULL.
     * 
     * @param row_index Row index (0-based)
     * @param col_index Column index (0-based)
     * @return true if NULL, false otherwise
     */
    bool is_null(int64_t row_index, int32_t col_index) const noexcept;

    /**
     * Get value length.
     * 
     * @param row_index Row index (0-based)
     * @param col_index Column index (0-based)
     * @return Value length in bytes, or -1 if NULL/invalid
     */
    int32_t get_length(int64_t row_index, int32_t col_index) const noexcept;

    /**
     * Get scalar value (single row, single column).
     * 
     * @return Scalar value, or nullptr if not scalar
     */
    const char* scalar() const noexcept;

    /**
     * Get result status.
     * 
     * @return Status string
     */
    const char* status() const noexcept;

    /**
     * Get error message if any.
     * 
     * @return Error message, or nullptr if no error
     */
    const char* error_message() const noexcept;

private:
    pg_result* result_;
    int64_t row_count_;
    int32_t field_count_;
};
