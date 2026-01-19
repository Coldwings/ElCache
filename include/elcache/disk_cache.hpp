#pragma once

#include "cache.hpp"
#include "config.hpp"
#include <elio/coro/task.hpp>
#include <elio/sync/primitives.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/io/io_context.hpp>
#include <elio/io/io_awaitables.hpp>
#include <filesystem>
#include <set>

namespace elcache {

// Forward declarations
class DiskStore;

// Disk cache implementation with partial read support
class DiskCache : public ICache, public IChunkCache {
public:
    explicit DiskCache(const DiskCacheConfig& config, elio::io::io_context& ctx);
    ~DiskCache() override;
    
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
    DiskCacheConfig config_;
    elio::io::io_context& io_ctx_;
    std::unique_ptr<DiskStore> store_;
    
    // Index of what's on disk
    mutable elio::sync::shared_mutex index_mutex_;
    std::unordered_map<CacheKey, ValueDescriptor> descriptors_;
    std::unordered_map<ChunkId, std::filesystem::path> chunk_paths_;
    
    // LRU for eviction (by access time)
    struct AccessEntry {
        ChunkId id;
        TimePoint last_access;
        size_t size;
        
        bool operator<(const AccessEntry& other) const {
            return last_access < other.last_access;
        }
    };
    std::set<AccessEntry> access_order_;
    
    // Statistics
    mutable std::atomic<uint64_t> hits_{0};
    mutable std::atomic<uint64_t> misses_{0};
    mutable std::atomic<uint64_t> bytes_read_{0};
    mutable std::atomic<uint64_t> bytes_written_{0};
    mutable std::atomic<uint64_t> evictions_{0};
    mutable std::atomic<size_t> current_size_{0};
    
    // Internal helpers
    std::filesystem::path chunk_path(const ChunkId& id) const;
    std::filesystem::path descriptor_path(const CacheKey& key) const;
    elio::coro::task<Status> load_index();
    elio::coro::task<Status> save_index();
};

// Low-level disk I/O (uses async file operations)
class DiskStore {
public:
    explicit DiskStore(const DiskCacheConfig& config, elio::io::io_context& ctx);
    ~DiskStore();
    
    elio::coro::task<Status> init();
    
    // File operations
    elio::coro::task<ByteBuffer> read_file(
        const std::filesystem::path& path,
        uint64_t offset = 0,
        uint64_t length = 0);  // 0 = entire file
    
    elio::coro::task<Status> write_file(
        const std::filesystem::path& path,
        ByteView data,
        bool sync = false);
    
    elio::coro::task<Status> delete_file(const std::filesystem::path& path);
    
    elio::coro::task<bool> file_exists(const std::filesystem::path& path);
    elio::coro::task<uint64_t> file_size(const std::filesystem::path& path);
    
    // Directory operations
    elio::coro::task<Status> ensure_directory(const std::filesystem::path& path);
    
private:
    DiskCacheConfig config_;
    elio::io::io_context& io_ctx_;
    bool initialized_ = false;
};

}  // namespace elcache
