#pragma once

#include "types.hpp"
#include "chunk.hpp"
#include <elio/coro/task.hpp>

namespace elcache {

// Result of a cache read operation
struct ReadResult {
    CacheResult result = CacheResult::Miss;
    ByteBuffer data;
    Range actual_range;  // Actual range that was read
    std::optional<CacheMetadata> metadata;
    
    bool is_hit() const noexcept { return result == CacheResult::Hit; }
    bool is_partial() const noexcept { return result == CacheResult::PartialHit; }
    bool is_miss() const noexcept { return result == CacheResult::Miss; }
};

// Result of checking what's available
struct AvailabilityResult {
    bool exists = false;
    std::optional<CacheMetadata> metadata;
    std::vector<Range> available_ranges;
    std::vector<Range> missing_ranges;
    
    bool is_complete() const {
        return exists && missing_ranges.empty();
    }
    
    bool has_range(const Range& range) const;
};

// Write options
struct WriteOptions {
    std::optional<std::chrono::seconds> ttl;  // Time-to-live
    uint32_t flags = 0;
    bool no_memory = false;  // Skip memory cache
    bool no_disk = false;    // Skip disk cache
    bool no_cluster = false; // Don't replicate to cluster
    bool overwrite = true;   // Overwrite if exists
};

// Read options
struct ReadOptions {
    bool allow_partial = true;     // Return partial data if available
    bool prefer_local = true;      // Prefer local cache over network
    bool update_access_time = true; // Update LRU on access
    std::optional<Range> range;     // Partial read range
    std::chrono::milliseconds timeout{30000};
};

// Delete options
struct DeleteOptions {
    bool from_memory = true;
    bool from_disk = true;
    bool from_cluster = true;
};

// Abstract cache interface
class ICache {
public:
    virtual ~ICache() = default;
    
    // Core operations (async)
    virtual elio::coro::task<ReadResult> get(
        const CacheKey& key,
        const ReadOptions& opts = {}) = 0;
    
    virtual elio::coro::task<Status> put(
        const CacheKey& key,
        ByteView value,
        const WriteOptions& opts = {}) = 0;
    
    virtual elio::coro::task<Status> remove(
        const CacheKey& key,
        const DeleteOptions& opts = {}) = 0;
    
    // Availability check (useful for partial caching)
    virtual elio::coro::task<AvailabilityResult> check(
        const CacheKey& key,
        std::optional<Range> range = std::nullopt) = 0;
    
    // Metadata only
    virtual elio::coro::task<std::optional<CacheMetadata>> metadata(
        const CacheKey& key) = 0;
    
    // Statistics
    struct Stats {
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t partial_hits = 0;
        uint64_t bytes_read = 0;
        uint64_t bytes_written = 0;
        uint64_t evictions = 0;
        size_t entry_count = 0;
        size_t size_bytes = 0;
        size_t capacity_bytes = 0;
    };
    
    virtual Stats stats() const = 0;
    
    // Lifecycle
    virtual elio::coro::task<Status> start() = 0;
    virtual elio::coro::task<void> stop() = 0;
};

// Chunk-level cache interface (lower level)
class IChunkCache {
public:
    virtual ~IChunkCache() = default;
    
    // Chunk operations
    virtual elio::coro::task<SharedChunk> get_chunk(const ChunkId& id) = 0;
    virtual elio::coro::task<Status> put_chunk(SharedChunk chunk) = 0;
    virtual elio::coro::task<Status> remove_chunk(const ChunkId& id) = 0;
    virtual elio::coro::task<bool> has_chunk(const ChunkId& id) = 0;
    
    // Value descriptor operations
    virtual elio::coro::task<std::optional<ValueDescriptor>> get_descriptor(
        const CacheKey& key) = 0;
    virtual elio::coro::task<Status> put_descriptor(
        const ValueDescriptor& desc) = 0;
    virtual elio::coro::task<Status> remove_descriptor(const CacheKey& key) = 0;
    
    // Bulk operations
    virtual elio::coro::task<std::vector<SharedChunk>> get_chunks(
        const std::vector<ChunkId>& ids) = 0;
    
    // Eviction
    virtual elio::coro::task<size_t> evict(size_t bytes_needed) = 0;
};

}  // namespace elcache
