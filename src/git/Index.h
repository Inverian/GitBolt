#pragma once

#include "git/Error.h"

#include <string>
#include <vector>

struct git_index;
struct git_repository;

namespace gitbolt::git {

/// git_repository_index(), reloaded first if the index file changed
/// on disk since libgit2 last read it. libgit2 keeps its copy of the
/// index for the life of the repository, and git (the CLI: merge,
/// rebase, cherry-pick, `git add` in a terminal) writes the file
/// behind its back: staging into the stale copy and writing it out
/// undid what git had staged — a conflicted merge's cleanly merged
/// files, say — and committing it left them out of the commit.
/// Returns a libgit2 error code; free *out with git_index_free.
int freshIndex(git_index** out, git_repository* repo);

class Index {
public:
    explicit Index(git_repository* repo);

    Result<void> addPath(const std::string& path);
    Result<void> addAll();
    Result<void> removePath(const std::string& path);
    Result<void> removeAll();

    // NOTE: there is deliberately no hunk-level API here. Partial
    // staging is implemented as patch application instead — see
    // git/PatchBuilder.h (+ GitService::applyPatchToIndex), which
    // builds a minimal unified patch from the diff the user is
    // looking at and runs `git apply --cached`.

    Result<void> write();

private:
    git_repository* repo_;
};

} // namespace gitbolt::git
