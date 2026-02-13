// Coroutine Pool Implementation
//
// Global pool singleton and initialization

#include "coro_pool.h"
#include <mutex>
#include <memory>

namespace fasterapi {
namespace core {

// Global pool singleton
static std::unique_ptr<CoroPool> g_coro_pool;
static std::once_flag g_coro_pool_init_flag;

// Default pool size
static constexpr size_t kDefaultPoolSize = 4096;

CoroPool& global_coro_pool() {
    std::call_once(g_coro_pool_init_flag, []() {
        if (!g_coro_pool) {
            g_coro_pool = std::make_unique<CoroPool>(kDefaultPoolSize);
        }
    });
    return *g_coro_pool;
}

void init_global_coro_pool(size_t pool_size) {
    std::call_once(g_coro_pool_init_flag, [pool_size]() {
        g_coro_pool = std::make_unique<CoroPool>(pool_size);
    });
}

}  // namespace core
}  // namespace fasterapi
