#pragma once

#include "git/Diff.h"
#include "git/ObjectId.h"
#include "util/LruCache.h"

#include <optional>

namespace gitbolt::git {

/// Cache for DiffResult objects, keyed by a pair of tree ObjectIds.
/// Diff computation is expensive, so caching saves significant time.
class DiffCache {
public:
    static constexpr size_t DEFAULT_MAX_SIZE = 500;

    explicit DiffCache(size_t maxSize = DEFAULT_MAX_SIZE);

    /// Look up a cached diff by old/new tree ids.
    std::optional<DiffResult> lookup(const ObjectId& oldTree,
                                     const ObjectId& newTree) const;

    /// Store a diff result.
    void store(const ObjectId& oldTree, const ObjectId& newTree,
               const DiffResult& result);

    /// Clear all cached diffs.
    void clear();

    /// Number of cached entries.
    size_t size() const;

private:
    /// Composite key combining two ObjectIds.
    struct KeyPair {
        ObjectId first;
        ObjectId second;

        bool operator==(const KeyPair& other) const {
            return first == other.first && second == other.second;
        }
    };

    struct KeyPairHash {
        size_t operator()(const KeyPair& k) const {
            ObjectId::Hash h;
            size_t seed = h(k.first);
            seed ^= h(k.second) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            return seed;
        }
    };

    mutable util::LruCache<KeyPair, DiffResult, KeyPairHash> cache_;
};

} // namespace gitbolt::git
