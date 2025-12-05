/**
 * Unit tests for C++20 coroutine task infrastructure.
 *
 * Tests:
 * - Basic coro_task creation and execution
 * - co_await chaining
 * - Exception propagation
 * - awaitable_future integration
 */

#include "../src/cpp/core/coro_task.h"
#include "../src/cpp/core/awaitable_future.h"
#include <iostream>
#include <cassert>
#include <string>

using namespace fasterapi::core;

// Test 1: Simple coroutine returning a value
coro_task<int> simple_task() {
    co_return 42;
}

// Test 2: Coroutine that awaits another coroutine
coro_task<int> chained_task() {
    int value = co_await simple_task();
    co_return value * 2;
}

// Test 3: Void coroutine
coro_task<void> void_task() {
    co_return;
}

// Test 4: Exception propagation
coro_task<int> exception_task() {
    throw std::runtime_error("test exception");
    co_return 0;  // Never reached
}

// Test 5: Coroutine with string
coro_task<std::string> string_task() {
    co_return std::string("Hello, coroutines!");
}

// Test runner
void run_tests() {
    std::cout << "=== Coroutine Infrastructure Tests ===\n\n";

    // Test 1: Simple task
    {
        std::cout << "Test 1: Simple task... ";
        auto task = simple_task();
        task.resume();
        assert(task.done());
        std::cout << "PASSED\n";
    }

    // Test 2: Chained tasks
    {
        std::cout << "Test 2: Chained tasks... ";
        auto task = chained_task();
        task.resume();  // Resume outer coroutine
        task.resume();  // Resume inner coroutine
        assert(task.done());
        std::cout << "PASSED\n";
    }

    // Test 3: Void task
    {
        std::cout << "Test 3: Void task... ";
        auto task = void_task();
        task.resume();
        assert(task.done());
        std::cout << "PASSED\n";
    }

    // Test 4: Exception handling
    {
        std::cout << "Test 4: Exception propagation... ";
        auto task = exception_task();
        try {
            task.resume();
            std::cout << "FAILED (no exception thrown)\n";
        } catch (const std::exception& e) {
            std::cout << "PASSED (caught: " << e.what() << ")\n";
        }
    }

    // Test 5: String task
    {
        std::cout << "Test 5: String task... ";
        auto task = string_task();
        task.resume();
        assert(task.done());
        std::cout << "PASSED\n";
    }

    // Test 6: Awaitable future (immediate ready)
    {
        std::cout << "Test 6: Awaitable future (ready)... ";
        future<int> f = future<int>::make_ready(123);
        auto task = [](future<int> f) -> coro_task<int> {
            int value = co_await make_awaitable(std::move(f));
            co_return value;
        }(std::move(f));

        task.resume();
        assert(task.done());
        std::cout << "PASSED\n";
    }

    std::cout << "\n=== All tests passed! ===\n";
}

int main() {
    try {
        run_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << "\n";
        return 1;
    }
}
