#include "pg_connection_impl.h"
#include "pg_result.h"
#include <libpq-fe.h>
#include <cstring>
#include <chrono>
#include <thread>
#include <random>

// Global connection ID counter
static std::atomic<uint64_t> g_connection_id_counter{1};

std::unique_ptr<PgConnectionImpl> PgConnectionImpl::create(const std::string& dsn) noexcept {
    auto conn = std::make_unique<PgConnectionImpl>();
    if (!conn) {
        return nullptr;
    }
    
    // Connect to PostgreSQL
    conn->conn_ = PQconnectdb(dsn.c_str());
    if (!conn->conn_ || PQstatus(conn->conn_) != CONNECTION_OK) {
        return nullptr;
    }
    
    // Set non-blocking mode for high performance
    if (PQsetnonblocking(conn->conn_, 1) != 0) {
        PQfinish(conn->conn_);
        return nullptr;
    }
    
    // Generate unique connection ID
    conn->connection_id_ = g_connection_id_counter.fetch_add(1);
    conn->update_activity();
    
    return conn;
}

PgConnectionImpl::PgConnectionImpl() noexcept 
    : connection_id_(0), conn_(nullptr) {
}

PgConnectionImpl::~PgConnectionImpl() {
    if (conn_) {
        PQfinish(conn_);
    }
}

PgConnectionImpl::PgConnectionImpl(PgConnectionImpl&& other) noexcept
    : state_(other.state_.load()),
      last_activity_(other.last_activity_.load()),
      connection_id_(other.connection_id_),
      conn_(other.conn_),
      prepared_stmts_(std::move(other.prepared_stmts_)),
      next_stmt_id_(other.next_stmt_id_),
      total_queries_(other.total_queries_.load()),
      total_errors_(other.total_errors_.load()),
      total_bytes_sent_(other.total_bytes_sent_.load()),
      total_bytes_received_(other.total_bytes_received_.load()) {
    other.conn_ = nullptr;
    other.connection_id_ = 0;
}

PgConnectionImpl& PgConnectionImpl::operator=(PgConnectionImpl&& other) noexcept {
    if (this != &other) {
        if (conn_) {
            PQfinish(conn_);
        }
        
        state_ = other.state_.load();
        last_activity_ = other.last_activity_.load();
        connection_id_ = other.connection_id_;
        conn_ = other.conn_;
        prepared_stmts_ = std::move(other.prepared_stmts_);
        next_stmt_id_ = other.next_stmt_id_;
        total_queries_ = other.total_queries_.load();
        total_errors_ = other.total_errors_.load();
        total_bytes_sent_ = other.total_bytes_sent_.load();
        total_bytes_received_ = other.total_bytes_received_.load();
        
        other.conn_ = nullptr;
        other.connection_id_ = 0;
    }
    return *this;
}

PgResult* PgConnectionImpl::exec_query(
    const char* sql,
    uint32_t param_count,
    const char* const* params,
    int* error_out
) noexcept {
    if (!conn_ || state_.load() != State::IDLE) {
        if (error_out) *error_out = 1;
        return nullptr;
    }
    
    set_state(State::BUSY);
    update_activity();
    
    // Execute query
    pg_result* result = nullptr;
    if (param_count == 0) {
        result = PQexec(conn_, sql);
    } else {
        result = PQexecParams(conn_, sql, param_count, nullptr, params, nullptr, nullptr, 0);
    }
    
    if (!result) {
        total_errors_.fetch_add(1);
        set_state(State::ERROR);
        if (error_out) *error_out = 2;  // Query execution failed
        return nullptr;
    }
    
    if (PQresultStatus(result) != PGRES_TUPLES_OK && PQresultStatus(result) != PGRES_COMMAND_OK) {
        total_errors_.fetch_add(1);
        PQclear(result);
        set_state(State::ERROR);
        if (error_out) *error_out = 3;  // Query failed
        return nullptr;
    }
    
    total_queries_.fetch_add(1);
    set_state(State::IDLE);
    
    // Create wrapper result
    auto pg_result = new PgResult(result);
    
    if (error_out) *error_out = 0;
    return pg_result;
}

PgResult* PgConnectionImpl::exec_prepared(
    const char* stmt_name,
    uint32_t param_count,
    const char* const* params,
    int* error_out
) noexcept {
    if (!conn_ || state_.load() != State::IDLE) {
        if (error_out) *error_out = 1;
        return nullptr;
    }
    
    set_state(State::BUSY);
    update_activity();
    
    // Execute prepared statement
    pg_result* result = PQexecPrepared(conn_, stmt_name, param_count, params, nullptr, nullptr, 0);
    
    if (!result) {
        total_errors_.fetch_add(1);
        set_state(State::ERROR);
        if (error_out) *error_out = 2;
        return nullptr;
    }
    
    if (PQresultStatus(result) != PGRES_TUPLES_OK && PQresultStatus(result) != PGRES_COMMAND_OK) {
        total_errors_.fetch_add(1);
        PQclear(result);
        set_state(State::ERROR);
        if (error_out) *error_out = 3;
        return nullptr;
    }
    
    total_queries_.fetch_add(1);
    set_state(State::IDLE);
    
    // Create wrapper result
    auto pg_result = new PgResult(result);
    
    if (error_out) *error_out = 0;
    return pg_result;
}

int PgConnectionImpl::prepare(
    const char* stmt_name,
    const char* sql,
    uint32_t param_count,
    int* error_out
) noexcept {
    if (!conn_ || state_.load() != State::IDLE) {
        if (error_out) *error_out = 1;
        return 1;
    }
    
    set_state(State::BUSY);
    update_activity();
    
    // Prepare statement
    pg_result* result = PQprepare(conn_, stmt_name, sql, param_count, nullptr);
    
    if (!result) {
        total_errors_.fetch_add(1);
        set_state(State::ERROR);
        if (error_out) *error_out = 2;
        return 1;
    }
    
    if (PQresultStatus(result) != PGRES_COMMAND_OK) {
        total_errors_.fetch_add(1);
        PQclear(result);
        set_state(State::ERROR);
        if (error_out) *error_out = 3;
        return 1;
    }
    
    PQclear(result);
    set_state(State::IDLE);
    
    // Store in cache
    PreparedStmt stmt;
    stmt.name = stmt_name;
    stmt.sql = sql;
    stmt.param_count = param_count;
    stmt.last_used = get_time_ns();
    stmt.use_count = 1;
    
    prepared_stmts_[stmt_name] = stmt;
    
    if (error_out) *error_out = 0;
    return 0;
}

int PgConnectionImpl::begin_tx(IsolationLevel isolation, int* error_out) noexcept {
    if (!conn_ || state_.load() != State::IDLE) {
        if (error_out) *error_out = 1;
        return 1;
    }
    
    const char* isolation_str = "READ COMMITTED";
    switch (isolation) {
        case IsolationLevel::REPEATABLE_READ:
            isolation_str = "REPEATABLE READ";
            break;
        case IsolationLevel::SERIALIZABLE:
            isolation_str = "SERIALIZABLE";
            break;
        default:
            break;
    }
    
    char sql[256];
    snprintf(sql, sizeof(sql), "BEGIN TRANSACTION ISOLATION LEVEL %s", isolation_str);
    
    pg_result* result = PQexec(conn_, sql);
    if (!result || PQresultStatus(result) != PGRES_COMMAND_OK) {
        if (result) PQclear(result);
        total_errors_.fetch_add(1);
        if (error_out) *error_out = 2;
        return 1;
    }
    
    PQclear(result);
    set_state(State::TRANSACTION);
    update_activity();
    
    if (error_out) *error_out = 0;
    return 0;
}

int PgConnectionImpl::commit_tx(int* error_out) noexcept {
    if (!conn_ || state_.load() != State::TRANSACTION) {
        if (error_out) *error_out = 1;
        return 1;
    }
    
    pg_result* result = PQexec(conn_, "COMMIT");
    if (!result || PQresultStatus(result) != PGRES_COMMAND_OK) {
        if (result) PQclear(result);
        total_errors_.fetch_add(1);
        if (error_out) *error_out = 2;
        return 1;
    }
    
    PQclear(result);
    set_state(State::IDLE);
    update_activity();
    
    if (error_out) *error_out = 0;
    return 0;
}

int PgConnectionImpl::rollback_tx() noexcept {
    if (!conn_ || state_.load() != State::TRANSACTION) {
        return 1;
    }
    
    pg_result* result = PQexec(conn_, "ROLLBACK");
    if (!result || PQresultStatus(result) != PGRES_COMMAND_OK) {
        if (result) PQclear(result);
        total_errors_.fetch_add(1);
        return 1;
    }
    
    PQclear(result);
    set_state(State::IDLE);
    update_activity();
    
    return 0;
}

int PgConnectionImpl::copy_in_start(const char* sql, int* error_out) noexcept {
    if (!conn_ || state_.load() != State::IDLE) {
        if (error_out) *error_out = 1;
        return 1;
    }
    
    pg_result* result = PQexec(conn_, sql);
    if (!result || PQresultStatus(result) != PGRES_COPY_IN) {
        if (result) PQclear(result);
        total_errors_.fetch_add(1);
        if (error_out) *error_out = 2;
        return 1;
    }
    
    PQclear(result);
    set_state(State::COPY_IN);
    update_activity();
    
    if (error_out) *error_out = 0;
    return 0;
}

uint64_t PgConnectionImpl::copy_in_write(const char* data, uint64_t length, int* error_out) noexcept {
    if (!conn_ || state_.load() != State::COPY_IN) {
        if (error_out) *error_out = 1;
        return 0;
    }
    
    int result = PQputCopyData(conn_, data, static_cast<int>(length));
    if (result < 0) {
        total_errors_.fetch_add(1);
        if (error_out) *error_out = 2;
        return 0;
    }
    
    update_activity();
    return length;
}

int PgConnectionImpl::copy_in_end(int* error_out) noexcept {
    if (!conn_ || state_.load() != State::COPY_IN) {
        if (error_out) *error_out = 1;
        return 1;
    }
    
    int result = PQputCopyEnd(conn_, nullptr);
    if (result < 0) {
        total_errors_.fetch_add(1);
        if (error_out) *error_out = 2;
        return 1;
    }
    
    // Wait for completion
    pg_result* pg_result = PQgetResult(conn_);
    if (!pg_result || PQresultStatus(pg_result) != PGRES_COMMAND_OK) {
        if (pg_result) PQclear(pg_result);
        total_errors_.fetch_add(1);
        if (error_out) *error_out = 3;
        return 1;
    }
    
    PQclear(pg_result);
    set_state(State::IDLE);
    update_activity();
    
    if (error_out) *error_out = 0;
    return 0;
}

int PgConnectionImpl::copy_out_start(const char* sql, int* error_out) noexcept {
    if (!conn_ || state_.load() != State::IDLE) {
        if (error_out) *error_out = 1;
        return 1;
    }
    
    pg_result* result = PQexec(conn_, sql);
    if (!result || PQresultStatus(result) != PGRES_COPY_OUT) {
        if (result) PQclear(result);
        total_errors_.fetch_add(1);
        if (error_out) *error_out = 2;
        return 1;
    }
    
    PQclear(result);
    set_state(State::COPY_OUT);
    update_activity();
    
    if (error_out) *error_out = 0;
    return 0;
}

uint64_t PgConnectionImpl::copy_out_read(char* buffer, uint64_t buffer_size, int* error_out) noexcept {
    if (!conn_ || state_.load() != State::COPY_OUT) {
        if (error_out) *error_out = 1;
        return 0;
    }
    
    int result = PQgetCopyData(conn_, &buffer, static_cast<int>(buffer_size));
    if (result < 0) {
        if (error_out) *error_out = 2;
        return 0;
    }
    
    update_activity();
    return static_cast<uint64_t>(result);
}

int PgConnectionImpl::cancel() noexcept {
    if (!conn_) {
        return 1;
    }
    
    // Send cancel request
    char errbuf[256];
    PGcancel* cancel = PQgetCancel(conn_);
    if (!cancel) {
        return 1;
    }
    
    int result = PQcancel(cancel, errbuf, sizeof(errbuf));
    PQfreeCancel(cancel);
    
    if (result != 1) {
        return 1;
    }
    
    update_activity();
    return 0;
}

bool PgConnectionImpl::is_healthy() const noexcept {
    if (!conn_) {
        return false;
    }
    
    ConnStatusType status = PQstatus(conn_);
    return status == CONNECTION_OK;
}

int PgConnectionImpl::reset() noexcept {
    if (!conn_) {
        return 1;
    }
    
    // Reset connection state
    set_state(State::IDLE);
    update_activity();
    
    // Clear prepared statements cache
    prepared_stmts_.clear();
    
    return 0;
}

void PgConnectionImpl::update_activity() noexcept {
    last_activity_.store(get_time_ns());
}

uint64_t PgConnectionImpl::get_time_ns() const noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

void PgConnectionImpl::cleanup_prepared_stmts() noexcept {
    // Simple LRU eviction - remove oldest statements if cache is full
    const size_t max_cache_size = 100;
    if (prepared_stmts_.size() <= max_cache_size) {
        return;
    }
    
    // Find oldest statement
    auto oldest = prepared_stmts_.begin();
    for (auto it = prepared_stmts_.begin(); it != prepared_stmts_.end(); ++it) {
        if (it->second.last_used < oldest->second.last_used) {
            oldest = it;
        }
    }
    
    // Remove oldest statement
    prepared_stmts_.erase(oldest);
}

void PgConnectionImpl::set_state(State new_state) noexcept {
    state_.store(new_state);
}

