#pragma once

#include <cstdint>
#include <memory>
#include <string>

// Forward declarations
class PgResult;
class PgConnectionImpl;

/**
 * Single PostgreSQL connection with non-blocking I/O.
 *
 * Features:
 * - Non-blocking libpq integration (PQsetnonblocking)
 * - Query execution state machine
 * - Zero-copy result buffers
 * - Prepared statement versioning and caching
 * - Per-statement latency tracking
 */
class PgConnection {
public:
    /**
     * Default constructor (for pool use).
     */
    PgConnection() noexcept;
    
    /**
     * Create a new connection.
     *
     * @param dsn PostgreSQL connection string
     * @param out_error Error code output (0 = success)
     */
    explicit PgConnection(const char* dsn, int* out_error) noexcept;
    
    ~PgConnection();
    
    // Non-copyable, non-movable (pool owns lifetime)
    PgConnection(const PgConnection&) = delete;
    PgConnection& operator=(const PgConnection&) = delete;
    
    /**
     * Execute a query with parameters.
     *
     * @param sql SQL query string (use $1, $2, ... for params)
     * @param param_count Number of parameters
     * @param params Array of parameter strings (null-terminated)
     * @param out_error Error code output (0 = success)
     * @return Result handle (opaque pointer), or nullptr on error
     *
     * Supports both text and binary transmission/reception.
     * Row decoding happens lazily (zero-copy views).
     */
    PgResult* exec_query(
        const char* sql,
        uint32_t param_count,
        const char* const* params,
        int* out_error
    ) noexcept;
    
    /**
     * Prepare a query (compile once, run fast).
     *
     * @param sql SQL query string
     * @param stmt_name Prepared statement name (auto-generated if empty)
     * @param out_error Error code output
     * @return Statement ID for reuse, or 0 on error
     *
     * Prepared statements are cached per connection.
     * Implementation uses LRU eviction when cache is full.
     */
    uint32_t prepare(
        const char* sql,
        const char* stmt_name,
        int* out_error
    ) noexcept;
    
    /**
     * Execute a prepared statement.
     *
     * @param stmt_id Statement ID from prepare()
     * @param param_count Number of parameters
     * @param params Parameter array
     * @param out_error Error code output
     * @return Result handle, or nullptr on error
     */
    PgResult* exec_prepared(
        uint32_t stmt_id,
        uint32_t param_count,
        const char* const* params,
        int* out_error
    ) noexcept;
    
    /**
     * Begin a transaction.
     *
     * @param isolation Isolation level ("READ UNCOMMITTED", etc.)
     * @param out_error Error code output
     * @return 0 on success
     */
    int begin_tx(const char* isolation, int* out_error) noexcept;
    
    /**
     * Commit the current transaction.
     *
     * @param out_error Error code output
     * @return 0 on success
     */
    int commit_tx(int* out_error) noexcept;
    
    /**
     * Rollback the current transaction.
     *
     * @return 0 on success
     */
    int rollback_tx() noexcept;
    
    /**
     * Start a COPY IN operation.
     *
     * @param sql COPY command (e.g., "COPY table FROM stdin")
     * @param out_error Error code output
     * @return 0 on success
     */
    int copy_in_start(const char* sql, int* out_error) noexcept;
    
    /**
     * Write data to COPY IN.
     *
     * @param data Buffer to write
     * @param length Bytes to write
     * @param out_error Error code output
     * @return Bytes written (may be < length on backpressure)
     */
    uint64_t copy_in_write(
        const char* data,
        uint64_t length,
        int* out_error
    ) noexcept;
    
    /**
     * End COPY IN operation.
     *
     * @param out_error Error code output
     * @return 0 on success
     */
    int copy_in_end(int* out_error) noexcept;
    
    /**
     * Start a COPY OUT operation.
     *
     * @param sql COPY command (e.g., "COPY table TO stdout")
     * @param out_error Error code output
     * @return 0 on success
     */
    int copy_out_start(const char* sql, int* out_error) noexcept;
    
    /**
     * Read data from COPY OUT.
     *
     * @param out_data Output buffer pointer (points to internal memory)
     * @param out_length Bytes available to read
     * @param out_error Error code output
     * @return 0 on success, >0 if more data, <0 if done
     */
    int copy_out_read(
        const char** out_data,
        uint64_t* out_length,
        int* out_error
    ) noexcept;
    
    /**
     * Cancel the current query.
     *
     * @return 0 on success
     *
     * Safe to call from another thread. Sends PG_CANCEL to server.
     */
    int cancel() noexcept;
    
    /**
     * Check if connection is healthy.
     *
     * @return true if connection is alive and ready
     */
    bool is_healthy() const noexcept;
    
    /**
     * Reset connection for reuse (clear state, close active queries).
     *
     * @return 0 on success
     */
    int reset() noexcept;
    
    /**
     * Get connection ID for pool management.
     *
     * @return Connection ID
     */
    uint64_t get_id() const noexcept;
    
private:
    std::unique_ptr<PgConnectionImpl> impl_;
    
    // For lock-free queue in pool
    friend class PgPoolImpl;
    PgConnection* next_available_{nullptr};
};

// Implementation moved to pg_connection.cpp to avoid circular dependencies
