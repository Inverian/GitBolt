#include "git/Index.h"
#include <git2.h>

namespace gitbolt::git {

Index::Index(git_repository* repo) : repo_(repo) {}

Result<void> Index::addPath(const std::string& path) {
    git_index* idx = nullptr;
    int err = git_repository_index(&idx, repo_);
    if (err < 0) return GitError::fromLibgit2(err);

    err = git_index_add_bypath(idx, path.c_str());
    if (err < 0) { git_index_free(idx); return GitError::fromLibgit2(err); }

    err = git_index_write(idx);
    git_index_free(idx);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> Index::addAll() {
    git_index* idx = nullptr;
    int err = git_repository_index(&idx, repo_);
    if (err < 0) return GitError::fromLibgit2(err);

    const char* paths[] = {"."};
    git_strarray arr = {const_cast<char**>(paths), 1};
    err = git_index_add_all(idx, &arr, 0, nullptr, nullptr);
    if (err < 0) { git_index_free(idx); return GitError::fromLibgit2(err); }

    err = git_index_write(idx);
    git_index_free(idx);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> Index::removePath(const std::string& path) {
    git_index* idx = nullptr;
    int err = git_repository_index(&idx, repo_);
    if (err < 0) return GitError::fromLibgit2(err);

    err = git_index_remove_bypath(idx, path.c_str());
    if (err < 0) { git_index_free(idx); return GitError::fromLibgit2(err); }

    err = git_index_write(idx);
    git_index_free(idx);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> Index::removeAll() {
    git_index* idx = nullptr;
    int err = git_repository_index(&idx, repo_);
    if (err < 0) return GitError::fromLibgit2(err);

    const char* paths[] = {"."};
    git_strarray arr = {const_cast<char**>(paths), 1};
    err = git_index_remove_all(idx, &arr, nullptr, nullptr);
    if (err < 0) { git_index_free(idx); return GitError::fromLibgit2(err); }

    err = git_index_write(idx);
    git_index_free(idx);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> Index::write() {
    git_index* idx = nullptr;
    int err = git_repository_index(&idx, repo_);
    if (err < 0) return GitError::fromLibgit2(err);

    err = git_index_write(idx);
    git_index_free(idx);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

} // namespace gitbolt::git
