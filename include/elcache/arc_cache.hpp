#pragma once

#include "elcache/types.hpp"
#include "elcache/chunk.hpp"
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <list>
#include <atomic>

namespace elcache {

// ARC (Adaptive Replacement Cache) implementation
// Balances between recency (LRU) and frequency (LFU)
// Paper: "ARC: A Self-Tuning, Low Overhead Replacement Cache"

template<typename K, typename V, typename Hash = std::hash<K>>
class ARCCache {
public:
    using key_type = K;
    using value_type = V;
    using size_func = std::function<size_t(const V&)>;
    
    explicit ARCCache(size_t capacity, size_func size_fn = nullptr)
        : capacity_(capacity)
        , size_fn_(size_fn ? size_fn : [](const V&) { return 1; })
        , p_(0)
        , current_size_(0)
    {}
    
    // Lookup value, returns nullptr if not found
    // Promotes entry if found
    V* get(const K& key) {
        std::unique_lock lock(mutex_);
        return get_impl(key);
    }
    
    // Insert or update value
    // Returns evicted entries (caller may need to clean up)
    std::vector<std::pair<K, V>> put(const K& key, V value) {
        std::unique_lock lock(mutex_);
        return put_impl(key, std::move(value));
    }
    
    // Remove entry
    bool remove(const K& key) {
        std::unique_lock lock(mutex_);
        return remove_impl(key);
    }
    
    // Check existence without promoting
    bool contains(const K& key) const {
        std::shared_lock lock(mutex_);
        return t1_.contains(key) || t2_.contains(key);
    }
    
    // Current size
    size_t size() const noexcept {
        return current_size_.load(std::memory_order_relaxed);
    }
    
    size_t capacity() const noexcept { return capacity_; }
    
    // Statistics
    struct Stats {
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t t1_size = 0;
        uint64_t t2_size = 0;
        uint64_t b1_size = 0;
        uint64_t b2_size = 0;
        double p = 0;  // Adaptation parameter
    };
    
    Stats stats() const {
        std::shared_lock lock(mutex_);
        Stats s;
        s.hits = hits_.load();
        s.misses = misses_.load();
        s.t1_size = t1_.size();
        s.t2_size = t2_.size();
        s.b1_size = b1_.size();
        s.b2_size = b2_.size();
        s.p = p_;
        return s;
    }
    
    // Clear all entries
    void clear() {
        std::unique_lock lock(mutex_);
        t1_.clear();
        t2_.clear();
        b1_.clear();
        b2_.clear();
        p_ = 0;
        current_size_ = 0;
    }
    
    // Iterate over all entries (for diagnostics)
    template<typename F>
    void for_each(F&& func) const {
        std::shared_lock lock(mutex_);
        for (const auto& [key, entry] : t1_.map()) {
            func(key, entry.value);
        }
        for (const auto& [key, entry] : t2_.map()) {
            func(key, entry.value);
        }
    }
    
private:
    // LRU list with fast lookup
    template<typename T>
    class LRUList {
    public:
        struct Entry {
            K key;
            T value;
            size_t size;
        };
        
        using list_type = std::list<Entry>;
        using iterator = typename list_type::iterator;
        using map_type = std::unordered_map<K, iterator, Hash>;
        
        bool contains(const K& key) const {
            return map_.find(key) != map_.end();
        }
        
        T* get(const K& key) {
            auto it = map_.find(key);
            if (it == map_.end()) return nullptr;
            return &it->second->value;
        }
        
        iterator find(const K& key) {
            auto it = map_.find(key);
            if (it == map_.end()) return list_.end();
            return it->second;
        }
        
        void push_front(const K& key, T value, size_t size) {
            list_.push_front({key, std::move(value), size});
            map_[key] = list_.begin();
        }
        
        void move_to_front(iterator it) {
            list_.splice(list_.begin(), list_, it);
        }
        
        Entry pop_back() {
            Entry e = std::move(list_.back());
            map_.erase(e.key);
            list_.pop_back();
            return e;
        }
        
        void erase(const K& key) {
            auto it = map_.find(key);
            if (it != map_.end()) {
                list_.erase(it->second);
                map_.erase(it);
            }
        }
        
        void erase(iterator it) {
            map_.erase(it->key);
            list_.erase(it);
        }
        
        bool empty() const { return list_.empty(); }
        size_t size() const { return list_.size(); }
        
        void clear() {
            list_.clear();
            map_.clear();
        }
        
        const map_type& map() const { return map_; }
        
        // Get total size of all entries
        size_t total_size() const {
            size_t total = 0;
            for (const auto& e : list_) {
                total += e.size;
            }
            return total;
        }
        
        iterator begin() { return list_.begin(); }
        iterator end() { return list_.end(); }
        
    private:
        list_type list_;
        map_type map_;
    };
    
    // Ghost list (stores only keys, no values)
    class GhostList {
    public:
        bool contains(const K& key) const {
            return map_.find(key) != map_.end();
        }
        
        void push_front(const K& key) {
            list_.push_front(key);
            map_[key] = list_.begin();
        }
        
        K pop_back() {
            K key = list_.back();
            map_.erase(key);
            list_.pop_back();
            return key;
        }
        
        void erase(const K& key) {
            auto it = map_.find(key);
            if (it != map_.end()) {
                list_.erase(it->second);
                map_.erase(it);
            }
        }
        
        bool empty() const { return list_.empty(); }
        size_t size() const { return list_.size(); }
        
        void clear() {
            list_.clear();
            map_.clear();
        }
        
    private:
        std::list<K> list_;
        std::unordered_map<K, typename std::list<K>::iterator, Hash> map_;
    };
    
    V* get_impl(const K& key) {
        // Case 1: Key in T1 (recently accessed once)
        if (auto it = t1_.find(key); it != t1_.end()) {
            // Move to T2 (frequently accessed)
            V value = std::move(it->value);
            size_t size = it->size;
            t1_.erase(it);
            t2_.push_front(key, std::move(value), size);
            hits_++;
            return t2_.get(key);
        }
        
        // Case 2: Key in T2 (frequently accessed)
        if (auto it = t2_.find(key); it != t2_.end()) {
            t2_.move_to_front(it);
            hits_++;
            return &it->value;
        }
        
        misses_++;
        return nullptr;
    }
    
    std::vector<std::pair<K, V>> put_impl(const K& key, V value) {
        std::vector<std::pair<K, V>> evicted;
        size_t value_size = size_fn_(value);
        
        // Case 1: Key already in T1
        if (auto it = t1_.find(key); it != t1_.end()) {
            current_size_ -= it->size;
            it->value = std::move(value);
            it->size = value_size;
            current_size_ += value_size;
            // Move to T2
            V v = std::move(it->value);
            t1_.erase(it);
            t2_.push_front(key, std::move(v), value_size);
            return evicted;
        }
        
        // Case 2: Key already in T2
        if (auto it = t2_.find(key); it != t2_.end()) {
            current_size_ -= it->size;
            it->value = std::move(value);
            it->size = value_size;
            current_size_ += value_size;
            t2_.move_to_front(it);
            return evicted;
        }
        
        // Case 3: Key in B1 (ghost list for T1)
        if (b1_.contains(key)) {
            // Adapt: increase preference for T1
            double delta = b2_.size() >= b1_.size() 
                ? 1.0 
                : static_cast<double>(b2_.size()) / b1_.size();
            p_ = std::min(p_ + delta, static_cast<double>(capacity_));
            
            b1_.erase(key);
            evict_for_size(value_size, evicted);
            t2_.push_front(key, std::move(value), value_size);
            current_size_ += value_size;
            return evicted;
        }
        
        // Case 4: Key in B2 (ghost list for T2)
        if (b2_.contains(key)) {
            // Adapt: decrease preference for T1
            double delta = b1_.size() >= b2_.size()
                ? 1.0
                : static_cast<double>(b1_.size()) / b2_.size();
            p_ = std::max(p_ - delta, 0.0);
            
            b2_.erase(key);
            evict_for_size(value_size, evicted);
            t2_.push_front(key, std::move(value), value_size);
            current_size_ += value_size;
            return evicted;
        }
        
        // Case 5: Key not in cache or ghost lists
        size_t total_ghost = b1_.size() + b2_.size();
        if (t1_.size() + b1_.size() >= capacity_) {
            // B1 is full, remove oldest from B1
            if (!b1_.empty()) {
                b1_.pop_back();
            }
        } else if (t1_.size() + t2_.size() + total_ghost >= capacity_ * 2) {
            // Total is full, remove from B2
            if (!b2_.empty()) {
                b2_.pop_back();
            }
        }
        
        evict_for_size(value_size, evicted);
        t1_.push_front(key, std::move(value), value_size);
        current_size_ += value_size;
        
        return evicted;
    }
    
    void evict_for_size(size_t needed, std::vector<std::pair<K, V>>& evicted) {
        while (current_size_ + needed > capacity_ && (!t1_.empty() || !t2_.empty())) {
            evict_one(evicted);
        }
    }
    
    void evict_one(std::vector<std::pair<K, V>>& evicted) {
        size_t t1_size = t1_.total_size();
        
        if (!t1_.empty() && (t1_size > static_cast<size_t>(p_) || t2_.empty())) {
            // Evict from T1
            auto entry = t1_.pop_back();
            current_size_ -= entry.size;
            b1_.push_front(entry.key);
            evicted.emplace_back(entry.key, std::move(entry.value));
        } else if (!t2_.empty()) {
            // Evict from T2
            auto entry = t2_.pop_back();
            current_size_ -= entry.size;
            b2_.push_front(entry.key);
            evicted.emplace_back(entry.key, std::move(entry.value));
        }
    }
    
    bool remove_impl(const K& key) {
        if (auto it = t1_.find(key); it != t1_.end()) {
            current_size_ -= it->size;
            t1_.erase(it);
            return true;
        }
        if (auto it = t2_.find(key); it != t2_.end()) {
            current_size_ -= it->size;
            t2_.erase(it);
            return true;
        }
        b1_.erase(key);
        b2_.erase(key);
        return false;
    }
    
    mutable std::shared_mutex mutex_;
    size_t capacity_;
    size_func size_fn_;
    
    // T1: Recently accessed (recency)
    // T2: Frequently accessed (frequency)
    LRUList<V> t1_;
    LRUList<V> t2_;
    
    // B1, B2: Ghost lists (keys only, for adaptation)
    GhostList b1_;
    GhostList b2_;
    
    // Adaptation parameter: target size for T1
    double p_;
    
    std::atomic<size_t> current_size_;
    std::atomic<uint64_t> hits_{0};
    std::atomic<uint64_t> misses_{0};
};

}  // namespace elcache
