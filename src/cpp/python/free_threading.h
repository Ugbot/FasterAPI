/**
 * @file free_threading.h
 * @brief Python 3.13+ free-threading (nogil) detection and optimization
 *
 * Python 3.13 introduces PEP 703: Making the Global Interpreter Lock Optional.
 * This enables true multi-threaded Python execution without GIL contention.
 *
 * Key concepts:
 * - Python 3.13+ built with --disable-gil has free-threading
 * - Can check at runtime with Py_IsFreeThreading()
 * - When active, GIL acquire/release becomes no-op
 * - Enables 10-100x speedup for CPU-bound Python on multi-core
 *
 * Trade-offs:
 * - ~40% single-thread overhead due to reference counting changes
 * - Massive multi-core speedup (near-linear scaling)
 * - Not all C extensions are compatible yet
 *
 * References:
 * - PEP 703: https://peps.python.org/pep-0703/
 * - Build docs: https://docs.python.org/3.13/using/configure.html#cmdoption-disable-gil
 */

#pragma once

#include <Python.h>

// Check for Python 3.13+ free-threading support
#if PY_VERSION_HEX >= 0x030D0000
#define FASTERAPI_FREE_THREADING_AVAILABLE
#endif

namespace fasterapi {
namespace python {

/**
 * Free-threading detection and configuration.
 */
class FreeThreading {
public:
    /**
     * Check if Python was built with free-threading support.
     *
     * This checks if the Python interpreter was compiled with --disable-gil.
     * Only available on Python 3.13+.
     *
     * @return true if free-threading is active, false otherwise
     */
    static bool is_enabled() noexcept {
#ifdef FASTERAPI_FREE_THREADING_AVAILABLE
        // Python 3.13+ has Py_IsFreeThreading()
        // Note: This requires linking against Python 3.13+
        #ifdef Py_IsFreeThreading
            return Py_IsFreeThreading() != 0;
        #else
            // Fallback: Check if GIL is enabled (older 3.13 alpha/beta builds)
            // In free-threaded builds, PyGILState_Check() always returns 1
            return false;  // Conservative: assume GIL is present
        #endif
#else
        return false;
#endif
    }

    /**
     * Check if GIL operations are needed.
     *
     * In free-threaded mode, GIL acquire/release is a no-op.
     * This allows us to skip expensive GIL operations when not needed.
     *
     * @return true if GIL operations are needed, false if they can be skipped
     */
    static bool needs_gil() noexcept {
        return !is_enabled();
    }

    /**
     * Get Python version info.
     */
    struct VersionInfo {
        int major;
        int minor;
        int micro;
        bool has_free_threading_support;  // Compiled with --disable-gil
        bool is_free_threading_build;     // Actually running in nogil mode
    };

    static VersionInfo get_version_info() noexcept {
        VersionInfo info;
        info.major = PY_MAJOR_VERSION;
        info.minor = PY_MINOR_VERSION;
        info.micro = PY_MICRO_VERSION;

#ifdef FASTERAPI_FREE_THREADING_AVAILABLE
        info.has_free_threading_support = true;
        info.is_free_threading_build = is_enabled();
#else
        info.has_free_threading_support = false;
        info.is_free_threading_build = false;
#endif

        return info;
    }

    /**
     * Print Python configuration to stdout.
     *
     * Useful for debugging and verifying Python build configuration.
     */
    static void print_info() noexcept;
};

/**
 * Conditional GIL guard that only acquires GIL when needed.
 *
 * In Python 3.13+ free-threaded builds, this becomes a no-op.
 * In Python 3.12 and earlier, this acquires the GIL as usual.
 *
 * Usage:
 * ```cpp
 * {
 *     ConditionalGILGuard gil;
 *     // Call Python API safely
 *     PyObject* result = PyObject_CallNoArgs(callable);
 * }
 * ```
 */
class ConditionalGILGuard {
public:
    ConditionalGILGuard() noexcept {
        if (FreeThreading::needs_gil()) {
            state_ = PyGILState_Ensure();
            acquired_ = true;
        }
    }

    ~ConditionalGILGuard() noexcept {
        if (acquired_) {
            PyGILState_Release(state_);
        }
    }

    // Non-copyable, non-movable
    ConditionalGILGuard(const ConditionalGILGuard&) = delete;
    ConditionalGILGuard& operator=(const ConditionalGILGuard&) = delete;

private:
    PyGILState_STATE state_;
    bool acquired_{false};
};

/**
 * Conditional GIL release guard.
 *
 * Releases GIL for long-running C++ operations, but only if GIL exists.
 * In free-threaded builds, this is a no-op.
 *
 * Usage:
 * ```cpp
 * {
 *     ConditionalGILReleaseGuard no_gil;
 *     // Expensive C++ operation that doesn't touch Python objects
 *     expensive_computation();
 * }
 * ```
 */
class ConditionalGILReleaseGuard {
public:
    ConditionalGILReleaseGuard() noexcept {
        if (FreeThreading::needs_gil()) {
            state_ = PyEval_SaveThread();
            released_ = true;
        }
    }

    ~ConditionalGILReleaseGuard() noexcept {
        if (released_) {
            PyEval_RestoreThread(state_);
        }
    }

    // Non-copyable, non-movable
    ConditionalGILReleaseGuard(const ConditionalGILReleaseGuard&) = delete;
    ConditionalGILReleaseGuard& operator=(const ConditionalGILReleaseGuard&) = delete;

private:
    PyThreadState* state_{nullptr};
    bool released_{false};
};

/**
 * Performance metrics for free-threading vs subinterpreter strategies.
 */
struct ThreadingMetrics {
    // Configuration
    bool using_free_threading;
    bool using_subinterpreters;
    uint32_t num_interpreters;

    // Performance counters
    uint64_t total_requests{0};
    uint64_t total_time_ns{0};
    uint64_t avg_time_ns{0};

    // GIL contention (only relevant when not free-threaded)
    uint64_t gil_wait_time_ns{0};
    uint64_t gil_contentions{0};
};

/**
 * Strategy selector for choosing between subinterpreters and free-threading.
 *
 * Decision logic:
 * - Python 3.13+ with --disable-gil: Use free-threading (best performance)
 * - Python 3.12+ without --disable-gil: Use subinterpreters (good performance)
 * - Python < 3.12: Use main interpreter only (GIL-limited performance)
 */
class ThreadingStrategy {
public:
    enum class Strategy {
        MAIN_INTERPRETER_ONLY,  // Python < 3.12 or fallback
        SUBINTERPRETERS,         // Python 3.12+ with per-interpreter GIL
        FREE_THREADING,          // Python 3.13+ with --disable-gil
    };

    /**
     * Determine optimal threading strategy for current Python version.
     */
    static Strategy get_optimal_strategy() noexcept {
#ifdef FASTERAPI_FREE_THREADING_AVAILABLE
        if (FreeThreading::is_enabled()) {
            return Strategy::FREE_THREADING;
        }
#endif

#ifdef FASTERAPI_SUBINTERPRETERS_AVAILABLE
        return Strategy::SUBINTERPRETERS;
#endif

        return Strategy::MAIN_INTERPRETER_ONLY;
    }

    /**
     * Get strategy name as string.
     */
    static const char* strategy_name(Strategy strategy) noexcept {
        switch (strategy) {
            case Strategy::MAIN_INTERPRETER_ONLY:
                return "main_interpreter_only";
            case Strategy::SUBINTERPRETERS:
                return "subinterpreters";
            case Strategy::FREE_THREADING:
                return "free_threading";
            default:
                return "unknown";
        }
    }

    /**
     * Get expected performance multiplier vs single-threaded.
     *
     * @param strategy Threading strategy
     * @param num_cores Number of CPU cores
     * @return Expected speedup (e.g., 8.0 = 8x faster)
     */
    static double expected_speedup(Strategy strategy, uint32_t num_cores) noexcept {
        switch (strategy) {
            case Strategy::MAIN_INTERPRETER_ONLY:
                return 1.0;  // Single-threaded GIL-limited

            case Strategy::SUBINTERPRETERS:
                // Near-linear scaling, but some overhead
                return num_cores * 0.90;  // ~90% efficiency

            case Strategy::FREE_THREADING:
                // Linear scaling, but ~40% single-thread overhead
                // Net: 0.6 * num_cores for CPU-bound workloads
                return num_cores * 0.60;

            default:
                return 1.0;
        }
    }
};

} // namespace python
} // namespace fasterapi
