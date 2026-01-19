#include "elcache/elcache.hpp"
#include <elio/runtime/scheduler.hpp>
#include <iostream>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <thread>

namespace {

std::atomic<bool> g_running{true};
elio::runtime::scheduler* g_scheduler = nullptr;

void signal_handler(int sig) {
    std::cout << "\nReceived signal " << sig << ", shutting down...\n";
    g_running = false;
    // Wake up the scheduler by setting a flag
    if (g_scheduler) {
        g_scheduler->shutdown();
    }
}

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -c, --config <file>    Configuration file path\n"
              << "  -p, --port <port>      HTTP port (default: 8080)\n"
              << "  -m, --memory <size>    Memory cache size in MB (default: 1024)\n"
              << "  -d, --disk <path>      Disk cache directory\n"
              << "  -D, --disk-size <size> Disk cache size in GB (default: 100)\n"
              << "  -s, --seed <addr>      Seed node address (host:port)\n"
              << "  -h, --help             Show this help\n"
              << "  -v, --version          Show version\n";
}

void print_version() {
    std::cout << "ElCache version " << elcache::Version::string() << "\n"
              << "Distributed blob cache with partial read support\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    elcache::Config config;
    std::string config_file;
    
    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        
        if (arg == "-v" || arg == "--version") {
            print_version();
            return 0;
        }
        
        if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            config_file = argv[++i];
        }
        else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            config.network.http_port = std::stoi(argv[++i]);
        }
        else if ((arg == "-m" || arg == "--memory") && i + 1 < argc) {
            config.memory.max_size = std::stoull(argv[++i]) * 1024 * 1024;
        }
        else if ((arg == "-d" || arg == "--disk") && i + 1 < argc) {
            config.disk.path = argv[++i];
        }
        else if ((arg == "-D" || arg == "--disk-size") && i + 1 < argc) {
            config.disk.max_size = std::stoull(argv[++i]) * 1024ULL * 1024 * 1024;
        }
        else if ((arg == "-s" || arg == "--seed") && i + 1 < argc) {
            config.cluster.seed_nodes.push_back(argv[++i]);
        }
        else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }
    
    // Load config file if specified
    if (!config_file.empty()) {
        try {
            config = elcache::Config::load(config_file);
        } catch (const std::exception& e) {
            std::cerr << "Failed to load config: " << e.what() << "\n";
            return 1;
        }
    }
    
    // Validate config
    auto status = config.validate();
    if (!status) {
        std::cerr << "Invalid configuration: " << status.message() << "\n";
        return 1;
    }
    
    // Setup signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    // Print startup info
    std::cout << "Starting ElCache " << elcache::Version::string() << "\n";
    std::cout << "  Memory cache: " << (config.memory.max_size / (1024 * 1024)) << " MB\n";
    if (config.disk.max_size > 0) {
        std::cout << "  Disk cache: " << (config.disk.max_size / (1024ULL * 1024 * 1024)) 
                  << " GB at " << config.disk.path << "\n";
    }
    std::cout << "  HTTP port: " << config.network.http_port << "\n";
    std::cout << "  Cluster port: " << config.network.cluster_port << "\n";
    
    if (!config.cluster.seed_nodes.empty()) {
        std::cout << "  Seed nodes:";
        for (const auto& seed : config.cluster.seed_nodes) {
            std::cout << " " << seed;
        }
        std::cout << "\n";
    }
    
    // Create and start server using Elio's scheduler
    try {
        // Determine number of worker threads
        size_t num_threads = config.perf.worker_threads;
        if (num_threads == 0) {
            num_threads = std::thread::hardware_concurrency();
            if (num_threads == 0) num_threads = 4;
        }
        
        // Create scheduler
        elio::runtime::scheduler sched(num_threads);
        g_scheduler = &sched;
        
        // Create server
        elcache::ElCacheServer server(config);
        
        // Connect scheduler to server's io_context
        sched.set_io_context(&server.io_context());
        
        // Start the scheduler
        sched.start();
        
        // Create startup task
        auto startup_task = [&server, &sched]() -> elio::coro::task<void> {
            auto status = co_await server.start(sched);
            if (!status) {
                std::cerr << "Failed to start server: " << status.message() << "\n";
                g_running = false;
            } else {
                std::cout << "ElCache server started successfully\n";
                std::cout << "Press Ctrl+C to stop\n";
            }
        };
        
        // Spawn startup task
        auto task = startup_task();
        sched.spawn(task.release());
        
        // Main loop - poll IO and check for shutdown
        while (g_running && sched.is_running()) {
            // Let the scheduler do work
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        std::cout << "Shutting down...\n";
        
        // Create shutdown task
        auto shutdown_task = [&server]() -> elio::coro::task<void> {
            co_await server.stop();
        };
        
        // Give server time to shutdown gracefully
        if (sched.is_running()) {
            auto stop_task = shutdown_task();
            sched.spawn(stop_task.release());
            
            // Wait briefly for shutdown to complete
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        
        // Shutdown scheduler
        sched.shutdown();
        g_scheduler = nullptr;
        
        std::cout << "ElCache server stopped\n";
        
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}

// ElCacheServer implementation

namespace elcache {

ElCacheServer::ElCacheServer(const Config& config)
    : config_(config)
    , io_ctx_()
{
    coordinator_ = std::make_unique<CacheCoordinator>(config, io_ctx_);
    
    if (!config.cluster.seed_nodes.empty() || config.pool.memory_contribution > 0 ||
        config.pool.disk_contribution > 0) {
        cluster_ = std::make_shared<Cluster>(config, io_ctx_);
        coordinator_->set_cluster(cluster_);
    }
    
    // Setup metrics collector
    metrics_collector_ = std::make_unique<MetricsCollector>();
    metrics_collector_->set_cache(coordinator_.get());
    if (cluster_) {
        metrics_collector_->set_cluster(cluster_.get());
    }
    
    http_handler_ = std::make_unique<HttpHandler>(*coordinator_);
    http_handler_->set_metrics_collector(metrics_collector_.get());
    http_server_ = std::make_unique<HttpServer>(config.network, *http_handler_);
}

ElCacheServer::~ElCacheServer() = default;

elio::coro::task<Status> ElCacheServer::start(elio::runtime::scheduler& sched) {
    running_ = true;
    
    auto status = co_await coordinator_->start();
    if (!status) {
        co_return status;
    }
    
    if (cluster_) {
        status = co_await cluster_->start(sched);
        if (!status) {
            co_return status;
        }
    }
    
    status = co_await http_server_->start(io_ctx_, sched);
    if (!status) {
        co_return status;
    }
    
    co_return Status::make_ok();
}

elio::coro::task<void> ElCacheServer::stop() {
    running_ = false;
    
    co_await http_server_->stop();
    
    if (cluster_) {
        co_await cluster_->stop();
    }
    
    co_await coordinator_->stop();
}

}  // namespace elcache
