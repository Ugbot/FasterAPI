#pragma once

#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <chrono>
#include <queue>
#include <mutex>

// Forward declarations
struct pg_conn;
struct pg_result;
class PgResult;

/**
 * High-performance PostgreSQL connection implementation with lock-free operations.
 * 
 * Features:
 * - Non-blocking I/O with libpq
 * - Prepared statement caching (LRU)
 * - Binary protocol support
 * - Transaction management
 * - COPY streaming
 * - Zero-copy row decoding
 */
class PgConnectionImpl {
public:
    // Connection states
    enum class State {
        IDLE,
        BUSY,
        TRANSACTION,
        COPY_IN,
        COPY_OUT,
        ERROR,
        CLOSED
    };

    // Transaction isolation levels
    enum class IsolationLevel {
        READ_COMMITTED,
        REPEATABLE_READ,
        SERIALIZABLE
    };

    /**
     * Create a new connection.
     * 
     * @param dsn PostgreSQL connection string
     * @return New connection, or nullptr on error
     */
    static std::unique_ptr<PgConnectionImpl> create(const std::string& dsn) noexcept;

    /**
     * Destructor.
     */
    ~PgConnectionImpl();

    // Non-copyable, movable
    PgConnectionImpl(const PgConnectionImpl&) = delete;
    PgConnectionImpl& operator=(const PgConnectionImpl&) = delete;
    PgConnectionImpl(PgConnectionImpl&&) noexcept;
    PgConnectionImpl& operator=(PgConnectionImpl&&) noexcept;

    /**
     * Execute a query with parameters.
     * 
     * @param sql SQL query string
     * @param param_count Number of parameters
     * @param params Array of parameter strings
     * @param error_out Output error code (0 = success)
     * @return Result handle, or nullptr on error
     */
    PgResult* exec_query(
        const char* sql,
        uint32_t param_count,
        const char* const* params,
        int* error_out
    ) noexcept;

    /**
     * Execute a prepared statement.
     * 
     * @param stmt_name Prepared statement name
     * @param param_count Number of parameters
     * @param params Array of parameter strings
     * @param error_out Output error code (0 = success)
     * @return Result handle, or nullptr on error
     */
    PgResult* exec_prepared(
        const char* stmt_name,
        uint32_t param_count,
        const char* const* params,
        int* error_out
    ) noexcept;

    /**
     * Prepare a statement.
     * 
     * @param stmt_name Statement name
     * @param sql SQL query string
     * @param param_count Number of parameters
     * @param error_out Output error code (0 = success)
     * @return Error code (0 = success)
     */
    int prepare(
        const char* stmt_name,
        const char* sql,
        uint32_t param_count,
        int* error_out
    ) noexcept;

    /**
     * Begin a transaction.
     * 
     * @param isolation Isolation level
     * @param error_out Output error code (0 = success)
     * @return Error code (0 = success)
     */
    int begin_tx(IsolationLevel isolation, int* error_out) noexcept;

    /**
     * Commit a transaction.
     * 
     * @param error_out Output error code (0 = success)
     * @return Error code (0 = success)
     */
    int commit_tx(int* error_out) noexcept;

    /**
     * Rollback a transaction.
     * 
     * @return Error code (0 = success)
     */
    int rollback_tx() noexcept;

    /**
     * Start COPY IN operation.
     * 
     * @param sql COPY command
     * @param error_out Output error code (0 = success)
     * @return Error code (0 = success)
     */
    int copy_in_start(const char* sql, int* error_out) noexcept;

    /**
     * Write data to COPY IN.
     * 
     * @param data Data to write
     * @param length Data length
     * @param error_out Output error code (0 = success)
     * @return Bytes written
     */
    uint64_t copy_in_write(const char* data, uint64_t length, int* error_out) noexcept;

    /**
     * End COPY IN operation.
     * 
     * @param error_out Output error code (0 = success)
     * @return Error code (0 = success)
     */
    int copy_in_end(int* error_out) noexcept;

    /**
     * Start COPY OUT operation.
     * 
     * @param sql COPY command
     * @param error_out Output error code (0 = success)
     * @return Error code (0 = success)
     */
    int copy_out_start(const char* sql, int* error_out) noexcept;

    /**
     * Read data from COPY OUT.
     * 
     * @param buffer Buffer to read into
     * @param buffer_size Buffer size
     * @param error_out Output error code (0 = success)
     * @return Bytes read
     */
    uint64_t copy_out_read(char* buffer, uint64_t buffer_size, int* error_out) noexcept;

    /**
     * Cancel current operation.
     * 
     * @return Error code (0 = success)
     */
    int cancel() noexcept;

    /**
     * Check if connection is healthy.
     * 
     * @return true if healthy, false otherwise
     */
    bool is_healthy() const noexcept;

    /**
     * Reset connection to clean state.
     * 
     * @return Error code (0 = success)
     */
    int reset() noexcept;

    /**
     * Get connection state.
     * 
     * @return Current state
     */
    State get_state() const noexcept { return state_.load(); }

    /**
     * Get last activity time.
     * 
     * @return Last activity time in nanoseconds
     */
    uint64_t get_last_activity() const noexcept { return last_activity_.load(); }

    /**
     * Get connection ID.
     * 
     * @return Connection ID
     */
    uint64_t get_id() const noexcept { return connection_id_; }

public:
    // Constructor
    PgConnectionImpl() noexcept;

private:

    // Connection state
    std::atomic<State> state_{State::IDLE};
    std::atomic<uint64_t> last_activity_{0};
    uint64_t connection_id_;
    
    // libpq connection
    pg_conn* conn_;
    
    // Prepared statements cache (LRU)
    struct PreparedStmt {
        std::string name;
        std::string sql;
        uint32_t param_count;
        uint64_t last_used;
        uint64_t use_count;
    };
    
    std::unordered_map<std::string, PreparedStmt> prepared_stmts_;
    uint64_t next_stmt_id_{1};
    
    // Statistics
    std::atomic<uint64_t> total_queries_{0};
    std::atomic<uint64_t> total_errors_{0};
    std::atomic<uint64_t> total_bytes_sent_{0};
    std::atomic<uint64_t> total_bytes_received_{0};

    /**
     * Update last activity time.
     */
    void update_activity() noexcept;

    /**
     * Get current time in nanoseconds.
     * 
     * @return Current time in nanoseconds
     */
    uint64_t get_time_ns() const noexcept;

    /**
     * Clean up old prepared statements (LRU eviction).
     */
    void cleanup_prepared_stmts() noexcept;

    /**
     * Set connection state.
     * 
     * @param new_state New state
     */
    void set_state(State new_state) noexcept;
};
