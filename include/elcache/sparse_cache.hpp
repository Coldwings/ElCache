#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace elcache {

// Sparse entry for position-based writes
// Tracks which byte ranges have been written
struct SparseEntry {
    uint64_t total_size;
    std::vector<uint8_t> data;
    std::vector<bool> written;  // Track which byte ranges are written
    bool finalized;
    
    explicit SparseEntry(uint64_t size) 
        : total_size(size)
        , data(size, 0)
        , written(size, false)
        , finalized(false) 
    {}
    
    bool write_range(uint64_t offset, const uint8_t* buf, size_t len) {
        if (offset + len > total_size) return false;
        std::memcpy(data.data() + offset, buf, len);
        for (size_t i = 0; i < len; i++) {
            written[offset + i] = true;
        }
        return true;
    }
    
    bool is_complete() const {
        for (bool w : written) {
            if (!w) return false;
        }
        return true;
    }
    
    // Get percentage of bytes written
    double completion_percent() const {
        if (total_size == 0) return 100.0;
        size_t count = 0;
        for (bool w : written) {
            if (w) count++;
        }
        return static_cast<double>(count) / total_size * 100.0;
    }
};

// Thread-safe sparse cache for tracking incomplete entries
class SparseCache {
public:
    // Create a new sparse entry
    // Returns false if entry already exists
    bool create(const std::string& key, uint64_t total_size) {
        std::unique_lock lock(mutex_);
        if (entries_.find(key) != entries_.end()) {
            return false;  // Already exists
        }
        entries_.emplace(key, std::make_unique<SparseEntry>(total_size));
        return true;
    }
    
    // Write data at a specific offset
    // Returns false if entry doesn't exist or range is invalid
    bool write_range(const std::string& key, uint64_t offset, const uint8_t* data, size_t len) {
        std::unique_lock lock(mutex_);
        auto it = entries_.find(key);
        if (it == entries_.end()) return false;
        return it->second->write_range(offset, data, len);
    }
    
    // Finalize a sparse entry
    // Returns: 0 = success (data moved to output), 1 = not found, 2 = incomplete
    int finalize(const std::string& key, std::vector<uint8_t>& out_data) {
        std::unique_lock lock(mutex_);
        auto it = entries_.find(key);
        if (it == entries_.end()) return 1;  // Not found
        if (!it->second->is_complete()) return 2;  // Not complete
        
        // Move data out
        out_data = std::move(it->second->data);
        entries_.erase(it);
        return 0;  // Success
    }
    
    // Check if a sparse entry exists
    bool exists(const std::string& key) const {
        std::shared_lock lock(mutex_);
        return entries_.find(key) != entries_.end();
    }
    
    // Get total size of a sparse entry
    uint64_t get_total_size(const std::string& key) const {
        std::shared_lock lock(mutex_);
        auto it = entries_.find(key);
        if (it == entries_.end()) return 0;
        return it->second->total_size;
    }
    
    // Get completion percentage
    double get_completion(const std::string& key) const {
        std::shared_lock lock(mutex_);
        auto it = entries_.find(key);
        if (it == entries_.end()) return 0.0;
        return it->second->completion_percent();
    }
    
    // Remove a sparse entry without finalizing (abort)
    bool remove(const std::string& key) {
        std::unique_lock lock(mutex_);
        return entries_.erase(key) > 0;
    }
    
    // Get count of active sparse entries
    size_t count() const {
        std::shared_lock lock(mutex_);
        return entries_.size();
    }
    
private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<SparseEntry>> entries_;
};

// Global sparse cache instance
SparseCache& global_sparse_cache();

}  // namespace elcache
