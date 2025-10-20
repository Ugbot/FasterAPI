#include "pg_pool_impl.h"
#include "pg_connection_impl.h"
#include "pg_connection.h"
#include <thread>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <unistd.h>
#include <sys/syscall.h>
#include <sched.h>

PgPoolImpl::PgPoolImpl(
    const std::string& dsn, 
    uint32_t min_size, 
    uint32_t max_size,
    uint32_t idle_timeout_secs,
    uint32_t health_check_interval_secs
) : dsn_(dsn), 
    min_size_per_core_(min_size), 
    max_size_per_core_(max_size),
    idle_timeout_secs_(idle_timeout_secs),
    health_check_interval_secs_(health_check_interval_secs),
    num_cores_(std::thread::hardware_concurrency()),
    pool_start_time_ns_(get_time_ns()) {
    
    // Initialize per-core pools
    core_pools_.reserve(num_cores_);
    for (uint32_t i = 0; i < num_cores_; ++i) {
        auto core_pool = std::make_unique<CorePool>();
        core_pool->min_size = min_size_per_core_;
        core_pool->max_size = max_size_per_core_;
        core_pools_.push_back(std::move(core_pool));
    }
    
    // Initialize connections for all cores
    for (uint32_t i = 0; i < num_cores_; ++i) {
        init_core_connections(i);
    }
}

PgPoolImpl::~PgPoolImpl() {
    close();
}

PgConnection* PgPoolImpl::get(uint32_t core_id, uint64_t deadline_ms, int* error_out) noexcept {
    if (closed_.load()) {
        if (error_out) *error_out = 3;  // Pool closed
        return nullptr;
    }
    
    // Ensure core_id is valid
    if (core_id >= num_cores_) {
        core_id = get_current_core();
    }
    
    auto* core_pool = core_pools_[core_id].get();
    auto start_time = get_time_ns();
    auto deadline_ns = start_time + (deadline_ms * 1000000ULL);
    
    // Try to get connection from available queue (lock-free)
    PgConnection* conn = remove_from_available_queue(core_pool);
    
    if (conn) {
        // Update statistics
        core_pool->stats.active_connections.fetch_add(1);
        core_pool->stats.idle_connections.fetch_sub(1);
        core_pool->stats.total_requests.fetch_add(1);
        core_pool->stats.last_activity_ns.store(get_time_ns());
        
        if (error_out) *error_out = 0;
        return conn;
    }
    
    // No available connection, try to create new one
    if (core_pool->current_size < core_pool->max_size) {
        auto new_conn = create_connection();
        if (new_conn) {
            core_pool->connections.push_back(std::move(new_conn));
            core_pool->current_size++;
            
            conn = core_pool->connections.back().get();
            core_pool->stats.active_connections.fetch_add(1);
            core_pool->stats.total_requests.fetch_add(1);
            core_pool->stats.last_activity_ns.store(get_time_ns());
            
            if (error_out) *error_out = 0;
            return conn;
        }
    }
    
    // Wait for connection to become available (spin-wait)
    while (get_time_ns() < deadline_ns) {
        conn = remove_from_available_queue(core_pool);
        if (conn) {
            core_pool->stats.active_connections.fetch_add(1);
            core_pool->stats.idle_connections.fetch_sub(1);
            core_pool->stats.total_requests.fetch_add(1);
            core_pool->stats.last_activity_ns.store(get_time_ns());
            
            if (error_out) *error_out = 0;
            return conn;
        }
        
        // Brief pause to avoid busy-waiting
        std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
    
    // Timeout
    core_pool->stats.errors.fetch_add(1);
    if (error_out) *error_out = 4;  // Timeout
    return nullptr;
}

int PgPoolImpl::release(PgConnection* conn) noexcept {
    if (!conn || closed_.load()) {
        return 1;
    }
    
    // Determine which core this connection belongs to
    // For simplicity, we'll use round-robin based on connection ID
    uint32_t core_id = conn->get_id() % num_cores_;
    auto* core_pool = core_pools_[core_id].get();
    
    // Add back to available queue (lock-free)
    add_to_available_queue(core_pool, conn);
    
    // Update statistics
    core_pool->stats.active_connections.fetch_sub(1);
    core_pool->stats.idle_connections.fetch_add(1);
    core_pool->stats.last_activity_ns.store(get_time_ns());
    
    return 0;
}

PgPoolImpl::PoolStats PgPoolImpl::stats() const noexcept {
    PoolStats stats{};
    stats.total_cores = num_cores_;
    stats.min_size_per_core = min_size_per_core_;
    stats.max_size_per_core = max_size_per_core_;
    stats.timestamp_ns = get_time_ns();
    
    for (const auto& core_pool : core_pools_) {
        stats.total_active += core_pool->stats.active_connections.load();
        stats.total_idle += core_pool->stats.idle_connections.load();
        stats.total_requests += core_pool->stats.total_requests.load();
        stats.total_errors += core_pool->stats.errors.load();
        stats.avg_wait_time_ns += core_pool->stats.total_wait_time_ns.load();
    }
    
    if (stats.total_requests > 0) {
        stats.avg_wait_time_ns /= stats.total_requests;
    }
    
    return stats;
}

void PgPoolImpl::close() noexcept {
    if (closed_.exchange(true)) {
        return;  // Already closed
    }
    
    // Close all connections
    for (auto& core_pool : core_pools_) {
        for (auto& conn : core_pool->connections) {
            if (conn) {
                // Connection will be closed in destructor
                conn.reset();
            }
        }
        core_pool->connections.clear();
        core_pool->current_size = 0;
    }
}

int PgPoolImpl::init_core_connections(uint32_t core_id) noexcept {
    if (core_id >= num_cores_) {
        return 1;
    }
    
    auto* core_pool = core_pools_[core_id].get();
    
    // Create minimum number of connections
    for (uint32_t i = 0; i < min_size_per_core_; ++i) {
        auto conn = create_connection();
        if (!conn) {
            return 2;  // Failed to create connection
        }
        
        core_pool->connections.push_back(std::move(conn));
        core_pool->current_size++;
        
        // Add to available queue
        add_to_available_queue(core_pool, core_pool->connections.back().get());
    }
    
    return 0;
}

uint32_t PgPoolImpl::get_current_core() const noexcept {
    // Use thread ID hash to determine core (cross-platform)
    auto thread_id = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return static_cast<uint32_t>(thread_id) % num_cores_;
}

std::unique_ptr<PgConnection> PgPoolImpl::create_connection() noexcept {
    auto conn_impl = PgConnectionImpl::create(dsn_);
    if (!conn_impl) {
        return nullptr;
    }
    
    // Create wrapper connection
    auto conn = std::make_unique<PgConnection>();
    conn->impl_ = std::move(conn_impl);
    
    return conn;
}

void PgPoolImpl::health_check_core(uint32_t core_id) noexcept {
    if (core_id >= num_cores_) {
        return;
    }
    
    auto* core_pool = core_pools_[core_id].get();
    auto now = get_time_ns();
    
    // Check if health check is already running
    if (core_pool->health_check_running.exchange(true)) {
        return;  // Already running
    }
    
    // Perform health check on idle connections
    for (auto& conn : core_pool->connections) {
        if (conn && !conn->is_healthy()) {
            // Connection is unhealthy, remove it
            conn.reset();
            core_pool->current_size--;
        }
    }
    
    // Create new connections if needed
    while (core_pool->current_size < min_size_per_core_) {
        auto new_conn = create_connection();
        if (new_conn) {
            core_pool->connections.push_back(std::move(new_conn));
            core_pool->current_size++;
            add_to_available_queue(core_pool, core_pool->connections.back().get());
        } else {
            break;  // Failed to create connection
        }
    }
    
    core_pool->last_health_check_ns.store(now);
    core_pool->health_check_running.store(false);
}

uint64_t PgPoolImpl::get_time_ns() const noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

void PgPoolImpl::add_to_available_queue(CorePool* core_pool, PgConnection* conn) noexcept {
    // Lock-free queue implementation
    conn->next_available_ = nullptr;
    
    PgConnection* prev_tail = core_pool->available_tail.exchange(conn);
    if (prev_tail) {
        prev_tail->next_available_ = conn;
    } else {
        core_pool->available_head.store(conn);
    }
    
    core_pool->stats.idle_connections.fetch_add(1);
}

PgConnection* PgPoolImpl::remove_from_available_queue(CorePool* core_pool) noexcept {
    // Lock-free queue implementation
    PgConnection* head = core_pool->available_head.load();
    if (!head) {
        return nullptr;
    }
    
    PgConnection* next = head->next_available_;
    if (next) {
        core_pool->available_head.store(next);
    } else {
        // Queue is now empty
        if (core_pool->available_head.compare_exchange_strong(head, nullptr)) {
            core_pool->available_tail.store(nullptr);
        } else {
            // Another thread added something, try again
            return remove_from_available_queue(core_pool);
        }
    }
    
    core_pool->stats.idle_connections.fetch_sub(1);
    return head;
}
