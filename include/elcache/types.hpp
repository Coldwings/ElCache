#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <memory>
#include <chrono>
#include <optional>
#include <variant>
#include <array>
#include <functional>
#include <atomic>

namespace elcache {

// Constants
constexpr size_t MAX_KEY_SIZE = 8 * 1024;           // 8KB
constexpr size_t MAX_VALUE_SIZE = 20ULL * 1024 * 1024 * 1024 * 1024;  // 20TB
constexpr size_t CHUNK_SIZE = 4 * 1024 * 1024;      // 4MB
constexpr size_t MAX_CHUNKS_PER_VALUE = (MAX_VALUE_SIZE + CHUNK_SIZE - 1) / CHUNK_SIZE;

// Forward declarations
class CacheKey;
class ChunkId;
class NodeId;

// Time types
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration = Clock::duration;
using SystemClock = std::chrono::system_clock;
using Timestamp = SystemClock::time_point;

// Hash type (128-bit for content addressing)
struct Hash128 {
    uint64_t low = 0;
    uint64_t high = 0;
    
    bool operator==(const Hash128& other) const noexcept {
        return low == other.low && high == other.high;
    }
    
    bool operator<(const Hash128& other) const noexcept {
        return high < other.high || (high == other.high && low < other.low);
    }
    
    bool is_zero() const noexcept { return low == 0 && high == 0; }
    
    std::string to_hex() const;
    static Hash128 from_hex(std::string_view hex);
};

// Cache key with efficient storage
class CacheKey {
public:
    CacheKey() = default;
    explicit CacheKey(std::string_view key);
    
    std::string_view view() const noexcept { return data_; }
    const std::string& str() const noexcept { return data_; }
    size_t size() const noexcept { return data_.size(); }
    bool empty() const noexcept { return data_.empty(); }
    
    // Pre-computed hash for fast lookups
    uint64_t hash() const noexcept { return hash_; }
    Hash128 hash128() const noexcept { return hash128_; }
    
    bool operator==(const CacheKey& other) const noexcept {
        return hash_ == other.hash_ && data_ == other.data_;
    }
    
private:
    std::string data_;
    uint64_t hash_ = 0;
    Hash128 hash128_;
    
    void compute_hash();
};

// Chunk identifier (content-addressed)
class ChunkId {
public:
    ChunkId() = default;
    explicit ChunkId(const Hash128& hash) : hash_(hash) {}
    
    const Hash128& hash() const noexcept { return hash_; }
    bool is_valid() const noexcept { return !hash_.is_zero(); }
    
    bool operator==(const ChunkId& other) const noexcept {
        return hash_ == other.hash_;
    }
    
    bool operator<(const ChunkId& other) const noexcept {
        return hash_ < other.hash_;
    }
    
    std::string to_string() const { return hash_.to_hex(); }
    
private:
    Hash128 hash_;
};

// Node identifier
class NodeId {
public:
    NodeId() = default;
    explicit NodeId(uint64_t id) : id_(id) {}
    static NodeId generate();
    static NodeId from_address(std::string_view addr, uint16_t port);
    
    uint64_t value() const noexcept { return id_; }
    bool is_valid() const noexcept { return id_ != 0; }
    
    bool operator==(const NodeId& other) const noexcept { return id_ == other.id_; }
    bool operator<(const NodeId& other) const noexcept { return id_ < other.id_; }
    
    std::string to_string() const;
    
private:
    uint64_t id_ = 0;
};

// Range for partial reads
struct Range {
    uint64_t offset = 0;
    uint64_t length = 0;
    
    uint64_t end() const noexcept { return offset + length; }
    
    bool overlaps(const Range& other) const noexcept {
        return offset < other.end() && other.offset < end();
    }
    
    Range intersect(const Range& other) const noexcept {
        uint64_t start = std::max(offset, other.offset);
        uint64_t stop = std::min(end(), other.end());
        if (start >= stop) return {0, 0};
        return {start, stop - start};
    }
    
    bool contains(const Range& other) const noexcept {
        return offset <= other.offset && end() >= other.end();
    }
    
    bool is_empty() const noexcept { return length == 0; }
};

// Chunk range mapping
struct ChunkRange {
    uint64_t chunk_index;      // Which chunk in the value
    uint64_t offset_in_chunk;  // Offset within the chunk
    uint64_t length;           // Length to read from this chunk
    
    static std::vector<ChunkRange> compute(const Range& range, size_t chunk_size = CHUNK_SIZE);
};

// Cache entry metadata
struct CacheMetadata {
    uint64_t total_size = 0;           // Total value size
    uint64_t chunk_count = 0;          // Number of chunks
    Timestamp created_at;              // Creation time
    Timestamp last_accessed;           // Last access time
    std::optional<Timestamp> expires_at;  // Optional expiration
    uint32_t flags = 0;                // User-defined flags
    
    bool is_expired() const noexcept {
        if (!expires_at) return false;
        return SystemClock::now() >= *expires_at;
    }
};

// Result types
enum class CacheResult {
    Hit,
    Miss,
    PartialHit,
    Error,
    Expired,
    NotFound
};

// Error codes
enum class ErrorCode {
    Ok = 0,
    KeyTooLarge,
    ValueTooLarge,
    NotFound,
    PartialData,
    NetworkError,
    DiskError,
    Timeout,
    InvalidArgument,
    OutOfMemory,
    InternalError
};

// Status wrapper
class Status {
public:
    Status() : code_(ErrorCode::Ok) {}
    explicit Status(ErrorCode code, std::string msg = {})
        : code_(code), message_(std::move(msg)) {}
    
    static Status make_ok() { return Status(); }
    static Status error(ErrorCode code, std::string msg = {}) {
        return Status(code, std::move(msg));
    }
    
    bool ok() const noexcept { return code_ == ErrorCode::Ok; }
    bool is_ok() const noexcept { return code_ == ErrorCode::Ok; }
    bool is_error() const noexcept { return code_ != ErrorCode::Ok; }
    explicit operator bool() const noexcept { return is_ok(); }
    
    ErrorCode code() const noexcept { return code_; }
    const std::string& message() const noexcept { return message_; }
    
private:
    ErrorCode code_;
    std::string message_;
};

// Buffer types
using ByteBuffer = std::vector<uint8_t>;
using ByteView = std::span<const uint8_t>;
using MutableByteView = std::span<uint8_t>;

// Callbacks
using ReadCallback = std::function<void(Status, ByteBuffer)>;
using WriteCallback = std::function<void(Status)>;

}  // namespace elcache

// Hash specializations for standard containers
namespace std {

template<>
struct hash<elcache::CacheKey> {
    size_t operator()(const elcache::CacheKey& k) const noexcept {
        return k.hash();
    }
};

template<>
struct hash<elcache::ChunkId> {
    size_t operator()(const elcache::ChunkId& c) const noexcept {
        return c.hash().low ^ c.hash().high;
    }
};

template<>
struct hash<elcache::NodeId> {
    size_t operator()(const elcache::NodeId& n) const noexcept {
        return n.value();
    }
};

template<>
struct hash<elcache::Hash128> {
    size_t operator()(const elcache::Hash128& h) const noexcept {
        return h.low ^ h.high;
    }
};

}  // namespace std
