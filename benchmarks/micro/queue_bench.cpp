/**
 * @file queue_bench.cpp
 * @brief Google Benchmark suite for lock-free queue implementations
 *
 * Benchmarks:
 * - AeronSPSCQueue: Single Producer, Single Consumer
 * - AeronMPMCQueue: Multi-Producer, Multi-Consumer
 *
 * Performance targets:
 * - SPSC push/pop: <100ns
 * - MPMC push/pop: <500ns (under contention)
 */

#include <benchmark/benchmark.h>
#include "src/cpp/core/lockfree_queue.h"

#include <thread>
#include <vector>
#include <atomic>

using namespace fasterapi::core;

// =============================================================================
// AeronSPSCQueue Benchmarks
// =============================================================================

static void BM_SPSCQueue_TryPush(benchmark::State& state) {
    AeronSPSCQueue<int> queue(8192);
    int value = 42;
    int out;

    for (auto _ : state) {
        queue.try_push(value);
        queue.try_pop(out);  // Keep queue from filling
        benchmark::DoNotOptimize(out);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SPSCQueue_TryPush);

static void BM_SPSCQueue_TryPush_Only(benchmark::State& state) {
    AeronSPSCQueue<int> queue(65536);
    int value = 42;

    for (auto _ : state) {
        benchmark::DoNotOptimize(queue.try_push(value));
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SPSCQueue_TryPush_Only)->Iterations(60000);

static void BM_SPSCQueue_TryPop_Only(benchmark::State& state) {
    AeronSPSCQueue<int> queue(65536);

    // Pre-fill
    for (int i = 0; i < 60000; ++i) {
        queue.try_push(i);
    }

    int out;
    for (auto _ : state) {
        benchmark::DoNotOptimize(queue.try_pop(out));
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SPSCQueue_TryPop_Only)->Iterations(60000);

static void BM_SPSCQueue_MoveSemantics(benchmark::State& state) {
    AeronSPSCQueue<std::string> queue(4096);
    std::string value(64, 'x');  // 64-byte string
    std::string out;

    for (auto _ : state) {
        std::string temp = value;
        queue.try_push(std::move(temp));
        queue.try_pop(out);
        benchmark::DoNotOptimize(out);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SPSCQueue_MoveSemantics);

static void BM_SPSCQueue_Size(benchmark::State& state) {
    AeronSPSCQueue<int> queue(4096);

    // Add some items
    for (int i = 0; i < 100; ++i) {
        queue.try_push(i);
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(queue.size());
    }
}
BENCHMARK(BM_SPSCQueue_Size);

static void BM_SPSCQueue_Empty(benchmark::State& state) {
    AeronSPSCQueue<int> queue(4096);

    for (auto _ : state) {
        benchmark::DoNotOptimize(queue.empty());
    }
}
BENCHMARK(BM_SPSCQueue_Empty);

// Varying queue sizes
static void BM_SPSCQueue_CapacitySweep(benchmark::State& state) {
    const size_t capacity = state.range(0);
    AeronSPSCQueue<int> queue(capacity);
    int value = 42;
    int out;

    for (auto _ : state) {
        queue.try_push(value);
        queue.try_pop(out);
        benchmark::DoNotOptimize(out);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SPSCQueue_CapacitySweep)->Range(64, 65536);

// =============================================================================
// AeronMPMCQueue Benchmarks
// =============================================================================

static void BM_MPMCQueue_TryPush(benchmark::State& state) {
    AeronMPMCQueue<int> queue(8192);
    int value = 42;
    int out;

    for (auto _ : state) {
        queue.try_push(value);
        queue.try_pop(out);
        benchmark::DoNotOptimize(out);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MPMCQueue_TryPush);

static void BM_MPMCQueue_TryPush_Only(benchmark::State& state) {
    AeronMPMCQueue<int> queue(65536);
    int value = 42;

    for (auto _ : state) {
        benchmark::DoNotOptimize(queue.try_push(value));
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MPMCQueue_TryPush_Only)->Iterations(60000);

static void BM_MPMCQueue_TryPop_Only(benchmark::State& state) {
    AeronMPMCQueue<int> queue(65536);

    // Pre-fill
    for (int i = 0; i < 60000; ++i) {
        queue.try_push(i);
    }

    int out;
    for (auto _ : state) {
        benchmark::DoNotOptimize(queue.try_pop(out));
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MPMCQueue_TryPop_Only)->Iterations(60000);

// =============================================================================
// Concurrent SPSC Benchmark
// =============================================================================

static void BM_SPSCQueue_Concurrent(benchmark::State& state) {
    AeronSPSCQueue<int> queue(16384);

    std::atomic<bool> running{true};
    std::atomic<int64_t> consumer_count{0};

    // Consumer thread
    std::thread consumer([&]() {
        int val;
        while (running.load(std::memory_order_relaxed)) {
            if (queue.try_pop(val)) {
                consumer_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Drain remaining
        while (queue.try_pop(val)) {
            consumer_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    int64_t produced = 0;
    for (auto _ : state) {
        while (!queue.try_push(42)) {
            // Spin if full
        }
        ++produced;
    }

    running.store(false, std::memory_order_relaxed);
    consumer.join();

    state.SetItemsProcessed(produced);
    state.counters["consumed"] = consumer_count.load();
}
BENCHMARK(BM_SPSCQueue_Concurrent)->UseRealTime()->Threads(1);

// =============================================================================
// Concurrent MPMC Benchmarks
// =============================================================================

static void BM_MPMCQueue_MultiProducer(benchmark::State& state) {
    static AeronMPMCQueue<int>* shared_queue = nullptr;
    static std::atomic<bool> running{true};
    static std::atomic<int64_t> total_produced{0};

    if (state.thread_index() == 0) {
        shared_queue = new AeronMPMCQueue<int>(65536);
        running.store(true, std::memory_order_relaxed);
        total_produced.store(0, std::memory_order_relaxed);

        // Start consumer thread
        std::thread consumer([&]() {
            int val;
            while (running.load(std::memory_order_relaxed)) {
                shared_queue->try_pop(val);
            }
            // Drain
            while (shared_queue->try_pop(val)) {}
        });
        consumer.detach();
    }

    int64_t local_produced = 0;
    for (auto _ : state) {
        if (shared_queue->try_push(42)) {
            ++local_produced;
        }
    }

    total_produced.fetch_add(local_produced, std::memory_order_relaxed);

    if (state.thread_index() == 0) {
        running.store(false, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        delete shared_queue;
        shared_queue = nullptr;
    }

    state.SetItemsProcessed(local_produced);
}
BENCHMARK(BM_MPMCQueue_MultiProducer)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

static void BM_MPMCQueue_MultiConsumer(benchmark::State& state) {
    static AeronMPMCQueue<int>* shared_queue = nullptr;
    static std::atomic<bool> running{true};
    static std::atomic<int64_t> total_consumed{0};

    if (state.thread_index() == 0) {
        shared_queue = new AeronMPMCQueue<int>(65536);
        running.store(true, std::memory_order_relaxed);
        total_consumed.store(0, std::memory_order_relaxed);

        // Pre-fill queue
        for (int i = 0; i < 50000; ++i) {
            shared_queue->try_push(i);
        }

        // Start producer thread to keep queue fed
        std::thread producer([&]() {
            int i = 0;
            while (running.load(std::memory_order_relaxed)) {
                shared_queue->try_push(i++);
            }
        });
        producer.detach();
    }

    int64_t local_consumed = 0;
    int val;
    for (auto _ : state) {
        if (shared_queue->try_pop(val)) {
            ++local_consumed;
            benchmark::DoNotOptimize(val);
        }
    }

    total_consumed.fetch_add(local_consumed, std::memory_order_relaxed);

    if (state.thread_index() == 0) {
        running.store(false, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        delete shared_queue;
        shared_queue = nullptr;
    }

    state.SetItemsProcessed(local_consumed);
}
BENCHMARK(BM_MPMCQueue_MultiConsumer)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

// =============================================================================
// Comparison: SPSC vs MPMC
// =============================================================================

static void BM_Compare_SPSC_SingleThread(benchmark::State& state) {
    AeronSPSCQueue<int> queue(4096);
    int out;

    for (auto _ : state) {
        queue.try_push(42);
        queue.try_pop(out);
        benchmark::DoNotOptimize(out);
    }

    state.SetItemsProcessed(state.iterations() * 2);
    state.SetLabel("SPSC");
}
BENCHMARK(BM_Compare_SPSC_SingleThread);

static void BM_Compare_MPMC_SingleThread(benchmark::State& state) {
    AeronMPMCQueue<int> queue(4096);
    int out;

    for (auto _ : state) {
        queue.try_push(42);
        queue.try_pop(out);
        benchmark::DoNotOptimize(out);
    }

    state.SetItemsProcessed(state.iterations() * 2);
    state.SetLabel("MPMC");
}
BENCHMARK(BM_Compare_MPMC_SingleThread);

// =============================================================================
// Throughput Tests
// =============================================================================

static void BM_SPSCQueue_Throughput(benchmark::State& state) {
    AeronSPSCQueue<int> queue(32768);
    constexpr int BATCH_SIZE = 1000;

    for (auto _ : state) {
        // Batch push
        for (int i = 0; i < BATCH_SIZE; ++i) {
            while (!queue.try_push(i)) {}
        }

        // Batch pop
        int val;
        for (int i = 0; i < BATCH_SIZE; ++i) {
            while (!queue.try_pop(val)) {}
            benchmark::DoNotOptimize(val);
        }
    }

    state.SetItemsProcessed(state.iterations() * BATCH_SIZE * 2);
}
BENCHMARK(BM_SPSCQueue_Throughput);

// =============================================================================
// Main
// =============================================================================

BENCHMARK_MAIN();
