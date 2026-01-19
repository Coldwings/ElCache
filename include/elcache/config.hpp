#pragma once

#include "types.hpp"
#include <string>
#include <vector>
#include <optional>
#include <filesystem>

namespace elcache {

// Memory cache configuration
struct MemoryCacheConfig {
    size_t max_size = 1ULL * 1024 * 1024 * 1024;  // 1GB default
    size_t arc_ghost_size = 0;  // 0 = auto (same as max_size)
    double high_watermark = 0.95;  // Start eviction at 95%
    double low_watermark = 0.85;   // Stop eviction at 85%
};

// Disk cache configuration
struct DiskCacheConfig {
    std::filesystem::path path = "/var/cache/elcache";
    size_t max_size = 100ULL * 1024 * 1024 * 1024;  // 100GB default
    size_t block_size = 4096;  // Filesystem block size
    bool use_direct_io = true;  // O_DIRECT for large files
    bool use_fallocate = true;  // Pre-allocate space
    size_t write_buffer_size = 64 * 1024 * 1024;  // 64MB write buffer
    double high_watermark = 0.90;
    double low_watermark = 0.80;
};

// Network configuration
struct NetworkConfig {
    std::string bind_address = "0.0.0.0";
    uint16_t cluster_port = 7890;     // Inter-node communication
    uint16_t http_port = 8080;        // HTTP API
    uint16_t sdk_port = 7891;         // Unix socket/SHM for SDK
    std::string unix_socket_path = "/var/run/elcache/elcache.sock";
    std::string shm_path = "/elcache_shm";
    size_t shm_size = 256 * 1024 * 1024;  // 256MB shared memory
    
    // Connection settings
    size_t max_connections = 10000;
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds read_timeout{30000};
    std::chrono::milliseconds write_timeout{30000};
    
    // Buffer sizes
    size_t recv_buffer_size = 256 * 1024;
    size_t send_buffer_size = 256 * 1024;
};

// Cluster configuration
struct ClusterConfig {
    std::string cluster_name = "elcache";
    std::vector<std::string> seed_nodes;  // Initial nodes to contact
    
    // Gossip settings
    std::chrono::milliseconds gossip_interval{1000};
    std::chrono::milliseconds failure_detection_timeout{10000};
    size_t gossip_fanout = 3;  // Number of nodes to gossip to
    
    // Replication
    size_t replication_factor = 2;  // Number of copies in cluster
    bool prefer_local = true;  // Prefer local cache before network
    
    // Consistent hashing
    size_t virtual_nodes = 150;  // Virtual nodes per physical node
};

// Pool configuration (resources shared with cluster)
struct PoolConfig {
    size_t memory_contribution = 0;  // Bytes to contribute (0 = none)
    size_t disk_contribution = 0;    // Bytes to contribute (0 = none)
    bool accept_remote_reads = true;
    bool accept_remote_writes = true;
};

// Performance tuning
struct PerformanceConfig {
    size_t io_threads = 0;  // 0 = auto (based on CPU count)
    size_t worker_threads = 0;  // 0 = auto
    size_t max_concurrent_disk_ops = 64;
    size_t max_concurrent_network_ops = 256;
    size_t prefetch_chunks = 2;  // Prefetch N chunks ahead
    bool enable_metrics = true;
    std::chrono::seconds metrics_interval{60};
};

// Main configuration
struct Config {
    std::string node_name;  // Unique node identifier (empty = generate)
    
    MemoryCacheConfig memory;
    DiskCacheConfig disk;
    NetworkConfig network;
    ClusterConfig cluster;
    PoolConfig pool;
    PerformanceConfig perf;
    
    // Load from file
    static Config load(const std::filesystem::path& path);
    static Config load_json(const std::string& json);
    
    // Save to file
    void save(const std::filesystem::path& path) const;
    std::string to_json() const;
    
    // Validation
    Status validate() const;
};

}  // namespace elcache
