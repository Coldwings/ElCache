#pragma once

#include "types.hpp"
#include <memory>
#include <optional>

namespace elcache {

// A chunk is the basic unit of storage (4MB default)
// Values are split into chunks for efficient partial reads and distributed storage
class Chunk {
public:
    Chunk() = default;
    explicit Chunk(ByteBuffer data);
    Chunk(const uint8_t* data, size_t size);
    
    // Move-only (large data)
    Chunk(Chunk&&) = default;
    Chunk& operator=(Chunk&&) = default;
    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;
    
    // Access
    ByteView data() const noexcept { return {data_.data(), data_.size()}; }
    MutableByteView mutable_data() noexcept { return {data_.data(), data_.size()}; }
    size_t size() const noexcept { return data_.size(); }
    bool empty() const noexcept { return data_.empty(); }
    
    // Content-addressed ID (computed lazily)
    const ChunkId& id() const;
    
    // Read a portion of this chunk
    ByteBuffer read(uint64_t offset, uint64_t length) const;
    
    // Release ownership of data
    ByteBuffer release() { return std::move(data_); }
    
private:
    ByteBuffer data_;
    mutable std::optional<ChunkId> id_;  // Lazy computed
};

// Shared chunk for zero-copy sharing between caches
using SharedChunk = std::shared_ptr<Chunk>;

// Chunk metadata stored separately from data
struct ChunkMeta {
    ChunkId id;
    uint32_t size;
    uint16_t flags;
    Timestamp created_at;
    
    enum Flags : uint16_t {
        None = 0,
        Remote = 1 << 0,  // Stored on remote node
    };
    
    bool is_remote() const noexcept { return flags & Remote; }
};

// Value descriptor - describes a complete value across multiple chunks
struct ValueDescriptor {
    CacheKey key;
    uint64_t total_size;
    std::vector<ChunkId> chunks;  // Ordered list of chunk IDs
    CacheMetadata metadata;
    
    // Get chunk index and offset for a byte position
    std::pair<uint64_t, uint64_t> locate_chunk(uint64_t byte_offset) const {
        uint64_t chunk_index = byte_offset / CHUNK_SIZE;
        uint64_t offset_in_chunk = byte_offset % CHUNK_SIZE;
        return {chunk_index, offset_in_chunk};
    }
    
    // Get all chunks needed for a range
    std::vector<uint64_t> chunks_for_range(const Range& range) const;
    
    // Check if we have all chunks
    bool is_complete() const {
        size_t expected = (total_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
        return chunks.size() == expected;
    }
};

// Partial value - tracks which chunks are available
class PartialValue {
public:
    PartialValue() = default;
    explicit PartialValue(const ValueDescriptor& desc);
    
    // Check chunk availability
    bool has_chunk(uint64_t index) const;
    void mark_chunk_available(uint64_t index, const ChunkId& id);
    void mark_chunk_unavailable(uint64_t index);
    
    // Range queries
    bool has_range(const Range& range) const;
    std::vector<Range> missing_ranges(const Range& range) const;
    std::vector<Range> available_ranges() const;
    
    // Progress
    double completion_ratio() const;
    size_t available_bytes() const;
    size_t total_bytes() const { return total_size_; }
    
private:
    uint64_t total_size_ = 0;
    std::vector<bool> chunk_availability_;
    std::vector<ChunkId> chunk_ids_;
};

}  // namespace elcache
