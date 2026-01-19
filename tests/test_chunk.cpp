#include <catch2/catch_test_macros.hpp>
#include "elcache/chunk.hpp"

using namespace elcache;

TEST_CASE("Chunk basic operations", "[chunk]") {
    SECTION("Create from buffer") {
        ByteBuffer data = {'h', 'e', 'l', 'l', 'o'};
        Chunk chunk(std::move(data));
        
        REQUIRE(chunk.size() == 5);
        REQUIRE(!chunk.empty());
    }
    
    SECTION("Content-addressed ID") {
        ByteBuffer data1 = {'t', 'e', 's', 't'};
        ByteBuffer data2 = {'t', 'e', 's', 't'};
        ByteBuffer data3 = {'o', 't', 'h', 'e', 'r'};
        
        Chunk c1(std::move(data1));
        Chunk c2(std::move(data2));
        Chunk c3(std::move(data3));
        
        // Same content should produce same ID
        REQUIRE(c1.id() == c2.id());
        // Different content should produce different ID
        REQUIRE(!(c1.id() == c3.id()));
    }
    
    SECTION("Partial read") {
        ByteBuffer data = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
        Chunk chunk(std::move(data));
        
        auto part = chunk.read(2, 5);
        REQUIRE(part.size() == 5);
        REQUIRE(part[0] == '2');
        REQUIRE(part[4] == '6');
    }
    
    SECTION("Read beyond bounds") {
        ByteBuffer data = {'a', 'b', 'c'};
        Chunk chunk(std::move(data));
        
        auto part = chunk.read(1, 10);  // Request more than available
        REQUIRE(part.size() == 2);  // Should only get what's available
    }
}

TEST_CASE("ValueDescriptor operations", "[chunk]") {
    SECTION("Chunks for range") {
        ValueDescriptor desc;
        desc.total_size = 10 * 1024 * 1024;  // 10MB
        desc.chunks.resize(3);  // 3 chunks of 4MB each
        
        // Full range
        auto indices = desc.chunks_for_range({0, desc.total_size});
        REQUIRE(indices.size() == 3);
        
        // Partial range in first chunk
        indices = desc.chunks_for_range({0, 1000});
        REQUIRE(indices.size() == 1);
        REQUIRE(indices[0] == 0);
        
        // Range spanning two chunks
        indices = desc.chunks_for_range({3 * 1024 * 1024, 2 * 1024 * 1024});
        REQUIRE(indices.size() == 2);
    }
    
    SECTION("Locate chunk") {
        ValueDescriptor desc;
        desc.total_size = 10 * 1024 * 1024;
        
        auto [idx, offset] = desc.locate_chunk(0);
        REQUIRE(idx == 0);
        REQUIRE(offset == 0);
        
        std::tie(idx, offset) = desc.locate_chunk(5 * 1024 * 1024);
        REQUIRE(idx == 1);
        REQUIRE(offset == 1 * 1024 * 1024);
    }
}

TEST_CASE("PartialValue operations", "[chunk]") {
    SECTION("Track chunk availability") {
        ValueDescriptor desc;
        desc.total_size = 12 * 1024 * 1024;  // 12MB = 3 chunks
        desc.chunks.resize(3);
        
        PartialValue pv(desc);
        
        REQUIRE(!pv.has_chunk(0));
        REQUIRE(!pv.has_chunk(1));
        REQUIRE(!pv.has_chunk(2));
        
        pv.mark_chunk_available(0, ChunkId());
        pv.mark_chunk_available(2, ChunkId());
        
        REQUIRE(pv.has_chunk(0));
        REQUIRE(!pv.has_chunk(1));
        REQUIRE(pv.has_chunk(2));
    }
    
    SECTION("Check range availability") {
        ValueDescriptor desc;
        desc.total_size = 12 * 1024 * 1024;
        desc.chunks.resize(3);
        
        PartialValue pv(desc);
        pv.mark_chunk_available(0, ChunkId());
        
        // First chunk is available
        REQUIRE(pv.has_range({0, 4 * 1024 * 1024}));
        // Second chunk is not
        REQUIRE(!pv.has_range({4 * 1024 * 1024, 4 * 1024 * 1024}));
        // Range spanning available and unavailable
        REQUIRE(!pv.has_range({0, 8 * 1024 * 1024}));
    }
    
    SECTION("Completion ratio") {
        ValueDescriptor desc;
        desc.total_size = 12 * 1024 * 1024;
        desc.chunks.resize(3);
        
        PartialValue pv(desc);
        REQUIRE(pv.completion_ratio() == 0.0);
        
        pv.mark_chunk_available(0, ChunkId());
        REQUIRE(pv.completion_ratio() > 0.3);
        REQUIRE(pv.completion_ratio() < 0.4);
        
        pv.mark_chunk_available(1, ChunkId());
        pv.mark_chunk_available(2, ChunkId());
        REQUIRE(pv.completion_ratio() == 1.0);
    }
}
