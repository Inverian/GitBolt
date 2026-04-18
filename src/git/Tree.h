#pragma once

#include "git/ObjectId.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gitbolt::git {

/// One entry in a git tree (directory listing). Returned in the
/// flat depth-first order produced by libgit2's git_tree_walk in
/// pre-order: the parent directory always appears before any of
/// its children, so consumers can build a hierarchical model in
/// a single pass without forward references.
struct TreeEntry {
    std::string name;     ///< Basename, e.g. "Repository.cpp"
    std::string path;     ///< Full path from the root tree
    ObjectId    oid;      ///< Blob OID for files, tree OID for directories
    bool        isTree = false;
    uint32_t    mode   = 0;     ///< git filemode (100644, 100755, 40000, ...)
    uint64_t    size   = 0;     ///< Blob size in bytes; 0 for directories
};

} // namespace gitbolt::git
