/**
 * Middleware Example - Request logging, authentication, rate limiting
 *
 * Demonstrates:
 * - Global middleware (applied to all routes)
 * - Path-specific middleware
 * - Request logging with timing
 * - API key authentication
 * - Simple rate limiting
 * - CORS headers
 * - Middleware chaining (next() pattern)
 *
 * Build:
 *   cmake --build build --target middleware_example
 *
 * Run:
 *   DYLD_LIBRARY_PATH=build/lib ./build/examples/middleware_example
 *
 * Test:
 *   curl http://localhost:8080/public           # No auth required
 *   curl http://localhost:8080/api/data         # Requires X-API-Key header
 *   curl -H "X-API-Key: secret123" http://localhost:8080/api/data
 */

#include "../src/cpp/http/app.h"
#include <iostream>
#include <chrono>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <iomanip>
#include <ctime>

using namespace fasterapi;

// Simple rate limiter (per-IP, per-minute)
class RateLimiter {
public:
    explicit RateLimiter(int requests_per_minute = 60)
        : limit_(requests_per_minute) {}

    bool allow(const std::string& ip) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto now = std::chrono::steady_clock::now();
        auto& bucket = buckets_[ip];

        // Reset bucket if window expired
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - bucket.window_start).count();
        if (elapsed >= 60) {
            bucket.count = 0;
            bucket.window_start = now;
        }

        if (bucket.count >= limit_) {
            return false;  // Rate limited
        }

        bucket.count++;
        return true;
    }

private:
    struct Bucket {
        int count = 0;
        std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
    };

    std::mutex mutex_;
    std::unordered_map<std::string, Bucket> buckets_;
    int limit_;
};

// Request logger with colorized output
void log_request(const std::string& method, const std::string& path,
                 int status, double duration_ms) {
    // Get current time
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
    localtime_r(&time, &tm_buf);

    // Color codes based on status
    const char* color = "\033[32m";  // Green for 2xx
    if (status >= 400 && status < 500) color = "\033[33m";  // Yellow for 4xx
    if (status >= 500) color = "\033[31m";  // Red for 5xx
    const char* reset = "\033[0m";

    std::cout << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
              << " | " << std::left << std::setw(7) << method
              << " | " << std::left << std::setw(30) << path
              << " | " << color << status << reset
              << " | " << std::fixed << std::setprecision(2) << duration_ms << "ms"
              << std::endl;
}

int main() {
    std::cout << "=== Middleware Example ===" << std::endl;

    RateLimiter rate_limiter(30);  // 30 requests per minute

    App::Config config;
    config.pure_cpp_mode = true;
    App app(config);

    // Global middleware: Request timing and logging
    app.use([](Request& req, Response& res, std::function<void()> next) {
        auto start = std::chrono::high_resolution_clock::now();

        // Call next middleware/handler
        next();

        // Log after response
        auto end = std::chrono::high_resolution_clock::now();
        double duration = std::chrono::duration<double, std::milli>(end - start).count();

        // Note: In a real app, we'd get status from response
        log_request(req.method(), req.path(), 200, duration);
    });

    // Global middleware: CORS headers
    app.use([](Request& req, Response& res, std::function<void()> next) {
        res.header("Access-Control-Allow-Origin", "*");
        res.header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        res.header("Access-Control-Allow-Headers", "Content-Type, X-API-Key");

        // Handle preflight
        if (req.method() == "OPTIONS") {
            res.no_content().send("");
            return;
        }

        next();
    });

    // Path middleware: Rate limiting for /api/*
    app.use("/api", [&rate_limiter](Request& req, Response& res, std::function<void()> next) {
        std::string client_ip = req.client_ip();
        if (client_ip.empty()) client_ip = "127.0.0.1";

        if (!rate_limiter.allow(client_ip)) {
            res.status(429)
               .header("Retry-After", "60")
               .json(R"({"error":"Rate limit exceeded","retry_after":60})");
            return;
        }

        next();
    });

    // Path middleware: API key authentication for /api/*
    app.use("/api", [](Request& req, Response& res, std::function<void()> next) {
        std::string api_key = req.header("X-API-Key");

        // Simple API key validation (in production, use secure comparison)
        if (api_key != "secret123") {
            res.unauthorized()
               .header("WWW-Authenticate", "API-Key")
               .json(R"({"error":"Invalid or missing API key"})");
            return;
        }

        next();
    });

    // Public routes (no auth required)
    app.get("/", [](Request& req, Response& res) {
        res.html(R"(
<!DOCTYPE html>
<html>
<head><title>Middleware Example</title></head>
<body>
    <h1>Middleware Example</h1>
    <h2>Public Endpoints (no auth):</h2>
    <ul>
        <li>GET /public</li>
        <li>GET /health</li>
    </ul>
    <h2>Protected Endpoints (require X-API-Key: secret123):</h2>
    <ul>
        <li>GET /api/data</li>
        <li>POST /api/data</li>
        <li>GET /api/user</li>
    </ul>
</body>
</html>
)");
    });

    app.get("/public", [](Request& req, Response& res) {
        res.json(R"({"message":"This is public data, no auth required"})");
    });

    app.get("/health", [](Request& req, Response& res) {
        res.json(R"({"status":"healthy"})");
    });

    // Protected routes (require API key)
    app.get("/api/data", [](Request& req, Response& res) {
        res.json(R"({
            "message": "Protected data - you're authenticated!",
            "data": [1, 2, 3, 4, 5],
            "timestamp": "2024-01-01T00:00:00Z"
        })");
    });

    app.post("/api/data", [](Request& req, Response& res) {
        std::string body = req.body();
        res.created().json(R"({"message":"Data received","size":)" +
                          std::to_string(body.size()) + "}");
    });

    app.get("/api/user", [](Request& req, Response& res) {
        res.json(R"({
            "id": 1,
            "name": "Authenticated User",
            "role": "admin"
        })");
    });

    std::cout << "\nStarting on http://localhost:8080" << std::endl;
    std::cout << "\nMiddleware chain:" << std::endl;
    std::cout << "  1. Request logging (all routes)" << std::endl;
    std::cout << "  2. CORS headers (all routes)" << std::endl;
    std::cout << "  3. Rate limiting (/api/* only)" << std::endl;
    std::cout << "  4. API key auth (/api/* only)" << std::endl;
    std::cout << "\nTest commands:" << std::endl;
    std::cout << "  curl http://localhost:8080/public" << std::endl;
    std::cout << "  curl http://localhost:8080/api/data                    # 401" << std::endl;
    std::cout << "  curl -H 'X-API-Key: secret123' http://localhost:8080/api/data" << std::endl;
    std::cout << std::endl;

    return app.run_unified("0.0.0.0", 8080);
}
