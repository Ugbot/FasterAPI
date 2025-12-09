/**
 * Prometheus Metrics Unit Tests
 *
 * Tests the metrics collection system:
 * - Counter (increment, labels)
 * - Gauge (set, inc, dec, labels)
 * - Histogram (observe, buckets, labels)
 * - Prometheus text format export
 * - HttpMetrics convenience class
 * - RequestTimer RAII helper
 */

#include <gtest/gtest.h>
#include "../../src/cpp/http/metrics.h"
#include <thread>
#include <vector>
#include <regex>
#include <sstream>

namespace fasterapi {
namespace test {

// ===========================================================================
// Counter Tests
// ===========================================================================

class CounterTest : public ::testing::Test {
protected:
    void SetUp() override {
        Metrics::instance().reset();
    }
};

TEST_F(CounterTest, BasicIncrement) {
    auto& counter = Metrics::instance().counter("test_counter", "A test counter");

    EXPECT_EQ(counter.value(), 0u);

    counter.inc();
    EXPECT_EQ(counter.value(), 1u);

    counter.inc(5);
    EXPECT_EQ(counter.value(), 6u);
}

TEST_F(CounterTest, LabeledIncrement) {
    auto& counter = Metrics::instance().counter("labeled_counter", "Counter with labels");

    counter.labels({{"method", "GET"}, {"status", "200"}}).inc();
    counter.labels({{"method", "GET"}, {"status", "200"}}).inc(4);
    counter.labels({{"method", "POST"}, {"status", "201"}}).inc(2);

    EXPECT_EQ(counter.labels({{"method", "GET"}, {"status", "200"}}).value(), 5u);
    EXPECT_EQ(counter.labels({{"method", "POST"}, {"status", "201"}}).value(), 2u);
}

TEST_F(CounterTest, PrometheusFormat) {
    auto& counter = Metrics::instance().counter("http_requests", "Total HTTP requests");

    counter.labels({{"method", "GET"}, {"path", "/api"}}).inc(100);
    counter.labels({{"method", "POST"}, {"path", "/api"}}).inc(50);

    std::string output = counter.to_prometheus();

    EXPECT_NE(output.find("# HELP http_requests Total HTTP requests"), std::string::npos);
    EXPECT_NE(output.find("# TYPE http_requests counter"), std::string::npos);
    EXPECT_NE(output.find("http_requests{"), std::string::npos);
    EXPECT_NE(output.find("method=\"GET\""), std::string::npos);
    EXPECT_NE(output.find("100"), std::string::npos);
}

TEST_F(CounterTest, ThreadSafety) {
    auto& counter = Metrics::instance().counter("thread_counter");

    const int num_threads = 8;
    const int increments_per_thread = 10000;

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&counter, increments_per_thread]() {
            for (int j = 0; j < increments_per_thread; j++) {
                counter.inc();
            }
        });
    }

    for (auto& t : threads) t.join();

    EXPECT_EQ(counter.value(), num_threads * increments_per_thread);
}

// ===========================================================================
// Gauge Tests
// ===========================================================================

class GaugeTest : public ::testing::Test {
protected:
    void SetUp() override {
        Metrics::instance().reset();
    }
};

TEST_F(GaugeTest, SetValue) {
    auto& gauge = Metrics::instance().gauge("test_gauge", "A test gauge");

    gauge.set(42.5);
    EXPECT_DOUBLE_EQ(gauge.value(), 42.5);

    gauge.set(0.0);
    EXPECT_DOUBLE_EQ(gauge.value(), 0.0);

    gauge.set(-10.5);
    EXPECT_DOUBLE_EQ(gauge.value(), -10.5);
}

TEST_F(GaugeTest, IncDec) {
    auto& gauge = Metrics::instance().gauge("inc_dec_gauge");

    gauge.inc();
    EXPECT_DOUBLE_EQ(gauge.value(), 1.0);

    gauge.inc(5.5);
    EXPECT_DOUBLE_EQ(gauge.value(), 6.5);

    gauge.dec(2.0);
    EXPECT_DOUBLE_EQ(gauge.value(), 4.5);

    gauge.dec();
    EXPECT_DOUBLE_EQ(gauge.value(), 3.5);
}

TEST_F(GaugeTest, LabeledGauge) {
    auto& gauge = Metrics::instance().gauge("connections", "Active connections");

    gauge.labels({{"protocol", "http1"}}).set(100);
    gauge.labels({{"protocol", "http2"}}).set(50);
    gauge.labels({{"protocol", "http3"}}).set(25);

    EXPECT_DOUBLE_EQ(gauge.labels({{"protocol", "http1"}}).value(), 100.0);
    EXPECT_DOUBLE_EQ(gauge.labels({{"protocol", "http2"}}).value(), 50.0);
    EXPECT_DOUBLE_EQ(gauge.labels({{"protocol", "http3"}}).value(), 25.0);
}

TEST_F(GaugeTest, PrometheusFormat) {
    auto& gauge = Metrics::instance().gauge("active_requests", "Currently active requests");
    gauge.set(42);

    std::string output = gauge.to_prometheus();

    EXPECT_NE(output.find("# HELP active_requests"), std::string::npos);
    EXPECT_NE(output.find("# TYPE active_requests gauge"), std::string::npos);
    EXPECT_NE(output.find("active_requests 42"), std::string::npos);
}

// ===========================================================================
// Histogram Tests
// ===========================================================================

class HistogramTest : public ::testing::Test {
protected:
    void SetUp() override {
        Metrics::instance().reset();
    }
};

TEST_F(HistogramTest, BasicObserve) {
    auto& histogram = Metrics::instance().histogram("request_latency", "Request latency in seconds");

    histogram.observe(0.001);  // 1ms
    histogram.observe(0.005);  // 5ms
    histogram.observe(0.010);  // 10ms
    histogram.observe(0.100);  // 100ms
    histogram.observe(1.000);  // 1s

    std::string output = histogram.to_prometheus();

    EXPECT_NE(output.find("# TYPE request_latency histogram"), std::string::npos);
    EXPECT_NE(output.find("request_latency_bucket"), std::string::npos);
    EXPECT_NE(output.find("request_latency_sum"), std::string::npos);
    EXPECT_NE(output.find("request_latency_count 5"), std::string::npos);
}

TEST_F(HistogramTest, CustomBuckets) {
    std::vector<double> buckets = {0.1, 0.5, 1.0, 5.0};
    auto& histogram = Metrics::instance().histogram("custom_hist", "Custom buckets", buckets);

    histogram.observe(0.05);  // < 0.1
    histogram.observe(0.2);   // < 0.5
    histogram.observe(0.8);   // < 1.0
    histogram.observe(2.0);   // < 5.0
    histogram.observe(10.0);  // > 5.0

    std::string output = histogram.to_prometheus();

    // Check bucket boundaries are present (values are formatted with full precision)
    EXPECT_NE(output.find("le=\"0.1"), std::string::npos);  // May have more decimals
    EXPECT_NE(output.find("le=\"0.5"), std::string::npos);
    EXPECT_NE(output.find("le=\"1"), std::string::npos);
    EXPECT_NE(output.find("le=\"5"), std::string::npos);
    EXPECT_NE(output.find("le=\"+Inf\""), std::string::npos);
}

TEST_F(HistogramTest, LabeledHistogram) {
    auto& histogram = Metrics::instance().histogram("api_latency", "API latency");

    histogram.labels({{"endpoint", "/users"}}).observe(0.025);
    histogram.labels({{"endpoint", "/users"}}).observe(0.030);
    histogram.labels({{"endpoint", "/items"}}).observe(0.010);

    std::string output = histogram.to_prometheus();

    EXPECT_NE(output.find("endpoint=\"/users\""), std::string::npos);
    EXPECT_NE(output.find("endpoint=\"/items\""), std::string::npos);
}

TEST_F(HistogramTest, DefaultBuckets) {
    // Default buckets: 1ms, 5ms, 10ms, 25ms, 50ms, 100ms, 250ms, 500ms, 1s, 2.5s, 5s, 10s
    EXPECT_EQ(Histogram::DEFAULT_BUCKETS.size(), 12u);
    EXPECT_DOUBLE_EQ(Histogram::DEFAULT_BUCKETS[0], 0.001);
    EXPECT_DOUBLE_EQ(Histogram::DEFAULT_BUCKETS[11], 10.0);
}

// ===========================================================================
// Metrics Registry Tests
// ===========================================================================

class MetricsRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        Metrics::instance().reset();
    }
};

TEST_F(MetricsRegistryTest, GetOrCreate) {
    auto& counter1 = Metrics::instance().counter("my_counter", "First access");
    auto& counter2 = Metrics::instance().counter("my_counter", "Second access");

    counter1.inc(10);
    EXPECT_EQ(counter2.value(), 10u);  // Same instance
}

TEST_F(MetricsRegistryTest, ExportAll) {
    Metrics::instance().counter("requests_total", "Total requests").inc(100);
    Metrics::instance().gauge("active_connections", "Active connections").set(50);
    Metrics::instance().histogram("latency_seconds", "Request latency").observe(0.025);

    std::string output = Metrics::instance().export_prometheus();

    EXPECT_NE(output.find("requests_total 100"), std::string::npos);
    EXPECT_NE(output.find("active_connections 50"), std::string::npos);
    EXPECT_NE(output.find("latency_seconds_count 1"), std::string::npos);
}

TEST_F(MetricsRegistryTest, Reset) {
    Metrics::instance().counter("test_counter").inc(100);
    Metrics::instance().reset();

    auto& counter = Metrics::instance().counter("test_counter");
    EXPECT_EQ(counter.value(), 0u);
}

// ===========================================================================
// HttpMetrics Tests
// ===========================================================================

// Note: HttpMetrics is a singleton that holds references to metrics created
// during initialization. We cannot reset Metrics::instance() without invalidating
// those references. These tests work with accumulated state.
class HttpMetricsTest : public ::testing::Test {
protected:
    // Don't reset Metrics - HttpMetrics holds references to the metrics
    // and resetting would invalidate them causing crashes.
};

TEST_F(HttpMetricsTest, RequestCompleted) {
    HttpMetrics::instance().request_completed("GET", "/api/users", 200, 0.025);
    HttpMetrics::instance().request_completed("POST", "/api/users", 201, 0.050);
    HttpMetrics::instance().request_completed("GET", "/api/users", 500, 0.100);

    std::string output = HttpMetrics::instance().export_prometheus();

    EXPECT_NE(output.find("http_requests_total"), std::string::npos);
    EXPECT_NE(output.find("http_request_duration_seconds"), std::string::npos);
    EXPECT_NE(output.find("http_responses_2xx_total"), std::string::npos);
    EXPECT_NE(output.find("http_responses_5xx_total"), std::string::npos);
}

TEST_F(HttpMetricsTest, ConnectionTracking) {
    HttpMetrics::instance().connection_opened();
    HttpMetrics::instance().connection_opened();
    HttpMetrics::instance().connection_opened();
    HttpMetrics::instance().connection_closed();

    std::string output = HttpMetrics::instance().export_prometheus();

    EXPECT_NE(output.find("http_connections_active"), std::string::npos);
    EXPECT_NE(output.find("http_connections_total"), std::string::npos);
}

TEST_F(HttpMetricsTest, PathNormalization) {
    // Numeric IDs should be normalized
    HttpMetrics::instance().request_completed("GET", "/users/12345", 200, 0.01);
    HttpMetrics::instance().request_completed("GET", "/users/67890", 200, 0.01);

    std::string output = HttpMetrics::instance().export_prometheus();

    // Both should be counted under /users/:id
    EXPECT_NE(output.find("/users/:id"), std::string::npos);
}

TEST_F(HttpMetricsTest, UuidNormalization) {
    // UUIDs should be normalized
    HttpMetrics::instance().request_completed("GET", "/items/550e8400-e29b-41d4-a716-446655440000", 200, 0.01);

    std::string output = HttpMetrics::instance().export_prometheus();

    EXPECT_NE(output.find("/items/:uuid"), std::string::npos);
}

// ===========================================================================
// RequestTimer Tests
// ===========================================================================

// Note: RequestTimer uses HttpMetrics singleton, same caveat applies.
class RequestTimerTest : public ::testing::Test {
protected:
    // Don't reset Metrics - HttpMetrics holds references to the metrics
    // and resetting would invalidate them causing crashes.
};

TEST_F(RequestTimerTest, BasicTiming) {
    {
        RequestTimer timer("GET", "/api/test");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        timer.complete(200);
    }

    std::string output = HttpMetrics::instance().export_prometheus();

    EXPECT_NE(output.find("http_requests_total"), std::string::npos);
    EXPECT_NE(output.find("status=\"200\""), std::string::npos);
}

TEST_F(RequestTimerTest, AutoCompleteOnDestruction) {
    {
        RequestTimer timer("GET", "/api/test");
        // Don't call complete() - should auto-complete with 500
    }

    std::string output = HttpMetrics::instance().export_prometheus();

    EXPECT_NE(output.find("status=\"500\""), std::string::npos);
}

TEST_F(RequestTimerTest, RequestsInFlight) {
    // Get baseline value (may be non-zero from previous tests)
    std::string baseline = HttpMetrics::instance().export_prometheus();

    HttpMetrics::instance().request_started();
    HttpMetrics::instance().request_started();

    std::string output1 = HttpMetrics::instance().export_prometheus();
    // Just verify the metric exists and has some value
    EXPECT_NE(output1.find("http_requests_in_flight"), std::string::npos);

    // Complete both requests to restore balance
    HttpMetrics::instance().request_completed("GET", "/", 200, 0.01);
    HttpMetrics::instance().request_completed("GET", "/", 200, 0.01);

    std::string output2 = HttpMetrics::instance().export_prometheus();
    EXPECT_NE(output2.find("http_requests_in_flight"), std::string::npos);
}

// ===========================================================================
// Labels Tests
// ===========================================================================

class LabelsTest : public ::testing::Test {};

TEST_F(LabelsTest, EmptyLabels) {
    Labels labels;
    EXPECT_TRUE(labels.empty());
    EXPECT_EQ(labels.to_prometheus(), "");
}

TEST_F(LabelsTest, SingleLabel) {
    Labels labels{{"method", "GET"}};
    std::string output = labels.to_prometheus();

    EXPECT_EQ(output, "{method=\"GET\"}");
}

TEST_F(LabelsTest, MultipleLabels) {
    Labels labels{{"method", "POST"}, {"status", "201"}};
    std::string output = labels.to_prometheus();

    EXPECT_NE(output.find("method=\"POST\""), std::string::npos);
    EXPECT_NE(output.find("status=\"201\""), std::string::npos);
}

TEST_F(LabelsTest, EscapedValues) {
    Labels labels{{"path", "/api?foo=\"bar\""}};
    std::string output = labels.to_prometheus();

    // Quotes should be escaped
    EXPECT_NE(output.find("\\\"bar\\\""), std::string::npos);
}

// ===========================================================================
// Performance Tests
// ===========================================================================

class MetricsPerformanceTest : public ::testing::Test {
protected:
    void SetUp() override {
        Metrics::instance().reset();
    }
};

TEST_F(MetricsPerformanceTest, CounterIncrementPerformance) {
    auto& counter = Metrics::instance().counter("perf_counter");
    const int iterations = 1000000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        counter.inc();
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "Counter increment: " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 100.0);  // Should be very fast
}

TEST_F(MetricsPerformanceTest, HistogramObservePerformance) {
    auto& histogram = Metrics::instance().histogram("perf_histogram");
    const int iterations = 100000;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        histogram.observe(0.025);
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "Histogram observe: " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 500.0);
}

TEST_F(MetricsPerformanceTest, LabeledCounterPerformance) {
    auto& counter = Metrics::instance().counter("labeled_perf_counter");
    const int iterations = 100000;
    Labels labels{{"method", "GET"}, {"status", "200"}};

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        counter.labels(labels).inc();
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << "Labeled counter increment: " << ns_per_op << " ns/op\n";
    EXPECT_LT(ns_per_op, 1000.0);
}

}  // namespace test
}  // namespace fasterapi

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
