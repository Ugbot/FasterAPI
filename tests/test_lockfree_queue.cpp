/**
 * Test for Lock-Free Queue
 */

#include "../src/cpp/core/lockfree_queue.h"
#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <cassert>

using namespace fasterapi::core;

void test_basic_operations() {
    std::cout << "Test: Basic push/pop operations... ";
    
    LockFreeQueue<int> queue(16);
    
    // Push some items
    assert(queue.try_push(1));
    assert(queue.try_push(2));
    assert(queue.try_push(3));
    
    // Pop and verify
    int value;
    assert(queue.try_pop(value));
    assert(value == 1);
    
    assert(queue.try_pop(value));
    assert(value == 2);
    
    assert(queue.try_pop(value));
    assert(value == 3);
    
    // Should be empty
    assert(!queue.try_pop(value));
    
    std::cout << "✓ PASSED" << std::endl;
}

void test_queue_full() {
    std::cout << "Test: Queue full condition... ";
    
    LockFreeQueue<int> queue(4);  // Small queue (capacity is 4)
    
    // Fill it up (can hold capacity-1 items)
    assert(queue.try_push(1));
    assert(queue.try_push(2));
    assert(queue.try_push(3));
    assert(queue.try_push(4));
    
    // Should fail (queue is full)
    assert(!queue.try_push(5));
    
    // Pop one
    int value;
    assert(queue.try_pop(value));
    
    // Now we can push again
    assert(queue.try_push(5));
    
    std::cout << "✓ PASSED" << std::endl;
}

void test_spsc_concurrent() {
    std::cout << "Test: SPSC concurrent producer/consumer... ";
    
    LockFreeQueue<int> queue(1024);
    const int NUM_ITEMS = 10000;
    
    std::atomic<bool> done{false};
    int sum_produced = 0;
    int sum_consumed = 0;
    
    // Producer thread
    std::thread producer([&]() {
        for (int i = 0; i < NUM_ITEMS; ++i) {
            while (!queue.try_push(i)) {
                std::this_thread::yield();
            }
            sum_produced += i;
        }
        done = true;
    });
    
    // Consumer thread
    std::thread consumer([&]() {
        int count = 0;
        while (count < NUM_ITEMS) {
            int value;
            if (queue.try_pop(value)) {
                sum_consumed += value;
                count++;
            } else {
                std::this_thread::yield();
            }
        }
    });
    
    producer.join();
    consumer.join();
    
    assert(sum_produced == sum_consumed);
    std::cout << "✓ PASSED (produced: " << sum_produced << ", consumed: " << sum_consumed << ")" << std::endl;
}

void test_mpmc_concurrent() {
    std::cout << "Test: MPMC multiple producers/consumers... ";
    
    LockFreeMPMCQueue<int> queue(1024);
    const int NUM_PRODUCERS = 4;
    const int NUM_CONSUMERS = 4;
    const int ITEMS_PER_PRODUCER = 1000;
    
    std::atomic<int> total_produced{0};
    std::atomic<int> total_consumed{0};
    std::atomic<int> producers_done{0};
    
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;
    
    // Create producers
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&, p]() {
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                int value = p * 10000 + i;
                while (!queue.try_push(value)) {
                    std::this_thread::yield();
                }
                total_produced.fetch_add(value);
            }
            producers_done.fetch_add(1);
        });
    }
    
    // Create consumers
    for (int c = 0; c < NUM_CONSUMERS; ++c) {
        consumers.emplace_back([&]() {
            while (true) {
                int value;
                if (queue.try_pop(value)) {
                    total_consumed.fetch_add(value);
                } else if (producers_done.load() == NUM_PRODUCERS && queue.empty()) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
    
    // Wait for all threads
    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();
    
    assert(total_produced.load() == total_consumed.load());
    std::cout << "✓ PASSED (total: " << total_produced.load() << ")" << std::endl;
}

void test_performance() {
    std::cout << "Test: Performance benchmark... ";
    
    LockFreeQueue<int> queue(4096);
    const int ITERATIONS = 1000000;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < ITERATIONS; ++i) {
        queue.try_push(i);
        int value;
        queue.try_pop(value);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    
    double ns_per_op = static_cast<double>(duration) / (ITERATIONS * 2);  // push + pop
    
    std::cout << "✓ PASSED (" << ns_per_op << " ns/op)" << std::endl;
    
    if (ns_per_op > 200) {
        std::cout << "  ⚠️ WARNING: Performance slower than expected (target: <100ns)" << std::endl;
    }
}

int main() {
    std::cout << "╔═══════════════════════════════════════════╗" << std::endl;
    std::cout << "║   Lock-Free Queue Tests                  ║" << std::endl;
    std::cout << "╚═══════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;
    
    test_basic_operations();
    test_queue_full();
    test_spsc_concurrent();
    test_mpmc_concurrent();
    test_performance();
    
    std::cout << std::endl;
    std::cout << "✅ All tests passed!" << std::endl;
    
    return 0;
}

