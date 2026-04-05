#include "git/Status.h"

namespace gitbolt::git {

bool StatusEntry::isStaged() const {
    return hasFlag(status, FileStatus::IndexNew) ||
           hasFlag(status, FileStatus::IndexModified) ||
           hasFlag(status, FileStatus::IndexDeleted) ||
           hasFlag(status, FileStatus::IndexRenamed) ||
           hasFlag(status, FileStatus::IndexTypeChange);
}

bool StatusEntry::isWorkingTree() const {
    return hasFlag(status, FileStatus::WtNew) ||
           hasFlag(status, FileStatus::WtModified) ||
           hasFlag(status, FileStatus::WtDeleted) ||
           hasFlag(status, FileStatus::WtTypeChange) ||
           hasFlag(status, FileStatus::WtRenamed) ||
           hasFlag(status, FileStatus::WtUnreadable);
}

bool StatusEntry::isConflicted() const {
    return hasFlag(status, FileStatus::Conflicted);
}

bool StatusEntry::isUntracked() const {
    return hasFlag(status, FileStatus::WtNew) && !isStaged();
}

} // namespace gitbolt::git
