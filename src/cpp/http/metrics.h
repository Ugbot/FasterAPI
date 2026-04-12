/**
 * @file metrics.h
 * @brief Prometheus-compatible metrics collection system
 *
 * Provides thread-safe metrics collection with Prometheus text format export:
 * - Counters: Monotonically increasing values (e.g., request counts)
 * - Gauges: Values that can go up/down (e.g., active connections)
 * - Histograms: Distribution of values (e.g., request latencies)
 *
 * Example:
 * @code
 * auto& metrics = Metrics::instance();
 *
 * // Increment counter
 * metrics.counter("http_requests_total")
 *        .labels({{"method", "GET"}, {"path", "/api"}, {"status", "200"}})
 *        .inc();
 *
 * // Record histogram value
 * metrics.histogram("http_request_duration_seconds")
 *        .labels({{"method", "GET"}, {"path", "/api"}})
 *        .observe(0.025);  // 25ms
 *
 * // Set gauge
 * metrics.gauge("http_connections_active").set(42);
 *
 * // Export to Prometheus format
 * std::string output = metrics.export_prometheus();
 * @endcode
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <memory>
#include <array>

namespace fasterapi {

/**
 * Label set for metrics - immutable after creation.
 */
class Labels {
public:
    Labels() = default;
    Labels(std::initializer_list<std::pair<std::string, std::string>> init)
        : labels_(init.begin(), init.end()) {}

    std::string to_prometheus() const {
        if (labels_.empty()) return "";

        std::ostringstream oss;
        oss << "{";
        bool first = true;
        for (const auto& [key, value] : labels_) {
            if (!first) oss << ",";
            oss << key << "=\"" << escape_value(value) << "\"";
            first = false;
        }
        oss << "}";
        return oss.str();
    }

    std::string key() const {
        std::ostringstream oss;
        for (const auto& [k, v] : labels_) {
            oss << k << "=" << v << ";";
        }
        return oss.str();
    }

    bool empty() const { return labels_.empty(); }

private:
    std::map<std::string, std::string> labels_;

    static std::string escape_value(const std::string& s) {
        std::string result;
        result.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '\\': result += "\\\\"; break;
                case '"': result += "\\\""; break;
                case '\n': result += "\\n"; break;
                default: result += c;
            }
        }
        return result;
    }
};

/**
 * Counter metric - monotonically increasing value.
 */
class Counter {
public:
    explicit Counter(const std::string& name, const std::string& help = "")
        : name_(name), help_(help) {}

    struct LabeledCounter {
        std::atomic<uint64_t> value{0};
        Labels labels;
    };

    class LabeledHandle {
    public:
        LabeledHandle(LabeledCounter* counter) : counter_(counter) {}

        void inc(uint64_t n = 1) {
            counter_->value.fetch_add(n, std::memory_order_relaxed);
        }

        uint64_t value() const {
            return counter_->value.load(std::memory_order_relaxed);
        }

    private:
        LabeledCounter* counter_;
    };

    LabeledHandle labels(const Labels& l) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string key = l.key();
        auto it = labeled_.find(key);
        if (it == labeled_.end()) {
            auto ptr = std::make_unique<LabeledCounter>();
            ptr->labels = l;
            it = labeled_.emplace(key, std::move(ptr)).first;
        }
        return LabeledHandle(it->second.get());
    }

    // No-label shortcut
    void inc(uint64_t n = 1) {
        no_label_value_.fetch_add(n, std::memory_order_relaxed);
    }

    uint64_t value() const {
        return no_label_value_.load(std::memory_order_relaxed);
    }

    std::string to_prometheus() const {
        std::ostringstream oss;

        if (!help_.empty()) {
            oss << "# HELP " << name_ << " " << help_ << "\n";
        }
        oss << "# TYPE " << name_ << " counter\n";

        // No-label value
        uint64_t nv = no_label_value_.load(std::memory_order_relaxed);
        if (nv > 0 || labeled_.empty()) {
            oss << name_ << " " << nv << "\n";
        }

        // Labeled values
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [key, counter] : labeled_) {
            oss << name_ << counter->labels.to_prometheus() << " "
                << counter->value.load(std::memory_order_relaxed) << "\n";
        }

        return oss.str();
    }

    const std::string& name() const { return name_; }

private:
    std::string name_;
    std::string help_;
    std::atomic<uint64_t> no_label_value_{0};
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<LabeledCounter>> labeled_;
};

/**
 * Gauge metric - value that can go up or down.
 */
class Gauge {
public:
    explicit Gauge(const std::string& name, const std::string& help = "")
        : name_(name), help_(help) {}

    struct LabeledGauge {
        std::atomic<double> value{0.0};
        Labels labels;
    };

    class LabeledHandle {
    public:
        LabeledHandle(LabeledGauge* gauge) : gauge_(gauge) {}

        void set(double v) {
            gauge_->value.store(v, std::memory_order_relaxed);
        }

        void inc(double n = 1.0) {
            double old = gauge_->value.load(std::memory_order_relaxed);
            while (!gauge_->value.compare_exchange_weak(old, old + n,
                    std::memory_order_relaxed, std::memory_order_relaxed)) {}
        }

        void dec(double n = 1.0) { inc(-n); }

        double value() const {
            return gauge_->value.load(std::memory_order_relaxed);
        }

    private:
        LabeledGauge* gauge_;
    };

    LabeledHandle labels(const Labels& l) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string key = l.key();
        auto it = labeled_.find(key);
        if (it == labeled_.end()) {
            auto ptr = std::make_unique<LabeledGauge>();
            ptr->labels = l;
            it = labeled_.emplace(key, std::move(ptr)).first;
        }
        return LabeledHandle(it->second.get());
    }

    // No-label shortcuts
    void set(double v) {
        no_label_value_.store(v, std::memory_order_relaxed);
    }

    void inc(double n = 1.0) {
        double old = no_label_value_.load(std::memory_order_relaxed);
        while (!no_label_value_.compare_exchange_weak(old, old + n,
                std::memory_order_relaxed, std::memory_order_relaxed)) {}
    }

    void dec(double n = 1.0) { inc(-n); }

    double value() const {
        return no_label_value_.load(std::memory_order_relaxed);
    }

    std::string to_prometheus() const {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6);

        if (!help_.empty()) {
            oss << "# HELP " << name_ << " " << help_ << "\n";
        }
        oss << "# TYPE " << name_ << " gauge\n";

        // No-label value
        double nv = no_label_value_.load(std::memory_order_relaxed);
        if (nv != 0.0 || labeled_.empty()) {
            oss << name_ << " " << nv << "\n";
        }

        // Labeled values
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [key, gauge] : labeled_) {
            oss << name_ << gauge->labels.to_prometheus() << " "
                << gauge->value.load(std::memory_order_relaxed) << "\n";
        }

        return oss.str();
    }

    const std::string& name() const { return name_; }

private:
    std::string name_;
    std::string help_;
    std::atomic<double> no_label_value_{0.0};
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<LabeledGauge>> labeled_;
};

/**
 * Histogram metric - distribution of values with configurable buckets.
 */
class Histogram {
public:
    // Default latency buckets (seconds): 1ms, 5ms, 10ms, 25ms, 50ms, 100ms, 250ms, 500ms, 1s, 2.5s, 5s, 10s
    static constexpr std::array<double, 12> DEFAULT_BUCKETS = {
        0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0
    };

    explicit Histogram(const std::string& name, const std::string& help = "",
                       const std::vector<double>& buckets = {})
        : name_(name), help_(help) {
        if (buckets.empty()) {
            buckets_.assign(DEFAULT_BUCKETS.begin(), DEFAULT_BUCKETS.end());
        } else {
            buckets_ = buckets;
        }
        std::sort(buckets_.begin(), buckets_.end());
    }

    struct LabeledHistogram {
        std::vector<std::atomic<uint64_t>> bucket_counts;
        std::atomic<uint64_t> count{0};
        std::atomic<double> sum{0.0};
        Labels labels;

        LabeledHistogram(size_t num_buckets) : bucket_counts(num_buckets + 1) {
            for (auto& c : bucket_counts) c = 0;
        }
    };

    class LabeledHandle {
    public:
        LabeledHandle(LabeledHistogram* hist, const std::vector<double>& buckets)
            : hist_(hist), buckets_(buckets) {}

        void observe(double value) {
            // Find bucket
            for (size_t i = 0; i < buckets_.size(); i++) {
                if (value <= buckets_[i]) {
                    hist_->bucket_counts[i].fetch_add(1, std::memory_order_relaxed);
                }
            }
            // +Inf bucket
            hist_->bucket_counts[buckets_.size()].fetch_add(1, std::memory_order_relaxed);

            // Update sum and count
            hist_->count.fetch_add(1, std::memory_order_relaxed);

            double old = hist_->sum.load(std::memory_order_relaxed);
            while (!hist_->sum.compare_exchange_weak(old, old + value,
                    std::memory_order_relaxed, std::memory_order_relaxed)) {}
        }

    private:
        LabeledHistogram* hist_;
        const std::vector<double>& buckets_;
    };

    LabeledHandle labels(const Labels& l) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string key = l.key();
        auto it = labeled_.find(key);
        if (it == labeled_.end()) {
            auto ptr = std::make_unique<LabeledHistogram>(buckets_.size());
            ptr->labels = l;
            it = labeled_.emplace(key, std::move(ptr)).first;
        }
        return LabeledHandle(it->second.get(), buckets_);
    }

    // No-label observe
    void observe(double value) {
        std::call_once(no_label_init_, [this]() {
            no_label_ = std::make_unique<LabeledHistogram>(buckets_.size());
        });
        LabeledHandle(no_label_.get(), buckets_).observe(value);
    }

    std::string to_prometheus() const {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6);

        if (!help_.empty()) {
            oss << "# HELP " << name_ << " " << help_ << "\n";
        }
        oss << "# TYPE " << name_ << " histogram\n";

        auto format_histogram = [&](const LabeledHistogram& h, const std::string& label_str) {
            uint64_t cumulative = 0;
            for (size_t i = 0; i < buckets_.size(); i++) {
                cumulative += h.bucket_counts[i].load(std::memory_order_relaxed);
                if (label_str.empty()) {
                    oss << name_ << "_bucket{le=\"" << buckets_[i] << "\"} " << cumulative << "\n";
                } else {
                    // Insert le before closing brace
                    std::string labels = label_str;
                    labels.pop_back(); // remove }
                    oss << name_ << "_bucket" << labels << ",le=\"" << buckets_[i] << "\"} " << cumulative << "\n";
                }
            }
            // +Inf bucket
            cumulative += h.bucket_counts[buckets_.size()].load(std::memory_order_relaxed);
            if (label_str.empty()) {
                oss << name_ << "_bucket{le=\"+Inf\"} " << cumulative << "\n";
                oss << name_ << "_sum " << h.sum.load(std::memory_order_relaxed) << "\n";
                oss << name_ << "_count " << h.count.load(std::memory_order_relaxed) << "\n";
            } else {
                std::string labels = label_str;
                labels.pop_back();
                oss << name_ << "_bucket" << labels << ",le=\"+Inf\"} " << cumulative << "\n";
                oss << name_ << "_sum" << label_str << " " << h.sum.load(std::memory_order_relaxed) << "\n";
                oss << name_ << "_count" << label_str << " " << h.count.load(std::memory_order_relaxed) << "\n";
            }
        };

        // No-label histogram
        if (no_label_) {
            format_histogram(*no_label_, "");
        }

        // Labeled histograms
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [key, hist] : labeled_) {
            format_histogram(*hist, hist->labels.to_prometheus());
        }

        return oss.str();
    }

    const std::string& name() const { return name_; }

private:
    std::string name_;
    std::string help_;
    std::vector<double> buckets_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<LabeledHistogram>> labeled_;
    std::unique_ptr<LabeledHistogram> no_label_;
    std::once_flag no_label_init_;
};

/**
 * Metrics registry - singleton for all application metrics.
 */
class Metrics {
public:
    static Metrics& instance() {
        static Metrics instance;
        return instance;
    }

    // Create/get counter
    Counter& counter(const std::string& name, const std::string& help = "") {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = counters_.find(name);
        if (it == counters_.end()) {
            it = counters_.emplace(name, std::make_unique<Counter>(name, help)).first;
        }
        return *it->second;
    }

    // Create/get gauge
    Gauge& gauge(const std::string& name, const std::string& help = "") {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = gauges_.find(name);
        if (it == gauges_.end()) {
            it = gauges_.emplace(name, std::make_unique<Gauge>(name, help)).first;
        }
        return *it->second;
    }

    // Create/get histogram
    Histogram& histogram(const std::string& name, const std::string& help = "",
                         const std::vector<double>& buckets = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = histograms_.find(name);
        if (it == histograms_.end()) {
            it = histograms_.emplace(name, std::make_unique<Histogram>(name, help, buckets)).first;
        }
        return *it->second;
    }

    // Export all metrics in Prometheus text format
    std::string export_prometheus() const {
        std::ostringstream oss;

        std::lock_guard<std::mutex> lock(mutex_);

        for (const auto& [name, counter] : counters_) {
            oss << counter->to_prometheus();
        }

        for (const auto& [name, gauge] : gauges_) {
            oss << gauge->to_prometheus();
        }

        for (const auto& [name, histogram] : histograms_) {
            oss << histogram->to_prometheus();
        }

        return oss.str();
    }

    // Reset all metrics (for testing)
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        counters_.clear();
        gauges_.clear();
        histograms_.clear();
    }

private:
    Metrics() = default;
    ~Metrics() = default;

    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<Counter>> counters_;
    std::unordered_map<std::string, std::unique_ptr<Gauge>> gauges_;
    std::unordered_map<std::string, std::unique_ptr<Histogram>> histograms_;
};

/**
 * Pre-defined HTTP metrics following Prometheus conventions.
 */
class HttpMetrics {
public:
    static HttpMetrics& instance() {
        static HttpMetrics instance;
        return instance;
    }

    // Record request start
    void request_started() {
        requests_in_flight_.inc();
    }

    // Record request completion
    void request_completed(const std::string& method, const std::string& path,
                          int status_code, double duration_seconds) {
        requests_in_flight_.dec();

        // Normalize path for metrics (remove dynamic segments)
        std::string normalized_path = normalize_path(path);

        Labels labels{{"method", method}, {"path", normalized_path}, {"status", std::to_string(status_code)}};

        requests_total_.labels(labels).inc();
        request_duration_.labels(Labels{{"method", method}, {"path", normalized_path}}).observe(duration_seconds);

        // Status code family counters
        if (status_code >= 200 && status_code < 300) {
            responses_2xx_.inc();
        } else if (status_code >= 300 && status_code < 400) {
            responses_3xx_.inc();
        } else if (status_code >= 400 && status_code < 500) {
            responses_4xx_.inc();
        } else if (status_code >= 500) {
            responses_5xx_.inc();
        }
    }

    // Connection tracking
    void connection_opened() {
        connections_active_.inc();
        connections_total_.inc();
    }

    void connection_closed() {
        connections_active_.dec();
    }

    // Request/response size tracking
    void record_request_size(size_t bytes) {
        request_size_.observe(static_cast<double>(bytes));
    }

    void record_response_size(size_t bytes) {
        response_size_.observe(static_cast<double>(bytes));
    }

    // Get Prometheus export
    std::string export_prometheus() const {
        return Metrics::instance().export_prometheus();
    }

private:
    HttpMetrics()
        : requests_total_(Metrics::instance().counter(
              "http_requests_total", "Total number of HTTP requests")),
          request_duration_(Metrics::instance().histogram(
              "http_request_duration_seconds", "HTTP request duration in seconds")),
          requests_in_flight_(Metrics::instance().gauge(
              "http_requests_in_flight", "Number of HTTP requests currently being processed")),
          connections_active_(Metrics::instance().gauge(
              "http_connections_active", "Number of active HTTP connections")),
          connections_total_(Metrics::instance().counter(
              "http_connections_total", "Total number of HTTP connections")),
          responses_2xx_(Metrics::instance().counter(
              "http_responses_2xx_total", "Total number of 2xx responses")),
          responses_3xx_(Metrics::instance().counter(
              "http_responses_3xx_total", "Total number of 3xx responses")),
          responses_4xx_(Metrics::instance().counter(
              "http_responses_4xx_total", "Total number of 4xx responses")),
          responses_5xx_(Metrics::instance().counter(
              "http_responses_5xx_total", "Total number of 5xx responses")),
          request_size_(Metrics::instance().histogram(
              "http_request_size_bytes", "HTTP request body size in bytes",
              {100, 1000, 10000, 100000, 1000000, 10000000})),
          response_size_(Metrics::instance().histogram(
              "http_response_size_bytes", "HTTP response body size in bytes",
              {100, 1000, 10000, 100000, 1000000, 10000000})) {}

    // Normalize path by replacing UUIDs and numeric IDs with placeholders
    static std::string normalize_path(const std::string& path) {
        std::string result = path;

        // Remove query string
        size_t query_pos = result.find('?');
        if (query_pos != std::string::npos) {
            result = result.substr(0, query_pos);
        }

        // Replace UUID patterns: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
        // Replace numeric IDs: /users/123 -> /users/:id
        // Simple heuristic: replace path segments that are all digits or look like UUIDs
        std::ostringstream normalized;
        size_t pos = 0;
        while (pos < result.size()) {
            size_t next_slash = result.find('/', pos + 1);
            if (next_slash == std::string::npos) next_slash = result.size();

            std::string segment = result.substr(pos, next_slash - pos);

            // Check if segment is numeric (after slash)
            if (segment.size() > 1 && segment[0] == '/') {
                std::string value = segment.substr(1);
                bool is_numeric = !value.empty() &&
                    std::all_of(value.begin(), value.end(), ::isdigit);
                bool is_uuid = value.size() == 36 && value[8] == '-' &&
                    value[13] == '-' && value[18] == '-' && value[23] == '-';

                if (is_numeric) {
                    normalized << "/:id";
                } else if (is_uuid) {
                    normalized << "/:uuid";
                } else {
                    normalized << segment;
                }
            } else {
                normalized << segment;
            }

            pos = next_slash;
        }

        std::string final_result = normalized.str();
        return final_result.empty() ? "/" : final_result;
    }

    Counter& requests_total_;
    Histogram& request_duration_;
    Gauge& requests_in_flight_;
    Gauge& connections_active_;
    Counter& connections_total_;
    Counter& responses_2xx_;
    Counter& responses_3xx_;
    Counter& responses_4xx_;
    Counter& responses_5xx_;
    Histogram& request_size_;
    Histogram& response_size_;
};

/**
 * RAII timer for measuring request duration.
 */
class RequestTimer {
public:
    RequestTimer(const std::string& method, const std::string& path)
        : method_(method), path_(path),
          start_(std::chrono::steady_clock::now()) {
        HttpMetrics::instance().request_started();
    }

    void complete(int status_code) {
        if (completed_) return;
        completed_ = true;

        auto end = std::chrono::steady_clock::now();
        double duration = std::chrono::duration<double>(end - start_).count();

        HttpMetrics::instance().request_completed(method_, path_, status_code, duration);
    }

    ~RequestTimer() {
        if (!completed_) {
            complete(500); // Assume error if not explicitly completed
        }
    }

private:
    std::string method_;
    std::string path_;
    std::chrono::steady_clock::time_point start_;
    bool completed_ = false;
};

} // namespace fasterapi
