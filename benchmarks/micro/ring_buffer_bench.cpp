/**
 * @file ring_buffer_bench.cpp
 * @brief Google Benchmark suite for ring buffer implementations
 *
 * Benchmarks:
 * - SPSCRingBuffer: Lock-free SPSC ring buffer
 * - RingBuffer: Byte-oriented streaming buffer
 * - MessageBuffer: Length-prefixed message buffer
 *
 * Performance targets:
 * - SPSCRingBuffer write: <50ns
 * - SPSCRingBuffer read: <30ns
 * - RingBuffer write: <100ns per KB
 * - MessageBuffer claim/commit: <100ns
 */

#include <benchmark/benchmark.h>
#include "src/cpp/core/ring_buffer.h"

#include <random>
#include <vector>
#include <thread>

using namespace fasterapi::core;

// =============================================================================
// SPSCRingBuffer Benchmarks
// =============================================================================

static void BM_SPSCRingBuffer_TryWrite(benchmark::State& state) {
    SPSCRingBuffer<int, 4096> buffer;
    int value = 42;
    int out;

    for (auto _ : state) {
        buffer.try_write(value);
        buffer.try_read(out);  // Keep buffer from filling
        benchmark::DoNotOptimize(out);
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SPSCRingBuffer_TryWrite);

static void BM_SPSCRingBuffer_TryWrite_Only(benchmark::State& state) {
    SPSCRingBuffer<int, 65536> buffer;  // Large buffer
    int value = 42;

    for (auto _ : state) {
        benchmark::DoNotOptimize(buffer.try_write(value));
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SPSCRingBuffer_TryWrite_Only)->Iterations(50000);  // Stop before filling

static void BM_SPSCRingBuffer_TryRead_Only(benchmark::State& state) {
    SPSCRingBuffer<int, 65536> buffer;

    // Pre-fill
    for (int i = 0; i < 50000; ++i) {
        buffer.try_write(i);
    }

    int out;
    for (auto _ : state) {
        benchmark::DoNotOptimize(buffer.try_read(out));
    }

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SPSCRingBuffer_TryRead_Only)->Iterations(50000);

static void BM_SPSCRingBuffer_Size_Check(benchmark::State& state) {
    SPSCRingBuffer<int, 4096> buffer;

    // Add some items
    for (int i = 0; i < 100; ++i) {
        buffer.try_write(i);
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(buffer.size());
    }
}
BENCHMARK(BM_SPSCRingBuffer_Size_Check);

static void BM_SPSCRingBuffer_Empty_Check(benchmark::State& state) {
    SPSCRingBuffer<int, 4096> buffer;

    for (auto _ : state) {
        benchmark::DoNotOptimize(buffer.empty());
    }
}
BENCHMARK(BM_SPSCRingBuffer_Empty_Check);

// Different value sizes
template<size_t ValueSize>
static void BM_SPSCRingBuffer_LargeValue(benchmark::State& state) {
    struct LargeValue {
        char data[ValueSize];
    };

    SPSCRingBuffer<LargeValue, 1024> buffer;
    LargeValue value{};
    LargeValue out;

    for (auto _ : state) {
        buffer.try_write(value);
        buffer.try_read(out);
        benchmark::DoNotOptimize(out);
    }

    state.SetBytesProcessed(state.iterations() * ValueSize);
}
BENCHMARK_TEMPLATE(BM_SPSCRingBuffer_LargeValue, 64);
BENCHMARK_TEMPLATE(BM_SPSCRingBuffer_LargeValue, 256);
BENCHMARK_TEMPLATE(BM_SPSCRingBuffer_LargeValue, 1024);

// =============================================================================
// RingBuffer (Byte-oriented) Benchmarks
// =============================================================================

static void BM_RingBuffer_Write(benchmark::State& state) {
    const size_t write_size = state.range(0);
    RingBuffer buffer(1024 * 1024);  // 1MB

    std::vector<uint8_t> data(write_size, 0xAB);

    for (auto _ : state) {
        size_t written = buffer.write(data.data(), data.size());
        benchmark::DoNotOptimize(written);

        // Prevent buffer from filling
        if (buffer.available() > 512 * 1024) {
            buffer.discard(buffer.available() / 2);
        }
    }

    state.SetBytesProcessed(state.iterations() * write_size);
}
BENCHMARK(BM_RingBuffer_Write)->Range(64, 8192);

static void BM_RingBuffer_Read(benchmark::State& state) {
    const size_t read_size = state.range(0);
    RingBuffer buffer(1024 * 1024);

    // Pre-fill
    std::vector<uint8_t> data(512 * 1024, 0xAB);
    buffer.write(data.data(), data.size());

    std::vector<uint8_t> output(read_size);

    for (auto _ : state) {
        size_t read_bytes = buffer.read(output.data(), output.size());
        benchmark::DoNotOptimize(read_bytes);

        // Refill when low
        if (buffer.available() < read_size * 2) {
            buffer.write(data.data(), data.size() / 2);
        }
    }

    state.SetBytesProcessed(state.iterations() * read_size);
}
BENCHMARK(BM_RingBuffer_Read)->Range(64, 8192);

static void BM_RingBuffer_Peek(benchmark::State& state) {
    RingBuffer buffer(65536);

    std::vector<uint8_t> data(32768, 0xAB);
    buffer.write(data.data(), data.size());

    std::vector<uint8_t> output(1024);

    for (auto _ : state) {
        size_t peeked = buffer.peek(output.data(), output.size());
        benchmark::DoNotOptimize(peeked);
    }

    state.SetBytesProcessed(state.iterations() * 1024);
}
BENCHMARK(BM_RingBuffer_Peek);

static void BM_RingBuffer_Available(benchmark::State& state) {
    RingBuffer buffer(65536);

    std::vector<uint8_t> data(32768, 0xAB);
    buffer.write(data.data(), data.size());

    for (auto _ : state) {
        benchmark::DoNotOptimize(buffer.available());
    }
}
BENCHMARK(BM_RingBuffer_Available);

// =============================================================================
// MessageBuffer Benchmarks
// =============================================================================

static void BM_MessageBuffer_ClaimCommit(benchmark::State& state) {
    const size_t msg_size = state.range(0);
    MessageBuffer buffer;

    for (auto _ : state) {
        uint8_t* ptr = buffer.claim(msg_size);
        if (ptr) {
            benchmark::DoNotOptimize(ptr);
            buffer.commit(msg_size);
        }

        // Read to prevent overflow
        const uint8_t* read_ptr;
        size_t read_size;
        if (buffer.available() > MessageBuffer::BUFFER_SIZE / 2) {
            while (buffer.read(&read_ptr, &read_size)) {
                benchmark::DoNotOptimize(read_ptr);
            }
        }
    }

    state.SetBytesProcessed(state.iterations() * msg_size);
}
BENCHMARK(BM_MessageBuffer_ClaimCommit)->Range(64, 16384);

static void BM_MessageBuffer_Read(benchmark::State& state) {
    const size_t msg_size = state.range(0);
    MessageBuffer buffer;

    // Pre-fill with messages
    for (int i = 0; i < 1000; ++i) {
        uint8_t* ptr = buffer.claim(msg_size);
        if (ptr) {
            buffer.commit(msg_size);
        }
    }

    const uint8_t* read_ptr;
    size_t read_size;
    int refill_counter = 0;

    for (auto _ : state) {
        if (buffer.read(&read_ptr, &read_size)) {
            benchmark::DoNotOptimize(read_ptr);
            benchmark::DoNotOptimize(read_size);
        } else {
            // Refill
            for (int i = 0; i < 100; ++i) {
                uint8_t* ptr = buffer.claim(msg_size);
                if (ptr) buffer.commit(msg_size);
            }
        }
    }

    state.SetBytesProcessed(state.iterations() * msg_size);
}
BENCHMARK(BM_MessageBuffer_Read)->Range(64, 16384);

// =============================================================================
// Concurrent Benchmarks
// =============================================================================

static void BM_SPSCRingBuffer_Concurrent(benchmark::State& state) {
    SPSCRingBuffer<int, 8192> buffer;

    std::atomic<bool> running{true};
    std::atomic<int64_t> consumer_count{0};

    // Consumer thread
    std::thread consumer([&]() {
        int val;
        while (running.load(std::memory_order_relaxed)) {
            if (buffer.try_read(val)) {
                consumer_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Drain remaining
        while (buffer.try_read(val)) {
            consumer_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    int64_t produced = 0;
    for (auto _ : state) {
        while (!buffer.try_write(42)) {
            // Spin if full
        }
        ++produced;
    }

    running.store(false, std::memory_order_relaxed);
    consumer.join();

    state.SetItemsProcessed(produced);
    state.counters["consumed"] = consumer_count.load();
}
BENCHMARK(BM_SPSCRingBuffer_Concurrent)->UseRealTime()->Threads(1);

// =============================================================================
// Main
// =============================================================================

BENCHMARK_MAIN();
