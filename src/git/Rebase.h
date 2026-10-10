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
    /// The commit's message, as it is now. Only its first line goes
    /// into git's todo list.
    std::string message;
    /// Reword only: the message the commit gets instead. Empty keeps
    /// its own.
    std::string newMessage = {};
};

struct RebasePlan {
    ObjectId onto;
    /// Where HEAD was when the plan was made: the commit, and the
    /// branch checked out (its short name; empty when HEAD was
    /// detached). git rebases whatever HEAD is when it runs, with this
    /// plan's todo list in place of its own, so a plan made for another
    /// HEAD would drop the commits it doesn't list, or rewrite another
    /// branch with them. GitService refuses to run it then.
    ObjectId head;
    std::string branch;
    /// Newest first, as the log and the Rebase dialog list commits
    /// (git's todo list runs the other way).
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
