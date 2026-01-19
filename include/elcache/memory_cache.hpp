#pragma once

#include "cache.hpp"
#include "arc_cache.hpp"
#include "config.hpp"
#include <elio/sync/primitives.hpp>
#include <elio/runtime/scheduler.hpp>

namespace elcache {

// Memory cache implementation using ARC algorithm
class MemoryCache : public ICache, public IChunkCache {
public:
    explicit MemoryCache(const MemoryCacheConfig& config);
    ~MemoryCache() override;
    
    // ICache interface
    elio::coro::task<ReadResult> get(
        const CacheKey& key,
        const ReadOptions& opts = {}) override;
    
    elio::coro::task<Status> put(
        const CacheKey& key,
        ByteView value,
        const WriteOptions& opts = {}) override;
    
    elio::coro::task<Status> remove(
        const CacheKey& key,
        const DeleteOptions& opts = {}) override;
    
    elio::coro::task<AvailabilityResult> check(
        const CacheKey& key,
        std::optional<Range> range = std::nullopt) override;
    
    elio::coro::task<std::optional<CacheMetadata>> metadata(
        const CacheKey& key) override;
    
    Stats stats() const override;
    
    elio::coro::task<Status> start() override;
    elio::coro::task<void> stop() override;
    
    // IChunkCache interface
    elio::coro::task<SharedChunk> get_chunk(const ChunkId& id) override;
    elio::coro::task<Status> put_chunk(SharedChunk chunk) override;
    elio::coro::task<Status> remove_chunk(const ChunkId& id) override;
    elio::coro::task<bool> has_chunk(const ChunkId& id) override;
    
    elio::coro::task<std::optional<ValueDescriptor>> get_descriptor(
        const CacheKey& key) override;
    elio::coro::task<Status> put_descriptor(
        const ValueDescriptor& desc) override;
    elio::coro::task<Status> remove_descriptor(const CacheKey& key) override;
    
    elio::coro::task<std::vector<SharedChunk>> get_chunks(
        const std::vector<ChunkId>& ids) override;
    
    elio::coro::task<size_t> evict(size_t bytes_needed) override;
    
private:
    MemoryCacheConfig config_;
    
    // Chunk cache with ARC
    ARCCache<ChunkId, SharedChunk> chunk_cache_;
    
    // Value descriptors (small, use simple LRU)
    mutable elio::sync::shared_mutex desc_mutex_;
    std::unordered_map<CacheKey, ValueDescriptor> descriptors_;
    
    // Statistics
    mutable std::atomic<uint64_t> hits_{0};
    mutable std::atomic<uint64_t> misses_{0};
    mutable std::atomic<uint64_t> partial_hits_{0};
    mutable std::atomic<uint64_t> bytes_read_{0};
    mutable std::atomic<uint64_t> bytes_written_{0};
    mutable std::atomic<uint64_t> evictions_{0};
    
    // Internal helpers
    std::vector<SharedChunk> split_value(ByteView value);
    ByteBuffer assemble_value(const ValueDescriptor& desc, 
                               const std::vector<SharedChunk>& chunks,
                               const Range& range);
};

}  // namespace elcache
