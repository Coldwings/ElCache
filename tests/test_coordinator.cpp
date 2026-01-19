#include <catch2/catch_test_macros.hpp>
#include "elcache/cache_coordinator.hpp"
#include "elcache/config.hpp"
#include <elio/io/io_context.hpp>
#include <filesystem>
#include <random>

using namespace elcache;

// Helper to create a temp directory
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() / 
                ("elcache_coord_test_" + std::to_string(rd()));
        std::filesystem::create_directories(path_);
    }
    
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    
    const std::filesystem::path& path() const { return path_; }
    
private:
    std::filesystem::path path_;
};

TEST_CASE("CacheCoordinator configuration", "[coordinator]") {
    TempDir tmp;
    elio::io::io_context io_ctx;
    
    SECTION("Default configuration") {
        Config config;
        config.disk.path = tmp.path();
        config.disk.use_direct_io = false;
        
        CacheCoordinator coordinator(config, io_ctx);
        
        // Should have memory cache initialized
        REQUIRE(coordinator.memory_cache() != nullptr);
        
        // Should have disk cache initialized
        REQUIRE(coordinator.disk_cache() != nullptr);
        
        // No cluster by default
        REQUIRE(coordinator.cluster() == nullptr);
    }
    
    SECTION("Memory-only mode") {
        Config config;
        config.memory.max_size = 128 * 1024 * 1024;  // 128MB
        config.disk.max_size = 0;  // Disable disk cache
        config.disk.path = tmp.path();
        
        CacheCoordinator coordinator(config, io_ctx);
        
        REQUIRE(coordinator.memory_cache() != nullptr);
        // Disk cache may still be created but with 0 size
    }
    
    SECTION("Custom memory size") {
        Config config;
        config.memory.max_size = 256 * 1024 * 1024;  // 256MB
        config.disk.path = tmp.path();
        config.disk.use_direct_io = false;
        
        CacheCoordinator coordinator(config, io_ctx);
        auto stats = coordinator.stats();
        
        // Stats should reflect configured sizes
        REQUIRE(stats.capacity_bytes > 0);
    }
}

TEST_CASE("CacheCoordinator stats aggregation", "[coordinator]") {
    TempDir tmp;
    elio::io::io_context io_ctx;
    
    Config config;
    config.memory.max_size = 64 * 1024 * 1024;
    config.disk.path = tmp.path();
    config.disk.max_size = 1024 * 1024 * 1024;
    config.disk.use_direct_io = false;
    
    CacheCoordinator coordinator(config, io_ctx);
    
    SECTION("Initial stats") {
        auto stats = coordinator.stats();
        
        REQUIRE(stats.hits == 0);
        REQUIRE(stats.misses == 0);
        REQUIRE(stats.evictions == 0);
        REQUIRE(stats.size_bytes == 0);
    }
    
    SECTION("Stats structure completeness") {
        auto stats = coordinator.stats();
        
        // All stat fields should be accessible
        REQUIRE(stats.capacity_bytes > 0);
    }
}

TEST_CASE("CacheCoordinator layer access", "[coordinator]") {
    TempDir tmp;
    elio::io::io_context io_ctx;
    
    Config config;
    config.disk.path = tmp.path();
    config.disk.use_direct_io = false;
    
    CacheCoordinator coordinator(config, io_ctx);
    
    SECTION("Memory cache access") {
        auto* memory = coordinator.memory_cache();
        REQUIRE(memory != nullptr);
        
        auto mem_stats = memory->stats();
        REQUIRE(mem_stats.capacity_bytes == config.memory.max_size);
    }
    
    SECTION("Disk cache access") {
        auto* disk = coordinator.disk_cache();
        REQUIRE(disk != nullptr);
        
        auto disk_stats = disk->stats();
        REQUIRE(disk_stats.capacity_bytes == config.disk.max_size);
    }
}

TEST_CASE("WriteStream and ReadStream interfaces", "[coordinator]") {
    // Document the streaming interfaces for large values
    
    SECTION("WriteStream interface") {
        // WriteStream should support:
        // - write(ByteView chunk): append data
        // - finish(): complete the write
        // - abort(): cancel the write
        
        // These are abstract interfaces tested via integration
    }
    
    SECTION("ReadStream interface") {
        // ReadStream should support:
        // - read(max_bytes): get next chunk
        // - eof(): check if complete
        // - bytes_read(): progress
        // - total_size(): full size
        
        // These are abstract interfaces tested via integration
    }
}

TEST_CASE("Config validation", "[coordinator]") {
    TempDir tmp;
    
    SECTION("Valid config") {
        Config config;
        config.disk.path = tmp.path();
        
        auto status = config.validate();
        REQUIRE(status.ok());
    }
    
    SECTION("Invalid watermarks - memory") {
        Config config;
        config.memory.high_watermark = 0.5;
        config.memory.low_watermark = 0.9;  // Low > high is invalid
        config.disk.path = tmp.path();
        
        auto status = config.validate();
        REQUIRE(!status.ok());
    }
    
    SECTION("Invalid watermarks - disk") {
        Config config;
        config.disk.high_watermark = 0.5;
        config.disk.low_watermark = 0.9;  // Low > high is invalid
        config.disk.path = tmp.path();
        
        auto status = config.validate();
        REQUIRE(!status.ok());
    }
    
    SECTION("Zero replication factor") {
        Config config;
        config.cluster.replication_factor = 0;
        config.disk.path = tmp.path();
        
        auto status = config.validate();
        REQUIRE(!status.ok());
    }
    
    SECTION("Port conflicts") {
        Config config;
        config.network.http_port = 8080;
        config.network.cluster_port = 8080;  // Same port is invalid
        config.disk.path = tmp.path();
        
        auto status = config.validate();
        REQUIRE(!status.ok());
    }
}

TEST_CASE("Cache hierarchy principles", "[coordinator]") {
    // Document the expected behavior of the multi-level cache
    
    SECTION("Read path: Memory -> Disk -> Cluster") {
        // 1. Check memory cache first (fastest)
        // 2. If miss, check disk cache
        // 3. If miss, query cluster peers
        // 4. On hit from lower level, promote to higher levels
    }
    
    SECTION("Write path: Memory -> Disk + Cluster") {
        // 1. Write to memory cache
        // 2. Async write-through to disk
        // 3. Async replicate to cluster peers
    }
    
    SECTION("Eviction: Memory evicts to Disk") {
        // When memory is full:
        // 1. Evict cold items from memory
        // 2. Write evicted items to disk cache
        // 3. Disk eviction is independent (LRU based)
    }
}

// Note: Full async tests require an Elio runtime.
// These would test actual get/put/remove operations.

/*
 * Integration tests that would run with Elio:
 *
 * TEST_CASE("CacheCoordinator async operations", "[coordinator][integration]") {
 *     SECTION("Put and get - memory hit") {
 *         // Store small value, retrieve from memory
 *     }
 *     
 *     SECTION("Put and get - disk hit") {
 *         // Store value, evict from memory, retrieve from disk
 *     }
 *     
 *     SECTION("Partial read across chunks") {
 *         // Store 20MB value, read 1MB from middle
 *     }
 *     
 *     SECTION("Write stream for large value") {
 *         // Use WriteStream to store 100MB value in chunks
 *     }
 *     
 *     SECTION("Read stream for large value") {
 *         // Use ReadStream to read 100MB value progressively
 *     }
 *     
 *     SECTION("Concurrent operations") {
 *         // Multiple coroutines reading/writing different keys
 *     }
 *     
 *     SECTION("Cache promotion") {
 *         // Read from disk, verify promoted to memory
 *     }
 * }
 */
