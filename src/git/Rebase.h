#pragma once

#include "git/ObjectId.h"

#include <string>
#include <vector>

namespace gitbolt::git {

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

} // namespace gitbolt::git
