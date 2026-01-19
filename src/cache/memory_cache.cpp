#include "elcache/memory_cache.hpp"
#include <algorithm>

namespace elcache {

MemoryCache::MemoryCache(const MemoryCacheConfig& config)
    : config_(config)
    , chunk_cache_(config.max_size, [](const SharedChunk& chunk) {
        return chunk ? chunk->size() : 0;
    })
{}

MemoryCache::~MemoryCache() = default;

elio::coro::task<ReadResult> MemoryCache::get(
    const CacheKey& key,
    const ReadOptions& opts)
{
    ReadResult result;
    result.result = CacheResult::Miss;
    
    // Get value descriptor
    auto desc_opt = co_await get_descriptor(key);
    if (!desc_opt) {
        misses_++;
        co_return result;
    }
    
    const auto& desc = *desc_opt;
    
    // Check expiration
    if (desc.metadata.is_expired()) {
        misses_++;
        result.result = CacheResult::Expired;
        co_return result;
    }
    
    // Determine read range
    Range range;
    if (opts.range) {
        range = *opts.range;
    } else {
        range = {0, desc.total_size};
    }
    
    // Clamp range to actual size
    if (range.offset >= desc.total_size) {
        result.result = CacheResult::Hit;
        result.actual_range = {range.offset, 0};
        result.metadata = desc.metadata;
        co_return result;
    }
    
    uint64_t effective_length = std::min(range.length, desc.total_size - range.offset);
    range.length = effective_length;
    
    // Get required chunk indices
    auto chunk_indices = desc.chunks_for_range(range);
    
    // Fetch chunks
    std::vector<SharedChunk> chunks;
    chunks.reserve(chunk_indices.size());
    bool all_found = true;
    
    for (uint64_t idx : chunk_indices) {
        if (idx >= desc.chunks.size()) {
            all_found = false;
            break;
        }
        
        auto chunk = co_await get_chunk(desc.chunks[idx]);
        if (!chunk) {
            all_found = false;
            if (!opts.allow_partial) {
                break;
            }
        }
        chunks.push_back(chunk);
    }
    
    if (chunks.empty() || (!all_found && !opts.allow_partial)) {
        misses_++;
        co_return result;
    }
    
    // Assemble result
    result.data = assemble_value(desc, chunks, range);
    result.actual_range = {range.offset, result.data.size()};
    result.metadata = desc.metadata;
    
    if (all_found && result.data.size() == effective_length) {
        result.result = CacheResult::Hit;
        hits_++;
    } else {
        result.result = CacheResult::PartialHit;
        partial_hits_++;
    }
    
    bytes_read_ += result.data.size();
    
    // Update access time
    if (opts.update_access_time) {
        // Touch the descriptor (would need modification to track this)
    }
    
    co_return result;
}

elio::coro::task<Status> MemoryCache::put(
    const CacheKey& key,
    ByteView value,
    const WriteOptions& opts)
{
    if (key.size() > MAX_KEY_SIZE) {
        co_return Status::error(ErrorCode::KeyTooLarge, "Key exceeds maximum size");
    }
    
    if (value.size() > MAX_VALUE_SIZE) {
        co_return Status::error(ErrorCode::ValueTooLarge, "Value exceeds maximum size");
    }
    
    // Split value into chunks
    auto chunks = split_value(value);
    
    // Create value descriptor
    ValueDescriptor desc;
    desc.key = key;
    desc.total_size = value.size();
    desc.metadata.total_size = value.size();
    desc.metadata.chunk_count = chunks.size();
    desc.metadata.created_at = SystemClock::now();
    desc.metadata.last_accessed = desc.metadata.created_at;
    desc.metadata.flags = opts.flags;
    
    if (opts.ttl) {
        desc.metadata.expires_at = desc.metadata.created_at + *opts.ttl;
    }
    
    // Store chunks
    for (auto& chunk : chunks) {
        desc.chunks.push_back(chunk->id());
        auto status = co_await put_chunk(std::move(chunk));
        if (!status) {
            co_return status;
        }
    }
    
    // Store descriptor
    auto status = co_await put_descriptor(desc);
    if (!status) {
        co_return status;
    }
    
    bytes_written_ += value.size();
    co_return Status::make_ok();
}

elio::coro::task<Status> MemoryCache::remove(
    const CacheKey& key,
    const DeleteOptions& opts)
{
    // Get descriptor first to find chunks
    auto desc_opt = co_await get_descriptor(key);
    if (!desc_opt) {
        co_return Status::error(ErrorCode::NotFound, "Key not found");
    }
    
    // Remove chunks
    for (const auto& chunk_id : desc_opt->chunks) {
        co_await remove_chunk(chunk_id);
    }
    
    // Remove descriptor
    co_await remove_descriptor(key);
    
    co_return Status::make_ok();
}

elio::coro::task<AvailabilityResult> MemoryCache::check(
    const CacheKey& key,
    std::optional<Range> range)
{
    AvailabilityResult result;
    result.exists = false;
    
    auto desc_opt = co_await get_descriptor(key);
    if (!desc_opt) {
        co_return result;
    }
    
    result.exists = true;
    result.metadata = desc_opt->metadata;
    
    // Check which chunks we have
    Range check_range = range.value_or(Range{0, desc_opt->total_size});
    auto chunk_indices = desc_opt->chunks_for_range(check_range);
    
    PartialValue partial(*desc_opt);
    
    for (uint64_t idx : chunk_indices) {
        if (idx < desc_opt->chunks.size()) {
            bool has = co_await has_chunk(desc_opt->chunks[idx]);
            if (has) {
                partial.mark_chunk_available(idx, desc_opt->chunks[idx]);
            }
        }
    }
    
    result.available_ranges = partial.available_ranges();
    result.missing_ranges = partial.missing_ranges(check_range);
    
    co_return result;
}

elio::coro::task<std::optional<CacheMetadata>> MemoryCache::metadata(
    const CacheKey& key)
{
    auto desc = co_await get_descriptor(key);
    if (desc) {
        co_return desc->metadata;
    }
    co_return std::nullopt;
}

ICache::Stats MemoryCache::stats() const {
    Stats s;
    s.hits = hits_.load();
    s.misses = misses_.load();
    s.partial_hits = partial_hits_.load();
    s.bytes_read = bytes_read_.load();
    s.bytes_written = bytes_written_.load();
    s.evictions = evictions_.load();
    s.size_bytes = chunk_cache_.size();
    s.capacity_bytes = chunk_cache_.capacity();
    
    // Use try_lock_shared for non-coroutine context
    while (!desc_mutex_.try_lock_shared()) {
        // Spin briefly - this is a synchronous stats() call
    }
    s.entry_count = descriptors_.size();
    desc_mutex_.unlock_shared();
    
    return s;
}

elio::coro::task<Status> MemoryCache::start() {
    co_return Status::make_ok();
}

elio::coro::task<void> MemoryCache::stop() {
    chunk_cache_.clear();
    co_await desc_mutex_.lock();
    descriptors_.clear();
    desc_mutex_.unlock();
    co_return;
}

// IChunkCache implementation

elio::coro::task<SharedChunk> MemoryCache::get_chunk(const ChunkId& id) {
    auto* chunk = chunk_cache_.get(id);
    if (chunk) {
        co_return *chunk;
    }
    co_return nullptr;
}

elio::coro::task<Status> MemoryCache::put_chunk(SharedChunk chunk) {
    if (!chunk) {
        co_return Status::error(ErrorCode::InvalidArgument, "Null chunk");
    }
    
    auto evicted = chunk_cache_.put(chunk->id(), chunk);
    evictions_ += evicted.size();
    
    co_return Status::make_ok();
}

elio::coro::task<Status> MemoryCache::remove_chunk(const ChunkId& id) {
    chunk_cache_.remove(id);
    co_return Status::make_ok();
}

elio::coro::task<bool> MemoryCache::has_chunk(const ChunkId& id) {
    co_return chunk_cache_.contains(id);
}

elio::coro::task<std::optional<ValueDescriptor>> MemoryCache::get_descriptor(
    const CacheKey& key)
{
    co_await desc_mutex_.lock_shared();
    auto it = descriptors_.find(key);
    if (it != descriptors_.end()) {
        auto result = it->second;
        desc_mutex_.unlock_shared();
        co_return result;
    }
    desc_mutex_.unlock_shared();
    co_return std::nullopt;
}

elio::coro::task<Status> MemoryCache::put_descriptor(const ValueDescriptor& desc) {
    co_await desc_mutex_.lock();
    descriptors_[desc.key] = desc;
    desc_mutex_.unlock();
    co_return Status::make_ok();
}

elio::coro::task<Status> MemoryCache::remove_descriptor(const CacheKey& key) {
    co_await desc_mutex_.lock();
    descriptors_.erase(key);
    desc_mutex_.unlock();
    co_return Status::make_ok();
}

elio::coro::task<std::vector<SharedChunk>> MemoryCache::get_chunks(
    const std::vector<ChunkId>& ids)
{
    std::vector<SharedChunk> result;
    result.reserve(ids.size());
    
    for (const auto& id : ids) {
        result.push_back(co_await get_chunk(id));
    }
    
    co_return result;
}

elio::coro::task<size_t> MemoryCache::evict(size_t bytes_needed) {
    // Force eviction by temporarily reducing capacity
    // This is a simplified approach; a real implementation would
    // have explicit eviction control in ARC cache
    size_t evicted = 0;
    
    // The ARC cache handles eviction internally during put operations
    // For explicit eviction, we'd need to extend the ARC implementation
    
    co_return evicted;
}

// Internal helpers

std::vector<SharedChunk> MemoryCache::split_value(ByteView value) {
    std::vector<SharedChunk> chunks;
    
    size_t offset = 0;
    while (offset < value.size()) {
        size_t chunk_size = std::min(CHUNK_SIZE, value.size() - offset);
        ByteBuffer data(value.begin() + offset, value.begin() + offset + chunk_size);
        chunks.push_back(std::make_shared<Chunk>(std::move(data)));
        offset += chunk_size;
    }
    
    return chunks;
}

ByteBuffer MemoryCache::assemble_value(
    const ValueDescriptor& desc,
    const std::vector<SharedChunk>& chunks,
    const Range& range)
{
    ByteBuffer result;
    result.reserve(range.length);
    
    uint64_t current_offset = range.offset;
    uint64_t remaining = range.length;
    size_t chunk_idx = 0;
    
    // Find starting chunk
    uint64_t start_chunk = range.offset / CHUNK_SIZE;
    
    for (size_t i = 0; i < chunks.size() && remaining > 0; ++i) {
        if (!chunks[i]) {
            // Missing chunk - skip
            uint64_t skip = std::min(remaining, static_cast<uint64_t>(CHUNK_SIZE));
            remaining -= skip;
            current_offset += skip;
            continue;
        }
        
        uint64_t chunk_global_offset = (start_chunk + i) * CHUNK_SIZE;
        uint64_t offset_in_chunk = (current_offset > chunk_global_offset) 
            ? (current_offset - chunk_global_offset) : 0;
        
        uint64_t available = chunks[i]->size() - offset_in_chunk;
        uint64_t to_copy = std::min(remaining, available);
        
        auto chunk_data = chunks[i]->read(offset_in_chunk, to_copy);
        result.insert(result.end(), chunk_data.begin(), chunk_data.end());
        
        current_offset += to_copy;
        remaining -= to_copy;
    }
    
    return result;
}

}  // namespace elcache
