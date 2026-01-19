#include "elcache/types.hpp"
#include <random>
#include <sstream>
#include <iomanip>
#include <cstring>

// Use xxHash if available, otherwise fall back to simple hash
#ifdef ELCACHE_USE_XXHASH
#include <xxhash.h>
#else
// Simple FNV-1a hash fallback
namespace {
    uint64_t fnv1a_64(const void* data, size_t len) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        uint64_t hash = 14695981039346656037ULL;
        for (size_t i = 0; i < len; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ULL;
        }
        return hash;
    }
}
#endif

namespace elcache {

// Hash128 implementation
std::string Hash128::to_hex() const {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    oss << std::setw(16) << high << std::setw(16) << low;
    return oss.str();
}

Hash128 Hash128::from_hex(std::string_view hex) {
    Hash128 h;
    if (hex.size() >= 32) {
        auto high_str = std::string(hex.substr(0, 16));
        auto low_str = std::string(hex.substr(16, 16));
        h.high = std::stoull(high_str, nullptr, 16);
        h.low = std::stoull(low_str, nullptr, 16);
    }
    return h;
}

// CacheKey implementation
CacheKey::CacheKey(std::string_view key) : data_(key) {
    compute_hash();
}

void CacheKey::compute_hash() {
    if (data_.empty()) {
        hash_ = 0;
        hash128_ = Hash128{};
        return;
    }
    
#ifdef ELCACHE_USE_XXHASH
    hash_ = XXH3_64bits(data_.data(), data_.size());
    XXH128_hash_t h128 = XXH3_128bits(data_.data(), data_.size());
    hash128_.low = h128.low64;
    hash128_.high = h128.high64;
#else
    hash_ = fnv1a_64(data_.data(), data_.size());
    // Simple 128-bit extension
    hash128_.low = hash_;
    hash128_.high = fnv1a_64(data_.data(), data_.size()) ^ 0x9E3779B97F4A7C15ULL;
#endif
}

// NodeId implementation
NodeId NodeId::generate() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
    return NodeId(dis(gen));
}

NodeId NodeId::from_address(std::string_view addr, uint16_t port) {
    // Create deterministic ID from address
    std::string combined = std::string(addr) + ":" + std::to_string(port);
#ifdef ELCACHE_USE_XXHASH
    return NodeId(XXH3_64bits(combined.data(), combined.size()));
#else
    return NodeId(fnv1a_64(combined.data(), combined.size()));
#endif
}

std::string NodeId::to_string() const {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << id_;
    return oss.str();
}

// ChunkRange implementation
std::vector<ChunkRange> ChunkRange::compute(const Range& range, size_t chunk_size) {
    std::vector<ChunkRange> ranges;
    if (range.length == 0) return ranges;
    
    uint64_t current_offset = range.offset;
    uint64_t remaining = range.length;
    
    while (remaining > 0) {
        uint64_t chunk_index = current_offset / chunk_size;
        uint64_t offset_in_chunk = current_offset % chunk_size;
        uint64_t available_in_chunk = chunk_size - offset_in_chunk;
        uint64_t to_read = std::min(remaining, available_in_chunk);
        
        ranges.push_back({chunk_index, offset_in_chunk, to_read});
        
        current_offset += to_read;
        remaining -= to_read;
    }
    
    return ranges;
}

// Status helpers
const char* error_code_string(ErrorCode code) {
    switch (code) {
        case ErrorCode::Ok: return "Ok";
        case ErrorCode::KeyTooLarge: return "Key too large";
        case ErrorCode::ValueTooLarge: return "Value too large";
        case ErrorCode::NotFound: return "Not found";
        case ErrorCode::PartialData: return "Partial data";
        case ErrorCode::NetworkError: return "Network error";
        case ErrorCode::DiskError: return "Disk error";
        case ErrorCode::Timeout: return "Timeout";
        case ErrorCode::InvalidArgument: return "Invalid argument";
        case ErrorCode::OutOfMemory: return "Out of memory";
        case ErrorCode::InternalError: return "Internal error";
        default: return "Unknown error";
    }
}

}  // namespace elcache
