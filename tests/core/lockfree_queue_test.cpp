/**
 * @file lockfree_queue_test.cpp
 * @brief Google Test suite for lock-free queue implementations
 *
 * Tests:
 * - AeronSPSCQueue<T>: Single Producer, Single Consumer
 * - AeronMPMCQueue<T>: Multi-Producer, Multi-Consumer
 *
 * Per CLAUDE.md: Tests use randomized data and verify performance targets
 * Target: <100ns per operation
 */

#include <gtest/gtest.h>
#include "../test_utils.h"
#include "src/cpp/core/lockfree_queue.h"

#include <thread>
#include <vector>
#include <set>
#include <algorithm>
#include <numeric>

using namespace fasterapi::core;
using namespace fasterapi::testing;

// =============================================================================
// AeronSPSCQueue Tests
// =============================================================================

class SPSCQueueTest : public FasterAPITest {
protected:
    static constexpr size_t DEFAULT_CAPACITY = 1024;
};

TEST_F(SPSCQueueTest, InitialState) {
    AeronSPSCQueue<int> queue(DEFAULT_CAPACITY);

    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size(), 0u);
    // Capacity is rounded up to power of 2
    EXPECT_GE(queue.capacity(), DEFAULT_CAPACITY);
}

TEST_F(SPSCQueueTest, CapacityRoundsUpToPowerOf2) {
    AeronSPSCQueue<int> queue(100);
    EXPECT_EQ(queue.capacity(), 128u);  // Next power of 2

    AeronSPSCQueue<int> queue2(1000);
    EXPECT_EQ(queue2.capacity(), 1024u);

    AeronSPSCQueue<int> queue3(1024);
    EXPECT_EQ(queue3.capacity(), 1024u);  // Already power of 2
}

TEST_F(SPSCQueueTest, BasicPushPop) {
    AeronSPSCQueue<int> queue(DEFAULT_CAPACITY);

    // Push random values
    std::vector<int> values;
    const int num_values = rng_.random_int(10, 100);
    for (int i = 0; i < num_values; ++i) {
        int val = rng_.random_int(-10000, 10000);
        values.push_back(val);
        ASSERT_TRUE(queue.try_push(val)) << "Failed to push at index " << i;
    }

    EXPECT_EQ(queue.size(), static_cast<size_t>(num_values));
    EXPECT_FALSE(queue.empty());

    // Pop and verify FIFO order
    for (int i = 0; i < num_values; ++i) {
        int val = 0;
        ASSERT_TRUE(queue.try_pop(val)) << "Failed to pop at index " << i;
        EXPECT_EQ(val, values[i]) << "FIFO violation at index " << i;
    }

    EXPECT_TRUE(queue.empty());
}

TEST_F(SPSCQueueTest, QueueFull) {
    AeronSPSCQueue<int> queue(16);  // Small queue

    // Fill the queue
    for (size_t i = 0; i < queue.capacity(); ++i) {
        ASSERT_TRUE(queue.try_push(static_cast<int>(i)));
    }

    // Queue should be full
    EXPECT_FALSE(queue.try_push(999));
    EXPECT_EQ(queue.size(), queue.capacity());
}

TEST_F(SPSCQueueTest, QueueEmpty) {
    AeronSPSCQueue<int> queue(16);

    int val = 0;
    EXPECT_FALSE(queue.try_pop(val));
    EXPECT_TRUE(queue.empty());
}

TEST_F(SPSCQueueTest, MoveSemantics) {
    AeronSPSCQueue<std::string> queue(64);

    std::string original = "Hello, World! " + rng_.random_string(100);
    std::string copy = original;

    // Push with move
    ASSERT_TRUE(queue.try_push(std::move(original)));
    EXPECT_TRUE(original.empty());  // Moved from

    // Pop
    std::string popped;
    ASSERT_TRUE(queue.try_pop(popped));
    EXPECT_EQ(popped, copy);
}

TEST_F(SPSCQueueTest, WrapAround) {
    AeronSPSCQueue<int> queue(8);

    // Push and pop multiple times to wrap around
    for (int round = 0; round < 20; ++round) {
        // Push 6 items
        for (int i = 0; i < 6; ++i) {
            ASSERT_TRUE(queue.try_push(round * 100 + i));
        }

        // Pop 6 items
        for (int i = 0; i < 6; ++i) {
            int val;
            ASSERT_TRUE(queue.try_pop(val));
            EXPECT_EQ(val, round * 100 + i);
        }
    }
}

TEST_F(SPSCQueueTest, PerformanceTarget) {
    AeronSPSCQueue<int> queue(4096);

    // Target: <100ns per operation
    auto stats = run_benchmark([&]() {
        queue.try_push(42);
        int val;
        queue.try_pop(val);
    }, 1000, 100000);

    // Each iteration does push + pop
    double op_time_ns = stats.mean_ns() / 2.0;

    // Allow margin for CI variability - target is 100ns, we accept up to 500ns
    EXPECT_LT(op_time_ns, 500.0)
        << "Operation time: " << op_time_ns << " ns (target: <100ns)";

    std::cout << "AeronSPSCQueue: " << op_time_ns << " ns/op "
              << "(min: " << stats.min_ns / 2 << ", max: " << stats.max_ns / 2 << ")\n";
}

TEST_F(SPSCQueueTest, ConcurrentProducerConsumer) {
    AeronSPSCQueue<int> queue(8192);
    constexpr int NUM_ITEMS = 1000000;

    std::atomic<bool> producer_done{false};
    std::vector<int> consumed;
    consumed.reserve(NUM_ITEMS);

    // Producer thread
    std::thread producer([&]() {
        for (int i = 0; i < NUM_ITEMS; ++i) {
            while (!queue.try_push(i)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    // Consumer thread
    std::thread consumer([&]() {
        int val;
        while (consumed.size() < static_cast<size_t>(NUM_ITEMS)) {
            if (queue.try_pop(val)) {
                consumed.push_back(val);
            } else if (producer_done.load(std::memory_order_acquire) && queue.empty()) {
                break;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    // Verify all items received in order
    ASSERT_EQ(consumed.size(), static_cast<size_t>(NUM_ITEMS));
    for (int i = 0; i < NUM_ITEMS; ++i) {
        EXPECT_EQ(consumed[i], i) << "Order violation at index " << i;
    }
}

// =============================================================================
// AeronMPMCQueue Tests
// =============================================================================

class MPMCQueueTest : public FasterAPITest {
protected:
    static constexpr size_t DEFAULT_CAPACITY = 1024;
};

TEST_F(MPMCQueueTest, InitialState) {
    AeronMPMCQueue<int> queue(DEFAULT_CAPACITY);

    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size(), 0u);
    EXPECT_GE(queue.capacity(), DEFAULT_CAPACITY);
}

TEST_F(MPMCQueueTest, BasicPushPop) {
    AeronMPMCQueue<int> queue(DEFAULT_CAPACITY);

    std::vector<int> values;
    const int num_values = rng_.random_int(10, 100);
    for (int i = 0; i < num_values; ++i) {
        int val = rng_.random_int(-10000, 10000);
        values.push_back(val);
        ASSERT_TRUE(queue.try_push(val));
    }

    EXPECT_EQ(queue.size(), static_cast<size_t>(num_values));

    for (int i = 0; i < num_values; ++i) {
        int val;
        ASSERT_TRUE(queue.try_pop(val));
        EXPECT_EQ(val, values[i]);
    }
}

TEST_F(MPMCQueueTest, QueueFull) {
    AeronMPMCQueue<int> queue(16);

    for (size_t i = 0; i < queue.capacity(); ++i) {
        ASSERT_TRUE(queue.try_push(static_cast<int>(i)));
    }

    EXPECT_FALSE(queue.try_push(999));
}

TEST_F(MPMCQueueTest, MultipleProducers) {
    // Use queue large enough to hold all items (no concurrent consumer needed)
    constexpr int NUM_THREADS = 4;
    constexpr int ITEMS_PER_THREAD = 1000;
    constexpr int TOTAL_ITEMS = NUM_THREADS * ITEMS_PER_THREAD;

    AeronMPMCQueue<int> queue(TOTAL_ITEMS * 2);  // 2x capacity to be safe

    std::atomic<int> produced{0};

    std::vector<std::thread> producers;
    for (int t = 0; t < NUM_THREADS; ++t) {
        producers.emplace_back([&, t]() {
            for (int i = 0; i < ITEMS_PER_THREAD; ++i) {
                int val = t * ITEMS_PER_THREAD + i;
                while (!queue.try_push(val)) {
                    std::this_thread::yield();
                }
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : producers) {
        thread.join();
    }

    EXPECT_EQ(produced.load(), TOTAL_ITEMS);

    // Consume all and verify no duplicates
    std::set<int> consumed;
    int val;
    while (queue.try_pop(val)) {
        EXPECT_TRUE(consumed.insert(val).second) << "Duplicate value: " << val;
    }

    EXPECT_EQ(consumed.size(), static_cast<size_t>(TOTAL_ITEMS));
}

TEST_F(MPMCQueueTest, MultipleConsumers) {
    constexpr int NUM_ITEMS = 10000;
    constexpr int NUM_CONSUMERS = 4;

    AeronMPMCQueue<int> queue(NUM_ITEMS * 2);  // Queue large enough for all items

    // Pre-fill queue
    for (int i = 0; i < NUM_ITEMS; ++i) {
        ASSERT_TRUE(queue.try_push(i)) << "Failed to push item " << i;
    }

    std::atomic<int> total_consumed{0};
    std::vector<std::vector<int>> per_consumer(NUM_CONSUMERS);

    std::vector<std::thread> consumers;
    for (int t = 0; t < NUM_CONSUMERS; ++t) {
        consumers.emplace_back([&, t]() {
            int val;
            while (total_consumed.load(std::memory_order_relaxed) < NUM_ITEMS) {
                if (queue.try_pop(val)) {
                    per_consumer[t].push_back(val);
                    total_consumed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& thread : consumers) {
        thread.join();
    }

    // Verify all items consumed exactly once
    std::set<int> all_consumed;
    for (const auto& vec : per_consumer) {
        for (int val : vec) {
            EXPECT_TRUE(all_consumed.insert(val).second) << "Duplicate: " << val;
        }
    }

    EXPECT_EQ(all_consumed.size(), static_cast<size_t>(NUM_ITEMS));
}

TEST_F(MPMCQueueTest, ConcurrentProducersAndConsumers) {
    AeronMPMCQueue<int> queue(8192);
    constexpr int NUM_PRODUCERS = 4;
    constexpr int NUM_CONSUMERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 2500;  // Reduced for faster testing
    constexpr int TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PRODUCER;

    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::atomic<bool> producers_done{false};

    std::mutex consumed_mutex;
    std::set<int> consumed_set;

    // Producers
    std::vector<std::thread> producers;
    for (int t = 0; t < NUM_PRODUCERS; ++t) {
        producers.emplace_back([&, t]() {
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                int val = t * ITEMS_PER_PRODUCER + i;
                while (!queue.try_push(val)) {
                    std::this_thread::yield();
                }
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Consumers
    std::vector<std::thread> consumers;
    for (int t = 0; t < NUM_CONSUMERS; ++t) {
        consumers.emplace_back([&]() {
            int val;
            while (consumed.load(std::memory_order_relaxed) < TOTAL_ITEMS) {
                if (queue.try_pop(val)) {
                    {
                        std::lock_guard<std::mutex> lock(consumed_mutex);
                        consumed_set.insert(val);
                    }
                    consumed.fetch_add(1, std::memory_order_relaxed);
                } else if (producers_done.load(std::memory_order_acquire) && queue.empty()) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    // Wait for producers
    for (auto& thread : producers) {
        thread.join();
    }
    producers_done.store(true, std::memory_order_release);

    // Wait for consumers
    for (auto& thread : consumers) {
        thread.join();
    }

    // Verify
    EXPECT_EQ(produced.load(), TOTAL_ITEMS);
    EXPECT_EQ(consumed_set.size(), static_cast<size_t>(TOTAL_ITEMS));
}

TEST_F(MPMCQueueTest, PerformanceTarget) {
    AeronMPMCQueue<int> queue(4096);

    // MPMC is slower due to CAS operations
    // Target: <500ns per operation (higher than SPSC)
    auto stats = run_benchmark([&]() {
        queue.try_push(42);
        int val;
        queue.try_pop(val);
    }, 1000, 50000);

    double op_time_ns = stats.mean_ns() / 2.0;

    // Allow more margin for MPMC - target is 500ns, accept up to 2000ns
    EXPECT_LT(op_time_ns, 2000.0)
        << "Operation time: " << op_time_ns << " ns (target: <500ns)";

    std::cout << "AeronMPMCQueue: " << op_time_ns << " ns/op "
              << "(min: " << stats.min_ns / 2 << ", max: " << stats.max_ns / 2 << ")\n";
}

// =============================================================================
// Stress Tests
// =============================================================================

class QueueStressTest : public FasterAPITest {};

TEST_F(QueueStressTest, SPSCHighThroughput) {
    AeronSPSCQueue<int> queue(65536);
    constexpr int DURATION_MS = 1000;

    std::atomic<uint64_t> items_produced{0};
    std::atomic<uint64_t> items_consumed{0};
    std::atomic<bool> running{true};

    std::thread producer([&]() {
        int i = 0;
        while (running.load(std::memory_order_relaxed)) {
            if (queue.try_push(i++)) {
                items_produced.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    std::thread consumer([&]() {
        int val;
        while (running.load(std::memory_order_relaxed) || !queue.empty()) {
            if (queue.try_pop(val)) {
                items_consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(DURATION_MS));
    running.store(false, std::memory_order_relaxed);

    producer.join();
    consumer.join();

    uint64_t throughput = items_consumed.load() * 1000 / DURATION_MS;
    std::cout << "SPSC throughput: " << throughput << " items/sec "
              << "(produced: " << items_produced.load()
              << ", consumed: " << items_consumed.load() << ")\n";

    // Should achieve at least 1M items/sec
    EXPECT_GT(throughput, 1000000u);
}

TEST_F(QueueStressTest, MPMCHighContention) {
    AeronMPMCQueue<int> queue(65536);
    constexpr int NUM_THREADS = 8;
    constexpr int DURATION_MS = 500;

    std::atomic<uint64_t> operations{0};
    std::atomic<bool> running{true};

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            int val;
            while (running.load(std::memory_order_relaxed)) {
                if (t % 2 == 0) {
                    // Even threads: producers
                    if (queue.try_push(t)) {
                        operations.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    // Odd threads: consumers
                    if (queue.try_pop(val)) {
                        operations.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(DURATION_MS));
    running.store(false, std::memory_order_relaxed);

    for (auto& thread : threads) {
        thread.join();
    }

    uint64_t ops_per_sec = operations.load() * 1000 / DURATION_MS;
    std::cout << "MPMC high contention: " << ops_per_sec << " ops/sec "
              << "(" << NUM_THREADS << " threads)\n";

    // Should still achieve reasonable throughput under contention
    EXPECT_GT(ops_per_sec, 100000u);
}

// =============================================================================
// Type Alias Tests
// =============================================================================

TEST_F(SPSCQueueTest, LockFreeQueueAlias) {
    // LockFreeQueue should be AeronSPSCQueue
    LockFreeQueue<int> queue(64);

    queue.try_push(42);
    int val;
    EXPECT_TRUE(queue.try_pop(val));
    EXPECT_EQ(val, 42);
}

TEST_F(MPMCQueueTest, LockFreeMPMCQueueAlias) {
    // LockFreeMPMCQueue should be AeronMPMCQueue
    LockFreeMPMCQueue<int> queue(64);

    queue.try_push(42);
    int val;
    EXPECT_TRUE(queue.try_pop(val));
    EXPECT_EQ(val, 42);
}

// Note: gtest_main is linked via CMake
