#include "elcache/metrics.hpp"
#include "elcache/cache_coordinator.hpp"
#include "elcache/cluster.hpp"
#include <iomanip>
#include <cmath>

namespace elcache {

// Global metrics instance
static Metrics g_metrics;

Metrics& metrics() {
    return g_metrics;
}

// LatencyHistogram implementation

LatencyHistogram::LatencyHistogram() {
    // Initialize default buckets
    for (double bound : DEFAULT_BUCKETS) {
        buckets_.emplace_back(bound);
    }
    // Add +Inf bucket
    buckets_.emplace_back(std::numeric_limits<double>::infinity());
}

void LatencyHistogram::observe(double value_ms) {
    // Find the right bucket and increment
    for (auto& bucket : buckets_) {
        if (value_ms <= bucket.upper_bound) {
            bucket.count->fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    
    count_.fetch_add(1, std::memory_order_relaxed);
    
    // Update sum (atomic double addition)
    double old_sum = sum_.load(std::memory_order_relaxed);
    while (!sum_.compare_exchange_weak(old_sum, old_sum + value_ms,
                                        std::memory_order_relaxed));
}

LatencyHistogram::Snapshot LatencyHistogram::snapshot() const {
    Snapshot snap;
    snap.count = count_.load(std::memory_order_relaxed);
    snap.sum = sum_.load(std::memory_order_relaxed);
    
    uint64_t cumulative = 0;
    for (const auto& bucket : buckets_) {
        cumulative += bucket.count->load(std::memory_order_relaxed);
        snap.buckets.emplace_back(bucket.upper_bound, cumulative);
    }
    
    return snap;
}

void LatencyHistogram::reset() {
    for (auto& bucket : buckets_) {
        bucket.count->store(0, std::memory_order_relaxed);
    }
    count_.store(0, std::memory_order_relaxed);
    sum_.store(0, std::memory_order_relaxed);
}

// MetricsCollector implementation

MetricsCollector::MetricsCollector() = default;

void MetricsCollector::collect() {
    auto& m = metrics();
    
    // Collect from cache coordinator
    if (cache_) {
        auto stats = cache_->stats();
        
        m.memory_cache_entries.set(static_cast<double>(stats.entry_count));
        m.memory_cache_size_bytes.set(static_cast<double>(stats.size_bytes));
        m.memory_cache_capacity_bytes.set(static_cast<double>(stats.capacity_bytes));
        
        // Disk cache stats would come from disk_cache() if available
        if (cache_->disk_cache()) {
            auto disk_stats = cache_->disk_cache()->stats();
            m.disk_cache_entries.set(static_cast<double>(disk_stats.entry_count));
            m.disk_cache_size_bytes.set(static_cast<double>(disk_stats.size_bytes));
            m.disk_cache_capacity_bytes.set(static_cast<double>(disk_stats.capacity_bytes));
        }
    }
    
    // Collect from cluster
    if (cluster_) {
        auto stats = cluster_->stats();
        m.cluster_nodes_total.set(static_cast<double>(stats.total_nodes));
        m.cluster_nodes_active.set(static_cast<double>(stats.active_nodes));
    }
}

void MetricsCollector::write_counter(std::ostringstream& out, 
                                      const std::string& name,
                                      const std::string& help, 
                                      uint64_t value) const {
    out << "# HELP " << name << " " << help << "\n";
    out << "# TYPE " << name << " counter\n";
    out << name << " " << value << "\n";
}

void MetricsCollector::write_gauge(std::ostringstream& out,
                                    const std::string& name,
                                    const std::string& help,
                                    double value) const {
    out << "# HELP " << name << " " << help << "\n";
    out << "# TYPE " << name << " gauge\n";
    out << name << " " << std::fixed << std::setprecision(2) << value << "\n";
}

void MetricsCollector::write_histogram(std::ostringstream& out,
                                        const std::string& name,
                                        const std::string& help,
                                        const LatencyHistogram::Snapshot& snap) const {
    out << "# HELP " << name << " " << help << "\n";
    out << "# TYPE " << name << " histogram\n";
    
    for (const auto& [bound, count] : snap.buckets) {
        out << name << "_bucket{le=\"";
        if (std::isinf(bound)) {
            out << "+Inf";
        } else {
            out << std::fixed << std::setprecision(1) << bound;
        }
        out << "\"} " << count << "\n";
    }
    
    out << name << "_sum " << std::fixed << std::setprecision(3) << snap.sum << "\n";
    out << name << "_count " << snap.count << "\n";
}

std::string MetricsCollector::export_prometheus() const {
    std::lock_guard lock(mutex_);
    std::ostringstream out;
    
    auto& m = metrics();
    
    // Cache operation counters
    write_counter(out, "elcache_cache_hits_total", 
                  "Total number of cache hits", m.cache_hits_total.get());
    write_counter(out, "elcache_cache_misses_total",
                  "Total number of cache misses", m.cache_misses_total.get());
    write_counter(out, "elcache_cache_partial_hits_total",
                  "Total number of partial cache hits", m.cache_partial_hits_total.get());
    write_counter(out, "elcache_cache_gets_total",
                  "Total number of get operations", m.cache_gets_total.get());
    write_counter(out, "elcache_cache_puts_total",
                  "Total number of put operations", m.cache_puts_total.get());
    write_counter(out, "elcache_cache_deletes_total",
                  "Total number of delete operations", m.cache_deletes_total.get());
    
    // Bytes
    write_counter(out, "elcache_bytes_read_total",
                  "Total bytes read from cache", m.bytes_read_total.get());
    write_counter(out, "elcache_bytes_written_total",
                  "Total bytes written to cache", m.bytes_written_total.get());
    
    // Memory cache gauges
    write_gauge(out, "elcache_memory_cache_size_bytes",
                "Current size of memory cache in bytes", m.memory_cache_size_bytes.get());
    write_gauge(out, "elcache_memory_cache_capacity_bytes",
                "Capacity of memory cache in bytes", m.memory_cache_capacity_bytes.get());
    write_gauge(out, "elcache_memory_cache_entries",
                "Number of entries in memory cache", m.memory_cache_entries.get());
    
    // Disk cache gauges
    write_gauge(out, "elcache_disk_cache_size_bytes",
                "Current size of disk cache in bytes", m.disk_cache_size_bytes.get());
    write_gauge(out, "elcache_disk_cache_capacity_bytes",
                "Capacity of disk cache in bytes", m.disk_cache_capacity_bytes.get());
    write_gauge(out, "elcache_disk_cache_entries",
                "Number of entries in disk cache", m.disk_cache_entries.get());
    
    // Evictions
    write_counter(out, "elcache_memory_evictions_total",
                  "Total memory cache evictions", m.memory_evictions_total.get());
    write_counter(out, "elcache_disk_evictions_total",
                  "Total disk cache evictions", m.disk_evictions_total.get());
    
    // Latency histograms
    write_histogram(out, "elcache_get_latency_ms",
                    "Get operation latency in milliseconds", m.get_latency_ms.snapshot());
    write_histogram(out, "elcache_put_latency_ms",
                    "Put operation latency in milliseconds", m.put_latency_ms.snapshot());
    write_histogram(out, "elcache_delete_latency_ms",
                    "Delete operation latency in milliseconds", m.delete_latency_ms.snapshot());
    
    // Cluster metrics
    write_gauge(out, "elcache_cluster_nodes_total",
                "Total number of nodes in cluster", m.cluster_nodes_total.get());
    write_gauge(out, "elcache_cluster_nodes_active",
                "Number of active nodes in cluster", m.cluster_nodes_active.get());
    write_counter(out, "elcache_cluster_messages_sent_total",
                  "Total cluster messages sent", m.cluster_messages_sent_total.get());
    write_counter(out, "elcache_cluster_messages_received_total",
                  "Total cluster messages received", m.cluster_messages_received_total.get());
    
    // HTTP metrics
    write_counter(out, "elcache_http_requests_total",
                  "Total HTTP requests handled", m.http_requests_total.get());
    write_counter(out, "elcache_http_errors_total",
                  "Total HTTP errors", m.http_errors_total.get());
    write_histogram(out, "elcache_http_request_latency_ms",
                    "HTTP request latency in milliseconds", 
                    m.http_request_latency_ms.snapshot());
    write_gauge(out, "elcache_http_connections_active",
                "Active HTTP connections", m.http_connections_active.get());
    
    // Chunk metrics
    write_counter(out, "elcache_chunks_read_total",
                  "Total chunks read", m.chunks_read_total.get());
    write_counter(out, "elcache_chunks_written_total",
                  "Total chunks written", m.chunks_written_total.get());
    write_gauge(out, "elcache_chunks_in_memory",
                "Chunks currently in memory", m.chunks_in_memory.get());
    write_gauge(out, "elcache_chunks_on_disk",
                "Chunks currently on disk", m.chunks_on_disk.get());
    
    // Uptime
    write_gauge(out, "elcache_uptime_seconds",
                "Time since server start in seconds", m.uptime_seconds());
    
    // Hit rate (calculated)
    uint64_t total_gets = m.cache_hits_total.get() + m.cache_misses_total.get() + 
                          m.cache_partial_hits_total.get();
    double hit_rate = total_gets > 0 
        ? static_cast<double>(m.cache_hits_total.get()) / total_gets 
        : 0;
    write_gauge(out, "elcache_cache_hit_rate",
                "Cache hit rate (hits / total requests)", hit_rate);
    
    return out.str();
}

std::string MetricsCollector::export_json() const {
    std::lock_guard lock(mutex_);
    std::ostringstream out;
    
    auto& m = metrics();
    
    out << "{\n";
    out << "  \"cache\": {\n";
    out << "    \"hits\": " << m.cache_hits_total.get() << ",\n";
    out << "    \"misses\": " << m.cache_misses_total.get() << ",\n";
    out << "    \"partial_hits\": " << m.cache_partial_hits_total.get() << ",\n";
    out << "    \"gets\": " << m.cache_gets_total.get() << ",\n";
    out << "    \"puts\": " << m.cache_puts_total.get() << ",\n";
    out << "    \"deletes\": " << m.cache_deletes_total.get() << ",\n";
    out << "    \"bytes_read\": " << m.bytes_read_total.get() << ",\n";
    out << "    \"bytes_written\": " << m.bytes_written_total.get() << "\n";
    out << "  },\n";
    
    out << "  \"memory_cache\": {\n";
    out << "    \"size_bytes\": " << static_cast<uint64_t>(m.memory_cache_size_bytes.get()) << ",\n";
    out << "    \"capacity_bytes\": " << static_cast<uint64_t>(m.memory_cache_capacity_bytes.get()) << ",\n";
    out << "    \"entries\": " << static_cast<uint64_t>(m.memory_cache_entries.get()) << ",\n";
    out << "    \"evictions\": " << m.memory_evictions_total.get() << "\n";
    out << "  },\n";
    
    out << "  \"disk_cache\": {\n";
    out << "    \"size_bytes\": " << static_cast<uint64_t>(m.disk_cache_size_bytes.get()) << ",\n";
    out << "    \"capacity_bytes\": " << static_cast<uint64_t>(m.disk_cache_capacity_bytes.get()) << ",\n";
    out << "    \"entries\": " << static_cast<uint64_t>(m.disk_cache_entries.get()) << ",\n";
    out << "    \"evictions\": " << m.disk_evictions_total.get() << "\n";
    out << "  },\n";
    
    out << "  \"cluster\": {\n";
    out << "    \"nodes_total\": " << static_cast<uint64_t>(m.cluster_nodes_total.get()) << ",\n";
    out << "    \"nodes_active\": " << static_cast<uint64_t>(m.cluster_nodes_active.get()) << ",\n";
    out << "    \"messages_sent\": " << m.cluster_messages_sent_total.get() << ",\n";
    out << "    \"messages_received\": " << m.cluster_messages_received_total.get() << "\n";
    out << "  },\n";
    
    out << "  \"http\": {\n";
    out << "    \"requests\": " << m.http_requests_total.get() << ",\n";
    out << "    \"errors\": " << m.http_errors_total.get() << ",\n";
    out << "    \"connections_active\": " << static_cast<uint64_t>(m.http_connections_active.get()) << "\n";
    out << "  },\n";
    
    auto get_latency = m.get_latency_ms.snapshot();
    double avg_get_latency = get_latency.count > 0 ? get_latency.sum / get_latency.count : 0;
    
    out << "  \"latency_ms\": {\n";
    out << "    \"get_avg\": " << std::fixed << std::setprecision(3) << avg_get_latency << ",\n";
    
    auto put_latency = m.put_latency_ms.snapshot();
    double avg_put_latency = put_latency.count > 0 ? put_latency.sum / put_latency.count : 0;
    out << "    \"put_avg\": " << std::fixed << std::setprecision(3) << avg_put_latency << "\n";
    out << "  },\n";
    
    out << "  \"uptime_seconds\": " << std::fixed << std::setprecision(1) << m.uptime_seconds() << "\n";
    out << "}\n";
    
    return out.str();
}

}  // namespace elcache
