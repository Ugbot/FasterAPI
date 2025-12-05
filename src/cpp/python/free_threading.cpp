/**
 * @file free_threading.cpp
 * @brief Implementation of Python 3.13+ free-threading detection
 */

#include "free_threading.h"
#include <iostream>
#include <iomanip>
#include <thread>

namespace fasterapi {
namespace python {

void FreeThreading::print_info() noexcept {
    auto info = get_version_info();

    std::cout << "=== Python Configuration ===\n";
    std::cout << "Version: " << info.major << "." << info.minor << "." << info.micro << "\n";
    std::cout << "Free-threading support: " << (info.has_free_threading_support ? "YES" : "NO") << "\n";
    std::cout << "Free-threading active: " << (info.is_free_threading_build ? "YES" : "NO") << "\n";

#ifdef FASTERAPI_SUBINTERPRETERS_AVAILABLE
    std::cout << "Subinterpreters available: YES (Python 3.12+)\n";
#else
    std::cout << "Subinterpreters available: NO (Python < 3.12)\n";
#endif

    auto strategy = ThreadingStrategy::get_optimal_strategy();
    std::cout << "Optimal strategy: " << ThreadingStrategy::strategy_name(strategy) << "\n";

    // Expected performance
    uint32_t num_cores = std::thread::hardware_concurrency();
    if (num_cores == 0) num_cores = 4;  // Fallback
    double speedup = ThreadingStrategy::expected_speedup(strategy, num_cores);
    std::cout << "Expected speedup (" << num_cores << " cores): "
              << std::fixed << std::setprecision(1) << speedup << "x\n";

    // Recommendations
    std::cout << "\n=== Recommendations ===\n";
    switch (strategy) {
        case ThreadingStrategy::Strategy::FREE_THREADING:
            std::cout << "✓ Using free-threading (optimal!)\n";
            std::cout << "  - True parallel Python execution\n";
            std::cout << "  - No GIL contention\n";
            std::cout << "  - Best for CPU-bound workloads\n";
            break;

        case ThreadingStrategy::Strategy::SUBINTERPRETERS:
            std::cout << "✓ Using subinterpreters (good performance)\n";
            std::cout << "  - Per-interpreter GIL\n";
            std::cout << "  - Near-linear scaling\n";
            std::cout << "  - Upgrade to Python 3.13 --disable-gil for best performance\n";
            break;

        case ThreadingStrategy::Strategy::MAIN_INTERPRETER_ONLY:
            std::cout << "⚠ Using main interpreter only (GIL-limited)\n";
            std::cout << "  - Single-threaded Python execution\n";
            std::cout << "  - Upgrade to Python 3.12+ for subinterpreters\n";
            std::cout << "  - Upgrade to Python 3.13+ --disable-gil for free-threading\n";
            break;
    }

    std::cout << "===========================\n";
}

} // namespace python
} // namespace fasterapi
