#include <catch2/catch_test_macros.hpp>
#include "elcache/memory_cache.hpp"
#include "elcache/chunk.hpp"
#include <thread>
#include <vector>

using namespace elcache;

TEST_CASE("MemoryCache configuration", "[memory_cache]") {
    SECTION("Default configuration") {
        MemoryCacheConfig config;
        
        REQUIRE(config.max_size > 0);
        REQUIRE(config.max_size == 1ULL * 1024 * 1024 * 1024);  // 1GB default
        REQUIRE(config.high_watermark > config.low_watermark);
        REQUIRE(config.high_watermark == 0.95);
        REQUIRE(config.low_watermark == 0.85);
    }
    
    SECTION("Custom configuration") {
        MemoryCacheConfig config;
        config.max_size = 512 * 1024 * 1024;  // 512MB
        
        MemoryCache cache(config);
        auto stats = cache.stats();
        
        REQUIRE(stats.capacity_bytes == config.max_size);
    }
    
    SECTION("ARC ghost size auto-config") {
        MemoryCacheConfig config;
        config.max_size = 256 * 1024 * 1024;
        config.arc_ghost_size = 0;  // Auto
        
        // 0 means auto (same as max_size in ARC terms)
        MemoryCache cache(config);
        auto stats = cache.stats();
        
        REQUIRE(stats.capacity_bytes == config.max_size);
    }
    
    SECTION("Custom watermarks") {
        MemoryCacheConfig config;
        config.high_watermark = 0.90;
        config.low_watermark = 0.70;
        
        REQUIRE(config.high_watermark == 0.90);
        REQUIRE(config.low_watermark == 0.70);
    }
}

TEST_CASE("MemoryCache stats", "[memory_cache]") {
    MemoryCacheConfig config;
    config.max_size = 64 * 1024 * 1024;  // 64MB for faster tests
    
    MemoryCache cache(config);
    
    SECTION("Initial stats") {
        auto stats = cache.stats();
        
        REQUIRE(stats.hits == 0);
        REQUIRE(stats.misses == 0);
        REQUIRE(stats.evictions == 0);
        REQUIRE(stats.size_bytes == 0);
        REQUIRE(stats.entry_count == 0);
    }
    
    SECTION("Stats structure") {
        auto stats = cache.stats();
        
        // Verify all fields are accessible
        REQUIRE(stats.capacity_bytes == 64 * 1024 * 1024);
    }
}

TEST_CASE("Chunk operations in memory", "[memory_cache]") {
    SECTION("Create and verify chunk") {
        ByteBuffer data = {'h', 'e', 'l', 'l', 'o'};
        auto chunk = std::make_shared<Chunk>(std::move(data));
        
        REQUIRE(chunk->size() == 5);
        REQUIRE(!chunk->empty());
        
        // ID should be computed on first access
        auto& id = chunk->id();
        REQUIRE(id.to_string().size() > 0);
    }
    
    SECTION("SharedChunk sharing") {
        ByteBuffer data = {'t', 'e', 's', 't'};
        auto chunk1 = std::make_shared<Chunk>(std::move(data));
        auto chunk2 = chunk1;  // Shared ownership
        
        REQUIRE(chunk1.use_count() == 2);
        REQUIRE(chunk1->size() == chunk2->size());
        REQUIRE(chunk1->id() == chunk2->id());
    }
    
    SECTION("Large chunk handling") {
        // Create a full 4MB chunk
        ByteBuffer data(CHUNK_SIZE, 'A');
        auto chunk = std::make_shared<Chunk>(std::move(data));
        
        REQUIRE(chunk->size() == CHUNK_SIZE);
        
        // Partial read
        auto partial = chunk->read(0, 1024);
        REQUIRE(partial.size() == 1024);
        REQUIRE(partial[0] == 'A');
    }
}

TEST_CASE("ValueDescriptor for multi-chunk values", "[memory_cache]") {
    SECTION("Create descriptor") {
        ValueDescriptor desc;
        desc.key = CacheKey("test-key");
        desc.total_size = 20 * 1024 * 1024;  // 20MB = 5 chunks
        desc.chunks.resize(5);
        
        REQUIRE(desc.is_complete() == true);  // All slots filled
        REQUIRE(desc.total_size == 20 * 1024 * 1024);
    }
    
    SECTION("Locate chunk for offset") {
        ValueDescriptor desc;
        desc.total_size = 12 * 1024 * 1024;  // 12MB = 3 chunks
        
        auto [idx, offset] = desc.locate_chunk(0);
        REQUIRE(idx == 0);
        REQUIRE(offset == 0);
        
        std::tie(idx, offset) = desc.locate_chunk(CHUNK_SIZE);
        REQUIRE(idx == 1);
        REQUIRE(offset == 0);
        
        std::tie(idx, offset) = desc.locate_chunk(CHUNK_SIZE + 1000);
        REQUIRE(idx == 1);
        REQUIRE(offset == 1000);
    }
    
    SECTION("Chunks for range") {
        ValueDescriptor desc;
        desc.total_size = 16 * 1024 * 1024;  // 16MB = 4 chunks
        desc.chunks.resize(4);
        
        // First chunk only
        auto indices = desc.chunks_for_range({0, CHUNK_SIZE});
        REQUIRE(indices.size() == 1);
        REQUIRE(indices[0] == 0);
        
        // Spanning two chunks
        indices = desc.chunks_for_range({CHUNK_SIZE - 100, 200});
        REQUIRE(indices.size() == 2);
        
        // All chunks
        indices = desc.chunks_for_range({0, desc.total_size});
        REQUIRE(indices.size() == 4);
    }
}

TEST_CASE("PartialValue tracking", "[memory_cache]") {
    ValueDescriptor desc;
    desc.total_size = 12 * 1024 * 1024;  // 3 chunks
    desc.chunks.resize(3);
    
    PartialValue pv(desc);
    
    SECTION("Initial state - nothing available") {
        REQUIRE(pv.completion_ratio() == 0.0);
        REQUIRE(pv.available_bytes() == 0);
        REQUIRE(!pv.has_chunk(0));
        REQUIRE(!pv.has_chunk(1));
        REQUIRE(!pv.has_chunk(2));
    }
    
    SECTION("Mark chunks available") {
        ChunkId id1, id2;
        pv.mark_chunk_available(0, id1);
        pv.mark_chunk_available(2, id2);
        
        REQUIRE(pv.has_chunk(0));
        REQUIRE(!pv.has_chunk(1));
        REQUIRE(pv.has_chunk(2));
        
        REQUIRE(pv.completion_ratio() > 0.6);  // 2/3
        REQUIRE(pv.completion_ratio() < 0.7);
    }
    
    SECTION("Range availability check") {
        pv.mark_chunk_available(0, ChunkId());
        
        // First chunk range - available
        REQUIRE(pv.has_range({0, CHUNK_SIZE}));
        
        // Second chunk range - not available
        REQUIRE(!pv.has_range({CHUNK_SIZE, CHUNK_SIZE}));
        
        // Range spanning available and unavailable - not available
        REQUIRE(!pv.has_range({0, 2 * CHUNK_SIZE}));
    }
    
    SECTION("Missing ranges calculation") {
        pv.mark_chunk_available(0, ChunkId());
        // Chunk 1 and 2 are missing
        
        auto missing = pv.missing_ranges({0, desc.total_size});
        
        // Should have ranges for chunks 1 and 2
        REQUIRE(!missing.empty());
        
        // First missing range starts at chunk 1
        REQUIRE(missing[0].offset == CHUNK_SIZE);
    }
    
    SECTION("Available bytes calculation") {
        pv.mark_chunk_available(0, ChunkId());
        
        REQUIRE(pv.available_bytes() == CHUNK_SIZE);
        
        pv.mark_chunk_available(1, ChunkId());
        
        REQUIRE(pv.available_bytes() == 2 * CHUNK_SIZE);
        
        // Last chunk may be partial
        pv.mark_chunk_available(2, ChunkId());
        
        REQUIRE(pv.available_bytes() == desc.total_size);
    }
    
    SECTION("Unmark chunk") {
        pv.mark_chunk_available(0, ChunkId());
        REQUIRE(pv.has_chunk(0));
        
        pv.mark_chunk_unavailable(0);
        REQUIRE(!pv.has_chunk(0));
    }
}

// Note: Full async tests require an Elio runtime.
// The following document expected behavior for integration tests.

/*
 * Integration tests that would run with Elio:
 * 
 * TEST_CASE("MemoryCache async operations", "[memory_cache][integration]") {
 *     SECTION("Put and get value") {
 *         // Store value, retrieve it, verify content
 *     }
 *     
 *     SECTION("Partial read") {
 *         // Store large value, read middle portion
 *     }
 *     
 *     SECTION("ARC eviction behavior") {
 *         // Fill cache, access patterns, verify ARC adaptation
 *     }
 *     
 *     SECTION("Concurrent access") {
 *         // Multiple coroutines reading/writing simultaneously
 *     }
 * }
 */
