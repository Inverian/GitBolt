#pragma once

#include "git/ObjectId.h"

#include <string>
#include <vector>

namespace gitbolt::git {

enum class MergePreference {
    Normal,
    FastForwardOnly,
    NoFastForward,
};

enum class MergeAnalysis {
    Normal,
    UpToDate,
    FastForward,
    Unborn,
};

/// Repository-wide in-progress operation state (git_repository_state).
/// Determines which abort command applies when the user bails out of
/// a conflicted operation: merge --abort, cherry-pick --abort, etc.
enum class RepoState {
    None,
    Merge,
    Revert,
    CherryPick,
    Rebase,
    Other,
};

struct MergeConflictEntry {
    std::string path;
    std::string ancestorContent;
    std::string oursContent;
    std::string theirsContent;
    ObjectId ancestorId;
    ObjectId oursId;
    ObjectId theirsId;
};

struct MergeResult {
    MergeAnalysis analysis;
    bool hasConflicts = false;
    std::vector<MergeConflictEntry> conflicts;
    ObjectId resultTreeId;
};

} // namespace gitbolt::git
