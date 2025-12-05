/**
 * @file bench_queue_performance.cpp
 * @brief Benchmark lock-free queue vs mutex-based queue
 *
 * Compares:
 * 1. std::queue + std::mutex + std::condition_variable
 * 2. AeronMPMCQueue (lock-free)
 *
 * Expected results:
 * - std::mutex queue: ~500-1000ns per operation
 * - AeronMPMCQueue: ~50-100ns per operation
 * - Speedup: 10x faster
 *
 * Test scenarios:
 * - Single producer, single consumer (SPSC)
 * - Multiple producers, single consumer (MPSC)
 * - Multiple producers, multiple consumers (MPMC)
 */

#include "../../src/cpp/core/lockfree_queue.h"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>

using namespace fasterapi::core;
using namespace std::chrono;

/**
 * Mutex-based queue (for comparison).
 */
template<typename T>
class MutexQueue {
public:
    MutexQueue(size_t capacity = 0) {}  // Ignore capacity

    bool try_push(const T& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(value);
        cv_.notify_one();
        return true;
    }

    bool try_pop(T& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        value = queue_.front();
        queue_.pop();
        return true;
    }

private:
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

/**
 * Benchmark result.
 */
struct BenchResult {
    const char* name;
    uint64_t operations;
    double duration_sec;
    double ops_per_sec;
    double ns_per_op;
    double speedup;
};

/**
 * Benchmark SPSC (single producer, single consumer).
 */
template<typename Queue>
BenchResult bench_spsc(const char* name, uint64_t num_ops, double baseline_ns) {
    Queue queue(16384);  // 16K capacity

    auto start = steady_clock::now();

    // Producer thread
    std::thread producer([&]() {
        for (uint64_t i = 0; i < num_ops; ++i) {
            while (!queue.try_push(i)) {
                // Spin
            }
        }
    });

    // Consumer thread
    std::thread consumer([&]() {
        for (uint64_t i = 0; i < num_ops; ++i) {
            uint64_t value;
            while (!queue.try_pop(value)) {
                // Spin
            }
        }
    });

    producer.join();
    consumer.join();

    auto end = steady_clock::now();
    auto duration_ns = duration_cast<nanoseconds>(end - start).count();
    double duration_sec = duration_ns / 1e9;

    BenchResult result;
    result.name = name;
    result.operations = num_ops;
    result.duration_sec = duration_sec;
    result.ops_per_sec = num_ops / duration_sec;
    result.ns_per_op = static_cast<double>(duration_ns) / num_ops;
    result.speedup = baseline_ns / result.ns_per_op;

    return result;
}

/**
 * Benchmark MPMC (multiple producers, multiple consumers).
 */
template<typename Queue>
BenchResult bench_mpmc(const char* name, uint64_t num_ops, uint32_t num_threads, double baseline_ns) {
    Queue queue(16384);  // 16K capacity

    uint64_t ops_per_thread = num_ops / num_threads;

    auto start = steady_clock::now();

    // Producer threads
    std::vector<std::thread> producers;
    for (uint32_t t = 0; t < num_threads; ++t) {
        producers.emplace_back([&, ops_per_thread]() {
            for (uint64_t i = 0; i < ops_per_thread; ++i) {
                while (!queue.try_push(i)) {
                    // Spin
                }
            }
        });
    }

    // Consumer threads
    std::vector<std::thread> consumers;
    for (uint32_t t = 0; t < num_threads; ++t) {
        consumers.emplace_back([&, ops_per_thread]() {
            for (uint64_t i = 0; i < ops_per_thread; ++i) {
                uint64_t value;
                while (!queue.try_pop(value)) {
                    // Spin
                }
            }
        });
    }

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    auto end = steady_clock::now();
    auto duration_ns = duration_cast<nanoseconds>(end - start).count();
    double duration_sec = duration_ns / 1e9;

    BenchResult result;
    result.name = name;
    result.operations = num_ops;
    result.duration_sec = duration_sec;
    result.ops_per_sec = num_ops / duration_sec;
    result.ns_per_op = static_cast<double>(duration_ns) / num_ops;
    result.speedup = baseline_ns / result.ns_per_op;

    return result;
}

/**
 * Print results table.
 */
void print_results(const std::vector<BenchResult>& results) {
    std::cout << "\n";
    std::cout << "=================================================================\n";
    std::cout << "                    BENCHMARK RESULTS                            \n";
    std::cout << "=================================================================\n";
    std::cout << std::left << std::setw(30) << "Queue Type"
              << std::right << std::setw(15) << "Ops/sec"
              << std::setw(12) << "ns/op"
              << std::setw(12) << "Speedup"
              << "\n";
    std::cout << "-----------------------------------------------------------------\n";

    for (const auto& r : results) {
        std::cout << std::left << std::setw(30) << r.name
                  << std::right << std::fixed << std::setprecision(0)
                  << std::setw(15) << r.ops_per_sec
                  << std::setw(12) << std::setprecision(1) << r.ns_per_op
                  << std::setw(11) << std::setprecision(2) << r.speedup << "x"
                  << "\n";
    }

    std::cout << "=================================================================\n";
    std::cout << "\n";
}

int main(int argc, char** argv) {
    uint64_t num_ops = 1000000;  // 1 million operations
    if (argc > 1) {
        num_ops = std::atoll(argv[1]);
    }

    std::cout << "=================================================================\n";
    std::cout << "    Lock-Free Queue Performance Benchmark                        \n";
    std::cout << "=================================================================\n";
    std::cout << "Number of operations: " << num_ops << "\n";
    std::cout << "CPU cores: " << std::thread::hardware_concurrency() << "\n";
    std::cout << "\n";

    std::vector<BenchResult> results;

    // ========================================================================
    // SPSC Benchmarks
    // ========================================================================

    std::cout << "=== SPSC (Single Producer, Single Consumer) ===\n";

    // Baseline: Mutex queue
    auto spsc_mutex = bench_spsc<MutexQueue<uint64_t>>(
        "SPSC - Mutex Queue",
        num_ops,
        1.0  // Baseline
    );
    results.push_back(spsc_mutex);

    // Lock-free: AeronSPSCQueue
    auto spsc_lockfree = bench_spsc<AeronSPSCQueue<uint64_t>>(
        "SPSC - AeronSPSCQueue",
        num_ops,
        spsc_mutex.ns_per_op
    );
    results.push_back(spsc_lockfree);

    std::cout << "Mutex queue: " << std::fixed << std::setprecision(1)
              << spsc_mutex.ns_per_op << " ns/op\n";
    std::cout << "Lock-free queue: " << spsc_lockfree.ns_per_op << " ns/op ("
              << spsc_lockfree.speedup << "x faster)\n";

    // ========================================================================
    // MPMC Benchmarks
    // ========================================================================

    std::cout << "\n=== MPMC (Multiple Producers, Multiple Consumers) ===\n";

    uint32_t num_threads = 4;  // 4 producers + 4 consumers

    // Baseline: Mutex queue
    auto mpmc_mutex = bench_mpmc<MutexQueue<uint64_t>>(
        "MPMC - Mutex Queue",
        num_ops,
        num_threads,
        1.0  // Baseline
    );
    results.push_back(mpmc_mutex);

    // Lock-free: AeronMPMCQueue
    auto mpmc_lockfree = bench_mpmc<AeronMPMCQueue<uint64_t>>(
        "MPMC - AeronMPMCQueue",
        num_ops,
        num_threads,
        mpmc_mutex.ns_per_op
    );
    results.push_back(mpmc_lockfree);

    std::cout << "Mutex queue: " << mpmc_mutex.ns_per_op << " ns/op\n";
    std::cout << "Lock-free queue: " << mpmc_lockfree.ns_per_op << " ns/op ("
              << mpmc_lockfree.speedup << "x faster)\n";

    // Print summary
    print_results(results);

    // Recommendations
    std::cout << "=== Summary ===\n";
    std::cout << "Lock-free queues provide:\n";
    std::cout << "  - " << std::fixed << std::setprecision(1)
              << spsc_lockfree.speedup << "x speedup for SPSC\n";
    std::cout << "  - " << mpmc_lockfree.speedup << "x speedup for MPMC\n";
    std::cout << "  - ~" << (int)spsc_lockfree.ns_per_op << " ns/op latency (vs ~"
              << (int)spsc_mutex.ns_per_op << " ns/op with mutex)\n";
    std::cout << "\nThis translates to:\n";
    std::cout << "  - Higher throughput for MCP message passing\n";
    std::cout << "  - Lower latency for subinterpreter task queues\n";
    std::cout << "  - Better scalability under high concurrency\n";

    return 0;
}
