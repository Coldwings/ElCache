#include "elcache/disk_cache.hpp"
#include <fstream>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace elcache {

// DiskStore implementation

DiskStore::DiskStore(const DiskCacheConfig& config, elio::io::io_context& ctx)
    : config_(config)
    , io_ctx_(ctx)
{}

DiskStore::~DiskStore() = default;

elio::coro::task<Status> DiskStore::init() {
    // Create cache directory if it doesn't exist
    auto status = co_await ensure_directory(config_.path);
    if (!status) {
        co_return status;
    }
    
    // Create subdirectories for chunks (sharding by first 2 hex chars)
    for (int i = 0; i < 256; ++i) {
        char dir[3];
        snprintf(dir, sizeof(dir), "%02x", i);
        auto subdir = config_.path / "chunks" / dir;
        status = co_await ensure_directory(subdir);
        if (!status) {
            co_return status;
        }
    }
    
    // Create descriptors directory
    status = co_await ensure_directory(config_.path / "descriptors");
    if (!status) {
        co_return status;
    }
    
    initialized_ = true;
    co_return Status::make_ok();
}

elio::coro::task<ByteBuffer> DiskStore::read_file(
    const std::filesystem::path& path,
    uint64_t offset,
    uint64_t length)
{
    int flags = O_RDONLY;
#ifdef O_DIRECT
    if (config_.use_direct_io && length >= config_.block_size) {
        flags |= O_DIRECT;
    }
#endif
    
    int fd = ::open(path.c_str(), flags);
    if (fd < 0) {
        co_return ByteBuffer{};
    }
    
    // Get file size if length is 0
    if (length == 0) {
        struct stat st;
        if (fstat(fd, &st) == 0) {
            length = st.st_size - offset;
        }
    }
    
    ByteBuffer buffer(length);
    size_t total_read = 0;
    
    // Use Elio async read with offset support
    while (total_read < length) {
        auto result = co_await elio::io::async_read(
            io_ctx_, fd, 
            buffer.data() + total_read, 
            length - total_read,
            static_cast<int64_t>(offset + total_read));
        
        if (!result.success() || result.result <= 0) {
            break;
        }
        total_read += result.result;
    }
    
    // Async close
    co_await elio::io::async_close(io_ctx_, fd);
    
    buffer.resize(total_read);
    co_return buffer;
}

elio::coro::task<Status> DiskStore::write_file(
    const std::filesystem::path& path,
    ByteView data,
    bool sync)
{
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_DIRECT
    if (config_.use_direct_io && data.size() >= config_.block_size) {
        flags |= O_DIRECT;
    }
#endif
    
    int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) {
        co_return Status::error(ErrorCode::DiskError, "Failed to open file for writing");
    }
    
    // Ensure file is readable regardless of umask
    fchmod(fd, 0644);
    
    // Pre-allocate if supported
#ifdef __linux__
    if (config_.use_fallocate) {
        fallocate(fd, 0, 0, data.size());
    }
#endif
    
    size_t total_written = 0;
    
    // Use Elio async write
    while (total_written < data.size()) {
        auto result = co_await elio::io::async_write(
            io_ctx_, fd,
            data.data() + total_written,
            data.size() - total_written,
            static_cast<int64_t>(total_written));
        
        if (!result.success() || result.result <= 0) {
            co_await elio::io::async_close(io_ctx_, fd);
            co_return Status::error(ErrorCode::DiskError, "Write failed");
        }
        total_written += result.result;
    }
    
    if (sync) {
        fsync(fd);  // TODO: Use async fsync when available in Elio
    }
    
    co_await elio::io::async_close(io_ctx_, fd);
    co_return Status::make_ok();
}

elio::coro::task<Status> DiskStore::delete_file(const std::filesystem::path& path) {
    if (std::filesystem::remove(path)) {
        co_return Status::make_ok();
    }
    co_return Status::error(ErrorCode::DiskError, "Failed to delete file");
}

elio::coro::task<bool> DiskStore::file_exists(const std::filesystem::path& path) {
    co_return std::filesystem::exists(path);
}

elio::coro::task<uint64_t> DiskStore::file_size(const std::filesystem::path& path) {
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        co_return 0;
    }
    co_return size;
}

elio::coro::task<Status> DiskStore::ensure_directory(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path)) {
        std::filesystem::create_directories(path, ec);
        if (ec) {
            co_return Status::error(ErrorCode::DiskError, 
                "Failed to create directory: " + ec.message());
        }
    }
    co_return Status::make_ok();
}

// DiskCache implementation

DiskCache::DiskCache(const DiskCacheConfig& config, elio::io::io_context& ctx)
    : config_(config)
    , io_ctx_(ctx)
    , store_(std::make_unique<DiskStore>(config, ctx))
{}

DiskCache::~DiskCache() = default;

elio::coro::task<Status> DiskCache::start() {
    auto status = co_await store_->init();
    if (!status) {
        co_return status;
    }
    
    // Load existing index
    status = co_await load_index();
    co_return status;
}

elio::coro::task<void> DiskCache::stop() {
    co_await save_index();
}

elio::coro::task<ReadResult> DiskCache::get(
    const CacheKey& key,
    const ReadOptions& opts)
{
    ReadResult result;
    result.result = CacheResult::Miss;
    
    auto desc_opt = co_await get_descriptor(key);
    if (!desc_opt) {
        misses_++;
        co_return result;
    }
    
    const auto& desc = *desc_opt;
    
    if (desc.metadata.is_expired()) {
        misses_++;
        result.result = CacheResult::Expired;
        co_return result;
    }
    
    Range range = opts.range.value_or(Range{0, desc.total_size});
    if (range.offset >= desc.total_size) {
        result.result = CacheResult::Hit;
        result.actual_range = {range.offset, 0};
        result.metadata = desc.metadata;
        co_return result;
    }
    
    uint64_t effective_length = std::min(range.length, desc.total_size - range.offset);
    range.length = effective_length;
    
    // Read required chunks
    auto chunk_indices = desc.chunks_for_range(range);
    std::vector<SharedChunk> chunks;
    bool all_found = true;
    
    for (uint64_t idx : chunk_indices) {
        if (idx >= desc.chunks.size()) {
            all_found = false;
            break;
        }
        
        auto chunk = co_await get_chunk(desc.chunks[idx]);
        if (!chunk) {
            all_found = false;
            if (!opts.allow_partial) break;
        }
        chunks.push_back(chunk);
    }
    
    if (chunks.empty() || (!all_found && !opts.allow_partial)) {
        misses_++;
        co_return result;
    }
    
    // Assemble result
    ByteBuffer data;
    data.reserve(range.length);
    
    uint64_t current = range.offset;
    uint64_t remaining = range.length;
    
    for (size_t i = 0; i < chunks.size() && remaining > 0; ++i) {
        if (!chunks[i]) continue;
        
        uint64_t chunk_start = (range.offset / CHUNK_SIZE + i) * CHUNK_SIZE;
        uint64_t offset_in_chunk = (current > chunk_start) ? (current - chunk_start) : 0;
        uint64_t to_copy = std::min(remaining, chunks[i]->size() - offset_in_chunk);
        
        auto chunk_data = chunks[i]->read(offset_in_chunk, to_copy);
        data.insert(data.end(), chunk_data.begin(), chunk_data.end());
        
        current += to_copy;
        remaining -= to_copy;
    }
    
    result.data = std::move(data);
    result.actual_range = {range.offset, result.data.size()};
    result.metadata = desc.metadata;
    
    if (all_found && result.data.size() == effective_length) {
        result.result = CacheResult::Hit;
        hits_++;
    } else {
        result.result = CacheResult::PartialHit;
    }
    
    bytes_read_ += result.data.size();
    co_return result;
}

elio::coro::task<Status> DiskCache::put(
    const CacheKey& key,
    ByteView value,
    const WriteOptions& opts)
{
    if (key.size() > MAX_KEY_SIZE) {
        co_return Status::error(ErrorCode::KeyTooLarge);
    }
    
    // Check space
    size_t needed = value.size();
    if (current_size_ + needed > config_.max_size * config_.high_watermark) {
        co_await evict(needed);
    }
    
    // Split and store chunks
    ValueDescriptor desc;
    desc.key = key;
    desc.total_size = value.size();
    desc.metadata.total_size = value.size();
    desc.metadata.created_at = SystemClock::now();
    desc.metadata.last_accessed = desc.metadata.created_at;
    desc.metadata.flags = opts.flags;
    
    if (opts.ttl) {
        desc.metadata.expires_at = desc.metadata.created_at + *opts.ttl;
    }
    
    size_t offset = 0;
    while (offset < value.size()) {
        size_t chunk_size = std::min(CHUNK_SIZE, value.size() - offset);
        ByteBuffer data(value.begin() + offset, value.begin() + offset + chunk_size);
        auto chunk = std::make_shared<Chunk>(std::move(data));
        
        desc.chunks.push_back(chunk->id());
        
        auto status = co_await put_chunk(chunk);
        if (!status) {
            co_return status;
        }
        
        offset += chunk_size;
    }
    
    desc.metadata.chunk_count = desc.chunks.size();
    
    auto status = co_await put_descriptor(desc);
    if (!status) {
        co_return status;
    }
    
    bytes_written_ += value.size();
    co_return Status::make_ok();
}

elio::coro::task<Status> DiskCache::remove(
    const CacheKey& key,
    const DeleteOptions& opts)
{
    auto desc_opt = co_await get_descriptor(key);
    if (!desc_opt) {
        co_return Status::error(ErrorCode::NotFound);
    }
    
    for (const auto& chunk_id : desc_opt->chunks) {
        co_await remove_chunk(chunk_id);
    }
    
    co_await remove_descriptor(key);
    co_return Status::make_ok();
}

elio::coro::task<AvailabilityResult> DiskCache::check(
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

elio::coro::task<std::optional<CacheMetadata>> DiskCache::metadata(
    const CacheKey& key)
{
    auto desc = co_await get_descriptor(key);
    if (desc) {
        co_return desc->metadata;
    }
    co_return std::nullopt;
}

ICache::Stats DiskCache::stats() const {
    Stats s;
    s.hits = hits_.load();
    s.misses = misses_.load();
    s.bytes_read = bytes_read_.load();
    s.bytes_written = bytes_written_.load();
    s.evictions = evictions_.load();
    s.size_bytes = current_size_.load();
    s.capacity_bytes = config_.max_size;
    
    // Use try_lock_shared for non-coroutine context
    while (!index_mutex_.try_lock_shared()) {
        // Spin briefly
    }
    s.entry_count = descriptors_.size();
    index_mutex_.unlock_shared();
    
    return s;
}

// Chunk operations

std::filesystem::path DiskCache::chunk_path(const ChunkId& id) const {
    auto hex = id.hash().to_hex();
    return config_.path / "chunks" / hex.substr(0, 2) / (hex + ".chunk");
}

std::filesystem::path DiskCache::descriptor_path(const CacheKey& key) const {
    auto hex = key.hash128().to_hex();
    return config_.path / "descriptors" / (hex + ".desc");
}

elio::coro::task<SharedChunk> DiskCache::get_chunk(const ChunkId& id) {
    auto path = chunk_path(id);
    auto data = co_await store_->read_file(path);
    
    if (data.empty()) {
        co_return nullptr;
    }
    
    // Update access order
    co_await index_mutex_.lock();
    // Remove old entry and add with new timestamp
    // (simplified - real implementation would use more efficient data structure)
    index_mutex_.unlock();
    
    co_return std::make_shared<Chunk>(std::move(data));
}

elio::coro::task<Status> DiskCache::put_chunk(SharedChunk chunk) {
    if (!chunk) {
        co_return Status::error(ErrorCode::InvalidArgument);
    }
    
    auto path = chunk_path(chunk->id());
    auto status = co_await store_->write_file(path, chunk->data());
    
    if (status) {
        co_await index_mutex_.lock();
        chunk_paths_[chunk->id()] = path;
        current_size_ += chunk->size();
        access_order_.insert({chunk->id(), Clock::now(), chunk->size()});
        index_mutex_.unlock();
    }
    
    co_return status;
}

elio::coro::task<Status> DiskCache::remove_chunk(const ChunkId& id) {
    auto path = chunk_path(id);
    
    co_await index_mutex_.lock();
    auto it = chunk_paths_.find(id);
    if (it != chunk_paths_.end()) {
        // Find and remove from access_order_
        for (auto ait = access_order_.begin(); ait != access_order_.end(); ++ait) {
            if (ait->id == id) {
                current_size_ -= ait->size;
                access_order_.erase(ait);
                break;
            }
        }
        chunk_paths_.erase(it);
    }
    index_mutex_.unlock();
    
    co_return co_await store_->delete_file(path);
}

elio::coro::task<bool> DiskCache::has_chunk(const ChunkId& id) {
    co_await index_mutex_.lock_shared();
    bool result = chunk_paths_.find(id) != chunk_paths_.end();
    index_mutex_.unlock_shared();
    co_return result;
}

elio::coro::task<std::optional<ValueDescriptor>> DiskCache::get_descriptor(
    const CacheKey& key)
{
    co_await index_mutex_.lock_shared();
    auto it = descriptors_.find(key);
    if (it != descriptors_.end()) {
        auto result = it->second;
        index_mutex_.unlock_shared();
        co_return result;
    }
    index_mutex_.unlock_shared();
    co_return std::nullopt;
}

elio::coro::task<Status> DiskCache::put_descriptor(const ValueDescriptor& desc) {
    // Serialize descriptor to disk
    auto path = descriptor_path(desc.key);
    
    // Simple serialization (in production, use proper format)
    ByteBuffer data;
    
    // Key length + key
    uint32_t key_len = desc.key.size();
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&key_len), 
                reinterpret_cast<uint8_t*>(&key_len) + 4);
    data.insert(data.end(), desc.key.view().begin(), desc.key.view().end());
    
    // Total size
    uint64_t total = desc.total_size;
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&total),
                reinterpret_cast<uint8_t*>(&total) + 8);
    
    // Chunk count and IDs
    uint64_t chunk_count = desc.chunks.size();
    data.insert(data.end(), reinterpret_cast<uint8_t*>(&chunk_count),
                reinterpret_cast<uint8_t*>(&chunk_count) + 8);
    
    for (const auto& chunk_id : desc.chunks) {
        auto hash = chunk_id.hash();
        data.insert(data.end(), reinterpret_cast<const uint8_t*>(&hash),
                    reinterpret_cast<const uint8_t*>(&hash) + sizeof(hash));
    }
    
    auto status = co_await store_->write_file(path, data, true);
    
    if (status) {
        co_await index_mutex_.lock();
        descriptors_[desc.key] = desc;
        index_mutex_.unlock();
    }
    
    co_return status;
}

elio::coro::task<Status> DiskCache::remove_descriptor(const CacheKey& key) {
    co_await index_mutex_.lock();
    descriptors_.erase(key);
    index_mutex_.unlock();
    
    auto path = descriptor_path(key);
    co_return co_await store_->delete_file(path);
}

elio::coro::task<std::vector<SharedChunk>> DiskCache::get_chunks(
    const std::vector<ChunkId>& ids)
{
    std::vector<SharedChunk> result;
    result.reserve(ids.size());
    
    for (const auto& id : ids) {
        result.push_back(co_await get_chunk(id));
    }
    
    co_return result;
}

elio::coro::task<size_t> DiskCache::evict(size_t bytes_needed) {
    size_t evicted = 0;
    size_t target = config_.max_size * config_.low_watermark;
    
    while (current_size_ > target || evicted < bytes_needed) {
        co_await index_mutex_.lock();
        
        if (access_order_.empty()) {
            index_mutex_.unlock();
            break;
        }
        
        // Get oldest entry
        auto it = access_order_.begin();
        ChunkId id = it->id;
        size_t size = it->size;
        
        access_order_.erase(it);
        chunk_paths_.erase(id);
        current_size_ -= size;
        evicted += size;
        evictions_++;
        
        index_mutex_.unlock();
        
        // Delete file
        co_await store_->delete_file(chunk_path(id));
    }
    
    co_return evicted;
}

elio::coro::task<Status> DiskCache::load_index() {
    // Scan descriptors directory
    auto desc_dir = config_.path / "descriptors";
    
    if (!std::filesystem::exists(desc_dir)) {
        co_return Status::make_ok();
    }
    
    for (const auto& entry : std::filesystem::directory_iterator(desc_dir)) {
        if (entry.path().extension() == ".desc") {
            auto data = co_await store_->read_file(entry.path());
            if (data.size() >= 12) {
                // Parse descriptor
                // (simplified - real implementation would have proper deserialization)
            }
        }
    }
    
    // Scan chunks directory
    auto chunks_dir = config_.path / "chunks";
    if (std::filesystem::exists(chunks_dir)) {
        for (const auto& subdir : std::filesystem::directory_iterator(chunks_dir)) {
            if (subdir.is_directory()) {
                for (const auto& chunk_file : std::filesystem::directory_iterator(subdir)) {
                    if (chunk_file.path().extension() == ".chunk") {
                        auto filename = chunk_file.path().stem().string();
                        auto hash = Hash128::from_hex(filename);
                        ChunkId id(hash);
                        
                        co_await index_mutex_.lock();
                        chunk_paths_[id] = chunk_file.path();
                        
                        auto size = std::filesystem::file_size(chunk_file.path());
                        current_size_ += size;
                        access_order_.insert({id, Clock::now(), size});
                        index_mutex_.unlock();
                    }
                }
            }
        }
    }
    
    co_return Status::make_ok();
}

elio::coro::task<Status> DiskCache::save_index() {
    // Index is saved incrementally with each put_descriptor
    co_return Status::make_ok();
}

}  // namespace elcache
