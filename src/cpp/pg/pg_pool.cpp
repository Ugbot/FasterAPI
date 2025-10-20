#include "pg_pool.h"
#include "pg_pool_impl.h"

// Implementation of PgPool wrapper
PgPool::PgPool(
    const char* dsn,
    uint32_t min_connections,
    uint32_t max_connections,
    uint32_t idle_timeout_secs,
    uint32_t health_check_interval_secs
) {
    impl_ = std::make_unique<PgPoolImpl>(
        dsn, min_connections, max_connections, 
        idle_timeout_secs, health_check_interval_secs
    );
}

PgPool::~PgPool() = default;

PgPool::PgPool(PgPool&&) noexcept = default;
PgPool& PgPool::operator=(PgPool&&) noexcept = default;

PgConnection* PgPool::get(uint32_t core_id, uint64_t deadline_ms, int* out_error) noexcept {
    return impl_->get(core_id, deadline_ms, out_error);
}

int PgPool::release(PgConnection* conn) noexcept {
    return impl_->release(conn);
}

int PgPool::close() noexcept {
    impl_->close();
    return 0;
}

int PgPool::stats(
    uint32_t* out_in_use,
    uint32_t* out_idle,
    uint32_t* out_waiting,
    uint64_t* out_total_created,
    uint64_t* out_total_recycled
) const noexcept {
    auto stats = impl_->stats();
    if (out_in_use) *out_in_use = static_cast<uint32_t>(stats.total_active);
    if (out_idle) *out_idle = static_cast<uint32_t>(stats.total_idle);
    if (out_waiting) *out_waiting = 0;  // Lock-free, no waiting
    if (out_total_created) *out_total_created = stats.total_requests;
    if (out_total_recycled) *out_total_recycled = stats.total_requests;
    return 0;
}
