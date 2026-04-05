#pragma once

#include <string>

namespace gitbolt::git {

struct WorktreeInfo {
    std::string name;
    std::string path;
    std::string branch;
    bool isLocked = false;
    bool isPrunable = false;
};

} // namespace gitbolt::git
