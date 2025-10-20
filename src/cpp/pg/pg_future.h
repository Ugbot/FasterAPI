#pragma once

#include "../core/future.h"
#include "pg_connection.h"
#include "pg_result.h"
#include <cstdint>

namespace fasterapi {
namespace pg {

using namespace fasterapi::core;

/**
 * Async PostgreSQL operations using futures.
 * 
 * This header provides future-based wrappers around PgConnection
 * operations, enabling high-performance async query execution.
 */

/**
 * Execute a query asynchronously.
 * 
 * @param conn PostgreSQL connection
 * @param sql SQL query string
 * @param param_count Number of parameters
 * @param params Parameter array
 * @return future<PgResult*> that will resolve with query results
 */
future<PgResult*> exec_async(
    PgConnection* conn,
    const char* sql,
    uint32_t param_count,
    const char* const* params
) noexcept;

/**
 * Execute a prepared statement asynchronously.
 * 
 * @param conn PostgreSQL connection
 * @param stmt_id Statement ID from prepare()
 * @param param_count Number of parameters
 * @param params Parameter array
 * @return future<PgResult*> that will resolve with query results
 */
future<PgResult*> exec_prepared_async(
    PgConnection* conn,
    uint32_t stmt_id,
    uint32_t param_count,
    const char* const* params
) noexcept;

/**
 * Begin a transaction asynchronously.
 * 
 * @param conn PostgreSQL connection
 * @param isolation Isolation level
 * @return future<int> that resolves to 0 on success
 */
future<int> begin_tx_async(
    PgConnection* conn,
    const char* isolation
) noexcept;

/**
 * Commit a transaction asynchronously.
 * 
 * @param conn PostgreSQL connection
 * @return future<int> that resolves to 0 on success
 */
future<int> commit_tx_async(PgConnection* conn) noexcept;

/**
 * Rollback a transaction asynchronously.
 * 
 * @param conn PostgreSQL connection
 * @return future<int> that resolves to 0 on success
 */
future<int> rollback_tx_async(PgConnection* conn) noexcept;

} // namespace pg
} // namespace fasterapi

