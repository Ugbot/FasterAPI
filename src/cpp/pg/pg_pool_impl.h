#pragma once

#include <atomic>
#include <vector>
#include <memory>
#include <string>
#include <thread>
#include <array>
#include <cstdint>
#include <chrono>
#include <queue>
#include <mutex>

// Forward declarations
struct pg_conn;
class PgConnection;

/**
 * Lock-free connection pool implementation with per-core sharding.
 * 
 * Each CPU core has its own connection pool to avoid cross-core contention.
 * Uses atomic operations and lock-free queues for maximum performance.
 */
class PgPoolImpl {
public:
    // Per-core pool statistics
    struct CoreStats {
        std::atomic<uint32_t> active_connections{0};
        std::atomic<uint32_t> idle_connections{0};
        std::atomic<uint64_t> total_requests{0};
        std::atomic<uint64_t> total_wait_time_ns{0};
        std::atomic<uint32_t> errors{0};
        std::atomic<uint64_t> last_activity_ns{0};
    };

    // Pool-wide statistics
    struct PoolStats {
        uint32_t total_cores;
        uint32_t min_size_per_core;
        uint32_t max_size_per_core;
        uint64_t total_active;
        uint64_t total_idle;
        uint64_t total_requests;
        uint64_t avg_wait_time_ns;
        uint32_t total_errors;
        uint64_t timestamp_ns;
    };

    /**
     * Create a new connection pool.
     * 
     * @param dsn PostgreSQL connection string
     * @param min_size Minimum connections per core
     * @param max_size Maximum connections per core
     * @param idle_timeout_secs Close idle connections after this time
     * @param health_check_interval_secs Health check interval (0 = disabled)
     */
    PgPoolImpl(
        const std::string& dsn, 
        uint32_t min_size, 
        uint32_t max_size,
        uint32_t idle_timeout_secs = 600,
        uint32_t health_check_interval_secs = 30
    );
    
    ~PgPoolImpl();

    /**
     * Get a connection from the pool (lock-free).
     * 
     * @param core_id CPU core for affinity (0-based)
     * @param deadline_ms Maximum time to wait in milliseconds
     * @param error_out Output error code (0 = success)
     * @return Connection handle, or nullptr on error/timeout
     */
    PgConnection* get(uint32_t core_id, uint64_t deadline_ms, int* error_out) noexcept;

    /**
     * Release a connection back to the pool (lock-free).
     * 
     * @param conn Connection to release
     * @return Error code (0 = success)
     */
    int release(PgConnection* conn) noexcept;

    /**
     * Get pool statistics.
     * 
     * @return Pool statistics
     */
    PoolStats stats() const noexcept;

    /**
     * Close the pool and all connections.
     */
    void close() noexcept;

private:
    // Per-core connection pool
    struct CorePool {
        // Lock-free queue for available connections
        std::atomic<PgConnection*> available_head{nullptr};
        std::atomic<PgConnection*> available_tail{nullptr};
        
        // Connection storage
        std::vector<std::unique_ptr<PgConnection>> connections;
        
        // Statistics
        CoreStats stats;
        
        // Configuration
        uint32_t min_size;
        uint32_t max_size;
        uint32_t current_size{0};
        
        // Health check
        std::atomic<uint64_t> last_health_check_ns{0};
        std::atomic<bool> health_check_running{false};
    };

    std::string dsn_;
    uint32_t min_size_per_core_;
    uint32_t max_size_per_core_;
    uint32_t idle_timeout_secs_;
    uint32_t health_check_interval_secs_;
    uint32_t num_cores_;
    
    // Per-core pools (one per CPU core)
    std::vector<std::unique_ptr<CorePool>> core_pools_;
    
    // Pool-wide state
    std::atomic<bool> closed_{false};
    std::atomic<uint64_t> pool_start_time_ns_{0};

    /**
     * Initialize connections for a specific core.
     * 
     * @param core_id Core ID
     * @return Error code (0 = success)
     */
    int init_core_connections(uint32_t core_id) noexcept;

    /**
     * Get the current CPU core ID.
     * 
     * @return Core ID (0-based)
     */
    uint32_t get_current_core() const noexcept;

    /**
     * Create a new connection.
     * 
     * @return New connection, or nullptr on error
     */
    std::unique_ptr<PgConnection> create_connection() noexcept;

    /**
     * Health check for a core pool.
     * 
     * @param core_id Core ID
     */
    void health_check_core(uint32_t core_id) noexcept;

    /**
     * Get current time in nanoseconds.
     * 
     * @return Current time in nanoseconds
     */
    uint64_t get_time_ns() const noexcept;

    /**
     * Add connection to available queue (lock-free).
     * 
     * @param core_pool Core pool
     * @param conn Connection to add
     */
    void add_to_available_queue(CorePool* core_pool, PgConnection* conn) noexcept;

    /**
     * Remove connection from available queue (lock-free).
     * 
     * @param core_pool Core pool
     * @return Connection, or nullptr if queue empty
     */
    PgConnection* remove_from_available_queue(CorePool* core_pool) noexcept;
};
