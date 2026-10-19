#pragma once

#include "git/ObjectId.h"

#include <cstdint>
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
    // Each side's file mode as git records it (0100644, 0100755,
    // 0120000 for a symbolic link, 0160000 for a submodule), 0 for a
    // side that doesn't exist.
    uint32_t ancestorMode = 0;
    uint32_t oursMode = 0;
    uint32_t theirsMode = 0;

    static constexpr uint32_t kSymlinkMode = 0120000;

    /// Ours or theirs is a symbolic link, whose content is the path it
    /// points to. Such a conflict is resolved by picking a side, never
    /// by editing: see Repository::resolveConflict.
    bool isSymlink() const {
        return oursMode == kSymlinkMode || theirsMode == kSymlinkMode;
    }
};

/// The side of a conflict a resolution takes as it is: Accept Ours or
/// Accept Theirs on a conflict on a symbolic link.
enum class ConflictSide {
    Ours,
    Theirs,
};

struct MergeResult {
    MergeAnalysis analysis;
    bool hasConflicts = false;
    std::vector<MergeConflictEntry> conflicts;
    ObjectId resultTreeId;
};

} // namespace gitbolt::git
