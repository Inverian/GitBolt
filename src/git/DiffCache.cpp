#include "git/DiffCache.h"

namespace gitbolt::git {

DiffCache::DiffCache(size_t maxSize) : cache_(maxSize) {}

std::optional<DiffResult> DiffCache::lookup(const ObjectId& oldTree,
                                            const ObjectId& newTree) const {
    return cache_.get(KeyPair{oldTree, newTree});
}

void DiffCache::store(const ObjectId& oldTree, const ObjectId& newTree,
                      const DiffResult& result) {
    cache_.put(KeyPair{oldTree, newTree}, result);
}

void DiffCache::clear() {
    cache_.clear();
}

size_t DiffCache::size() const {
    return cache_.size();
}

} // namespace gitbolt::git
