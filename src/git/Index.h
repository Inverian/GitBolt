#pragma once

#include "git/Error.h"

#include <string>
#include <vector>

struct git_repository;

namespace gitbolt::git {

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
