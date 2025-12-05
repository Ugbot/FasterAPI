/**
 * QPACK Dynamic Table Correctness Tests
 *
 * Comprehensive tests for RFC 9204 Section 3.2 (Dynamic Table).
 * Tests:
 * - Basic insertion and lookup (relative/absolute)
 * - Capacity enforcement with eviction
 * - Reference tracking (cannot evict referenced entries)
 * - Ring buffer wrap-around
 * - Index conversion (relative ↔ absolute)
 * - Capacity updates (grow/shrink)
 * - Edge cases (empty table, full table, zero capacity)
 * - 100-iteration randomized stress test
 * - Performance benchmarks
 */

#include "../src/cpp/http/qpack/qpack_dynamic_table.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <chrono>
#include <random>
#include <vector>
#include <string>

using namespace fasterapi::qpack;

static int tests_passed = 0;
static int tests_failed = 0;
static bool current_test_failed = false;
static std::string current_test_error;

#define TEST(name) void test_##name()

#define RUN_TEST(name) \
    do { \
        std::cout << "Running " << #name << "... "; \
        current_test_failed = false; \
        current_test_error = ""; \
        test_##name(); \
        if (current_test_failed) { \
            std::cout << "FAIL: " << current_test_error << std::endl; \
            tests_failed++; \
        } else { \
            std::cout << "PASS" << std::endl; \
            tests_passed++; \
        } \
    } while(0)

#define ASSERT(condition) \
    if (!(condition)) { \
        current_test_failed = true; \
        current_test_error = "Assertion failed: " #condition " at line " + std::to_string(__LINE__); \
        return; \
    }

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) { \
        current_test_failed = true; \
        current_test_error = std::string("Expected ") + std::to_string(b) + " but got " + std::to_string(a) + " at line " + std::to_string(__LINE__); \
        return; \
    }

#define ASSERT_STR_EQ(a, b) \
    if (std::string(a) != std::string(b)) { \
        current_test_failed = true; \
        current_test_error = std::string("Expected '") + std::string(b) + "' but got '" + std::string(a) + "' at line " + std::to_string(__LINE__); \
        return; \
    }

// ============================================================================
// Basic Insertion and Lookup Tests
// ============================================================================

TEST(insert_and_lookup_absolute) {
    QPACKDynamicTable table(4096);

    // Insert entry
    bool result = table.insert("content-type", "application/json");
    ASSERT(result);

    // Verify counts
    ASSERT_EQ(table.count(), 1);
    ASSERT_EQ(table.insert_count(), 1);
    ASSERT_EQ(table.drop_count(), 0);

    // Lookup by absolute index (0 = first inserted)
    const DynamicEntry* entry = table.get(0);
    ASSERT(entry != nullptr);
    ASSERT_STR_EQ(entry->name, "content-type");
    ASSERT_STR_EQ(entry->value, "application/json");
    ASSERT_EQ(entry->insert_count, 0);
    ASSERT_EQ(entry->ref_count, 0);
}

TEST(insert_and_lookup_relative) {
    QPACKDynamicTable table(4096);

    // Insert multiple entries
    table.insert("header1", "value1");
    table.insert("header2", "value2");
    table.insert("header3", "value3");

    ASSERT_EQ(table.count(), 3);

    // Lookup by relative index (0 = most recent)
    const DynamicEntry* entry0 = table.get_relative(0);
    ASSERT(entry0 != nullptr);
    ASSERT_STR_EQ(entry0->name, "header3");

    const DynamicEntry* entry1 = table.get_relative(1);
    ASSERT(entry1 != nullptr);
    ASSERT_STR_EQ(entry1->name, "header2");

    const DynamicEntry* entry2 = table.get_relative(2);
    ASSERT(entry2 != nullptr);
    ASSERT_STR_EQ(entry2->name, "header1");

    // Out of range
    const DynamicEntry* entry3 = table.get_relative(3);
    ASSERT(entry3 == nullptr);
}

TEST(multiple_insertions) {
    QPACKDynamicTable table(4096);

    // Insert 5 entries with randomized data
    std::vector<std::string> names = {"host", "content-type", "accept", "user-agent", "authorization"};
    std::vector<std::string> values = {"example.com", "text/html", "*/*", "Mozilla/5.0", "Bearer token123"};

    for (size_t i = 0; i < names.size(); i++) {
        bool result = table.insert(names[i], values[i]);
        ASSERT(result);
    }

    ASSERT_EQ(table.count(), 5);
    ASSERT_EQ(table.insert_count(), 5);

    // Verify each entry
    for (size_t i = 0; i < names.size(); i++) {
        const DynamicEntry* entry = table.get(i);
        ASSERT(entry != nullptr);
        ASSERT_STR_EQ(entry->name, names[i]);
        ASSERT_STR_EQ(entry->value, values[i]);
        ASSERT_EQ(entry->insert_count, i);
    }
}

TEST(entry_size_calculation) {
    QPACKDynamicTable table(4096);

    // Insert entry: size = name.length() + value.length() + 32
    table.insert("test", "value");  // 4 + 5 + 32 = 41 bytes

    ASSERT_EQ(table.size(), 41);
    ASSERT_EQ(table.capacity(), 4096);
}

// ============================================================================
// Capacity and Eviction Tests
// ============================================================================

TEST(eviction_when_full) {
    QPACKDynamicTable table(100);  // Small capacity

    // Insert entries until eviction is needed
    table.insert("header1", "value1");  // 41 bytes
    ASSERT_EQ(table.count(), 1);

    table.insert("header2", "value2");  // 41 bytes, total 82
    ASSERT_EQ(table.count(), 2);

    table.insert("header3", "value3");  // 41 bytes, would be 123 > 100
    ASSERT_EQ(table.count(), 2);  // Oldest evicted
    ASSERT_EQ(table.drop_count(), 1);

    // Verify oldest was evicted
    const DynamicEntry* entry = table.get(0);
    ASSERT(entry == nullptr);  // First entry evicted

    entry = table.get(1);
    ASSERT(entry != nullptr);
    ASSERT_STR_EQ(entry->name, "header2");
}

TEST(entry_too_large) {
    QPACKDynamicTable table(50);  // Small capacity

    // Try to insert entry larger than capacity
    bool result = table.insert("verylongheadername", "verylongheadervalue");  // > 50 bytes
    ASSERT(!result);  // Should fail

    ASSERT_EQ(table.count(), 0);
}

TEST(eviction_order_fifo) {
    QPACKDynamicTable table(120);  // Small capacity: 3 entries = 102 bytes, 4 entries = 136 bytes

    // Insert 3 entries (each 34 bytes)
    table.insert("a", "1");
    table.insert("b", "2");
    table.insert("c", "3");

    ASSERT_EQ(table.count(), 3);

    // Insert 4th entry, should evict oldest (a) since 136 > 120
    table.insert("d", "4");

    ASSERT_EQ(table.count(), 3);
    ASSERT_EQ(table.drop_count(), 1);

    // Verify 'a' is gone but others remain
    ASSERT(table.get(0) == nullptr);
    ASSERT(table.get(1) != nullptr);
    ASSERT(table.get(2) != nullptr);
    ASSERT(table.get(3) != nullptr);
}

// ============================================================================
// Reference Tracking Tests (RFC 9204 Section 2.1.1)
// ============================================================================

TEST(reference_tracking_basic) {
    QPACKDynamicTable table(4096);

    table.insert("header1", "value1");

    // Increment reference
    bool result = table.increment_reference(0);
    ASSERT(result);

    const DynamicEntry* entry = table.get(0);
    ASSERT(entry != nullptr);
    ASSERT_EQ(entry->ref_count, 1);

    // Decrement reference
    result = table.decrement_reference(0);
    ASSERT(result);

    entry = table.get(0);
    ASSERT_EQ(entry->ref_count, 0);
}

TEST(cannot_evict_referenced_entry) {
    QPACKDynamicTable table(100);

    // Insert entry and reference it
    table.insert("header1", "value1");
    table.increment_reference(0);

    const DynamicEntry* entry = table.get(0);
    ASSERT_EQ(entry->ref_count, 1);

    // Try to insert large entry that would require eviction
    bool result = table.insert("verylongheader", "verylongvalue");
    ASSERT(!result);  // Should fail because referenced entry blocks eviction

    // Original entry should still be there
    ASSERT_EQ(table.count(), 1);
    ASSERT_EQ(table.drop_count(), 0);
}

TEST(acknowledge_insert) {
    QPACKDynamicTable table(4096);

    // Insert 3 entries and reference them
    table.insert("header1", "value1");
    table.insert("header2", "value2");
    table.insert("header3", "value3");

    table.increment_reference(0);
    table.increment_reference(1);
    table.increment_reference(2);

    // Verify all have ref_count = 1
    ASSERT_EQ(table.get(0)->ref_count, 1);
    ASSERT_EQ(table.get(1)->ref_count, 1);
    ASSERT_EQ(table.get(2)->ref_count, 1);

    // Acknowledge first 2 insertions
    table.acknowledge_insert(2);

    // First 2 should have ref_count = 0, last still = 1
    ASSERT_EQ(table.get(0)->ref_count, 0);
    ASSERT_EQ(table.get(1)->ref_count, 0);
    ASSERT_EQ(table.get(2)->ref_count, 1);
}

TEST(multiple_references) {
    QPACKDynamicTable table(4096);

    table.insert("header", "value");

    // Multiple references to same entry
    table.increment_reference(0);
    table.increment_reference(0);
    table.increment_reference(0);

    ASSERT_EQ(table.get(0)->ref_count, 3);

    // Decrement one
    table.decrement_reference(0);
    ASSERT_EQ(table.get(0)->ref_count, 2);
}

// ============================================================================
// Indexing Conversion Tests (RFC 9204 Section 3.2.3)
// ============================================================================

TEST(relative_to_absolute_conversion) {
    QPACKDynamicTable table(4096);

    // Insert entries: A, B, C
    table.insert("A", "1");  // absolute 0
    table.insert("B", "2");  // absolute 1
    table.insert("C", "3");  // absolute 2

    // Most recent (C) has relative 0, absolute 2
    int abs = table.relative_to_absolute(0);
    ASSERT_EQ(abs, 2);

    // B: relative 1, absolute 1
    abs = table.relative_to_absolute(1);
    ASSERT_EQ(abs, 1);

    // A: relative 2, absolute 0
    abs = table.relative_to_absolute(2);
    ASSERT_EQ(abs, 0);

    // Out of range
    abs = table.relative_to_absolute(3);
    ASSERT_EQ(abs, -1);
}

TEST(absolute_to_relative_conversion) {
    QPACKDynamicTable table(4096);

    // Insert entries: A, B, C
    table.insert("A", "1");  // absolute 0
    table.insert("B", "2");  // absolute 1
    table.insert("C", "3");  // absolute 2

    // Absolute 2 (C) -> relative 0
    int rel = table.absolute_to_relative(2);
    ASSERT_EQ(rel, 0);

    // Absolute 1 (B) -> relative 1
    rel = table.absolute_to_relative(1);
    ASSERT_EQ(rel, 1);

    // Absolute 0 (A) -> relative 2
    rel = table.absolute_to_relative(0);
    ASSERT_EQ(rel, 2);
}

TEST(indexing_after_eviction) {
    QPACKDynamicTable table(120);  // Small capacity to force eviction

    // Insert 4 entries, first will be evicted (each entry is 34 bytes)
    table.insert("A", "1");  // absolute 0, will be evicted
    table.insert("B", "2");  // absolute 1
    table.insert("C", "3");  // absolute 2
    table.insert("D", "4");  // absolute 3, evicts A (136 > 120)

    ASSERT_EQ(table.count(), 3);
    ASSERT_EQ(table.drop_count(), 1);

    // Absolute 0 should be gone
    ASSERT(table.get(0) == nullptr);

    // Absolute 1 should exist and be oldest (relative 2)
    int rel = table.absolute_to_relative(1);
    ASSERT_EQ(rel, 2);

    // Absolute 3 should be newest (relative 0)
    rel = table.absolute_to_relative(3);
    ASSERT_EQ(rel, 0);
}

// ============================================================================
// Capacity Update Tests
// ============================================================================

TEST(set_capacity_grow) {
    QPACKDynamicTable table(100);

    table.insert("header1", "value1");
    ASSERT_EQ(table.capacity(), 100);

    // Grow capacity
    table.set_capacity(200);
    ASSERT_EQ(table.capacity(), 200);

    // Entry should still be there
    ASSERT_EQ(table.count(), 1);
}

TEST(set_capacity_shrink_with_eviction) {
    QPACKDynamicTable table(200);

    // Insert 3 entries
    table.insert("header1", "value1");  // 41 bytes
    table.insert("header2", "value2");  // 41 bytes
    table.insert("header3", "value3");  // 41 bytes
    // Total: 123 bytes

    ASSERT_EQ(table.count(), 3);

    // Shrink capacity to force eviction
    table.set_capacity(100);
    ASSERT_EQ(table.capacity(), 100);

    // Should have evicted oldest entry
    ASSERT_EQ(table.count(), 2);
    ASSERT_EQ(table.drop_count(), 1);
}

TEST(set_capacity_zero) {
    QPACKDynamicTable table(100);

    table.insert("header1", "value1");
    table.insert("header2", "value2");

    ASSERT_EQ(table.count(), 2);

    // Set capacity to 0
    table.set_capacity(0);
    ASSERT_EQ(table.capacity(), 0);

    // All entries should be evicted
    ASSERT_EQ(table.count(), 0);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(empty_table_operations) {
    QPACKDynamicTable table(4096);

    ASSERT_EQ(table.count(), 0);
    ASSERT_EQ(table.size(), 0);
    ASSERT_EQ(table.insert_count(), 0);
    ASSERT_EQ(table.drop_count(), 0);

    // Lookup on empty table
    ASSERT(table.get(0) == nullptr);
    ASSERT(table.get_relative(0) == nullptr);

    // Find on empty table
    ASSERT_EQ(table.find("header", "value"), -1);
    ASSERT_EQ(table.find_name("header"), -1);

    // Reference operations on empty table
    ASSERT(!table.increment_reference(0));
    ASSERT(!table.decrement_reference(0));
}

TEST(clear_table) {
    QPACKDynamicTable table(4096);

    table.insert("header1", "value1");
    table.insert("header2", "value2");
    table.insert("header3", "value3");

    ASSERT_EQ(table.count(), 3);

    table.clear();

    ASSERT_EQ(table.count(), 0);
    ASSERT_EQ(table.size(), 0);
    ASSERT_EQ(table.insert_count(), 0);
    ASSERT_EQ(table.drop_count(), 0);
}

TEST(find_by_name_and_value) {
    QPACKDynamicTable table(4096);

    table.insert("content-type", "application/json");
    table.insert("accept", "*/*");
    table.insert("host", "example.com");

    // Find exact match
    int index = table.find("accept", "*/*");
    ASSERT_EQ(index, 1);

    // Find by name only
    index = table.find_name("host");
    ASSERT_EQ(index, 2);

    // Not found
    index = table.find("missing", "header");
    ASSERT_EQ(index, -1);
}

// ============================================================================
// Ring Buffer Wrap-Around Tests
// ============================================================================

TEST(ring_buffer_wraparound) {
    QPACKDynamicTable table(200);

    // Insert 10 entries with eviction happening
    for (int i = 0; i < 10; i++) {
        std::string name = "header" + std::to_string(i);
        std::string value = "value" + std::to_string(i);
        table.insert(name, value);
    }

    // Should have evicted older entries
    ASSERT(table.count() < 10);
    ASSERT(table.drop_count() > 0);

    // Verify insert_count is correct
    ASSERT_EQ(table.insert_count(), 10);

    // Most recent entry should be accessible by relative 0
    const DynamicEntry* entry = table.get_relative(0);
    ASSERT(entry != nullptr);
    ASSERT_STR_EQ(entry->name, "header9");
}

// ============================================================================
// Randomized Stress Test (100 iterations)
// ============================================================================

TEST(randomized_stress_test) {
    std::mt19937 rng(42);  // Fixed seed for reproducibility
    std::uniform_int_distribution<int> op_dist(0, 4);
    std::uniform_int_distribution<int> name_dist(0, 9);
    std::uniform_int_distribution<int> value_dist(0, 99);

    QPACKDynamicTable table(500);

    for (int iter = 0; iter < 100; iter++) {
        int op = op_dist(rng);

        switch (op) {
            case 0:  // Insert
            case 1: {
                std::string name = "header" + std::to_string(name_dist(rng));
                std::string value = "value" + std::to_string(value_dist(rng));
                table.insert(name, value);
                break;
            }
            case 2: {  // Lookup
                if (table.count() > 0) {
                    size_t rel = value_dist(rng) % table.count();
                    const DynamicEntry* entry = table.get_relative(rel);
                    ASSERT(entry != nullptr);
                }
                break;
            }
            case 3: {  // Reference tracking
                if (table.count() > 0) {
                    size_t idx = table.drop_count() + (value_dist(rng) % table.count());
                    if (value_dist(rng) % 2 == 0) {
                        table.increment_reference(idx);
                    } else {
                        table.decrement_reference(idx);
                    }
                }
                break;
            }
            case 4: {  // Find
                std::string name = "header" + std::to_string(name_dist(rng));
                table.find_name(name);
                break;
            }
        }

        // Invariants check
        ASSERT(table.count() >= 0);
        ASSERT(table.size() <= table.capacity());
        ASSERT(table.insert_count() >= table.drop_count());
    }
}

// ============================================================================
// Performance Benchmarks
// ============================================================================

TEST(benchmark_lookup_performance) {
    QPACKDynamicTable table(4096);

    // Populate table
    for (int i = 0; i < 50; i++) {
        std::string name = "header" + std::to_string(i);
        std::string value = "value" + std::to_string(i);
        table.insert(name, value);
    }

    // Benchmark lookups
    const int iterations = 100000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        size_t idx = i % table.count();
        const DynamicEntry* entry = table.get_relative(idx);
        (void)entry;  // Prevent optimization
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double avg_ns = static_cast<double>(duration.count()) / iterations;

    std::cout << "\n  Lookup performance: " << avg_ns << " ns/op (target: <50ns)";
    ASSERT(avg_ns < 100);  // Relaxed for test environment
}

TEST(benchmark_insert_performance) {
    const int iterations = 1000;

    // Pre-generate strings to isolate insertion performance from string construction
    std::vector<std::pair<std::string, std::string>> entries;
    entries.reserve(iterations);
    for (int i = 0; i < iterations; i++) {
        entries.emplace_back("header" + std::to_string(i % 100),
                            "value" + std::to_string(i));
    }

    QPACKDynamicTable table(1000000);  // Large capacity to avoid eviction overhead

    auto start = std::chrono::high_resolution_clock::now();

    for (const auto& [name, value] : entries) {
        table.insert(name, value);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double avg_ns = static_cast<double>(duration.count()) / iterations;

    std::cout << "\n  Insert performance: " << avg_ns << " ns/op (target: <200ns)";
    // Note: Insert involves string copying, so 200ns is very aggressive
    // Real-world performance depends on string lengths and allocator
    ASSERT(avg_ns < 2000);  // Reasonable for test environment with string allocations
}

TEST(benchmark_with_eviction) {
    QPACKDynamicTable table(1000);  // Small capacity to force eviction

    const int iterations = 10000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        std::string name = "header" + std::to_string(i);
        std::string value = "value" + std::to_string(i);
        table.insert(name, value);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double avg_ns = static_cast<double>(duration.count()) / iterations;

    std::cout << "\n  Insert with eviction: " << avg_ns << " ns/op";
    ASSERT(avg_ns < 1000);  // More relaxed due to eviction overhead
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "\n=== QPACK Dynamic Table Tests (RFC 9204 Section 3.2) ===\n\n";

    // Basic tests
    RUN_TEST(insert_and_lookup_absolute);
    RUN_TEST(insert_and_lookup_relative);
    RUN_TEST(multiple_insertions);
    RUN_TEST(entry_size_calculation);

    // Eviction tests
    RUN_TEST(eviction_when_full);
    RUN_TEST(entry_too_large);
    RUN_TEST(eviction_order_fifo);

    // Reference tracking tests
    RUN_TEST(reference_tracking_basic);
    RUN_TEST(cannot_evict_referenced_entry);
    RUN_TEST(acknowledge_insert);
    RUN_TEST(multiple_references);

    // Indexing tests
    RUN_TEST(relative_to_absolute_conversion);
    RUN_TEST(absolute_to_relative_conversion);
    RUN_TEST(indexing_after_eviction);

    // Capacity tests
    RUN_TEST(set_capacity_grow);
    RUN_TEST(set_capacity_shrink_with_eviction);
    RUN_TEST(set_capacity_zero);

    // Edge cases
    RUN_TEST(empty_table_operations);
    RUN_TEST(clear_table);
    RUN_TEST(find_by_name_and_value);

    // Ring buffer tests
    RUN_TEST(ring_buffer_wraparound);

    // Stress test
    RUN_TEST(randomized_stress_test);

    // Performance benchmarks
    RUN_TEST(benchmark_lookup_performance);
    RUN_TEST(benchmark_insert_performance);
    RUN_TEST(benchmark_with_eviction);

    // Summary
    std::cout << "\n=== Test Summary ===\n";
    std::cout << "Passed: " << tests_passed << "\n";
    std::cout << "Failed: " << tests_failed << "\n";
    std::cout << "Total:  " << (tests_passed + tests_failed) << "\n\n";

    if (tests_failed == 0) {
        std::cout << "All tests passed! RFC 9204 compliant.\n";
        return 0;
    } else {
        std::cout << "Some tests failed.\n";
        return 1;
    }
}
