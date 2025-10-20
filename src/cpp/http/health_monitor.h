#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <atomic>
#include <mutex>
#include <functional>
#include <cstdint>

/**
 * Health monitoring system for HTTP server.
 * 
 * Features:
 * - Health check endpoints
 * - System metrics collection
 * - Performance monitoring
 * - Alerting system
 * - Graceful degradation
 */
class HealthMonitor {
public:
    // Health status
    enum class Status {
        HEALTHY,
        DEGRADED,
        UNHEALTHY,
        CRITICAL
    };

    // Metric types
    enum class MetricType {
        COUNTER,
        GAUGE,
        HISTOGRAM,
        TIMER
    };

    // Health check result
    struct HealthCheck {
        std::string name;
        Status status;
        std::string message;
        std::chrono::steady_clock::time_point last_check;
        std::chrono::milliseconds duration;
        std::unordered_map<std::string, std::string> details;
    };

    // Metric data
    struct Metric {
        std::string name;
        MetricType type;
        double value;
        std::chrono::steady_clock::time_point timestamp;
        std::unordered_map<std::string, std::string> labels;
    };

    // Alert configuration
    struct AlertConfig {
        std::string name;
        std::string condition;
        std::string severity;
        std::function<void(const std::string&)> callback;
        bool enabled = true;
    };

    /**
     * Constructor.
     */
    HealthMonitor();

    /**
     * Destructor.
     */
    ~HealthMonitor();

    /**
     * Initialize health monitor.
     * 
     * @return Error code (0 = success)
     */
    int initialize() noexcept;

    /**
     * Start health monitoring.
     * 
     * @return Error code (0 = success)
         */
    int start() noexcept;

    /**
     * Stop health monitoring.
     * 
     * @return Error code (0 = success)
     */
    int stop() noexcept;

    /**
     * Check if monitoring is active.
     * 
     * @return true if active, false otherwise
     */
    bool is_active() const noexcept;

    /**
     * Add health check.
     * 
     * @param name Check name
     * @param check_func Check function
     * @param interval Check interval
     * @return Error code (0 = success)
     */
    int add_health_check(const std::string& name, 
                        std::function<HealthCheck()> check_func,
                        std::chrono::seconds interval = std::chrono::seconds(30)) noexcept;

    /**
     * Remove health check.
     * 
     * @param name Check name
     * @return Error code (0 = success)
     */
    int remove_health_check(const std::string& name) noexcept;

    /**
     * Get health check result.
     * 
     * @param name Check name
     * @return Health check result
     */
    HealthCheck get_health_check(const std::string& name) const noexcept;

    /**
     * Get all health check results.
     * 
     * @return All health check results
     */
    std::vector<HealthCheck> get_all_health_checks() const noexcept;

    /**
     * Get overall system health.
     * 
     * @return Overall health status
     */
    Status get_overall_health() const noexcept;

    /**
     * Record metric.
     * 
     * @param name Metric name
     * @param type Metric type
     * @param value Metric value
     * @param labels Metric labels
     * @return Error code (0 = success)
     */
    int record_metric(const std::string& name, MetricType type, double value,
                     const std::unordered_map<std::string, std::string>& labels = {}) noexcept;

    /**
     * Increment counter metric.
     * 
     * @param name Metric name
     * @param value Increment value
     * @param labels Metric labels
     * @return Error code (0 = success)
     */
    int increment_counter(const std::string& name, double value = 1.0,
                         const std::unordered_map<std::string, std::string>& labels = {}) noexcept;

    /**
     * Set gauge metric.
     * 
     * @param name Metric name
     * @param value Gauge value
     * @param labels Metric labels
     * @return Error code (0 = success)
     */
    int set_gauge(const std::string& name, double value,
                 const std::unordered_map<std::string, std::string>& labels = {}) noexcept;

    /**
     * Record histogram metric.
     * 
     * @param name Metric name
     * @param value Histogram value
     * @param labels Metric labels
     * @return Error code (0 = success)
     */
    int record_histogram(const std::string& name, double value,
                        const std::unordered_map<std::string, std::string>& labels = {}) noexcept;

    /**
     * Record timer metric.
     * 
     * @param name Metric name
     * @param duration Timer duration
     * @param labels Metric labels
     * @return Error code (0 = success)
     */
    int record_timer(const std::string& name, std::chrono::milliseconds duration,
                    const std::unordered_map<std::string, std::string>& labels = {}) noexcept;

    /**
     * Get metric value.
     * 
     * @param name Metric name
     * @return Metric value
     */
    double get_metric(const std::string& name) const noexcept;

    /**
     * Get all metrics.
     * 
     * @return All metrics
     */
    std::vector<Metric> get_all_metrics() const noexcept;

    /**
     * Add alert configuration.
     * 
     * @param config Alert configuration
     * @return Error code (0 = success)
     */
    int add_alert(const AlertConfig& config) noexcept;

    /**
     * Remove alert configuration.
     * 
     * @param name Alert name
     * @return Error code (0 = success)
     */
    int remove_alert(const std::string& name) noexcept;

    /**
     * Trigger alert.
     * 
     * @param name Alert name
     * @param message Alert message
     * @return Error code (0 = success)
     */
    int trigger_alert(const std::string& name, const std::string& message) noexcept;

    /**
     * Get health status as JSON.
     * 
     * @return Health status JSON
     */
    std::string get_health_json() const noexcept;

    /**
     * Get metrics as JSON.
     * 
     * @return Metrics JSON
     */
    std::string get_metrics_json() const noexcept;

    /**
     * Get statistics.
     * 
     * @return Statistics map
     */
    std::unordered_map<std::string, uint64_t> get_stats() const noexcept;

private:
    std::atomic<bool> active_;
    std::atomic<bool> running_;
    
    // Health checks
    struct HealthCheckEntry {
        std::string name;
        std::function<HealthCheck()> check_func;
        std::chrono::seconds interval;
        std::chrono::steady_clock::time_point last_run;
        HealthCheck last_result;
    };
    
    std::unordered_map<std::string, std::unique_ptr<HealthCheckEntry>> health_checks_;
    mutable std::mutex health_checks_mutex_;
    
    // Metrics
    std::unordered_map<std::string, Metric> metrics_;
    mutable std::mutex metrics_mutex_;
    
    // Alerts
    std::unordered_map<std::string, AlertConfig> alerts_;
    mutable std::mutex alerts_mutex_;
    
    // Statistics
    std::atomic<uint64_t> total_health_checks_;
    std::atomic<uint64_t> failed_health_checks_;
    std::atomic<uint64_t> total_metrics_recorded_;
    std::atomic<uint64_t> alerts_triggered_;
    
    /**
     * Run health checks.
     */
    void run_health_checks() noexcept;
    
    /**
     * Check alert conditions.
     */
    void check_alerts() noexcept;
    
    /**
     * Convert status to string.
     * 
     * @param status Health status
     * @return Status string
     */
    static std::string status_to_string(Status status) noexcept;
    
    /**
     * Convert metric type to string.
     * 
     * @param type Metric type
     * @return Type string
     */
    static std::string metric_type_to_string(MetricType type) noexcept;
};
