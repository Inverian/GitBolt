#include "git/CommitCache.h"

namespace gitbolt::git {

CommitCache::CommitCache(size_t maxSize) : cache_(maxSize) {}

std::optional<CommitData> CommitCache::lookup(const ObjectId& id) const {
    return cache_.get(id);
}

void CommitCache::store(const CommitData& commit) {
    cache_.put(commit.id, commit);
}

void CommitCache::invalidate(const ObjectId& id) {
    cache_.remove(id);
}

void CommitCache::clear() {
    cache_.clear();
}

size_t CommitCache::size() const {
    return cache_.size();
}

} // namespace gitbolt::git
