#pragma once

#include "git/Commit.h"
#include "git/ObjectId.h"
#include "util/LruCache.h"

#include <optional>

namespace gitbolt::git {

/// Specialized cache for CommitData objects.
/// Thread-safe for concurrent background revwalk operations.
class CommitCache {
public:
    static constexpr size_t DEFAULT_MAX_SIZE = 10000;

    explicit CommitCache(size_t maxSize = DEFAULT_MAX_SIZE);

    /// Look up a commit by its ObjectId.
    std::optional<CommitData> lookup(const ObjectId& id) const;

    /// Store a commit in the cache.
    void store(const CommitData& commit);

    /// Remove a specific commit from the cache.
    void invalidate(const ObjectId& id);

    /// Clear all cached commits.
    void clear();

    /// Number of cached entries.
    size_t size() const;

private:
    mutable util::LruCache<ObjectId, CommitData, ObjectId::Hash> cache_;
};

} // namespace gitbolt::git
