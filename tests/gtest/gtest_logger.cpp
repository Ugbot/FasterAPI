/**
 * Structured Logger Unit Tests
 *
 * Tests the JSON logging system:
 * - Log levels (DEBUG, INFO, WARN, ERROR)
 * - JSON output format
 * - Request logging
 * - Request ID generation
 * - ScopedRequestLogger RAII helper
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/logger.h"
#include <sstream>
#include <regex>
#include <set>
#include <thread>
#include <chrono>

namespace fasterapi {
namespace test {

// ===========================================================================
// Log Level Tests
// ===========================================================================

class LogLevelTest : public ::testing::Test {};

TEST_F(LogLevelTest, LogLevelStrings) {
    EXPECT_STREQ(log_level_string(LogLevel::DEBUG), "DEBUG");
    EXPECT_STREQ(log_level_string(LogLevel::INFO), "INFO");
    EXPECT_STREQ(log_level_string(LogLevel::WARN), "WARN");
    EXPECT_STREQ(log_level_string(LogLevel::ERROR), "ERROR");
    EXPECT_STREQ(log_level_string(LogLevel::NONE), "NONE");
}

TEST_F(LogLevelTest, ParseLogLevel) {
    EXPECT_EQ(parse_log_level("DEBUG"), LogLevel::DEBUG);
    EXPECT_EQ(parse_log_level("debug"), LogLevel::DEBUG);
    EXPECT_EQ(parse_log_level("INFO"), LogLevel::INFO);
    EXPECT_EQ(parse_log_level("info"), LogLevel::INFO);
    EXPECT_EQ(parse_log_level("WARN"), LogLevel::WARN);
    EXPECT_EQ(parse_log_level("warn"), LogLevel::WARN);
    EXPECT_EQ(parse_log_level("WARNING"), LogLevel::WARN);
    EXPECT_EQ(parse_log_level("ERROR"), LogLevel::ERROR);
    EXPECT_EQ(parse_log_level("error"), LogLevel::ERROR);
    EXPECT_EQ(parse_log_level("NONE"), LogLevel::NONE);
    EXPECT_EQ(parse_log_level("unknown"), LogLevel::INFO);  // Default
}

// ===========================================================================
// JsonBuilder Tests
// ===========================================================================

class JsonBuilderTest : public ::testing::Test {};

TEST_F(JsonBuilderTest, EmptyObject) {
    JsonBuilder builder;
    EXPECT_EQ(builder.build(), "{}");
}

TEST_F(JsonBuilderTest, StringValue) {
    JsonBuilder builder;
    builder.add("name", "test");
    EXPECT_EQ(builder.build(), "{\"name\":\"test\"}");
}

TEST_F(JsonBuilderTest, IntValue) {
    JsonBuilder builder;
    builder.add("count", 42);
    EXPECT_EQ(builder.build(), "{\"count\":42}");
}

TEST_F(JsonBuilderTest, DoubleValue) {
    JsonBuilder builder;
    builder.add("latency", 25.5);
    std::string result = builder.build();
    EXPECT_NE(result.find("\"latency\":25.5"), std::string::npos);
}

TEST_F(JsonBuilderTest, BoolValue) {
    JsonBuilder builder;
    builder.add("enabled", true);
    builder.add("disabled", false);
    std::string result = builder.build();
    EXPECT_NE(result.find("\"enabled\":true"), std::string::npos);
    EXPECT_NE(result.find("\"disabled\":false"), std::string::npos);
}

TEST_F(JsonBuilderTest, MultipleValues) {
    JsonBuilder builder;
    builder.add("method", "GET")
           .add("path", "/api")
           .add("status", 200);
    std::string result = builder.build();
    EXPECT_NE(result.find("\"method\":\"GET\""), std::string::npos);
    EXPECT_NE(result.find("\"path\":\"/api\""), std::string::npos);
    EXPECT_NE(result.find("\"status\":200"), std::string::npos);
}

TEST_F(JsonBuilderTest, EscapedCharacters) {
    JsonBuilder builder;
    builder.add("message", "Line1\nLine2\tTab\"Quote\\Backslash");
    std::string result = builder.build();
    EXPECT_NE(result.find("\\n"), std::string::npos);
    EXPECT_NE(result.find("\\t"), std::string::npos);
    EXPECT_NE(result.find("\\\""), std::string::npos);
    EXPECT_NE(result.find("\\\\"), std::string::npos);
}

TEST_F(JsonBuilderTest, AddIf) {
    JsonBuilder builder;
    builder.add_if("present", "value")
           .add_if("empty", "");
    std::string result = builder.build();
    EXPECT_NE(result.find("\"present\":\"value\""), std::string::npos);
    EXPECT_EQ(result.find("\"empty\""), std::string::npos);  // Should not be present
}

TEST_F(JsonBuilderTest, AddMap) {
    JsonBuilder builder;
    std::map<std::string, std::string> extra = {{"key1", "val1"}, {"key2", "val2"}};
    builder.add_map(extra);
    std::string result = builder.build();
    EXPECT_NE(result.find("\"key1\":\"val1\""), std::string::npos);
    EXPECT_NE(result.find("\"key2\":\"val2\""), std::string::npos);
}

// ===========================================================================
// RequestIdGenerator Tests
// ===========================================================================

class RequestIdGeneratorTest : public ::testing::Test {};

TEST_F(RequestIdGeneratorTest, GeneratesUUID) {
    std::string id = RequestIdGenerator::instance().generate();

    // UUID v4 format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    EXPECT_EQ(id.length(), 36u);
    EXPECT_EQ(id[8], '-');
    EXPECT_EQ(id[13], '-');
    EXPECT_EQ(id[18], '-');
    EXPECT_EQ(id[23], '-');

    // Version 4 indicator
    EXPECT_EQ(id[14], '4');

    // Variant indicator (8, 9, a, or b)
    char variant = id[19];
    EXPECT_TRUE(variant == '8' || variant == '9' || variant == 'a' || variant == 'b');
}

TEST_F(RequestIdGeneratorTest, UniqueIds) {
    std::set<std::string> ids;
    const int count = 1000;

    for (int i = 0; i < count; i++) {
        ids.insert(RequestIdGenerator::instance().generate());
    }

    EXPECT_EQ(ids.size(), count);  // All unique
}

TEST_F(RequestIdGeneratorTest, ThreadSafe) {
    std::set<std::string> all_ids;
    std::mutex id_mutex;
    const int num_threads = 8;
    const int ids_per_thread = 100;

    std::vector<std::thread> thread_pool;
    for (int t = 0; t < num_threads; t++) {
        thread_pool.emplace_back([&]() {
            for (int i = 0; i < ids_per_thread; i++) {
                std::string id = RequestIdGenerator::instance().generate();
                std::lock_guard<std::mutex> lock(id_mutex);
                all_ids.insert(id);
            }
        });
    }

    for (auto& th : thread_pool) th.join();

    EXPECT_EQ(all_ids.size(), static_cast<size_t>(num_threads * ids_per_thread));
}

// ===========================================================================
// Logger Tests
// ===========================================================================

class LoggerTest : public ::testing::Test {
protected:
    std::ostringstream output_;

    void SetUp() override {
        Logger::instance().set_output(&output_);
        Logger::instance().set_level(LogLevel::DEBUG);
        Logger::instance().set_json_format(true);
    }

    void TearDown() override {
        Logger::instance().set_output(&std::cout);
        Logger::instance().set_level(LogLevel::INFO);
    }
};

TEST_F(LoggerTest, InfoLog) {
    Logger::instance().info("Server started", {{"port", "8080"}});

    std::string result = output_.str();
    EXPECT_NE(result.find("\"level\":\"INFO\""), std::string::npos);
    EXPECT_NE(result.find("\"message\":\"Server started\""), std::string::npos);
    EXPECT_NE(result.find("\"port\":\"8080\""), std::string::npos);
    EXPECT_NE(result.find("\"timestamp\":"), std::string::npos);
}

TEST_F(LoggerTest, DebugLog) {
    Logger::instance().debug("Debug message");

    std::string result = output_.str();
    EXPECT_NE(result.find("\"level\":\"DEBUG\""), std::string::npos);
}

TEST_F(LoggerTest, WarnLog) {
    Logger::instance().warn("Warning message");

    std::string result = output_.str();
    EXPECT_NE(result.find("\"level\":\"WARN\""), std::string::npos);
}

TEST_F(LoggerTest, ErrorLog) {
    Logger::instance().error("Error occurred", {{"code", "500"}});

    std::string result = output_.str();
    EXPECT_NE(result.find("\"level\":\"ERROR\""), std::string::npos);
    EXPECT_NE(result.find("\"code\":\"500\""), std::string::npos);
}

TEST_F(LoggerTest, LevelFiltering) {
    Logger::instance().set_level(LogLevel::WARN);

    Logger::instance().debug("Debug");
    Logger::instance().info("Info");
    Logger::instance().warn("Warn");
    Logger::instance().error("Error");

    std::string result = output_.str();
    EXPECT_EQ(result.find("Debug"), std::string::npos);
    EXPECT_EQ(result.find("Info"), std::string::npos);
    EXPECT_NE(result.find("Warn"), std::string::npos);
    EXPECT_NE(result.find("Error"), std::string::npos);
}

TEST_F(LoggerTest, PlainTextFormat) {
    Logger::instance().set_json_format(false);

    Logger::instance().info("Server started", {{"port", "8080"}});

    std::string result = output_.str();
    EXPECT_NE(result.find("INFO"), std::string::npos);
    EXPECT_NE(result.find("Server started"), std::string::npos);
    EXPECT_NE(result.find("port=8080"), std::string::npos);
    EXPECT_EQ(result.find("{"), std::string::npos);  // No JSON braces
}

TEST_F(LoggerTest, RequestLog) {
    RequestLogContext ctx;
    ctx.request_id = "abc-123";
    ctx.method = "GET";
    ctx.path = "/api/users";
    ctx.status = 200;
    ctx.latency_ms = 25.5;
    ctx.client_ip = "192.168.1.1";

    Logger::instance().request_log(ctx);

    std::string result = output_.str();
    EXPECT_NE(result.find("\"type\":\"request\""), std::string::npos);
    EXPECT_NE(result.find("\"request_id\":\"abc-123\""), std::string::npos);
    EXPECT_NE(result.find("\"method\":\"GET\""), std::string::npos);
    EXPECT_NE(result.find("\"path\":\"/api/users\""), std::string::npos);
    EXPECT_NE(result.find("\"status\":200"), std::string::npos);
    EXPECT_NE(result.find("\"latency_ms\":25.5"), std::string::npos);
    EXPECT_NE(result.find("\"client_ip\":\"192.168.1.1\""), std::string::npos);
}

TEST_F(LoggerTest, RequestLogPlainText) {
    Logger::instance().set_json_format(false);

    RequestLogContext ctx;
    ctx.request_id = "abc-123";
    ctx.method = "POST";
    ctx.path = "/api/create";
    ctx.status = 201;
    ctx.latency_ms = 50.0;
    ctx.client_ip = "10.0.0.1";

    Logger::instance().request_log(ctx);

    std::string result = output_.str();
    EXPECT_NE(result.find("[abc-123]"), std::string::npos);
    EXPECT_NE(result.find("POST /api/create 201"), std::string::npos);
    EXPECT_NE(result.find("50.00ms"), std::string::npos);
    EXPECT_NE(result.find("10.0.0.1"), std::string::npos);
}

TEST_F(LoggerTest, GenerateRequestId) {
    std::string id = Logger::instance().generate_request_id();
    EXPECT_EQ(id.length(), 36u);
}

// ===========================================================================
// ScopedRequestLogger Tests
// ===========================================================================

class ScopedRequestLoggerTest : public ::testing::Test {
protected:
    std::ostringstream output_;

    void SetUp() override {
        Logger::instance().set_output(&output_);
        Logger::instance().set_level(LogLevel::DEBUG);
        Logger::instance().set_json_format(true);
    }

    void TearDown() override {
        Logger::instance().set_output(&std::cout);
        Logger::instance().set_level(LogLevel::INFO);
    }
};

TEST_F(ScopedRequestLoggerTest, BasicUsage) {
    {
        ScopedRequestLogger logger("GET", "/api/test");
        logger.client_ip("127.0.0.1")
              .user_agent("TestClient/1.0");

        // Simulate some work
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        logger.complete(200, 1024);
    }

    std::string result = output_.str();
    EXPECT_NE(result.find("\"method\":\"GET\""), std::string::npos);
    EXPECT_NE(result.find("\"path\":\"/api/test\""), std::string::npos);
    EXPECT_NE(result.find("\"status\":200"), std::string::npos);
    EXPECT_NE(result.find("\"response_size\":1024"), std::string::npos);
    EXPECT_NE(result.find("\"client_ip\":\"127.0.0.1\""), std::string::npos);
    EXPECT_NE(result.find("\"request_id\":"), std::string::npos);
}

TEST_F(ScopedRequestLoggerTest, AutoCompleteOnDestruction) {
    {
        ScopedRequestLogger logger("POST", "/api/create");
        // Don't call complete() - should auto-complete with 500
    }

    std::string result = output_.str();
    EXPECT_NE(result.find("\"status\":500"), std::string::npos);
    EXPECT_NE(result.find("\"error\":\"Request handler did not complete normally\""), std::string::npos);
}

TEST_F(ScopedRequestLoggerTest, CustomRequestId) {
    {
        ScopedRequestLogger logger("GET", "/api/test", "custom-request-id-123");
        logger.complete(200);
    }

    std::string result = output_.str();
    EXPECT_NE(result.find("\"request_id\":\"custom-request-id-123\""), std::string::npos);
}

TEST_F(ScopedRequestLoggerTest, ExtraFields) {
    {
        ScopedRequestLogger logger("GET", "/api/users");
        logger.extra("user_id", "12345")
              .extra("action", "list")
              .complete(200);
    }

    std::string result = output_.str();
    EXPECT_NE(result.find("\"user_id\":\"12345\""), std::string::npos);
    EXPECT_NE(result.find("\"action\":\"list\""), std::string::npos);
}

TEST_F(ScopedRequestLoggerTest, LatencyMeasurement) {
    {
        ScopedRequestLogger logger("GET", "/slow");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        logger.complete(200);
    }

    std::string result = output_.str();

    // Find latency value and verify it's >= 50ms
    std::regex latency_regex("\"latency_ms\":([0-9.]+)");
    std::smatch match;
    ASSERT_TRUE(std::regex_search(result, match, latency_regex));

    double latency = std::stod(match[1].str());
    EXPECT_GE(latency, 50.0);
}

TEST_F(ScopedRequestLoggerTest, GetRequestId) {
    ScopedRequestLogger logger("GET", "/api/test");
    std::string id = logger.request_id();

    EXPECT_FALSE(id.empty());
    EXPECT_EQ(id.length(), 36u);

    logger.complete(200);
}

// ===========================================================================
// Timestamp Tests
// ===========================================================================

class TimestampTest : public ::testing::Test {
protected:
    std::ostringstream output_;

    void SetUp() override {
        Logger::instance().set_output(&output_);
        Logger::instance().set_level(LogLevel::DEBUG);
        Logger::instance().set_json_format(true);
    }

    void TearDown() override {
        Logger::instance().set_output(&std::cout);
    }
};

TEST_F(TimestampTest, ISO8601Format) {
    Logger::instance().info("Test");

    std::string result = output_.str();

    // Match ISO8601 format: YYYY-MM-DDTHH:MM:SS.mmmZ
    std::regex timestamp_regex("\"timestamp\":\"(\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}\\.\\d{3}Z)\"");
    std::smatch match;
    EXPECT_TRUE(std::regex_search(result, match, timestamp_regex));
}

// ===========================================================================
// Performance Tests
// ===========================================================================

class LoggerPerformanceTest : public ::testing::Test {
protected:
    std::ostringstream output_;

    void SetUp() override {
        Logger::instance().set_output(&output_);
        Logger::instance().set_level(LogLevel::INFO);
        Logger::instance().set_json_format(true);
    }

    void TearDown() override {
        Logger::instance().set_output(&std::cout);
    }
};

TEST_F(LoggerPerformanceTest, LoggingPerformance) {
    const int iterations = 10000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        Logger::instance().info("Test message", {{"iteration", std::to_string(i)}});
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "JSON log: " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 10000.0);  // Should be < 10µs per log
}

TEST_F(LoggerPerformanceTest, DisabledLevelPerformance) {
    Logger::instance().set_level(LogLevel::ERROR);
    const int iterations = 1000000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        Logger::instance().debug("This should not be logged");
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "Disabled log level: " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 50.0);  // Should be nearly free when disabled
}

TEST_F(LoggerPerformanceTest, RequestIdGenerationPerformance) {
    const int iterations = 100000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        volatile std::string id = Logger::instance().generate_request_id();
        (void)id;
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "Request ID generation: " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 5000.0);  // Should be < 5µs
}

}  // namespace test
}  // namespace fasterapi

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
