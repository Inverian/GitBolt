#pragma once

#include "gitbolt/core/object_id.h"

#include <string>

namespace gitbolt::core {

enum class BranchType {
    Local,
    Remote,
};

struct BranchInfo {
    std::string name;
    std::string fullRefName;
    ObjectId tipId;
    BranchType type;
    std::string upstream;
    bool isHead = false;
    int aheadCount = 0;
    int behindCount = 0;
};

} // namespace gitbolt::core
