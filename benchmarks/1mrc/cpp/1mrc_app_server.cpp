/**
 * 1 Million Request Challenge - FasterAPI App Server
 *
 * Uses fasterapi::App with pure_cpp_mode = true
 * Runs on UnifiedServer with native kqueue/epoll (no libuv)
 *
 * Endpoints:
 *   POST /event  - Accept event data: {"userId":"...","value":N}
 *   GET  /stats  - Return aggregated statistics
 *   POST /reset  - Reset statistics
 *   GET  /health - Health check
 *
 * Build:
 *   cmake --build build --target 1mrc_app_server
 *
 * Run:
 *   DYLD_LIBRARY_PATH=build/lib ./build/benchmarks/1mrc_app_server
 *
 * Benchmark with wrk:
 *   wrk -t8 -c256 -d30s -s benchmarks/1mrc/wrk_1mrc.lua http://localhost:8000
 *
 * Expected performance: 200K-500K req/s
 */

#include "../../../src/cpp/http/app.h"
#include "../../../src/cpp/http/http1_connection.h"
#include "../../../src/cpp/core/logger.h"

#include <iostream>
#include <csignal>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <charconv>

// ============================================================================
// Lock-free Statistics (Atomic Operations)
// ============================================================================

static std::atomic<uint64_t> g_total_requests{0};
static std::atomic<uint64_t> g_sum_scaled{0};  // Sum * 10000 to avoid floats

// Sharded user tracking to minimize lock contention
constexpr size_t NUM_SHARDS = 64;

struct UserShard {
    std::mutex mutex;
    std::unordered_map<std::string, bool> users;
};

static UserShard g_user_shards[NUM_SHARDS];

inline size_t shard_index(const std::string& user_id) {
    size_t hash = 0;
    for (char c : user_id) {
        hash = hash * 31 + c;
    }
    return hash % NUM_SHARDS;
}

void add_user(const std::string& user_id) {
    size_t shard = shard_index(user_id);
    std::lock_guard<std::mutex> lock(g_user_shards[shard].mutex);
    g_user_shards[shard].users[user_id] = true;
}

size_t count_unique_users() {
    size_t total = 0;
    for (size_t i = 0; i < NUM_SHARDS; ++i) {
        std::lock_guard<std::mutex> lock(g_user_shards[i].mutex);
        total += g_user_shards[i].users.size();
    }
    return total;
}

void reset_stats() {
    g_total_requests.store(0, std::memory_order_relaxed);
    g_sum_scaled.store(0, std::memory_order_relaxed);

    for (size_t i = 0; i < NUM_SHARDS; ++i) {
        std::lock_guard<std::mutex> lock(g_user_shards[i].mutex);
        g_user_shards[i].users.clear();
    }
}

// ============================================================================
// Fast JSON Parsing (Zero-copy)
// ============================================================================

struct EventData {
    std::string user_id;
    double value;
    bool valid;
};

EventData parse_event_json(const std::string& json) {
    EventData data;
    data.valid = false;
    data.value = 0.0;

    const char* str = json.c_str();
    size_t len = json.size();

    // Parse: {"userId":"user_12345","value":499.5}
    const char* user_id_pos = strstr(str, "\"userId\"");
    if (!user_id_pos) return data;

    const char* user_start = strchr(user_id_pos + 8, '"');
    if (!user_start) return data;
    user_start++;

    const char* user_end = strchr(user_start, '"');
    if (!user_end) return data;

    data.user_id = std::string(user_start, user_end - user_start);

    const char* value_pos = strstr(str, "\"value\"");
    if (!value_pos) return data;

    const char* value_start = strchr(value_pos + 7, ':');
    if (!value_start) return data;
    value_start++;

    // Skip whitespace
    while (*value_start == ' ' || *value_start == '\t') value_start++;

    char* value_end;
    data.value = std::strtod(value_start, &value_end);

    if (value_end == value_start) return data;

    data.valid = true;
    return data;
}

// Fast integer to string
inline void append_int(std::string& out, uint64_t val) {
    char buf[24];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), val);
    out.append(buf, ptr - buf);
}

// ============================================================================
// Signal Handler
// ============================================================================

static std::atomic<bool> g_running{true};

void signal_handler(int) {
    std::cout << "\nShutting down..." << std::endl;
    g_running = false;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    uint16_t port = 8000;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Configure logging - use DEBUG level temporarily
    fasterapi::core::Logger::instance().set_level(fasterapi::core::LogLevel::DEBUG);

    std::cout << "==============================================================" << std::endl;
    std::cout << "1MRC - FasterAPI App Server (Pure C++)" << std::endl;
    std::cout << "==============================================================" << std::endl;
    std::cout << std::endl;
    std::cout << "Architecture:" << std::endl;
    std::cout << "  - fasterapi::App with pure_cpp_mode = true" << std::endl;
    std::cout << "  - UnifiedServer (kqueue/epoll - no libuv)" << std::endl;
    std::cout << "  - Lock-free atomic counters" << std::endl;
    std::cout << "  - Sharded user tracking (64 shards)" << std::endl;
    std::cout << std::endl;
    std::cout << "Endpoints:" << std::endl;
    std::cout << "  POST /event  - Accept event data" << std::endl;
    std::cout << "  GET  /stats  - Get aggregated statistics" << std::endl;
    std::cout << "  POST /reset  - Reset statistics" << std::endl;
    std::cout << "  GET  /health - Health check" << std::endl;
    std::cout << std::endl;

    // Create App with pure C++ mode enabled
    fasterapi::App::Config config;
    config.pure_cpp_mode = true;  // No Python/ZMQ bridges
    config.enable_docs = false;    // No OpenAPI docs needed for benchmark

    fasterapi::App app(config);

    // POST /event - Accept event data
    app.post("/event", [](fasterapi::Request& req, fasterapi::Response& res) {
        std::string body = req.body();
        EventData event = parse_event_json(body);

        if (event.valid) {
            // Update statistics atomically
            g_total_requests.fetch_add(1, std::memory_order_relaxed);

            // Scale value by 10000 to avoid float atomics
            uint64_t scaled_value = static_cast<uint64_t>(event.value * 10000);
            g_sum_scaled.fetch_add(scaled_value, std::memory_order_relaxed);

            // Add user (uses sharded locks)
            add_user(event.user_id);

            res.status(201).json(R"({"status":"ok"})");
        } else {
            res.status(400).json(R"({"error":"Invalid request"})");
        }
    });

    // GET /stats - Return aggregated statistics
    app.get("/stats", [](fasterapi::Request& req, fasterapi::Response& res) {
        uint64_t total = g_total_requests.load(std::memory_order_relaxed);
        uint64_t sum_scaled = g_sum_scaled.load(std::memory_order_relaxed);
        double sum = sum_scaled / 10000.0;
        double avg = (total > 0) ? (sum / total) : 0.0;
        size_t unique = count_unique_users();

        // Build JSON manually for speed
        std::string json;
        json.reserve(128);
        json = "{\"totalRequests\":";
        append_int(json, total);
        json += ",\"uniqueUsers\":";
        append_int(json, unique);
        json += ",\"sum\":";

        // Format sum with 2 decimal places
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << sum;
        json += oss.str();

        json += ",\"avg\":";
        oss.str("");
        oss << std::fixed << std::setprecision(2) << avg;
        json += oss.str();
        json += "}";

        res.json(json);
    });

    // POST /reset - Reset statistics
    app.post("/reset", [](fasterapi::Request& req, fasterapi::Response& res) {
        reset_stats();
        res.json(R"({"status":"reset"})");
    });

    // GET /health - Health check
    app.get("/health", [](fasterapi::Request& req, fasterapi::Response& res) {
        res.json(R"({"status":"healthy","mode":"pure_cpp"})");
    });

    // Print registered routes
    std::cout << "Registered routes:" << std::endl;
    for (const auto& [method, path] : app.routes()) {
        std::cout << "  " << method << " " << path << std::endl;
    }
    std::cout << std::endl;

    // Debug: test handle_http1_fast locally
    std::cout << "Testing route matching..." << std::endl;
    fasterapi::http::Http1RequestView test_view;
    test_view.method = std::string_view("GET");
    test_view.path = std::string_view("/health");
    test_view.query_string = std::string_view("");
    test_view.body = std::string_view("");
    test_view.header_count = 0;

    auto test_resp = app.handle_http1_fast(test_view);
    std::cout << "Test response status: " << test_resp.status << std::endl;
    std::cout << "Test response body: " << test_resp.body << std::endl;
    std::cout << std::endl;

    // Ultra-fast callback handles all 1MRC endpoints directly for maximum performance
    app.set_ultra_fast_callback([](const fasterapi::http::Http1RequestView& req,
                                    fasterapi::http::FastResponseWriter& writer) -> size_t {
        // GET /health
        if (req.method == "GET" && req.path == "/health") {
            static constexpr char body[] = R"({"status":"healthy","mode":"pure_cpp"})";
            static constexpr size_t body_len = sizeof(body) - 1;

            writer.write_status_200();
            writer.write_content_type_json();
            writer.write_connection_keepalive();
            writer.write_content_length(body_len);
            writer.write_headers_end();
            writer.write(body, body_len);
            return writer.size;
        }

        // POST /event - Accept event data
        if (req.method == "POST" && req.path == "/event") {
            // Parse JSON from body
            EventData event = parse_event_json(std::string(req.body));

            if (event.valid) {
                // Update statistics atomically
                g_total_requests.fetch_add(1, std::memory_order_relaxed);

                // Scale value by 10000 to avoid float atomics
                uint64_t scaled_value = static_cast<uint64_t>(event.value * 10000);
                g_sum_scaled.fetch_add(scaled_value, std::memory_order_relaxed);

                // Add user (uses sharded locks)
                add_user(event.user_id);

                static constexpr char body[] = R"({"status":"ok"})";
                static constexpr size_t body_len = sizeof(body) - 1;

                writer.write_status_201();
                writer.write_content_type_json();
                writer.write_connection_keepalive();
                writer.write_content_length(body_len);
                writer.write_headers_end();
                writer.write(body, body_len);
            } else {
                static constexpr char body[] = R"({"error":"Invalid request"})";
                static constexpr size_t body_len = sizeof(body) - 1;

                writer.write_status_400();
                writer.write_content_type_json();
                writer.write_connection_keepalive();
                writer.write_content_length(body_len);
                writer.write_headers_end();
                writer.write(body, body_len);
            }
            return writer.size;
        }

        // GET /stats - Return aggregated statistics
        if (req.method == "GET" && req.path == "/stats") {
            uint64_t total = g_total_requests.load(std::memory_order_relaxed);
            uint64_t sum_scaled = g_sum_scaled.load(std::memory_order_relaxed);
            double sum = sum_scaled / 10000.0;
            double avg = (total > 0) ? (sum / total) : 0.0;
            size_t unique = count_unique_users();

            // Build JSON response
            char body[256];
            int body_len = snprintf(body, sizeof(body),
                R"({"totalRequests":%llu,"uniqueUsers":%zu,"sum":%.2f,"avg":%.2f})",
                static_cast<unsigned long long>(total), unique, sum, avg);

            writer.write_status_200();
            writer.write_content_type_json();
            writer.write_connection_keepalive();
            writer.write_content_length(body_len);
            writer.write_headers_end();
            writer.write(body, body_len);
            return writer.size;
        }

        // POST /reset - Reset statistics
        if (req.method == "POST" && req.path == "/reset") {
            reset_stats();

            static constexpr char body[] = R"({"status":"reset"})";
            static constexpr size_t body_len = sizeof(body) - 1;

            writer.write_status_200();
            writer.write_content_type_json();
            writer.write_connection_keepalive();
            writer.write_content_length(body_len);
            writer.write_headers_end();
            writer.write(body, body_len);
            return writer.size;
        }

        // Unhandled - fall through to normal routing
        return 0;
    });

    // Start server
    std::cout << "Starting server on http://0.0.0.0:" << port << std::endl;
    std::cout << "Ready to handle 1,000,000 requests!" << std::endl;
    std::cout << std::endl;
    std::cout << "Benchmark with wrk:" << std::endl;
    std::cout << "  wrk -t8 -c256 -d30s -s benchmarks/1mrc/wrk_1mrc.lua http://localhost:" << port << std::endl;
    std::cout << std::endl;
    std::cout << "Press Ctrl+C to stop..." << std::endl;
    std::cout << "==============================================================" << std::endl;

    int result = app.run_unified("0.0.0.0", port);

    if (result != 0) {
        std::cerr << "Server failed to start with error code: " << result << std::endl;
        return 1;
    }

    return 0;
}
