#pragma once

namespace gitbolt::git {

struct RevertResult {
    bool hasConflicts = false;
    /// The revert changed nothing: the commit's changes aren't on the
    /// branch (reverted already, or an empty commit). Nothing is left
    /// in progress (no REVERT_HEAD, no MERGE_MSG).
    bool nothingToRevert = false;
};

} // namespace gitbolt::git
