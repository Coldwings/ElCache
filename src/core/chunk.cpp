#include "elcache/chunk.hpp"
#include <cstring>
#include <algorithm>

// Use xxHash if available
#ifdef ELCACHE_USE_XXHASH
#include <xxhash.h>
#endif

namespace elcache {

// Chunk implementation
Chunk::Chunk(ByteBuffer data) : data_(std::move(data)) {}

Chunk::Chunk(const uint8_t* data, size_t size) : data_(data, data + size) {}

const ChunkId& Chunk::id() const {
    if (!id_) {
        // Compute content hash
        Hash128 hash;
#ifdef ELCACHE_USE_XXHASH
        XXH128_hash_t h = XXH3_128bits(data_.data(), data_.size());
        hash.low = h.low64;
        hash.high = h.high64;
#else
        // Simple hash fallback
        uint64_t h1 = 14695981039346656037ULL;
        uint64_t h2 = 14695981039346656037ULL ^ 0x9E3779B97F4A7C15ULL;
        for (size_t i = 0; i < data_.size(); ++i) {
            h1 ^= data_[i];
            h1 *= 1099511628211ULL;
            h2 ^= data_[i];
            h2 *= 1099511628211ULL;
            h2 = (h2 << 13) | (h2 >> 51);
        }
        hash.low = h1;
        hash.high = h2;
#endif
        id_ = ChunkId(hash);
    }
    return *id_;
}

ByteBuffer Chunk::read(uint64_t offset, uint64_t length) const {
    if (offset >= data_.size()) {
        return {};
    }
    
    size_t actual_length = std::min(static_cast<size_t>(length), 
                                     data_.size() - static_cast<size_t>(offset));
    return ByteBuffer(data_.begin() + offset, data_.begin() + offset + actual_length);
}

// ValueDescriptor implementation
std::vector<uint64_t> ValueDescriptor::chunks_for_range(const Range& range) const {
    std::vector<uint64_t> result;
    
    if (range.length == 0 || range.offset >= total_size) {
        return result;
    }
    
    uint64_t start_chunk = range.offset / CHUNK_SIZE;
    uint64_t end_offset = std::min(range.end(), total_size);
    uint64_t end_chunk = (end_offset > 0) ? ((end_offset - 1) / CHUNK_SIZE) : 0;
    
    for (uint64_t i = start_chunk; i <= end_chunk && i < chunks.size(); ++i) {
        result.push_back(i);
    }
    
    return result;
}

// PartialValue implementation
PartialValue::PartialValue(const ValueDescriptor& desc)
    : total_size_(desc.total_size)
    , chunk_availability_(desc.chunks.size(), false)
    , chunk_ids_(desc.chunks)
{}

bool PartialValue::has_chunk(uint64_t index) const {
    if (index >= chunk_availability_.size()) return false;
    return chunk_availability_[index];
}

void PartialValue::mark_chunk_available(uint64_t index, const ChunkId& id) {
    if (index >= chunk_availability_.size()) return;
    chunk_availability_[index] = true;
    chunk_ids_[index] = id;
}

void PartialValue::mark_chunk_unavailable(uint64_t index) {
    if (index >= chunk_availability_.size()) return;
    chunk_availability_[index] = false;
}

bool PartialValue::has_range(const Range& range) const {
    if (range.length == 0) return true;
    if (range.offset >= total_size_) return false;
    
    uint64_t start_chunk = range.offset / CHUNK_SIZE;
    uint64_t end_offset = std::min(range.end(), total_size_);
    uint64_t end_chunk = (end_offset > 0) ? ((end_offset - 1) / CHUNK_SIZE) : 0;
    
    for (uint64_t i = start_chunk; i <= end_chunk; ++i) {
        if (i >= chunk_availability_.size() || !chunk_availability_[i]) {
            return false;
        }
    }
    return true;
}

std::vector<Range> PartialValue::missing_ranges(const Range& range) const {
    std::vector<Range> missing;
    
    if (range.length == 0 || range.offset >= total_size_) {
        return missing;
    }
    
    uint64_t effective_end = std::min(range.end(), total_size_);
    uint64_t current = range.offset;
    
    while (current < effective_end) {
        uint64_t chunk_index = current / CHUNK_SIZE;
        uint64_t chunk_start = chunk_index * CHUNK_SIZE;
        uint64_t chunk_end = std::min(chunk_start + CHUNK_SIZE, total_size_);
        
        if (chunk_index >= chunk_availability_.size() || !chunk_availability_[chunk_index]) {
            // This chunk is missing
            uint64_t miss_start = std::max(current, chunk_start);
            uint64_t miss_end = std::min(effective_end, chunk_end);
            
            // Merge with previous if contiguous
            if (!missing.empty() && missing.back().end() == miss_start) {
                missing.back().length += (miss_end - miss_start);
            } else {
                missing.push_back({miss_start, miss_end - miss_start});
            }
        }
        
        current = chunk_end;
    }
    
    return missing;
}

std::vector<Range> PartialValue::available_ranges() const {
    std::vector<Range> available;
    
    uint64_t i = 0;
    while (i < chunk_availability_.size()) {
        if (chunk_availability_[i]) {
            uint64_t start = i * CHUNK_SIZE;
            uint64_t end = start;
            
            while (i < chunk_availability_.size() && chunk_availability_[i]) {
                end = std::min((i + 1) * CHUNK_SIZE, total_size_);
                ++i;
            }
            
            available.push_back({start, end - start});
        } else {
            ++i;
        }
    }
    
    return available;
}

double PartialValue::completion_ratio() const {
    if (chunk_availability_.empty()) return 0.0;
    
    size_t available = 0;
    for (bool b : chunk_availability_) {
        if (b) ++available;
    }
    
    return static_cast<double>(available) / chunk_availability_.size();
}

size_t PartialValue::available_bytes() const {
    size_t bytes = 0;
    for (size_t i = 0; i < chunk_availability_.size(); ++i) {
        if (chunk_availability_[i]) {
            if (i == chunk_availability_.size() - 1) {
                // Last chunk may be partial
                bytes += total_size_ - (i * CHUNK_SIZE);
            } else {
                bytes += CHUNK_SIZE;
            }
        }
    }
    return bytes;
}

}  // namespace elcache
