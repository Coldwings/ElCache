#pragma once

// Main ElCache header - includes everything needed

#include "types.hpp"
#include "config.hpp"
#include "chunk.hpp"
#include "cache.hpp"
#include "arc_cache.hpp"
#include "memory_cache.hpp"
#include "disk_cache.hpp"
#include "cache_coordinator.hpp"
#include "cluster.hpp"
#include "protocol.hpp"
#include "http_api.hpp"
#include "unix_server.hpp"
#include "metrics.hpp"
#include <elio/io/io_context.hpp>
#include <elio/runtime/scheduler.hpp>

namespace elcache {

// Main server class
class ElCacheServer {
public:
    explicit ElCacheServer(const Config& config);
    ~ElCacheServer();
    
    // Start all services (requires scheduler for HTTP server)
    elio::coro::task<Status> start(elio::runtime::scheduler& sched);
    
    // Stop all services gracefully
    elio::coro::task<void> stop();
    
    // Access components
    CacheCoordinator& cache() { return *coordinator_; }
    Cluster* cluster() { return cluster_.get(); }
    HttpServer* http_server() { return http_server_.get(); }
    UnixSocketServer* unix_server() { return unix_server_.get(); }
    MetricsCollector& metrics() { return *metrics_collector_; }
    elio::io::io_context& io_context() { return io_ctx_; }
    
    // Configuration
    const Config& config() const { return config_; }
    
private:
    Config config_;
    elio::io::io_context io_ctx_;  // Owned io_context for all async I/O
    std::unique_ptr<CacheCoordinator> coordinator_;
    std::shared_ptr<Cluster> cluster_;
    std::unique_ptr<MetricsCollector> metrics_collector_;
    std::unique_ptr<HttpHandler> http_handler_;
    std::unique_ptr<HttpServer> http_server_;
    std::unique_ptr<UnixSocketServer> unix_server_;
    
    std::atomic<bool> running_{false};
};

// Version information
struct Version {
    static constexpr int major = 0;
    static constexpr int minor = 1;
    static constexpr int patch = 0;
    static const char* string() { return "0.1.0"; }
};

}  // namespace elcache
