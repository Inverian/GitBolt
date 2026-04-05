#pragma once

#include "git/ObjectId.h"

#include <vector>

namespace gitbolt::git {

struct CherryPickResult {
    bool hasConflicts = false;
    ObjectId resultCommitId;
};

} // namespace gitbolt::git
