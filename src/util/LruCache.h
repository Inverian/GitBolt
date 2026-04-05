#pragma once

#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace gitbolt::util {

/// Thread-safe LRU cache with configurable maximum size.
template <typename Key, typename Value, typename Hash = std::hash<Key>>
class LruCache {
public:
    explicit LruCache(size_t maxSize = 1000) : maxSize_(maxSize) {}

    /// Look up a value by key.  Returns std::nullopt on miss.
    std::optional<Value> get(const Key& key) {
        std::lock_guard lock(mutex_);
        auto it = map_.find(key);
        if (it == map_.end()) return std::nullopt;

        // Move accessed item to front (most-recently-used)
        items_.splice(items_.begin(), items_, it->second);
        return it->second->second;
    }

    /// Insert or update a key-value pair.
    void put(const Key& key, const Value& value) {
        std::lock_guard lock(mutex_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            // Update existing entry and move to front
            it->second->second = value;
            items_.splice(items_.begin(), items_, it->second);
            return;
        }

        // Insert new entry at front
        items_.emplace_front(key, value);
        map_[key] = items_.begin();

        // Evict least-recently-used if over capacity
        while (map_.size() > maxSize_) {
            auto& back = items_.back();
            map_.erase(back.first);
            items_.pop_back();
        }
    }

    /// Remove a specific key.
    void remove(const Key& key) {
        std::lock_guard lock(mutex_);
        auto it = map_.find(key);
        if (it == map_.end()) return;
        items_.erase(it->second);
        map_.erase(it);
    }

    /// Remove all entries.
    void clear() {
        std::lock_guard lock(mutex_);
        map_.clear();
        items_.clear();
    }

    /// Current number of entries.
    size_t size() const {
        std::lock_guard lock(mutex_);
        return map_.size();
    }

    /// Change the maximum capacity (may evict entries).
    void setMaxSize(size_t maxSize) {
        std::lock_guard lock(mutex_);
        maxSize_ = maxSize;
        while (map_.size() > maxSize_) {
            auto& back = items_.back();
            map_.erase(back.first);
            items_.pop_back();
        }
    }

private:
    using ListType = std::list<std::pair<Key, Value>>;
    using MapType = std::unordered_map<Key, typename ListType::iterator, Hash>;

    size_t maxSize_;
    ListType items_;
    MapType map_;
    mutable std::mutex mutex_;
};

} // namespace gitbolt::util
