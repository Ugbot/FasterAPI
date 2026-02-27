/**
 * BodyBufferArena Unit Tests
 *
 * Tests thread-local tiered body buffer pool:
 * - Tier 1 (4x64KB) and Tier 2 (2x1MB) allocation
 * - Best-fit slot selection
 * - RAII handle release
 * - Pool exhaustion → heap fallback
 * - Move semantics
 */

#include "../src/cpp/http/body_buffer_arena.h"
#include <iostream>
#include <cstring>
#include <vector>
#include <random>
#include <algorithm>

using namespace fasterapi::http;

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
        current_test_error = "Assertion failed: " #condition " (line " + std::to_string(__LINE__) + ")"; \
        return; \
    }

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) { \
        current_test_failed = true; \
        current_test_error = "Expected " + std::to_string(b) + " but got " + std::to_string(a) + " (line " + std::to_string(__LINE__) + ")"; \
        return; \
    }

// ============================================================================
// Tier 1 Allocation
// ============================================================================

TEST(acquire_tier1_small) {
    auto& arena = thread_body_arena();
    size_t before = arena.slots_in_use();

    {
        auto handle = arena.acquire(32 * 1024);  // 32KB → should get 64KB slot
        ASSERT(static_cast<bool>(handle));
        ASSERT(handle.capacity() >= 32 * 1024);
        ASSERT_EQ(handle.capacity(), BodyBufferArena::TIER1_SIZE);
        ASSERT(handle.data() != nullptr);
        ASSERT_EQ(arena.slots_in_use(), before + 1);

        // Write and read back to verify buffer is usable
        std::memset(handle.data(), 0xAB, 32 * 1024);
        ASSERT(handle.data()[0] == 0xAB);
        ASSERT(handle.data()[32 * 1024 - 1] == 0xAB);
    }

    // Handle destroyed — slot released
    ASSERT_EQ(arena.slots_in_use(), before);
}

TEST(acquire_tier1_exact) {
    auto& arena = thread_body_arena();
    auto handle = arena.acquire(BodyBufferArena::TIER1_SIZE);
    ASSERT(static_cast<bool>(handle));
    ASSERT_EQ(handle.capacity(), BodyBufferArena::TIER1_SIZE);
}

// ============================================================================
// Tier 2 Allocation
// ============================================================================

TEST(acquire_tier2) {
    auto& arena = thread_body_arena();
    size_t before = arena.slots_in_use();

    auto handle = arena.acquire(500 * 1024);  // 500KB → should get 1MB slot
    ASSERT(static_cast<bool>(handle));
    ASSERT(handle.capacity() >= 500 * 1024);
    ASSERT_EQ(handle.capacity(), BodyBufferArena::TIER2_SIZE);
    ASSERT_EQ(arena.slots_in_use(), before + 1);

    // Write to the full capacity
    std::memset(handle.data(), 0xCD, handle.capacity());
    ASSERT(handle.data()[handle.capacity() - 1] == 0xCD);
}

// ============================================================================
// Best-Fit Selection
// ============================================================================

TEST(best_fit_prefers_smaller) {
    auto& arena = thread_body_arena();

    // Request 32KB — should get Tier 1 (64KB), NOT Tier 2 (1MB)
    auto handle = arena.acquire(32 * 1024);
    ASSERT(static_cast<bool>(handle));
    ASSERT_EQ(handle.capacity(), BodyBufferArena::TIER1_SIZE);
}

TEST(best_fit_upgrades_when_needed) {
    auto& arena = thread_body_arena();

    // Request 100KB — exceeds Tier 1 (64KB), should get Tier 2 (1MB)
    auto handle = arena.acquire(100 * 1024);
    ASSERT(static_cast<bool>(handle));
    ASSERT_EQ(handle.capacity(), BodyBufferArena::TIER2_SIZE);
}

// ============================================================================
// Heap Fallback
// ============================================================================

TEST(heap_fallback_oversized) {
    auto& arena = thread_body_arena();
    size_t before = arena.slots_in_use();

    // Request 2MB — exceeds all pool slots, should heap-allocate
    auto handle = arena.acquire(2 * 1024 * 1024);
    ASSERT(static_cast<bool>(handle));
    ASSERT(handle.capacity() >= 2 * 1024 * 1024);
    // Heap fallback doesn't count as pool slot
    ASSERT_EQ(arena.slots_in_use(), before);

    // Still usable
    std::memset(handle.data(), 0xEF, 2 * 1024 * 1024);
    ASSERT(handle.data()[2 * 1024 * 1024 - 1] == 0xEF);
}

// ============================================================================
// Pool Exhaustion
// ============================================================================

TEST(pool_exhaustion_falls_back_to_heap) {
    auto& arena = thread_body_arena();

    // Exhaust all Tier 1 slots (4 slots)
    std::vector<BodyBufferArena::Handle> tier1_handles;
    for (size_t i = 0; i < BodyBufferArena::TIER1_COUNT; i++) {
        tier1_handles.push_back(arena.acquire(1024));
        ASSERT(static_cast<bool>(tier1_handles.back()));
    }

    // Exhaust all Tier 2 slots (2 slots)
    std::vector<BodyBufferArena::Handle> tier2_handles;
    for (size_t i = 0; i < BodyBufferArena::TIER2_COUNT; i++) {
        tier2_handles.push_back(arena.acquire(100 * 1024));
        ASSERT(static_cast<bool>(tier2_handles.back()));
    }

    ASSERT_EQ(arena.slots_in_use(), BodyBufferArena::TOTAL_SLOTS);

    // Next acquire should fall back to heap
    auto heap_handle = arena.acquire(1024);
    ASSERT(static_cast<bool>(heap_handle));
    ASSERT(heap_handle.data() != nullptr);
    // Pool still fully used
    ASSERT_EQ(arena.slots_in_use(), BodyBufferArena::TOTAL_SLOTS);

    // Write to heap buffer
    std::memset(heap_handle.data(), 0x42, 1024);
    ASSERT(heap_handle.data()[0] == 0x42);
}

// ============================================================================
// RAII Release
// ============================================================================

TEST(raii_release_recycles_slot) {
    auto& arena = thread_body_arena();
    size_t before = arena.slots_in_use();

    // Acquire and release
    {
        auto handle = arena.acquire(1024);
        ASSERT_EQ(arena.slots_in_use(), before + 1);
    }
    ASSERT_EQ(arena.slots_in_use(), before);

    // Can re-acquire the same slot
    {
        auto handle = arena.acquire(1024);
        ASSERT_EQ(arena.slots_in_use(), before + 1);
    }
    ASSERT_EQ(arena.slots_in_use(), before);
}

// ============================================================================
// Move Semantics
// ============================================================================

TEST(move_construct) {
    auto& arena = thread_body_arena();

    auto handle1 = arena.acquire(4096);
    ASSERT(static_cast<bool>(handle1));
    uint8_t* orig_data = handle1.data();
    size_t orig_cap = handle1.capacity();

    // Move construct
    BodyBufferArena::Handle handle2(std::move(handle1));
    ASSERT(static_cast<bool>(handle2));
    ASSERT(!static_cast<bool>(handle1));
    ASSERT(handle2.data() == orig_data);
    ASSERT_EQ(handle2.capacity(), orig_cap);
}

TEST(move_assign) {
    auto& arena = thread_body_arena();
    size_t before = arena.slots_in_use();

    auto handle1 = arena.acquire(4096);
    auto handle2 = arena.acquire(4096);
    ASSERT_EQ(arena.slots_in_use(), before + 2);

    uint8_t* data2 = handle2.data();

    // Move assign — handle1's old slot should be released
    handle1 = std::move(handle2);
    ASSERT(static_cast<bool>(handle1));
    ASSERT(!static_cast<bool>(handle2));
    ASSERT(handle1.data() == data2);
    ASSERT_EQ(arena.slots_in_use(), before + 1);
}

TEST(default_handle_is_empty) {
    BodyBufferArena::Handle handle;
    ASSERT(!static_cast<bool>(handle));
    ASSERT(handle.data() == nullptr);
    ASSERT_EQ(handle.capacity(), static_cast<size_t>(0));
}

// ============================================================================
// Random Stress
// ============================================================================

TEST(random_acquire_release_stress) {
    auto& arena = thread_body_arena();
    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> size_dist(1, 2 * 1024 * 1024);

    std::vector<BodyBufferArena::Handle> handles;

    for (int i = 0; i < 100; i++) {
        size_t req_size = size_dist(rng);
        auto handle = arena.acquire(req_size);
        ASSERT(static_cast<bool>(handle));
        ASSERT(handle.capacity() >= req_size);

        // Write pattern
        uint8_t pattern = static_cast<uint8_t>(i & 0xFF);
        std::memset(handle.data(), pattern, std::min(req_size, static_cast<size_t>(1024)));

        handles.push_back(std::move(handle));

        // Randomly release some handles
        if (handles.size() > 3 && (rng() % 2 == 0)) {
            std::uniform_int_distribution<size_t> idx_dist(0, handles.size() - 1);
            size_t idx = idx_dist(rng);
            handles.erase(handles.begin() + static_cast<ptrdiff_t>(idx));
        }
    }

    // Release all
    handles.clear();
    ASSERT_EQ(arena.slots_in_use(), static_cast<size_t>(0));
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "=== BodyBufferArena Tests ===" << std::endl;

    RUN_TEST(acquire_tier1_small);
    RUN_TEST(acquire_tier1_exact);
    RUN_TEST(acquire_tier2);
    RUN_TEST(best_fit_prefers_smaller);
    RUN_TEST(best_fit_upgrades_when_needed);
    RUN_TEST(heap_fallback_oversized);
    RUN_TEST(pool_exhaustion_falls_back_to_heap);
    RUN_TEST(raii_release_recycles_slot);
    RUN_TEST(move_construct);
    RUN_TEST(move_assign);
    RUN_TEST(default_handle_is_empty);
    RUN_TEST(random_acquire_release_stress);

    std::cout << "\n=== Results: " << tests_passed << " passed, "
              << tests_failed << " failed ===" << std::endl;
    return tests_failed > 0 ? 1 : 0;
}
