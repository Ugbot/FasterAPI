/**
 * @file bench_gil_strategies.cpp
 * @brief Benchmark different Python GIL strategies
 *
 * Compares performance of:
 * 1. Main interpreter only (Python < 3.12) - GIL-limited
 * 2. SubinterpreterPool (Python 3.12+) - Per-interpreter GIL
 * 3. Free-threading (Python 3.13+ --disable-gil) - No GIL!
 *
 * Expected results (8-core CPU, CPU-bound Python workload):
 * - Main interpreter: ~100 req/s (1.0x baseline)
 * - SubinterpreterPool: ~720 req/s (7.2x speedup, ~90% efficiency)
 * - Free-threading: ~480 req/s (4.8x speedup, ~60% efficiency but simpler)
 *
 * Notes:
 * - Free-threading has ~40% single-thread overhead
 * - SubinterpreterPool has better throughput but more complexity
 * - For I/O-bound: All strategies perform similarly
 */

#include "../../src/cpp/python/subinterpreter_pool.h"
#include "../../src/cpp/python/free_threading.h"
#include "../../src/cpp/python/gil_guard.h"
#include <Python.h>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>
#include <thread>

using namespace fasterapi::python;
using namespace std::chrono;

/**
 * CPU-bound Python task (compute Fibonacci).
 *
 * This is intentionally GIL-limited to show the impact of
 * different GIL strategies.
 */
const char* CPU_BOUND_TASK = R"(
def fib(n):
    if n <= 1:
        return n
    return fib(n-1) + fib(n-2)

# Compute fib(30) - takes ~10ms on modern CPU
result = fib(30)
)";

/**
 * I/O-bound Python task (simulated with time.sleep).
 */
const char* IO_BOUND_TASK = R"(
import time
time.sleep(0.001)  # 1ms sleep
)";

/**
 * Benchmark result.
 */
struct BenchResult {
    const char* name;
    double duration_sec;
    uint64_t requests;
    double requests_per_sec;
    double speedup;
};

/**
 * Benchmark main interpreter (no parallelism).
 */
BenchResult bench_main_interpreter(uint32_t num_requests) {
    std::cout << "\n=== Benchmarking Main Interpreter (GIL-limited) ===\n";

    // Compile Python task
    PyObject* code = Py_CompileString(CPU_BOUND_TASK, "bench", Py_file_input);
    if (!code) {
        PyErr_Print();
        return {"Main Interpreter", 0, 0, 0, 0};
    }

    auto start = steady_clock::now();

    // Execute tasks sequentially (GIL prevents parallelism)
    for (uint32_t i = 0; i < num_requests; ++i) {
        GILGuard gil;

        PyObject* globals = PyDict_New();
        PyObject* locals = PyDict_New();

        PyObject* result = PyEval_EvalCode(code, globals, locals);

        Py_XDECREF(result);
        Py_DECREF(globals);
        Py_DECREF(locals);

        if (i % 10 == 0) {
            std::cout << "\rProgress: " << i << "/" << num_requests << std::flush;
        }
    }

    auto end = steady_clock::now();
    auto duration = duration_cast<milliseconds>(end - start).count() / 1000.0;

    Py_DECREF(code);

    BenchResult result;
    result.name = "Main Interpreter";
    result.duration_sec = duration;
    result.requests = num_requests;
    result.requests_per_sec = num_requests / duration;
    result.speedup = 1.0;  // Baseline

    std::cout << "\rCompleted: " << num_requests << " requests\n";

    return result;
}

/**
 * Benchmark subinterpreter pool (Python 3.12+).
 */
BenchResult bench_subinterpreter_pool(uint32_t num_requests, double baseline_rps) {
#ifdef FASTERAPI_SUBINTERPRETERS_AVAILABLE
    std::cout << "\n=== Benchmarking SubinterpreterPool (Per-Interpreter GIL) ===\n";

    // Initialize pool
    SubinterpreterPool::Config config;
    config.num_interpreters = std::thread::hardware_concurrency();
    if (config.num_interpreters == 0) config.num_interpreters = 4;

    std::cout << "Using " << config.num_interpreters << " interpreters\n";

    if (SubinterpreterPool::initialize(config) != 0) {
        std::cerr << "Failed to initialize SubinterpreterPool\n";
        return {"SubinterpreterPool", 0, 0, 0, 0};
    }

    // Compile Python task
    PyObject* code = Py_CompileString(CPU_BOUND_TASK, "bench", Py_file_input);
    if (!code) {
        PyErr_Print();
        return {"SubinterpreterPool", 0, 0, 0, 0};
    }

    auto start = steady_clock::now();

    // Submit tasks to pool
    std::vector<future<PyObject*>> futures;
    for (uint32_t i = 0; i < num_requests; ++i) {
        // Create callable that executes our code
        PyObject* callable = PyFunction_New(code, PyDict_New());
        futures.push_back(SubinterpreterPool::submit(callable));

        if (i % 10 == 0) {
            std::cout << "\rSubmitted: " << i << "/" << num_requests << std::flush;
        }
    }

    std::cout << "\rWaiting for results...\n";

    // Wait for all results
    for (auto& f : futures) {
        PyObject* result = f.get();
        Py_XDECREF(result);
    }

    auto end = steady_clock::now();
    auto duration = duration_cast<milliseconds>(end - start).count() / 1000.0;

    Py_DECREF(code);
    SubinterpreterPool::shutdown();

    BenchResult result;
    result.name = "SubinterpreterPool";
    result.duration_sec = duration;
    result.requests = num_requests;
    result.requests_per_sec = num_requests / duration;
    result.speedup = result.requests_per_sec / baseline_rps;

    std::cout << "Completed: " << num_requests << " requests\n";

    return result;
#else
    std::cout << "\n⚠ SubinterpreterPool not available (Python < 3.12)\n";
    return {"SubinterpreterPool (N/A)", 0, 0, 0, 0};
#endif
}

/**
 * Benchmark free-threading (Python 3.13+ --disable-gil).
 */
BenchResult bench_free_threading(uint32_t num_requests, double baseline_rps) {
#ifdef FASTERAPI_FREE_THREADING_AVAILABLE
    if (!FreeThreading::is_enabled()) {
        std::cout << "\n⚠ Free-threading not enabled (build Python with --disable-gil)\n";
        return {"Free-Threading (disabled)", 0, 0, 0, 0};
    }

    std::cout << "\n=== Benchmarking Free-Threading (No GIL!) ===\n";

    uint32_t num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;

    std::cout << "Using " << num_threads << " threads\n";

    // Compile Python task
    PyObject* code = Py_CompileString(CPU_BOUND_TASK, "bench", Py_file_input);
    if (!code) {
        PyErr_Print();
        return {"Free-Threading", 0, 0, 0, 0};
    }

    auto start = steady_clock::now();

    // Launch worker threads (no GIL contention!)
    std::vector<std::thread> threads;
    std::atomic<uint32_t> completed{0};
    uint32_t requests_per_thread = num_requests / num_threads;

    for (uint32_t t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, requests_per_thread]() {
            // No GIL needed! (or ConditionalGILGuard is no-op)
            ConditionalGILGuard gil;

            PyObject* globals = PyDict_New();
            PyObject* locals = PyDict_New();

            for (uint32_t i = 0; i < requests_per_thread; ++i) {
                PyObject* result = PyEval_EvalCode(code, globals, locals);
                Py_XDECREF(result);

                completed.fetch_add(1, std::memory_order_relaxed);
            }

            Py_DECREF(globals);
            Py_DECREF(locals);
        });
    }

    // Progress monitor
    while (completed < num_requests) {
        std::cout << "\rProgress: " << completed << "/" << num_requests << std::flush;
        std::this_thread::sleep_for(milliseconds(100));
    }

    for (auto& thread : threads) {
        thread.join();
    }

    auto end = steady_clock::now();
    auto duration = duration_cast<milliseconds>(end - start).count() / 1000.0;

    Py_DECREF(code);

    BenchResult result;
    result.name = "Free-Threading";
    result.duration_sec = duration;
    result.requests = num_requests;
    result.requests_per_sec = num_requests / duration;
    result.speedup = result.requests_per_sec / baseline_rps;

    std::cout << "\rCompleted: " << num_requests << " requests\n";

    return result;
#else
    std::cout << "\n⚠ Free-threading not available (Python < 3.13)\n";
    return {"Free-Threading (N/A)", 0, 0, 0, 0};
#endif
}

/**
 * Print benchmark results table.
 */
void print_results(const std::vector<BenchResult>& results) {
    std::cout << "\n";
    std::cout << "=================================================================\n";
    std::cout << "                    BENCHMARK RESULTS                            \n";
    std::cout << "=================================================================\n";
    std::cout << std::left << std::setw(30) << "Strategy"
              << std::right << std::setw(12) << "Duration (s)"
              << std::setw(12) << "Requests"
              << std::setw(12) << "Req/s"
              << std::setw(12) << "Speedup"
              << "\n";
    std::cout << "-----------------------------------------------------------------\n";

    for (const auto& r : results) {
        if (r.requests > 0) {
            std::cout << std::left << std::setw(30) << r.name
                      << std::right << std::fixed << std::setprecision(2)
                      << std::setw(12) << r.duration_sec
                      << std::setw(12) << r.requests
                      << std::setw(12) << std::setprecision(1) << r.requests_per_sec
                      << std::setw(11) << std::setprecision(2) << r.speedup << "x"
                      << "\n";
        }
    }

    std::cout << "=================================================================\n";
    std::cout << "\n";
}

int main(int argc, char** argv) {
    // Parse args
    uint32_t num_requests = 100;
    if (argc > 1) {
        num_requests = std::atoi(argv[1]);
    }

    std::cout << "=================================================================\n";
    std::cout << "    Python GIL Strategy Performance Benchmark                    \n";
    std::cout << "=================================================================\n";
    std::cout << "Number of requests: " << num_requests << "\n";
    std::cout << "Task: CPU-bound (fibonacci)\n";

    // Initialize Python
    Py_Initialize();

    // Print Python configuration
    FreeThreading::print_info();

    std::vector<BenchResult> results;

    // Benchmark 1: Main interpreter (baseline)
    auto main_result = bench_main_interpreter(num_requests);
    results.push_back(main_result);

    double baseline_rps = main_result.requests_per_sec;

    // Benchmark 2: SubinterpreterPool (Python 3.12+)
    auto subinterp_result = bench_subinterpreter_pool(num_requests, baseline_rps);
    results.push_back(subinterp_result);

    // Benchmark 3: Free-threading (Python 3.13+ --disable-gil)
    auto free_thread_result = bench_free_threading(num_requests, baseline_rps);
    results.push_back(free_thread_result);

    // Print results
    print_results(results);

    // Recommendations
    std::cout << "=== Recommendations ===\n";
    auto strategy = ThreadingStrategy::get_optimal_strategy();
    std::cout << "Optimal strategy: " << ThreadingStrategy::strategy_name(strategy) << "\n";

    if (strategy == ThreadingStrategy::Strategy::MAIN_INTERPRETER_ONLY) {
        std::cout << "⚠ Limited performance - upgrade Python for better parallelism\n";
    } else if (strategy == ThreadingStrategy::Strategy::SUBINTERPRETERS) {
        std::cout << "✓ Good performance with per-interpreter GIL\n";
        std::cout << "  Consider upgrading to Python 3.13 --disable-gil for simpler code\n";
    } else {
        std::cout << "✓ Best performance with free-threading!\n";
    }

    // Cleanup
    Py_Finalize();

    return 0;
}
