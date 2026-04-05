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
    Result<void> addByHunk(const std::string& path, const std::vector<int>& hunkIndices);

    Result<void> write();

private:
    git_repository* repo_;
};

} // namespace gitbolt::git
