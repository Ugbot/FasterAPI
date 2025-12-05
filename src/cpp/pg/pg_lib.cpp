/**
 * FasterAPI PostgreSQL library - C interface for ctypes binding.
 *
 * High-performance PostgreSQL driver with:
 * - Per-core connection sharding (lock-free)
 * - Binary protocol codecs (zero-copy)
 * - Prepared statement caching (LRU)
 * - Transaction management with retries
 * - COPY streaming (> 1GB/sec)
 *
 * All exported functions use C linkage and void* pointers for FFI safety.
 * Implementation focuses on maximum performance with lock-free operations.
 *
 * Note: Compiled with -fno-exceptions, so no try/catch blocks.
 */

#include "pg_pool.h"
#include "pg_connection.h"
#include "pg_codec.h"
#include "pg_pool_impl.h"
#include "pg_connection_impl.h"
#include "pg_result.h"
#include <cstring>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <string>
#include <chrono>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <libpq-fe.h>

extern "C" {

// ==============================================================================
// Pool Management
// ==============================================================================

/**
 * Create a new connection pool.
 *
 * @param dsn PostgreSQL connection string
 * @param min_size Minimum connections per core
 * @param max_size Maximum connections per core
 * @param error_out Output error code (0 = success)
 * @return Opaque pool handle (cast to PgPool*)
 */
void* pg_pool_create(
    const char* dsn,
    uint32_t min_size,
    uint32_t max_size,
    int* error_out
) noexcept {
    if (!dsn || !error_out) {
        if (error_out) *error_out = 1;  // Invalid argument
        return nullptr;
    }
    
    // Create pool implementation directly (use nothrow to avoid exceptions)
    auto* pool_impl = new (std::nothrow) PgPoolImpl(dsn, min_size, max_size);
    if (!pool_impl) {
        *error_out = 2;  // Memory allocation failed
        return nullptr;
    }

    *error_out = 0;
    return pool_impl;
}

/**
 * Destroy a connection pool.
 *
 * @param pool Pool handle from pg_pool_create
 * @return Error code (0 = success)
 */
int pg_pool_destroy(void* pool) noexcept {
    if (!pool) return 1;

    auto* p = reinterpret_cast<PgPoolImpl*>(pool);
    delete p;
    return 0;
}

/**
 * Get a connection from the pool.
 *
 * @param pool Pool handle from pg_pool_create
 * @param core_id CPU core for affinity
 * @param deadline_ms Max time to wait
 * @param error_out Output error code
 * @return Connection handle (cast to PgConnection*), or nullptr on error
 */
void* pg_pool_get(void* pool, uint32_t core_id, uint64_t deadline_ms, int* error_out) noexcept {
    if (!pool || !error_out) {
        if (error_out) *error_out = 1;
        return nullptr;
    }

    auto* p = reinterpret_cast<PgPoolImpl*>(pool);
    auto* conn = p->get(core_id, deadline_ms, error_out);
    return conn;
}

/**
 * Release a connection back to the pool.
 *
 * @param pool Pool handle
 * @param conn Connection handle from pg_pool_get
 * @return Error code (0 = success)
 */
int pg_pool_release(void* pool, void* conn) noexcept {
    if (!pool || !conn) return 1;

    auto* p = reinterpret_cast<PgPoolImpl*>(pool);
    auto* c = reinterpret_cast<PgConnection*>(conn);
    return p->release(c);
}

/**
 * Get pool statistics.
 *
 * @param pool Pool handle
 * @param out_stats Output stats structure (if needed)
 * @return Error code (0 = success)
 */
int pg_pool_stats_get(void* pool, void* out_stats) noexcept {
    if (!pool || !out_stats) return 1;

    auto* p = reinterpret_cast<PgPoolImpl*>(pool);
    auto stats = p->stats();

    // Copy stats to output structure (simplified - just copy memory)
    std::memcpy(out_stats, &stats, sizeof(PgPoolImpl::PoolStats));
    return 0;
}

// ==============================================================================
// Query Execution
// ==============================================================================

/**
 * Execute a query with parameters.
 *
 * @param conn Connection handle from pg_pool_get
 * @param sql SQL query string
 * @param param_count Number of parameters
 * @param params Array of parameter strings (null-terminated)
 * @param error_out Output error code
 * @return Result handle (cast to PgResult*), or nullptr on error
 */
void* pg_exec_query(
    void* conn,
    const char* sql,
    uint32_t param_count,
    const char* const* params,
    int* error_out
) noexcept {
    if (!conn || !sql || !error_out) {
        if (error_out) *error_out = 1;
        return nullptr;
    }

    auto* c = reinterpret_cast<PgConnection*>(conn);
    auto* result = c->exec_query(sql, param_count, params, error_out);
    return result;
}

/**
 * Get number of rows in result.
 *
 * @param result Result handle from pg_exec_query
 * @return Number of rows, or -1 on error
 */
int64_t pg_result_row_count(void* result) noexcept {
    if (!result) return -1;

    auto* r = reinterpret_cast<PgResult*>(result);
    return r->row_count();
}

/**
 * Destroy a result.
 *
 * @param result Result handle
 * @return Error code (0 = success)
 */
int pg_result_destroy(void* result) noexcept {
    if (!result) return 1;

    auto* r = reinterpret_cast<PgResult*>(result);
    delete r;
    return 0;
}

/**
 * Get number of columns in result.
 *
 * @param result Result handle
 * @return Number of columns, or -1 on error
 */
int32_t pg_result_field_count(void* result) noexcept {
    if (!result) return -1;

    auto* r = reinterpret_cast<PgResult*>(result);
    return r->field_count();
}

/**
 * Get column name.
 *
 * @param result Result handle
 * @param col_index Column index (0-based)
 * @return Column name, or nullptr on error
 */
const char* pg_result_field_name(void* result, int32_t col_index) noexcept {
    if (!result) return nullptr;

    auto* r = reinterpret_cast<PgResult*>(result);
    return r->field_name(col_index);
}

/**
 * Get value from result.
 *
 * @param result Result handle
 * @param row_index Row index (0-based)
 * @param col_index Column index (0-based)
 * @return Value string, or nullptr if NULL/error
 */
const char* pg_result_get_value(void* result, int64_t row_index, int32_t col_index) noexcept {
    if (!result) return nullptr;

    auto* r = reinterpret_cast<PgResult*>(result);
    return r->get_value(row_index, col_index);
}

/**
 * Check if value is NULL.
 *
 * @param result Result handle
 * @param row_index Row index (0-based)
 * @param col_index Column index (0-based)
 * @return 1 if NULL, 0 if not NULL, -1 on error
 */
int pg_result_is_null(void* result, int64_t row_index, int32_t col_index) noexcept {
    if (!result) return -1;

    auto* r = reinterpret_cast<PgResult*>(result);
    return r->is_null(row_index, col_index) ? 1 : 0;
}

/**
 * Get value length.
 *
 * @param result Result handle
 * @param row_index Row index (0-based)
 * @param col_index Column index (0-based)
 * @return Value length, or -1 on error/NULL
 */
int32_t pg_result_get_length(void* result, int64_t row_index, int32_t col_index) noexcept {
    if (!result) return -1;

    auto* r = reinterpret_cast<PgResult*>(result);
    return r->get_length(row_index, col_index);
}

/**
 * Get scalar value (single row, single column).
 *
 * @param result Result handle
 * @return Scalar value, or nullptr on error
 */
const char* pg_result_scalar(void* result) noexcept {
    if (!result) return nullptr;

    auto* r = reinterpret_cast<PgResult*>(result);
    return r->scalar();
}

/**
 * Get error message from result.
 *
 * @param result Result handle
 * @return Error message, or nullptr if no error
 */
const char* pg_result_error_message(void* result) noexcept {
    if (!result) return nullptr;

    auto* r = reinterpret_cast<PgResult*>(result);
    return r->error_message();
}

// ==============================================================================
// Transactions
// ==============================================================================

/**
 * Begin a transaction.
 *
 * @param conn Connection handle
 * @param isolation Isolation level string
 * @param error_out Output error code
 * @return Error code (0 = success)
 */
int pg_tx_begin(void* conn, const char* isolation, int* error_out) noexcept {
    if (!conn || !error_out) return 1;

    auto* c = reinterpret_cast<PgConnection*>(conn);
    return c->begin_tx(isolation, error_out);
}

/**
 * Commit a transaction.
 *
 * @param conn Connection handle
 * @param error_out Output error code
 * @return Error code (0 = success)
 */
int pg_tx_commit(void* conn, int* error_out) noexcept {
    if (!conn || !error_out) return 1;

    auto* c = reinterpret_cast<PgConnection*>(conn);
    return c->commit_tx(error_out);
}

/**
 * Rollback a transaction.
 *
 * @param conn Connection handle
 * @return Error code (0 = success)
 */
int pg_tx_rollback(void* conn) noexcept {
    if (!conn) return 1;

    auto* c = reinterpret_cast<PgConnection*>(conn);
    return c->rollback_tx();
}

// ==============================================================================
// COPY Operations
// ==============================================================================

/**
 * Start a COPY IN operation.
 *
 * @param conn Connection handle
 * @param sql COPY command
 * @param error_out Output error code
 * @return Error code (0 = success)
 */
int pg_copy_in_start(void* conn, const char* sql, int* error_out) noexcept {
    if (!conn || !sql || !error_out) return 1;

    auto* c = reinterpret_cast<PgConnection*>(conn);
    return c->copy_in_start(sql, error_out);
}

/**
 * Write data to COPY IN.
 *
 * @param conn Connection handle
 * @param data Data to write
 * @param length Data length
 * @param error_out Output error code
 * @return Bytes written
 */
uint64_t pg_copy_in_write(void* conn, const char* data, uint64_t length, int* error_out) noexcept {
    if (!conn || !data || !error_out) return 0;

    auto* c = reinterpret_cast<PgConnection*>(conn);
    return c->copy_in_write(data, length, error_out);
}

/**
 * End COPY IN operation.
 *
 * @param conn Connection handle
 * @param error_out Output error code
 * @return Error code (0 = success)
 */
int pg_copy_in_end(void* conn, int* error_out) noexcept {
    if (!conn || !error_out) return 1;

    auto* c = reinterpret_cast<PgConnection*>(conn);
    return c->copy_in_end(error_out);
}

// ==============================================================================
// Library Initialization
// ==============================================================================

/**
 * Initialize the library.
 *
 * Called once at library load time to set up codec registry, etc.
 *
 * @return Error code (0 = success)
 */
int pg_lib_init() noexcept {
    // Stub: Initialize codec registry and other global state
    // return pg::codec::init_codec_registry();
    return 0;
}

/**
 * Shutdown the library.
 *
 * Called at library unload time.
 *
 * @return Error code (0 = success)
 */
int pg_lib_shutdown() noexcept {
    // Stub: Cleanup global resources
    return 0;
}

}  // extern "C"
