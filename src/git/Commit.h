#pragma once

#include "git/ObjectId.h"
#include "git/Signature.h"

#include <string>
#include <vector>

namespace gitbolt::git {

struct CommitData {
    ObjectId id;
    ObjectId treeId;
    std::vector<ObjectId> parentIds;
    Signature author;
    Signature committer;
    std::string message;
    std::string summary;

    size_t parentCount() const { return parentIds.size(); }
    bool isMerge() const { return parentIds.size() > 1; }
    bool isRoot() const { return parentIds.empty(); }
};

} // namespace gitbolt::git
