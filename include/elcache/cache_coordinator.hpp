#pragma once

#include "cache.hpp"
#include "memory_cache.hpp"
#include "disk_cache.hpp"
#include "config.hpp"
#include <elio/io/io_context.hpp>
#include <memory>

namespace elcache {

// Forward declarations
class Cluster;

// Multi-level cache coordinator
// Orchestrates requests across memory, disk, and cluster caches
class CacheCoordinator : public ICache {
public:
    // Constructor requires io_context for async disk and network I/O
    CacheCoordinator(const Config& config, elio::io::io_context& io_ctx);
    ~CacheCoordinator() override;
    
    // Initialize cluster connection (optional)
    void set_cluster(std::shared_ptr<Cluster> cluster);
    
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
    
    // Direct access to layers (for advanced use)
    MemoryCache* memory_cache() { return memory_.get(); }
    DiskCache* disk_cache() { return disk_.get(); }
    Cluster* cluster() { return cluster_.get(); }
    
    // Streaming write for large values
    class WriteStream {
    public:
        virtual ~WriteStream() = default;
        virtual elio::coro::task<Status> write(ByteView chunk) = 0;
        virtual elio::coro::task<Status> finish() = 0;
        virtual void abort() = 0;
    };
    
    elio::coro::task<std::unique_ptr<WriteStream>> create_write_stream(
        const CacheKey& key,
        uint64_t total_size,
        const WriteOptions& opts = {});
    
    // Streaming read for large values
    class ReadStream {
    public:
        virtual ~ReadStream() = default;
        virtual elio::coro::task<ByteBuffer> read(size_t max_bytes) = 0;
        virtual bool eof() const = 0;
        virtual uint64_t bytes_read() const = 0;
        virtual uint64_t total_size() const = 0;
    };
    
    elio::coro::task<std::unique_ptr<ReadStream>> create_read_stream(
        const CacheKey& key,
        const ReadOptions& opts = {});
    
private:
    Config config_;
    elio::io::io_context& io_ctx_;
    std::unique_ptr<MemoryCache> memory_;
    std::unique_ptr<DiskCache> disk_;
    std::shared_ptr<Cluster> cluster_;
    
    // Try to fetch from cluster and populate local caches
    elio::coro::task<ReadResult> fetch_from_cluster(
        const CacheKey& key,
        const ReadOptions& opts);
    
    // Promote data up the cache hierarchy
    elio::coro::task<void> promote_to_memory(
        const CacheKey& key,
        const ValueDescriptor& desc,
        const std::vector<SharedChunk>& chunks);
    
    // Replicate to cluster
    elio::coro::task<Status> replicate_to_cluster(
        const CacheKey& key,
        const ValueDescriptor& desc,
        const std::vector<SharedChunk>& chunks);
};

}  // namespace elcache
