#include <catch2/catch_test_macros.hpp>
#include "elcache/metrics.hpp"
#include <thread>
#include <vector>

using namespace elcache;

TEST_CASE("Counter operations", "[metrics]") {
    Counter counter;
    
    SECTION("Initial value is zero") {
        REQUIRE(counter.get() == 0);
    }
    
    SECTION("Increment by 1") {
        counter.inc();
        REQUIRE(counter.get() == 1);
        counter.inc();
        REQUIRE(counter.get() == 2);
    }
    
    SECTION("Increment by N") {
        counter.inc(10);
        REQUIRE(counter.get() == 10);
        counter.inc(5);
        REQUIRE(counter.get() == 15);
    }
    
    SECTION("Reset") {
        counter.inc(100);
        counter.reset();
        REQUIRE(counter.get() == 0);
    }
    
    SECTION("Thread safety") {
        std::vector<std::thread> threads;
        for (int i = 0; i < 10; ++i) {
            threads.emplace_back([&counter]() {
                for (int j = 0; j < 1000; ++j) {
                    counter.inc();
                }
            });
        }
        for (auto& t : threads) {
            t.join();
        }
        REQUIRE(counter.get() == 10000);
    }
}

TEST_CASE("Gauge operations", "[metrics]") {
    Gauge gauge;
    
    SECTION("Initial value is zero") {
        REQUIRE(gauge.get() == 0.0);
    }
    
    SECTION("Set value") {
        gauge.set(42.5);
        REQUIRE(gauge.get() == 42.5);
    }
    
    SECTION("Increment") {
        gauge.set(10.0);
        gauge.inc(5.0);
        REQUIRE(gauge.get() == 15.0);
    }
    
    SECTION("Decrement") {
        gauge.set(10.0);
        gauge.dec(3.0);
        REQUIRE(gauge.get() == 7.0);
    }
}

TEST_CASE("LatencyHistogram operations", "[metrics]") {
    LatencyHistogram histogram;
    
    SECTION("Initial state") {
        auto snap = histogram.snapshot();
        REQUIRE(snap.count == 0);
        REQUIRE(snap.sum == 0.0);
    }
    
    SECTION("Observe values") {
        histogram.observe(0.05);  // 0.05ms
        histogram.observe(0.5);   // 0.5ms
        histogram.observe(5.0);   // 5ms
        histogram.observe(50.0);  // 50ms
        
        auto snap = histogram.snapshot();
        REQUIRE(snap.count == 4);
        REQUIRE(snap.sum > 55.0);
        REQUIRE(snap.sum < 56.0);
    }
    
    SECTION("Buckets are cumulative") {
        histogram.observe(0.05);
        histogram.observe(0.5);
        histogram.observe(5.0);
        
        auto snap = histogram.snapshot();
        
        // Each bucket should contain count of values <= bound
        // Find bucket for 1ms
        bool found_1ms_bucket = false;
        for (const auto& [bound, count] : snap.buckets) {
            if (bound == 1.0) {
                // Should have 2 values (0.05 and 0.5)
                REQUIRE(count == 2);
                found_1ms_bucket = true;
            }
        }
        REQUIRE(found_1ms_bucket);
    }
    
    SECTION("Reset") {
        histogram.observe(1.0);
        histogram.observe(2.0);
        histogram.reset();
        
        auto snap = histogram.snapshot();
        REQUIRE(snap.count == 0);
        REQUIRE(snap.sum == 0.0);
    }
}

TEST_CASE("LatencyTimer RAII", "[metrics]") {
    LatencyHistogram histogram;
    
    SECTION("Automatic observation on destruction") {
        {
            LatencyTimer timer(histogram);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        auto snap = histogram.snapshot();
        REQUIRE(snap.count == 1);
        REQUIRE(snap.sum >= 10.0);  // At least 10ms
    }
}

TEST_CASE("Global metrics instance", "[metrics]") {
    auto& m = metrics();
    
    SECTION("Can access global metrics") {
        uint64_t before = m.cache_hits_total.get();
        m.cache_hits_total.inc();
        REQUIRE(m.cache_hits_total.get() == before + 1);
    }
    
    SECTION("Uptime increases") {
        double t1 = m.uptime_seconds();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        double t2 = m.uptime_seconds();
        REQUIRE(t2 > t1);
    }
}

TEST_CASE("MetricsCollector export", "[metrics]") {
    MetricsCollector collector;
    
    SECTION("Prometheus format") {
        std::string output = collector.export_prometheus();
        
        // Check for expected metric names
        REQUIRE(output.find("elcache_cache_hits_total") != std::string::npos);
        REQUIRE(output.find("elcache_cache_misses_total") != std::string::npos);
        REQUIRE(output.find("elcache_uptime_seconds") != std::string::npos);
        
        // Check for Prometheus format elements
        REQUIRE(output.find("# HELP") != std::string::npos);
        REQUIRE(output.find("# TYPE") != std::string::npos);
    }
    
    SECTION("JSON format") {
        std::string output = collector.export_json();
        
        // Check for JSON structure
        REQUIRE(output.find("{") != std::string::npos);
        REQUIRE(output.find("}") != std::string::npos);
        REQUIRE(output.find("\"cache\"") != std::string::npos);
        REQUIRE(output.find("\"hits\"") != std::string::npos);
    }
}
