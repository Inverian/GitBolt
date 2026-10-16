#include "git/Index.h"
#include <git2.h>

namespace gitbolt::git {

int freshIndex(git_index** out, git_repository* repo) {
    int err = git_repository_index(out, repo);
    if (err < 0) return err;
    err = git_index_read(*out, /*force=*/1);
    if (err < 0) {
        git_index_free(*out);
        *out = nullptr;
    }
    return err;
}

namespace {
// Frees `idx` after a change to it, and reports `err`. A change that
// failed partway, or that couldn't be written (another git holds
// index.lock), is first dropped from the copy by reading the index
// file again: the copy is the repository's, which status reads only
// when the file changes, and the file hadn't. A file that failed to
// stage showed as staged, one that failed to unstage as unstaged.
Result<void> finish(git_index* idx, int err) {
    if (err < 0) {
        // Taken before the read, which may overwrite libgit2's last error.
        const GitError error = GitError::fromLibgit2(err);
        git_index_read(idx, /*force=*/1);
        git_index_free(idx);
        return error;
    }
    git_index_free(idx);
    return Result<void>::success();
}
} // namespace

Index::Index(git_repository* repo) : repo_(repo) {}

Result<void> Index::addPath(const std::string& path) {
    git_index* idx = nullptr;
    int err = freshIndex(&idx, repo_);
    if (err < 0) return GitError::fromLibgit2(err);

    err = git_index_add_bypath(idx, path.c_str());
    if (err >= 0)
        err = git_index_write(idx);
    return finish(idx, err);
}

Result<void> Index::addAll() {
    git_index* idx = nullptr;
    int err = freshIndex(&idx, repo_);
    if (err < 0) return GitError::fromLibgit2(err);

    const char* paths[] = {"."};
    git_strarray arr = {const_cast<char**>(paths), 1};
    err = git_index_add_all(idx, &arr, 0, nullptr, nullptr);
    if (err >= 0)
        err = git_index_write(idx);
    return finish(idx, err);
}

Result<void> Index::removePath(const std::string& path) {
    git_index* idx = nullptr;
    int err = freshIndex(&idx, repo_);
    if (err < 0) return GitError::fromLibgit2(err);

    err = git_index_remove_bypath(idx, path.c_str());
    if (err >= 0)
        err = git_index_write(idx);
    return finish(idx, err);
}

Result<void> Index::removeAll() {
    git_index* idx = nullptr;
    int err = freshIndex(&idx, repo_);
    if (err < 0) return GitError::fromLibgit2(err);

    const char* paths[] = {"."};
    git_strarray arr = {const_cast<char**>(paths), 1};
    err = git_index_remove_all(idx, &arr, nullptr, nullptr);
    if (err >= 0)
        err = git_index_write(idx);
    return finish(idx, err);
}

Result<void> Index::write() {
    git_index* idx = nullptr;
    int err = freshIndex(&idx, repo_);
    if (err < 0) return GitError::fromLibgit2(err);

    err = git_index_write(idx);
    git_index_free(idx);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

} // namespace gitbolt::git
