#pragma once

#include "git/ObjectId.h"

#include <vector>

namespace gitbolt::git {

struct CherryPickResult {
    bool hasConflicts = false;
    /// The pick changed nothing: its changes are on the branch already.
    /// Nothing is left in progress (no CHERRY_PICK_HEAD, no MERGE_MSG).
    bool alreadyApplied = false;
    ObjectId resultCommitId;
};

} // namespace gitbolt::git
