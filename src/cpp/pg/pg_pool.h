#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <chrono>

// Forward declarations
class PgConnection;
class PgPoolImpl;

/**
 * High-performance PostgreSQL connection pool with per-core sharding.
 *
 * Features:
 * - Per-core connection affinity (avoid cross-core locks)
 * - Prepared statement caching per connection (LRU)
 * - Health checks with exponential backoff
 * - Transaction pinning (sticky connections)
 * - Zero-copy row decoding
 *
 * Performance targets:
 * - Connection acquisition: < 100µs
 * - Query round-trip (simple): < 500µs
 * - COPY throughput: > 1GB/sec
 */
class PgPool {
public:
    /**
     * Create a new connection pool.
     *
     * @param dsn PostgreSQL connection string
     * @param min_connections Minimum connections per core
     * @param max_connections Maximum connections per core
     * @param idle_timeout_secs Close idle connections after this time
     * @param health_check_interval_secs Health check interval (0 = disabled)
     */
    PgPool(
        const char* dsn,
        uint32_t min_connections = 1,
        uint32_t max_connections = 20,
        uint32_t idle_timeout_secs = 600,
        uint32_t health_check_interval_secs = 30
    );
    
    ~PgPool();
    
    // Non-copyable, movable
    PgPool(const PgPool&) = delete;
    PgPool& operator=(const PgPool&) = delete;
    PgPool(PgPool&&) noexcept;
    PgPool& operator=(PgPool&&) noexcept;
    
    /**
     * Get a connection from the pool.
     *
     * @param core_id CPU core for affinity (connections from same core reused)
     * @param deadline_ms Max time to wait for available connection
     * @param out_error Error code output (0 = success)
     * @return Connection handle (opaque pointer), or nullptr on error
     */
    PgConnection* get(uint32_t core_id, uint64_t deadline_ms, int* out_error) noexcept;
    
    /**
     * Release a connection back to the pool.
     *
     * @param conn Connection from get()
     * @return 0 on success, error code otherwise
     */
    int release(PgConnection* conn) noexcept;
    
    /**
     * Close the pool and all connections.
     *
     * @return 0 on success
     */
    int close() noexcept;
    
    /**
     * Get pool statistics.
     *
     * @param out_in_use Number of connections in use
     * @param out_idle Number of idle connections
     * @param out_waiting Number of threads waiting for connection
     * @param out_total_created Total connections created
     * @param out_total_recycled Total connections recycled
     * @return 0 on success
     */
    int stats(
        uint32_t* out_in_use,
        uint32_t* out_idle,
        uint32_t* out_waiting,
        uint64_t* out_total_created,
        uint64_t* out_total_recycled
    ) const noexcept;
    
private:
    std::unique_ptr<PgPoolImpl> impl_;
};

// Implementation moved to pg_pool.cpp to avoid circular dependencies


/**
 * Statistics snapshot.
 *
 * Captured atomically for thread-safe reading.
 */
struct PgPoolStats {
    uint32_t in_use;
    uint32_t idle;
    uint32_t waiting;
    uint64_t total_created;
    uint64_t total_recycled;
    uint64_t timestamp_us;
};
