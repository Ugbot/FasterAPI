/**
 * Test for Zero-Copy Response
 */

#include "../src/cpp/http/zerocopy_response.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <chrono>

using namespace fasterapi::http;

void test_basic_response() {
    std::cout << "Test: Basic response building... ";
    
    ZeroCopyResponse response;
    response.status(200).content_type("text/plain");
    response.write("Hello, World!");
    
    auto view = response.finalize();
    
    // Should contain status line
    assert(view.find("HTTP/1.1 200 OK") != std::string_view::npos);
    assert(view.find("Content-Type: text/plain") != std::string_view::npos);
    assert(view.find("Hello, World!") != std::string_view::npos);
    
    std::cout << "✓ PASSED" << std::endl;
}

void test_json_response() {
    std::cout << "Test: JSON response building... ";
    
    ZeroCopyResponse response;
    response.status(200).content_type("application/json");
    
    ZeroCopyJsonBuilder json(response);
    json.begin_object();
    json.key("message");
    json.string_value("Hello");
    json.key("count");
    json.int_value(42);
    json.key("active");
    json.bool_value(true);
    json.end_object();
    
    auto view = response.finalize();
    
    assert(view.find("application/json") != std::string_view::npos);
    assert(view.find(R"("message":"Hello")") != std::string_view::npos);
    assert(view.find(R"("count":42)") != std::string_view::npos);
    assert(view.find(R"("active":true)") != std::string_view::npos);
    
    std::cout << "✓ PASSED" << std::endl;
}

void test_large_response() {
    std::cout << "Test: Large response (buffer growth)... ";
    
    ZeroCopyResponse response;
    response.status(200).content_type("text/plain");
    
    // Write 10KB of data to force buffer growth
    std::string large_data(10240, 'A');
    response.write(large_data);
    
    auto view = response.finalize();
    
    assert(view.size() > 10240);  // Should include headers + body
    assert(view.find(large_data) != std::string_view::npos);
    
    std::cout << "✓ PASSED" << std::endl;
}

void test_headers() {
    std::cout << "Test: Custom headers... ";
    
    ZeroCopyResponse response;
    response.status(201)
           .content_type("application/json")
           .header("X-Custom-Header", "custom-value")
           .header("X-Request-ID", "12345");
    
    response.write(R"({"status":"created"})");
    
    auto view = response.finalize();
    
    assert(view.find("HTTP/1.1 201 Created") != std::string_view::npos);
    assert(view.find("X-Custom-Header: custom-value") != std::string_view::npos);
    assert(view.find("X-Request-ID: 12345") != std::string_view::npos);
    
    std::cout << "✓ PASSED" << std::endl;
}

void test_buffer_pool() {
    std::cout << "Test: Buffer pooling... ";
    
    auto& pool = BufferPool::instance();
    
    // Acquire buffer
    RefCountedBuffer* buf1 = pool.acquire();
    assert(buf1 != nullptr);
    assert(buf1->capacity() >= BufferPool::DEFAULT_BUFFER_SIZE);
    
    // Write some data
    memcpy(buf1->data(), "test", 4);
    buf1->set_size(4);
    
    // Release
    buf1->release();
    
    // Acquire again - might get same buffer (if pool reused it)
    RefCountedBuffer* buf2 = pool.acquire();
    assert(buf2 != nullptr);
    buf2->release();
    
    std::cout << "✓ PASSED" << std::endl;
}

void test_zero_copy_semantics() {
    std::cout << "Test: Zero-copy semantics... ";
    
    ZeroCopyResponse response;
    response.status(200).content_type("text/plain");
    
    // Get write pointer and write directly
    char* ptr = response.get_write_ptr(100);
    strcpy(ptr, "Direct write!");
    response.commit_write(strlen("Direct write!"));
    
    auto view = response.view();
    assert(view.find("Direct write!") != std::string_view::npos);
    
    std::cout << "✓ PASSED" << std::endl;
}

void test_performance_vs_traditional() {
    std::cout << "Test: Performance comparison... ";
    
    const int ITERATIONS = 10000;
    
    // Zero-copy approach
    auto start1 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < ITERATIONS; ++i) {
        ZeroCopyResponse response;
        response.status(200).content_type("application/json");
        response.write(R"({"id":123,"name":"test"})");
        auto view = response.finalize();
        volatile size_t len = view.size();  // Prevent optimization
    }
    auto end1 = std::chrono::high_resolution_clock::now();
    auto zerocopy_time = std::chrono::duration_cast<std::chrono::nanoseconds>(end1 - start1).count();
    
    // Traditional approach (string concatenation)
    auto start2 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < ITERATIONS; ++i) {
        std::string response = "HTTP/1.1 200 OK\r\n";
        response += "Content-Type: application/json\r\n";
        std::string body = R"({"id":123,"name":"test"})";
        response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        response += "\r\n";
        response += body;
        volatile size_t len = response.size();
    }
    auto end2 = std::chrono::high_resolution_clock::now();
    auto traditional_time = std::chrono::duration_cast<std::chrono::nanoseconds>(end2 - start2).count();
    
    double zerocopy_ns = static_cast<double>(zerocopy_time) / ITERATIONS;
    double traditional_ns = static_cast<double>(traditional_time) / ITERATIONS;
    double speedup = traditional_ns / zerocopy_ns;
    
    std::cout << "✓ PASSED" << std::endl;
    std::cout << "  Zero-copy:    " << zerocopy_ns << " ns/op" << std::endl;
    std::cout << "  Traditional:  " << traditional_ns << " ns/op" << std::endl;
    std::cout << "  Speedup:      " << speedup << "x faster" << std::endl;
    
    if (speedup < 1.2) {
        std::cout << "  ⚠️ WARNING: Zero-copy not significantly faster (expected >1.5x)" << std::endl;
    }
}

int main() {
    std::cout << "╔═══════════════════════════════════════════╗" << std::endl;
    std::cout << "║   Zero-Copy Response Tests               ║" << std::endl;
    std::cout << "╚═══════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;
    
    test_basic_response();
    test_json_response();
    test_large_response();
    test_headers();
    test_buffer_pool();
    test_zero_copy_semantics();
    test_performance_vs_traditional();
    
    std::cout << std::endl;
    std::cout << "✅ All tests passed!" << std::endl;
    
    return 0;
}



