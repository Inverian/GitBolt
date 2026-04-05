#pragma once

#include "gitbolt/core/object_id.h"

#include <string>
#include <vector>

namespace gitbolt::core {

enum class RebaseOperationType {
    Pick,
    Reword,
    Edit,
    Squash,
    Fixup,
    Drop,
};

struct RebaseOperation {
    RebaseOperationType type;
    ObjectId commitId;
    std::string message;
};

struct RebasePlan {
    ObjectId onto;
    std::vector<RebaseOperation> operations;
};

enum class RebaseState {
    NotStarted,
    InProgress,
    Paused,
    ConflictPaused,
    Completed,
    Aborted,
};

} // namespace gitbolt::core
