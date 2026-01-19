/*
 * ElCache Basic Usage Example
 * 
 * This example demonstrates:
 * - Creating a cache coordinator
 * - Storing and retrieving values
 * - Partial reads for large values
 */

#include "elcache/elcache.hpp"
#include <elio/io/io_context.hpp>
#include <iostream>
#include <string>

// Note: This example shows the API structure
// Full functionality requires the Elio runtime

int main() {
    using namespace elcache;
    
    std::cout << "ElCache Basic Usage Example\n";
    std::cout << "===========================\n\n";
    
    // Create configuration
    Config config;
    config.memory.max_size = 100 * 1024 * 1024;  // 100MB memory cache
    config.disk.max_size = 0;  // Disable disk cache for this example
    
    // Validate configuration
    auto status = config.validate();
    if (!status) {
        std::cerr << "Invalid config: " << status.message() << "\n";
        return 1;
    }
    
    // Create io_context for async I/O operations
    elio::io::io_context io_ctx;
    
    // Create cache coordinator
    CacheCoordinator cache(config, io_ctx);
    
    std::cout << "Configuration:\n";
    std::cout << "  Memory cache: " << (config.memory.max_size / (1024 * 1024)) << " MB\n";
    std::cout << "  Chunk size: " << (CHUNK_SIZE / (1024 * 1024)) << " MB\n";
    std::cout << "  Max key size: " << MAX_KEY_SIZE << " bytes\n";
    std::cout << "  Max value size: " << (MAX_VALUE_SIZE / (1024ULL * 1024 * 1024 * 1024)) << " TB\n";
    std::cout << "\n";
    
    // Example: Create a cache key
    CacheKey key("example/data/file1.dat");
    std::cout << "Key: " << key.view() << "\n";
    std::cout << "Key hash: " << std::hex << key.hash() << std::dec << "\n\n";
    
    // Example: Compute chunk ranges for a partial read
    Range read_range{1024, 8192};  // Read 8KB starting at offset 1KB
    auto chunk_ranges = ChunkRange::compute(read_range, CHUNK_SIZE);
    
    std::cout << "Chunk ranges for reading " << read_range.length << " bytes at offset " 
              << read_range.offset << ":\n";
    for (const auto& cr : chunk_ranges) {
        std::cout << "  Chunk " << cr.chunk_index << ": offset=" << cr.offset_in_chunk 
                  << ", length=" << cr.length << "\n";
    }
    std::cout << "\n";
    
    // Example: Value descriptor for a large value
    ValueDescriptor desc;
    desc.key = key;
    desc.total_size = 100 * 1024 * 1024;  // 100MB value
    desc.metadata.total_size = desc.total_size;
    desc.metadata.chunk_count = (desc.total_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    desc.metadata.created_at = SystemClock::now();
    
    std::cout << "Value descriptor:\n";
    std::cout << "  Total size: " << (desc.total_size / (1024 * 1024)) << " MB\n";
    std::cout << "  Chunk count: " << desc.metadata.chunk_count << "\n";
    std::cout << "\n";
    
    // Example: Partial value tracking
    desc.chunks.resize(desc.metadata.chunk_count);
    PartialValue partial(desc);
    
    // Simulate some chunks being available
    partial.mark_chunk_available(0, ChunkId());
    partial.mark_chunk_available(1, ChunkId());
    partial.mark_chunk_available(5, ChunkId());
    
    std::cout << "Partial value state:\n";
    std::cout << "  Completion: " << (partial.completion_ratio() * 100) << "%\n";
    std::cout << "  Available bytes: " << (partial.available_bytes() / (1024 * 1024)) << " MB\n";
    
    auto available = partial.available_ranges();
    std::cout << "  Available ranges:\n";
    for (const auto& r : available) {
        std::cout << "    " << r.offset << " - " << r.end() << " (" 
                  << (r.length / (1024 * 1024)) << " MB)\n";
    }
    std::cout << "\n";
    
    // Example: Check what's missing for a specific read
    Range query{0, 30 * 1024 * 1024};  // Want to read first 30MB
    auto missing = partial.missing_ranges(query);
    
    std::cout << "To read " << (query.length / (1024 * 1024)) << " MB, missing:\n";
    for (const auto& r : missing) {
        std::cout << "  " << (r.offset / (1024 * 1024)) << " MB - " 
                  << (r.end() / (1024 * 1024)) << " MB\n";
    }
    std::cout << "\n";
    
    // Note: Actual async operations would require Elio runtime
    // Example usage with Elio would be:
    /*
    elio::runtime::scheduler sched(4);
    
    sched.spawn([&cache]() -> elio::coro::task<void> {
        // Store a value
        std::string value = "Hello, ElCache!";
        auto status = co_await cache.put(
            CacheKey("greeting"),
            ByteView(reinterpret_cast<const uint8_t*>(value.data()), value.size())
        );
        
        if (!status) {
            std::cerr << "Put failed: " << status.message() << "\n";
            co_return;
        }
        
        // Retrieve it
        auto result = co_await cache.get(CacheKey("greeting"));
        if (result.is_hit()) {
            std::string retrieved(result.data.begin(), result.data.end());
            std::cout << "Got: " << retrieved << "\n";
        }
        
        // Partial read example
        ReadOptions opts;
        opts.range = Range{0, 5};  // Just first 5 bytes
        result = co_await cache.get(CacheKey("greeting"), opts);
        if (result.is_hit()) {
            std::string partial(result.data.begin(), result.data.end());
            std::cout << "Partial read: " << partial << "\n";
        }
    }.handle());
    
    sched.start();
    sched.shutdown();
    */
    
    std::cout << "Example complete!\n";
    std::cout << "\nNote: Full async operations require running with Elio runtime.\n";
    
    return 0;
}
