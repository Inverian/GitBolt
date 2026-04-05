#pragma once

#include "gitbolt/core/object_id.h"
#include "gitbolt/core/signature.h"

#include <string>
#include <vector>

namespace gitbolt::core {

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

} // namespace gitbolt::core
