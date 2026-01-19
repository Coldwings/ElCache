#include "elcache/cache_coordinator.hpp"
#include "elcache/cluster.hpp"
#include <algorithm>

namespace elcache {

CacheCoordinator::CacheCoordinator(const Config& config, elio::io::io_context& io_ctx)
    : config_(config)
    , io_ctx_(io_ctx)
{
    if (config_.memory.max_size > 0) {
        memory_ = std::make_unique<MemoryCache>(config_.memory);
    }
    
    if (config_.disk.max_size > 0) {
        disk_ = std::make_unique<DiskCache>(config_.disk, io_ctx_);
    }
}

CacheCoordinator::~CacheCoordinator() = default;

void CacheCoordinator::set_cluster(std::shared_ptr<Cluster> cluster) {
    cluster_ = std::move(cluster);
}

elio::coro::task<Status> CacheCoordinator::start() {
    if (memory_) {
        auto status = co_await memory_->start();
        if (!status) co_return status;
    }
    
    if (disk_) {
        auto status = co_await disk_->start();
        if (!status) co_return status;
    }
    
    co_return Status::make_ok();
}

elio::coro::task<void> CacheCoordinator::stop() {
    if (memory_) {
        co_await memory_->stop();
    }
    
    if (disk_) {
        co_await disk_->stop();
    }
}

elio::coro::task<ReadResult> CacheCoordinator::get(
    const CacheKey& key,
    const ReadOptions& opts)
{
    // Try memory cache first (fastest)
    if (memory_ && config_.cluster.prefer_local) {
        auto result = co_await memory_->get(key, opts);
        if (result.is_hit()) {
            co_return result;
        }
        
        // If partial hit and we don't allow partial, continue to disk
        if (result.is_partial() && opts.allow_partial) {
            co_return result;
        }
    }
    
    // Try disk cache
    if (disk_) {
        auto result = co_await disk_->get(key, opts);
        if (result.is_hit()) {
            // Promote to memory cache
            if (memory_) {
                auto desc_opt = co_await disk_->metadata(key);
                if (desc_opt) {
                    // Get all chunks and promote
                    // (simplified - full implementation would be async)
                }
            }
            co_return result;
        }
        
        if (result.is_partial() && opts.allow_partial) {
            co_return result;
        }
    }
    
    // Try cluster
    if (cluster_ && !config_.cluster.seed_nodes.empty()) {
        auto result = co_await fetch_from_cluster(key, opts);
        if (result.is_hit() || (result.is_partial() && opts.allow_partial)) {
            co_return result;
        }
    }
    
    // Miss
    ReadResult result;
    result.result = CacheResult::Miss;
    co_return result;
}

elio::coro::task<Status> CacheCoordinator::put(
    const CacheKey& key,
    ByteView value,
    const WriteOptions& opts)
{
    Status status = Status::make_ok();
    
    // Write to memory cache
    if (memory_ && !opts.no_memory) {
        status = co_await memory_->put(key, value, opts);
        if (!status) {
            co_return status;
        }
    }
    
    // Write to disk cache
    if (disk_ && !opts.no_disk) {
        status = co_await disk_->put(key, value, opts);
        if (!status) {
            co_return status;
        }
    }
    
    // Replicate to cluster
    if (cluster_ && !opts.no_cluster) {
        // Get nodes responsible for this key
        auto nodes = cluster_->nodes_for_key(key);
        
        for (const auto& node : nodes) {
            if (node->id() == cluster_->local_node()->id()) {
                continue;  // Skip self
            }
            
            // Fire and forget replication (or wait based on consistency requirements)
            auto rep_status = co_await cluster_->remote_put(node, key, value, opts);
            // Log failures but don't fail the operation
            (void)rep_status;
        }
    }
    
    co_return Status::make_ok();
}

elio::coro::task<Status> CacheCoordinator::remove(
    const CacheKey& key,
    const DeleteOptions& opts)
{
    if (opts.from_memory && memory_) {
        co_await memory_->remove(key, opts);
    }
    
    if (opts.from_disk && disk_) {
        co_await disk_->remove(key, opts);
    }
    
    if (opts.from_cluster && cluster_) {
        auto nodes = cluster_->nodes_for_key(key);
        for (const auto& node : nodes) {
            if (node->id() != cluster_->local_node()->id()) {
                co_await cluster_->remote_delete(node, key);
            }
        }
    }
    
    co_return Status::make_ok();
}

elio::coro::task<AvailabilityResult> CacheCoordinator::check(
    const CacheKey& key,
    std::optional<Range> range)
{
    AvailabilityResult result;
    result.exists = false;
    
    // Check memory
    if (memory_) {
        result = co_await memory_->check(key, range);
        if (result.exists && result.missing_ranges.empty()) {
            co_return result;
        }
    }
    
    // Check disk
    if (disk_) {
        auto disk_result = co_await disk_->check(key, range);
        if (disk_result.exists) {
            // Merge available ranges
            if (!result.exists) {
                result = disk_result;
            } else {
                // Merge available_ranges from both
                // (simplified - real implementation would merge properly)
                for (const auto& r : disk_result.available_ranges) {
                    result.available_ranges.push_back(r);
                }
            }
        }
    }
    
    // Check cluster nodes
    if (cluster_ && !result.exists) {
        auto nodes = cluster_->nodes_for_key(key);
        for (const auto& node : nodes) {
            if (node->id() == cluster_->local_node()->id()) continue;
            
            auto remote_result = co_await cluster_->remote_check(node, key, range);
            if (remote_result.exists) {
                result = remote_result;
                break;
            }
        }
    }
    
    co_return result;
}

elio::coro::task<std::optional<CacheMetadata>> CacheCoordinator::metadata(
    const CacheKey& key)
{
    if (memory_) {
        auto meta = co_await memory_->metadata(key);
        if (meta) co_return meta;
    }
    
    if (disk_) {
        auto meta = co_await disk_->metadata(key);
        if (meta) co_return meta;
    }
    
    // Could also check cluster here
    
    co_return std::nullopt;
}

ICache::Stats CacheCoordinator::stats() const {
    Stats combined;
    
    if (memory_) {
        auto s = memory_->stats();
        combined.hits += s.hits;
        combined.misses += s.misses;
        combined.partial_hits += s.partial_hits;
        combined.bytes_read += s.bytes_read;
        combined.bytes_written += s.bytes_written;
        combined.evictions += s.evictions;
        combined.entry_count += s.entry_count;
        combined.size_bytes += s.size_bytes;
        combined.capacity_bytes += s.capacity_bytes;
    }
    
    if (disk_) {
        auto s = disk_->stats();
        combined.hits += s.hits;
        combined.misses += s.misses;
        combined.bytes_read += s.bytes_read;
        combined.bytes_written += s.bytes_written;
        combined.evictions += s.evictions;
        combined.entry_count += s.entry_count;
        combined.size_bytes += s.size_bytes;
        combined.capacity_bytes += s.capacity_bytes;
    }
    
    return combined;
}

elio::coro::task<ReadResult> CacheCoordinator::fetch_from_cluster(
    const CacheKey& key,
    const ReadOptions& opts)
{
    ReadResult result;
    result.result = CacheResult::Miss;
    
    if (!cluster_) {
        co_return result;
    }
    
    auto nodes = cluster_->nodes_for_key(key);
    
    for (const auto& node : nodes) {
        if (node->id() == cluster_->local_node()->id()) {
            continue;
        }
        
        result = co_await cluster_->remote_get(node, key, opts);
        if (result.is_hit()) {
            // Cache locally for future requests
            if (memory_ && !result.data.empty()) {
                WriteOptions write_opts;
                write_opts.no_cluster = true;  // Don't re-replicate
                co_await memory_->put(key, result.data, write_opts);
            }
            co_return result;
        }
        
        if (result.is_partial() && opts.allow_partial) {
            co_return result;
        }
    }
    
    co_return result;
}

// Streaming write implementation
class WriteStreamImpl : public CacheCoordinator::WriteStream {
public:
    WriteStreamImpl(CacheCoordinator* coord, const CacheKey& key, 
                    uint64_t total_size, const WriteOptions& opts)
        : coord_(coord)
        , key_(key)
        , total_size_(total_size)
        , opts_(opts)
        , written_(0)
    {}
    
    elio::coro::task<Status> write(ByteView chunk) override {
        // Buffer chunks and write when we have a full CHUNK_SIZE
        buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());
        
        while (buffer_.size() >= CHUNK_SIZE) {
            ByteBuffer chunk_data(buffer_.begin(), buffer_.begin() + CHUNK_SIZE);
            chunks_.push_back(std::move(chunk_data));
            buffer_.erase(buffer_.begin(), buffer_.begin() + CHUNK_SIZE);
        }
        
        written_ += chunk.size();
        co_return Status::make_ok();
    }
    
    elio::coro::task<Status> finish() override {
        // Handle remaining data
        if (!buffer_.empty()) {
            chunks_.push_back(std::move(buffer_));
        }
        
        // Assemble full value and write
        ByteBuffer full_value;
        full_value.reserve(total_size_);
        
        for (const auto& chunk : chunks_) {
            full_value.insert(full_value.end(), chunk.begin(), chunk.end());
        }
        
        co_return co_await coord_->put(key_, full_value, opts_);
    }
    
    void abort() override {
        buffer_.clear();
        chunks_.clear();
    }
    
private:
    CacheCoordinator* coord_;
    CacheKey key_;
    uint64_t total_size_;
    WriteOptions opts_;
    uint64_t written_;
    ByteBuffer buffer_;
    std::vector<ByteBuffer> chunks_;
};

elio::coro::task<std::unique_ptr<CacheCoordinator::WriteStream>> 
CacheCoordinator::create_write_stream(
    const CacheKey& key,
    uint64_t total_size,
    const WriteOptions& opts)
{
    co_return std::make_unique<WriteStreamImpl>(this, key, total_size, opts);
}

// Streaming read implementation
class ReadStreamImpl : public CacheCoordinator::ReadStream {
public:
    ReadStreamImpl(ByteBuffer data, uint64_t total_size)
        : data_(std::move(data))
        , total_size_(total_size)
        , position_(0)
    {}
    
    elio::coro::task<ByteBuffer> read(size_t max_bytes) override {
        size_t available = data_.size() - position_;
        size_t to_read = std::min(max_bytes, available);
        
        ByteBuffer result(data_.begin() + position_, 
                          data_.begin() + position_ + to_read);
        position_ += to_read;
        
        co_return result;
    }
    
    bool eof() const override {
        return position_ >= data_.size();
    }
    
    uint64_t bytes_read() const override {
        return position_;
    }
    
    uint64_t total_size() const override {
        return total_size_;
    }
    
private:
    ByteBuffer data_;
    uint64_t total_size_;
    size_t position_;
};

elio::coro::task<std::unique_ptr<CacheCoordinator::ReadStream>>
CacheCoordinator::create_read_stream(
    const CacheKey& key,
    const ReadOptions& opts)
{
    auto result = co_await get(key, opts);
    
    if (result.result == CacheResult::Miss) {
        co_return nullptr;
    }
    
    uint64_t total = result.metadata ? result.metadata->total_size : result.data.size();
    co_return std::make_unique<ReadStreamImpl>(std::move(result.data), total);
}

}  // namespace elcache
