#pragma once

#include "git/ObjectId.h"

#include <string>

namespace gitbolt::git {

enum class SubmoduleStatus {
    Clean,
    Dirty,
    Uninitialized,
    OutOfDate,
    Added,
    Deleted,
    Modified,
};

struct SubmoduleInfo {
    std::string name;
    std::string path;
    std::string url;
    std::string branch;
    ObjectId headId;
    ObjectId indexId;
    SubmoduleStatus status;
};

} // namespace gitbolt::git
