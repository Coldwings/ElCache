#include <catch2/catch_test_macros.hpp>
#include "elcache/types.hpp"

using namespace elcache;

TEST_CASE("CacheKey basic operations", "[types]") {
    SECTION("Empty key") {
        CacheKey key;
        REQUIRE(key.empty());
        REQUIRE(key.size() == 0);
    }
    
    SECTION("String key") {
        CacheKey key("test-key");
        REQUIRE(!key.empty());
        REQUIRE(key.size() == 8);
        REQUIRE(key.view() == "test-key");
    }
    
    SECTION("Key equality") {
        CacheKey key1("test");
        CacheKey key2("test");
        CacheKey key3("other");
        
        REQUIRE(key1 == key2);
        REQUIRE(!(key1 == key3));
    }
    
    SECTION("Key hashing") {
        CacheKey key1("test");
        CacheKey key2("test");
        
        REQUIRE(key1.hash() == key2.hash());
        REQUIRE(key1.hash() != 0);
    }
}

TEST_CASE("Hash128 operations", "[types]") {
    SECTION("Zero hash") {
        Hash128 h;
        REQUIRE(h.is_zero());
        REQUIRE(h.low == 0);
        REQUIRE(h.high == 0);
    }
    
    SECTION("Hex conversion") {
        Hash128 h;
        h.low = 0x123456789ABCDEF0ULL;
        h.high = 0xFEDCBA9876543210ULL;
        
        std::string hex = h.to_hex();
        REQUIRE(hex.size() == 32);
        
        Hash128 h2 = Hash128::from_hex(hex);
        REQUIRE(h2.low == h.low);
        REQUIRE(h2.high == h.high);
    }
}

TEST_CASE("Range operations", "[types]") {
    SECTION("Basic range") {
        Range r{100, 50};
        REQUIRE(r.offset == 100);
        REQUIRE(r.length == 50);
        REQUIRE(r.end() == 150);
    }
    
    SECTION("Range overlap") {
        Range r1{0, 100};
        Range r2{50, 100};
        Range r3{200, 50};
        
        REQUIRE(r1.overlaps(r2));
        REQUIRE(r2.overlaps(r1));
        REQUIRE(!r1.overlaps(r3));
    }
    
    SECTION("Range intersection") {
        Range r1{0, 100};
        Range r2{50, 100};
        
        Range inter = r1.intersect(r2);
        REQUIRE(inter.offset == 50);
        REQUIRE(inter.length == 50);
    }
    
    SECTION("Range contains") {
        Range r1{0, 100};
        Range r2{25, 50};
        Range r3{50, 100};
        
        REQUIRE(r1.contains(r2));
        REQUIRE(!r1.contains(r3));
    }
}

TEST_CASE("ChunkRange computation", "[types]") {
    SECTION("Single chunk") {
        Range r{0, 1000};
        auto chunks = ChunkRange::compute(r, 4 * 1024 * 1024);
        
        REQUIRE(chunks.size() == 1);
        REQUIRE(chunks[0].chunk_index == 0);
        REQUIRE(chunks[0].offset_in_chunk == 0);
        REQUIRE(chunks[0].length == 1000);
    }
    
    SECTION("Multiple chunks") {
        Range r{0, 10 * 1024 * 1024};  // 10MB
        auto chunks = ChunkRange::compute(r, 4 * 1024 * 1024);
        
        REQUIRE(chunks.size() == 3);
        REQUIRE(chunks[0].chunk_index == 0);
        REQUIRE(chunks[1].chunk_index == 1);
        REQUIRE(chunks[2].chunk_index == 2);
    }
    
    SECTION("Mid-chunk start") {
        Range r{1024 * 1024, 2 * 1024 * 1024};  // Start at 1MB, read 2MB
        auto chunks = ChunkRange::compute(r, 4 * 1024 * 1024);
        
        REQUIRE(chunks.size() == 1);
        REQUIRE(chunks[0].chunk_index == 0);
        REQUIRE(chunks[0].offset_in_chunk == 1024 * 1024);
    }
}

TEST_CASE("NodeId operations", "[types]") {
    SECTION("Generate unique IDs") {
        auto id1 = NodeId::generate();
        auto id2 = NodeId::generate();
        
        REQUIRE(id1.is_valid());
        REQUIRE(id2.is_valid());
        REQUIRE(!(id1 == id2));
    }
    
    SECTION("From address") {
        auto id1 = NodeId::from_address("192.168.1.1", 8080);
        auto id2 = NodeId::from_address("192.168.1.1", 8080);
        auto id3 = NodeId::from_address("192.168.1.2", 8080);
        
        REQUIRE(id1 == id2);
        REQUIRE(!(id1 == id3));
    }
}

TEST_CASE("Status operations", "[types]") {
    SECTION("Ok status") {
        auto s = Status::make_ok();
        REQUIRE(s.is_ok());
        REQUIRE(!s.is_error());
        REQUIRE(s);
    }
    
    SECTION("Error status") {
        auto s = Status::error(ErrorCode::NotFound, "Key not found");
        REQUIRE(!s.is_ok());
        REQUIRE(s.is_error());
        REQUIRE(s.code() == ErrorCode::NotFound);
        REQUIRE(s.message() == "Key not found");
    }
}
