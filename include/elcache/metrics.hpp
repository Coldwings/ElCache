#pragma once

#include "types.hpp"
#include "cache.hpp"
#include <string>
#include <sstream>
#include <atomic>
#include <mutex>
#include <memory>
#include <chrono>
#include <vector>
#include <unordered_map>

namespace elcache {

// Forward declarations
class CacheCoordinator;
class Cluster;

// Histogram bucket for latency tracking
struct HistogramBucket {
    double upper_bound;
    std::unique_ptr<std::atomic<uint64_t>> count;
    
    HistogramBucket(double bound) : upper_bound(bound), count(std::make_unique<std::atomic<uint64_t>>(0)) {}
};

// Latency histogram with configurable buckets
class LatencyHistogram {
public:
    LatencyHistogram();
    
    void observe(double value_ms);
    
    // Get bucket counts and sum for Prometheus format
    struct Snapshot {
        std::vector<std::pair<double, uint64_t>> buckets;
        uint64_t count;
        double sum;
    };
    
    Snapshot snapshot() const;
    void reset();
    
private:
    std::vector<HistogramBucket> buckets_;
    std::atomic<uint64_t> count_{0};
    std::atomic<double> sum_{0};
    
    // Default buckets in milliseconds
    static constexpr double DEFAULT_BUCKETS[] = {
        0.1, 0.5, 1, 2.5, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000
    };
};

// Counter metric
class Counter {
public:
    Counter() = default;
    
    void inc(uint64_t n = 1) { value_.fetch_add(n, std::memory_order_relaxed); }
    uint64_t get() const { return value_.load(std::memory_order_relaxed); }
    void reset() { value_.store(0, std::memory_order_relaxed); }
    
private:
    std::atomic<uint64_t> value_{0};
};

// Gauge metric (can go up or down)
class Gauge {
public:
    Gauge() = default;
    
    void set(double v) { value_.store(v, std::memory_order_relaxed); }
    void inc(double n = 1) { 
        double old = value_.load();
        while (!value_.compare_exchange_weak(old, old + n));
    }
    void dec(double n = 1) { inc(-n); }
    double get() const { return value_.load(std::memory_order_relaxed); }
    
private:
    std::atomic<double> value_{0};
};

// All metrics for ElCache
struct Metrics {
    // Cache operations
    Counter cache_hits_total;
    Counter cache_misses_total;
    Counter cache_partial_hits_total;
    Counter cache_gets_total;
    Counter cache_puts_total;
    Counter cache_deletes_total;
    
    // Bytes
    Counter bytes_read_total;
    Counter bytes_written_total;
    
    // Cache size
    Gauge memory_cache_size_bytes;
    Gauge memory_cache_capacity_bytes;
    Gauge memory_cache_entries;
    Gauge disk_cache_size_bytes;
    Gauge disk_cache_capacity_bytes;
    Gauge disk_cache_entries;
    
    // Evictions
    Counter memory_evictions_total;
    Counter disk_evictions_total;
    
    // Latency histograms
    LatencyHistogram get_latency_ms;
    LatencyHistogram put_latency_ms;
    LatencyHistogram delete_latency_ms;
    
    // Cluster metrics
    Gauge cluster_nodes_total;
    Gauge cluster_nodes_active;
    Counter cluster_messages_sent_total;
    Counter cluster_messages_received_total;
    Counter cluster_bytes_sent_total;
    Counter cluster_bytes_received_total;
    
    // HTTP server metrics
    Counter http_requests_total;
    Counter http_errors_total;
    LatencyHistogram http_request_latency_ms;
    Gauge http_connections_active;
    
    // Chunk metrics
    Counter chunks_read_total;
    Counter chunks_written_total;
    Counter chunks_deleted_total;
    Gauge chunks_in_memory;
    Gauge chunks_on_disk;
    
    // Uptime
    std::chrono::steady_clock::time_point start_time;
    
    Metrics() : start_time(std::chrono::steady_clock::now()) {}
    
    double uptime_seconds() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(now - start_time).count();
    }
};

// Global metrics instance
Metrics& metrics();

// Metrics collector - aggregates metrics from various sources
class MetricsCollector {
public:
    MetricsCollector();
    
    // Set sources for metrics collection
    void set_cache(CacheCoordinator* cache) { cache_ = cache; }
    void set_cluster(Cluster* cluster) { cluster_ = cluster; }
    
    // Update metrics from sources
    void collect();
    
    // Export to Prometheus format
    std::string export_prometheus() const;
    
    // Export to JSON format
    std::string export_json() const;
    
private:
    CacheCoordinator* cache_ = nullptr;
    Cluster* cluster_ = nullptr;
    mutable std::mutex mutex_;
    
    void write_counter(std::ostringstream& out, const std::string& name,
                       const std::string& help, uint64_t value) const;
    void write_gauge(std::ostringstream& out, const std::string& name,
                     const std::string& help, double value) const;
    void write_histogram(std::ostringstream& out, const std::string& name,
                         const std::string& help, 
                         const LatencyHistogram::Snapshot& snap) const;
};

// RAII timer for latency measurement
class LatencyTimer {
public:
    explicit LatencyTimer(LatencyHistogram& histogram)
        : histogram_(histogram)
        , start_(std::chrono::steady_clock::now())
    {}
    
    ~LatencyTimer() {
        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start_).count();
        histogram_.observe(ms);
    }
    
    // Non-copyable
    LatencyTimer(const LatencyTimer&) = delete;
    LatencyTimer& operator=(const LatencyTimer&) = delete;
    
private:
    LatencyHistogram& histogram_;
    std::chrono::steady_clock::time_point start_;
};

// Convenience macro for timing operations
#define ELCACHE_TIME_OPERATION(histogram) \
    elcache::LatencyTimer _timer_##__LINE__(histogram)

}  // namespace elcache
