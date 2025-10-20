#include "pg_future.h"
#include "../core/reactor.h"
#include "../core/task.h"

namespace fasterapi {
namespace pg {

using namespace fasterapi::core;

/**
 * Helper to execute blocking PG operation on reactor thread.
 */
template<typename Func>
static auto execute_on_reactor(Func&& func) -> future<std::invoke_result_t<Func>> {
    using result_type = std::invoke_result_t<Func>;
    
    // For now, execute synchronously and return ready future
    // TODO: Make truly async by scheduling on reactor
    // Note: no try/catch due to -fno-exceptions
    auto result = func();
    return make_ready_future<result_type>(static_cast<result_type&&>(result));
}

future<PgResult*> exec_async(
    PgConnection* conn,
    const char* sql,
    uint32_t param_count,
    const char* const* params
) noexcept {
    if (!conn || !sql) {
        return make_exception_future<PgResult*>("invalid parameters");
    }
    
    // Execute synchronously for now
    // TODO: Make async with non-blocking I/O
    int error = 0;
    PgResult* result = conn->exec_query(sql, param_count, params, &error);
    
    if (error != 0 || !result) {
        return make_exception_future<PgResult*>("query execution failed");
    }
    
    return make_ready_future<PgResult*>(result);
}

future<PgResult*> exec_prepared_async(
    PgConnection* conn,
    uint32_t stmt_id,
    uint32_t param_count,
    const char* const* params
) noexcept {
    if (!conn) {
        return make_exception_future<PgResult*>("invalid connection");
    }
    
    int error = 0;
    PgResult* result = conn->exec_prepared(stmt_id, param_count, params, &error);
    
    if (error != 0 || !result) {
        return make_exception_future<PgResult*>("prepared statement execution failed");
    }
    
    return make_ready_future<PgResult*>(result);
}

future<int> begin_tx_async(
    PgConnection* conn,
    const char* isolation
) noexcept {
    if (!conn) {
        return make_exception_future<int>("invalid connection");
    }
    
    int error = 0;
    int result = conn->begin_tx(isolation, &error);
    
    if (error != 0) {
        return make_exception_future<int>("begin transaction failed");
    }
    
    return make_ready_future<int>(result);
}

future<int> commit_tx_async(PgConnection* conn) noexcept {
    if (!conn) {
        return make_exception_future<int>("invalid connection");
    }
    
    int error = 0;
    int result = conn->commit_tx(&error);
    
    if (error != 0) {
        return make_exception_future<int>("commit failed");
    }
    
    return make_ready_future<int>(result);
}

future<int> rollback_tx_async(PgConnection* conn) noexcept {
    if (!conn) {
        return make_exception_future<int>("invalid connection");
    }
    
    int result = conn->rollback_tx();
    return make_ready_future<int>(result);
}

} // namespace pg
} // namespace fasterapi

