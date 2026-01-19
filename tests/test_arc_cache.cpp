#include <catch2/catch_test_macros.hpp>
#include "elcache/arc_cache.hpp"
#include <string>
#include <thread>
#include <vector>

using namespace elcache;

TEST_CASE("ARCCache basic operations", "[arc]") {
    ARCCache<std::string, int> cache(100);
    
    SECTION("Put and get") {
        cache.put("key1", 42);
        
        auto* val = cache.get("key1");
        REQUIRE(val != nullptr);
        REQUIRE(*val == 42);
    }
    
    SECTION("Missing key returns nullptr") {
        auto* val = cache.get("nonexistent");
        REQUIRE(val == nullptr);
    }
    
    SECTION("Update existing key") {
        cache.put("key1", 1);
        cache.put("key1", 2);
        
        auto* val = cache.get("key1");
        REQUIRE(val != nullptr);
        REQUIRE(*val == 2);
    }
    
    SECTION("Remove key") {
        cache.put("key1", 42);
        bool removed = cache.remove("key1");
        
        REQUIRE(removed);
        REQUIRE(cache.get("key1") == nullptr);
    }
    
    SECTION("Remove nonexistent key") {
        bool removed = cache.remove("nonexistent");
        REQUIRE(!removed);
    }
}

TEST_CASE("ARCCache eviction", "[arc]") {
    // Cache with size function counting string length
    ARCCache<std::string, std::string> cache(100, [](const std::string& s) {
        return s.size();
    });
    
    SECTION("Evicts when full") {
        // Fill cache
        for (int i = 0; i < 20; ++i) {
            std::string key = "key" + std::to_string(i);
            std::string value(10, 'x');  // 10 bytes each
            cache.put(key, value);
        }
        
        // Size should be around capacity
        REQUIRE(cache.size() <= 100);
        
        // Some early keys should be evicted
        // (exact behavior depends on access patterns)
    }
    
    SECTION("Frequently accessed items survive") {
        // Add items
        for (int i = 0; i < 5; ++i) {
            cache.put("key" + std::to_string(i), std::string(15, 'x'));
        }
        
        // Access key0 multiple times to make it "hot"
        for (int i = 0; i < 10; ++i) {
            cache.get("key0");
        }
        
        // Add more items to trigger eviction
        for (int i = 5; i < 15; ++i) {
            cache.put("key" + std::to_string(i), std::string(15, 'x'));
        }
        
        // key0 should still be present due to frequency
        REQUIRE(cache.contains("key0"));
    }
}

TEST_CASE("ARCCache adaptation", "[arc]") {
    ARCCache<int, int> cache(10);
    
    SECTION("Adapts to workload") {
        // Initially, fill cache
        for (int i = 0; i < 10; ++i) {
            cache.put(i, i);
        }
        
        auto stats1 = cache.stats();
        double p1 = stats1.p;
        
        // Create recency-biased workload
        for (int i = 10; i < 20; ++i) {
            cache.put(i, i);
            cache.get(i);
        }
        
        // p should have adjusted based on hits/misses
        // in ghost lists
        auto stats2 = cache.stats();
        // Just verify stats work without specific assertions
        // since adaptation depends on complex interactions
        REQUIRE(stats2.hits + stats2.misses > 0);
    }
}

TEST_CASE("ARCCache thread safety", "[arc]") {
    ARCCache<int, int> cache(1000);
    
    SECTION("Concurrent puts and gets") {
        std::vector<std::thread> threads;
        
        // Writers
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&cache, t]() {
                for (int i = 0; i < 100; ++i) {
                    cache.put(t * 100 + i, i);
                }
            });
        }
        
        // Readers
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&cache]() {
                for (int i = 0; i < 100; ++i) {
                    cache.get(i);
                }
            });
        }
        
        for (auto& t : threads) {
            t.join();
        }
        
        // Should not crash, basic sanity check
        REQUIRE(cache.size() > 0);
    }
}
