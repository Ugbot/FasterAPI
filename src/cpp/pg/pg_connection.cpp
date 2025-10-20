#include "pg_connection.h"
#include "pg_connection_impl.h"
#include <cstring>

// Implementation of PgConnection wrapper
PgConnection::PgConnection() noexcept {
    impl_ = nullptr;
}

PgConnection::PgConnection(const char* dsn, int* out_error) noexcept {
    impl_ = PgConnectionImpl::create(dsn);
    if (!impl_) {
        if (out_error) *out_error = 1;  // Connection failed
    } else {
        if (out_error) *out_error = 0;  // Success
    }
}

PgConnection::~PgConnection() = default;

PgResult* PgConnection::exec_query(
    const char* sql,
    uint32_t param_count,
    const char* const* params,
    int* out_error
) noexcept {
    return impl_->exec_query(sql, param_count, params, out_error);
}

uint32_t PgConnection::prepare(
    const char* sql,
    const char* stmt_name,
    int* out_error
) noexcept {
    return impl_->prepare(stmt_name, sql, 0, out_error);  // param_count will be determined from SQL
}

PgResult* PgConnection::exec_prepared(
    uint32_t stmt_id,
    uint32_t param_count,
    const char* const* params,
    int* out_error
) noexcept {
    // Convert stmt_id back to name (simplified)
    char stmt_name[32];
    snprintf(stmt_name, sizeof(stmt_name), "stmt_%u", stmt_id);
    return impl_->exec_prepared(stmt_name, param_count, params, out_error);
}

int PgConnection::begin_tx(const char* isolation, int* out_error) noexcept {
    PgConnectionImpl::IsolationLevel level = PgConnectionImpl::IsolationLevel::READ_COMMITTED;
    if (strcmp(isolation, "REPEATABLE READ") == 0) {
        level = PgConnectionImpl::IsolationLevel::REPEATABLE_READ;
    } else if (strcmp(isolation, "SERIALIZABLE") == 0) {
        level = PgConnectionImpl::IsolationLevel::SERIALIZABLE;
    }
    return impl_->begin_tx(level, out_error);
}

int PgConnection::commit_tx(int* out_error) noexcept {
    return impl_->commit_tx(out_error);
}

int PgConnection::rollback_tx() noexcept {
    return impl_->rollback_tx();
}

int PgConnection::copy_in_start(const char* sql, int* out_error) noexcept {
    return impl_->copy_in_start(sql, out_error);
}

uint64_t PgConnection::copy_in_write(
    const char* data,
    uint64_t length,
    int* out_error
) noexcept {
    return impl_->copy_in_write(data, length, out_error);
}

int PgConnection::copy_in_end(int* out_error) noexcept {
    return impl_->copy_in_end(out_error);
}

int PgConnection::copy_out_start(const char* sql, int* out_error) noexcept {
    return impl_->copy_out_start(sql, out_error);
}

int PgConnection::copy_out_read(
    const char** out_data,
    uint64_t* out_length,
    int* out_error
) noexcept {
    // Simplified implementation - would need proper buffer management
    static char buffer[8192];
    uint64_t bytes_read = impl_->copy_out_read(buffer, sizeof(buffer), out_error);
    if (bytes_read > 0) {
        *out_data = buffer;
        *out_length = bytes_read;
        return 1;  // More data available
    }
    return 0;  // No more data
}

int PgConnection::cancel() noexcept {
    return impl_->cancel();
}

bool PgConnection::is_healthy() const noexcept {
    return impl_->is_healthy();
}

int PgConnection::reset() noexcept {
    return impl_->reset();
}

uint64_t PgConnection::get_id() const noexcept {
    return impl_->get_id();
}
