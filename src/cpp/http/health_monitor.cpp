#include "health_monitor.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <algorithm>

HealthMonitor::HealthMonitor() 
    : active_(false), running_(false), total_health_checks_(0), failed_health_checks_(0),
      total_metrics_recorded_(0), alerts_triggered_(0) {
}

HealthMonitor::~HealthMonitor() {
    stop();
}

int HealthMonitor::initialize() noexcept {
    if (active_.load()) {
        return 1;  // Already initialized
    }
    
    active_.store(true);
    std::cout << "Health monitor initialized" << std::endl;
    return 0;
}

int HealthMonitor::start() noexcept {
    if (!active_.load()) {
        return 1;  // Not initialized
    }
    
    if (running_.load()) {
        return 1;  // Already running
    }
    
    running_.store(true);
    
    // Start health check thread
    std::thread health_thread([this]() {
        while (running_.load()) {
            run_health_checks();
            check_alerts();
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    });
    
    health_thread.detach();
    
    std::cout << "Health monitor started" << std::endl;
    return 0;
}

int HealthMonitor::stop() noexcept {
    if (!running_.load()) {
        return 0;  // Already stopped
    }
    
    running_.store(false);
    std::cout << "Health monitor stopped" << std::endl;
    return 0;
}

bool HealthMonitor::is_active() const noexcept {
    return active_.load() && running_.load();
}

int HealthMonitor::add_health_check(const std::string& name, 
                                   std::function<HealthCheck()> check_func,
                                   std::chrono::seconds interval) noexcept {
    if (!active_.load()) {
        return 1;  // Not initialized
    }
    
    std::lock_guard<std::mutex> lock(health_checks_mutex_);
    
    auto entry = std::make_unique<HealthCheckEntry>();
    entry->name = name;
    entry->check_func = std::move(check_func);
    entry->interval = interval;
    entry->last_run = std::chrono::steady_clock::now();
    entry->last_result = HealthCheck{name, Status::HEALTHY, "Not checked yet", 
                                    entry->last_run, std::chrono::milliseconds(0), {}};
    
    health_checks_[name] = std::move(entry);
    return 0;
}

int HealthMonitor::remove_health_check(const std::string& name) noexcept {
    std::lock_guard<std::mutex> lock(health_checks_mutex_);
    
    auto it = health_checks_.find(name);
    if (it != health_checks_.end()) {
        health_checks_.erase(it);
        return 0;
    }
    
    return 1;  // Not found
}

HealthMonitor::HealthCheck HealthMonitor::get_health_check(const std::string& name) const noexcept {
    std::lock_guard<std::mutex> lock(health_checks_mutex_);
    
    auto it = health_checks_.find(name);
    if (it != health_checks_.end()) {
        return it->second->last_result;
    }
    
    return HealthCheck{name, Status::UNHEALTHY, "Check not found", 
                      std::chrono::steady_clock::now(), std::chrono::milliseconds(0), {}};
}

std::vector<HealthMonitor::HealthCheck> HealthMonitor::get_all_health_checks() const noexcept {
    std::lock_guard<std::mutex> lock(health_checks_mutex_);
    
    std::vector<HealthCheck> results;
    results.reserve(health_checks_.size());
    
    for (const auto& [name, entry] : health_checks_) {
        results.push_back(entry->last_result);
    }
    
    return results;
}

HealthMonitor::Status HealthMonitor::get_overall_health() const noexcept {
    std::lock_guard<std::mutex> lock(health_checks_mutex_);
    
    if (health_checks_.empty()) {
        return Status::HEALTHY;
    }
    
    Status worst_status = Status::HEALTHY;
    
    for (const auto& [name, entry] : health_checks_) {
        if (entry->last_result.status > worst_status) {
            worst_status = entry->last_result.status;
        }
    }
    
    return worst_status;
}

int HealthMonitor::record_metric(const std::string& name, MetricType type, double value,
                                const std::unordered_map<std::string, std::string>& labels) noexcept {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    Metric metric;
    metric.name = name;
    metric.type = type;
    metric.value = value;
    metric.timestamp = std::chrono::steady_clock::now();
    metric.labels = labels;
    
    metrics_[name] = metric;
    total_metrics_recorded_.fetch_add(1);
    
    return 0;
}

int HealthMonitor::increment_counter(const std::string& name, double value,
                                    const std::unordered_map<std::string, std::string>& labels) noexcept {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    auto it = metrics_.find(name);
    if (it != metrics_.end() && it->second.type == MetricType::COUNTER) {
        it->second.value += value;
    } else {
        Metric metric;
        metric.name = name;
        metric.type = MetricType::COUNTER;
        metric.value = value;
        metric.timestamp = std::chrono::steady_clock::now();
        metric.labels = labels;
        metrics_[name] = metric;
    }
    
    total_metrics_recorded_.fetch_add(1);
    return 0;
}

int HealthMonitor::set_gauge(const std::string& name, double value,
                           const std::unordered_map<std::string, std::string>& labels) noexcept {
    return record_metric(name, MetricType::GAUGE, value, labels);
}

int HealthMonitor::record_histogram(const std::string& name, double value,
                                   const std::unordered_map<std::string, std::string>& labels) noexcept {
    return record_metric(name, MetricType::HISTOGRAM, value, labels);
}

int HealthMonitor::record_timer(const std::string& name, std::chrono::milliseconds duration,
                               const std::unordered_map<std::string, std::string>& labels) noexcept {
    return record_metric(name, MetricType::TIMER, duration.count(), labels);
}

double HealthMonitor::get_metric(const std::string& name) const noexcept {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    auto it = metrics_.find(name);
    if (it != metrics_.end()) {
        return it->second.value;
    }
    
    return 0.0;
}

std::vector<HealthMonitor::Metric> HealthMonitor::get_all_metrics() const noexcept {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    
    std::vector<Metric> results;
    results.reserve(metrics_.size());
    
    for (const auto& [name, metric] : metrics_) {
        results.push_back(metric);
    }
    
    return results;
}

int HealthMonitor::add_alert(const AlertConfig& config) noexcept {
    std::lock_guard<std::mutex> lock(alerts_mutex_);
    alerts_[config.name] = config;
    return 0;
}

int HealthMonitor::remove_alert(const std::string& name) noexcept {
    std::lock_guard<std::mutex> lock(alerts_mutex_);
    
    auto it = alerts_.find(name);
    if (it != alerts_.end()) {
        alerts_.erase(it);
        return 0;
    }
    
    return 1;  // Not found
}

int HealthMonitor::trigger_alert(const std::string& name, const std::string& message) noexcept {
    std::lock_guard<std::mutex> lock(alerts_mutex_);
    
    auto it = alerts_.find(name);
    if (it != alerts_.end() && it->second.enabled) {
        if (it->second.callback) {
            it->second.callback(message);
        }
        alerts_triggered_.fetch_add(1);
        return 0;
    }
    
    return 1;  // Alert not found or disabled
}

std::string HealthMonitor::get_health_json() const noexcept {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"status\": \"" << status_to_string(get_overall_health()) << "\",\n";
    oss << "  \"timestamp\": \"" << std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() << "\",\n";
    oss << "  \"checks\": [\n";
    
    auto checks = get_all_health_checks();
    for (size_t i = 0; i < checks.size(); ++i) {
        const auto& check = checks[i];
        oss << "    {\n";
        oss << "      \"name\": \"" << check.name << "\",\n";
        oss << "      \"status\": \"" << status_to_string(check.status) << "\",\n";
        oss << "      \"message\": \"" << check.message << "\",\n";
        oss << "      \"duration_ms\": " << check.duration.count() << "\n";
        oss << "    }";
        if (i < checks.size() - 1) {
            oss << ",";
        }
        oss << "\n";
    }
    
    oss << "  ]\n";
    oss << "}";
    
    return oss.str();
}

std::string HealthMonitor::get_metrics_json() const noexcept {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"timestamp\": \"" << std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() << "\",\n";
    oss << "  \"metrics\": [\n";
    
    auto metrics = get_all_metrics();
    for (size_t i = 0; i < metrics.size(); ++i) {
        const auto& metric = metrics[i];
        oss << "    {\n";
        oss << "      \"name\": \"" << metric.name << "\",\n";
        oss << "      \"type\": \"" << metric_type_to_string(metric.type) << "\",\n";
        oss << "      \"value\": " << metric.value << ",\n";
        oss << "      \"timestamp\": \"" << std::chrono::duration_cast<std::chrono::milliseconds>(
            metric.timestamp.time_since_epoch()).count() << "\"\n";
        oss << "    }";
        if (i < metrics.size() - 1) {
            oss << ",";
        }
        oss << "\n";
    }
    
    oss << "  ]\n";
    oss << "}";
    
    return oss.str();
}

std::unordered_map<std::string, uint64_t> HealthMonitor::get_stats() const noexcept {
    std::unordered_map<std::string, uint64_t> stats;
    stats["total_health_checks"] = total_health_checks_.load();
    stats["failed_health_checks"] = failed_health_checks_.load();
    stats["total_metrics_recorded"] = total_metrics_recorded_.load();
    stats["alerts_triggered"] = alerts_triggered_.load();
    stats["active_health_checks"] = health_checks_.size();
    stats["active_metrics"] = metrics_.size();
    stats["active_alerts"] = alerts_.size();
    return stats;
}

void HealthMonitor::run_health_checks() noexcept {
    std::lock_guard<std::mutex> lock(health_checks_mutex_);
    
    auto now = std::chrono::steady_clock::now();
    
    for (auto& [name, entry] : health_checks_) {
        if (now - entry->last_run >= entry->interval) {
            total_health_checks_.fetch_add(1);
            
            auto start = std::chrono::steady_clock::now();
            // Call health check function (no exceptions due to -fno-exceptions)
            entry->last_result = entry->check_func();
            entry->last_result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            entry->last_result.last_check = now;
            
            if (entry->last_result.status != Status::HEALTHY) {
                failed_health_checks_.fetch_add(1);
            }
            
            entry->last_run = now;
        }
    }
}

void HealthMonitor::check_alerts() noexcept {
    // TODO: Implement alert condition checking
    // For now, just check if any health checks are failing
    auto overall_health = get_overall_health();
    
    if (overall_health == Status::CRITICAL) {
        trigger_alert("system_critical", "System health is critical");
    } else if (overall_health == Status::UNHEALTHY) {
        trigger_alert("system_unhealthy", "System health is unhealthy");
    }
}

std::string HealthMonitor::status_to_string(Status status) noexcept {
    switch (status) {
        case Status::HEALTHY: return "healthy";
        case Status::DEGRADED: return "degraded";
        case Status::UNHEALTHY: return "unhealthy";
        case Status::CRITICAL: return "critical";
        default: return "unknown";
    }
}

std::string HealthMonitor::metric_type_to_string(MetricType type) noexcept {
    switch (type) {
        case MetricType::COUNTER: return "counter";
        case MetricType::GAUGE: return "gauge";
        case MetricType::HISTOGRAM: return "histogram";
        case MetricType::TIMER: return "timer";
        default: return "unknown";
    }
}
