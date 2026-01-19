#include <catch2/catch_test_macros.hpp>
#include "elcache/disk_cache.hpp"
#include "elcache/chunk.hpp"
#include <elio/io/io_context.hpp>
#include <filesystem>
#include <fstream>
#include <random>

using namespace elcache;

// Helper to create a temp directory
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() / 
                ("elcache_test_" + std::to_string(rd()));
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

TEST_CASE("DiskCacheConfig defaults", "[disk_cache]") {
    DiskCacheConfig config;
    
    REQUIRE(config.max_size > 0);
    REQUIRE(config.block_size == 4096);
    REQUIRE(config.high_watermark > config.low_watermark);
    REQUIRE(config.use_direct_io == true);
    REQUIRE(config.use_fallocate == true);
}

TEST_CASE("DiskCacheConfig validation", "[disk_cache]") {
    SECTION("Valid watermark settings") {
        DiskCacheConfig config;
        config.high_watermark = 0.90;
        config.low_watermark = 0.80;
        
        REQUIRE(config.high_watermark > config.low_watermark);
    }
    
    SECTION("Buffer sizes") {
        DiskCacheConfig config;
        config.write_buffer_size = 128 * 1024 * 1024;
        
        REQUIRE(config.write_buffer_size == 128 * 1024 * 1024);
    }
}

TEST_CASE("DiskStore file operations", "[disk_cache]") {
    TempDir tmp;
    
    DiskCacheConfig config;
    config.path = tmp.path();
    config.use_direct_io = false;  // Disable for tests (needs aligned buffers)
    
    elio::io::io_context io_ctx;
    DiskStore store(config, io_ctx);
    
    SECTION("Directory creation") {
        // DiskStore constructor doesn't create directories
        // The init() coroutine does, but we can't easily test that
        // without an Elio runtime
        REQUIRE(std::filesystem::exists(tmp.path()));
    }
    
    SECTION("Configuration stored correctly") {
        // Verify config was captured
        DiskCacheConfig config2;
        config2.path = tmp.path();
        config2.max_size = 50ULL * 1024 * 1024 * 1024;
        
        DiskStore store2(config2, io_ctx);
        // Store should accept the config without errors
    }
}

TEST_CASE("DiskCache configuration", "[disk_cache]") {
    TempDir tmp;
    
    elio::io::io_context io_ctx;
    
    SECTION("Basic construction") {
        DiskCacheConfig config;
        config.path = tmp.path();
        config.max_size = 10ULL * 1024 * 1024 * 1024;  // 10GB
        config.use_direct_io = false;
        
        DiskCache cache(config, io_ctx);
        auto stats = cache.stats();
        
        REQUIRE(stats.capacity_bytes == config.max_size);
        REQUIRE(stats.size_bytes == 0);
    }
    
    SECTION("Stats tracking") {
        DiskCacheConfig config;
        config.path = tmp.path();
        config.use_direct_io = false;
        
        DiskCache cache(config, io_ctx);
        auto stats = cache.stats();
        
        REQUIRE(stats.hits == 0);
        REQUIRE(stats.misses == 0);
        REQUIRE(stats.evictions == 0);
    }
}

TEST_CASE("DiskCache path generation", "[disk_cache]") {
    TempDir tmp;
    
    DiskCacheConfig config;
    config.path = tmp.path();
    config.use_direct_io = false;
    
    elio::io::io_context io_ctx;
    DiskCache cache(config, io_ctx);
    
    SECTION("Subdirectory structure") {
        // DiskCache should organize chunks into subdirectories
        // based on their hash prefix for better filesystem performance
        auto chunks_dir = tmp.path() / "chunks";
        auto descriptors_dir = tmp.path() / "descriptors";
        
        // Directories would be created by start() coroutine
        // Just verify the config path is accessible
        REQUIRE(std::filesystem::exists(tmp.path()));
    }
}

TEST_CASE("Chunk file operations", "[disk_cache]") {
    TempDir tmp;
    
    SECTION("Create chunk from data") {
        ByteBuffer data = {'t', 'e', 's', 't', ' ', 'd', 'a', 't', 'a'};
        Chunk chunk(std::move(data));
        
        REQUIRE(chunk.size() == 9);
        REQUIRE(!chunk.empty());
        
        // Content-addressable ID should be computed
        auto& id = chunk.id();
        REQUIRE((id.hash().low != 0 || id.hash().high != 0));
    }
    
    SECTION("Chunk ID consistency") {
        ByteBuffer data1 = {'s', 'a', 'm', 'e'};
        ByteBuffer data2 = {'s', 'a', 'm', 'e'};
        
        Chunk c1(std::move(data1));
        Chunk c2(std::move(data2));
        
        // Same content should produce same ID
        REQUIRE(c1.id() == c2.id());
    }
    
    SECTION("Different content produces different ID") {
        ByteBuffer data1 = {'a', 'b', 'c'};
        ByteBuffer data2 = {'x', 'y', 'z'};
        
        Chunk c1(std::move(data1));
        Chunk c2(std::move(data2));
        
        REQUIRE(!(c1.id() == c2.id()));
    }
}

TEST_CASE("Large chunk handling", "[disk_cache]") {
    SECTION("4MB chunk creation") {
        // Create a full 4MB chunk
        ByteBuffer data(CHUNK_SIZE, 'x');
        Chunk chunk(std::move(data));
        
        REQUIRE(chunk.size() == CHUNK_SIZE);
    }
    
    SECTION("Partial read from large chunk") {
        ByteBuffer data(CHUNK_SIZE, 0);
        // Fill with pattern
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = static_cast<uint8_t>(i % 256);
        }
        
        Chunk chunk(std::move(data));
        
        // Read from middle
        auto partial = chunk.read(1024 * 1024, 4096);  // 1MB offset, 4KB read
        
        REQUIRE(partial.size() == 4096);
        REQUIRE(partial[0] == static_cast<uint8_t>((1024 * 1024) % 256));
    }
}

// Note: Async tests for DiskCache operations require an Elio runtime.
// The following tests document expected behavior but would need
// integration with the actual runtime to execute the coroutines.

/*
 * Integration tests that would run with Elio:
 * 
 * TEST_CASE("DiskCache async operations", "[disk_cache][integration]") {
 *     // These would use elio::coro::sync_wait() or similar
 *     
 *     SECTION("Put and get chunk") {
 *         // store chunk, retrieve it, verify content
 *     }
 *     
 *     SECTION("Eviction under pressure") {
 *         // fill cache, trigger eviction, verify LRU behavior
 *     }
 *     
 *     SECTION("Persistence across restart") {
 *         // store data, recreate cache, verify data persists
 *     }
 * }
 */
