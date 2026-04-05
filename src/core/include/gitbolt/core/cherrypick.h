#pragma once

#include "gitbolt/core/object_id.h"

#include <vector>

namespace gitbolt::core {

struct CherryPickResult {
    bool hasConflicts = false;
    ObjectId resultCommitId;
};

} // namespace gitbolt::core
