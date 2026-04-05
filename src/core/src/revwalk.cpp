#include "gitbolt/core/revwalk.h"
#include <git2.h>
#include <cstring>

namespace gitbolt::core {

struct RevWalk::Impl {
    git_repository* repo = nullptr;
    git_revwalk* walker = nullptr;

    ~Impl() {
        if (walker) git_revwalk_free(walker);
    }

    CommitData loadCommit(const git_oid& oid) {
        CommitData data;
        data.id = ObjectId(&oid);

        git_commit* commit = nullptr;
        if (git_commit_lookup(&commit, repo, &oid) != 0) return data;

        data.treeId = ObjectId(git_commit_tree_id(commit));
        data.author = Signature::fromGit(git_commit_author(commit));
        data.committer = Signature::fromGit(git_commit_committer(commit));

        const char* msg = git_commit_message(commit);
        data.message = msg ? msg : "";
        const char* summary = git_commit_summary(commit);
        data.summary = summary ? summary : "";

        size_t parentCount = git_commit_parentcount(commit);
        data.parentIds.reserve(parentCount);
        for (size_t i = 0; i < parentCount; ++i) {
            data.parentIds.emplace_back(git_commit_parent_id(commit, static_cast<unsigned int>(i)));
        }

        git_commit_free(commit);
        return data;
    }
};

RevWalk::RevWalk(git_repository* repo) : impl_(new Impl) {
    impl_->repo = repo;
    git_revwalk_new(&impl_->walker, repo);
}

RevWalk::~RevWalk() { delete impl_; }

RevWalk::RevWalk(RevWalk&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
}

RevWalk& RevWalk::operator=(RevWalk&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

Result<void> RevWalk::pushHead() {
    int err = git_revwalk_push_head(impl_->walker);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> RevWalk::pushRef(const std::string& refname) {
    int err = git_revwalk_push_ref(impl_->walker, refname.c_str());
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> RevWalk::push(const ObjectId& id) {
    git_oid oid;
    std::memcpy(oid.id, id.raw().data(), ObjectId::RAW_SIZE);
    int err = git_revwalk_push(impl_->walker, &oid);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> RevWalk::hide(const ObjectId& id) {
    git_oid oid;
    std::memcpy(oid.id, id.raw().data(), ObjectId::RAW_SIZE);
    int err = git_revwalk_hide(impl_->walker, &oid);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> RevWalk::hideRef(const std::string& refname) {
    int err = git_revwalk_hide_ref(impl_->walker, refname.c_str());
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

void RevWalk::setSorting(SortOrder order) {
    unsigned int flags = GIT_SORT_NONE;
    switch (order) {
        case SortOrder::None: flags = GIT_SORT_NONE; break;
        case SortOrder::Topological: flags = GIT_SORT_TOPOLOGICAL; break;
        case SortOrder::Time: flags = GIT_SORT_TIME; break;
        case SortOrder::TopologicalTime: flags = GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME; break;
        case SortOrder::Reverse: flags = GIT_SORT_REVERSE; break;
    }
    git_revwalk_sorting(impl_->walker, flags);
}

void RevWalk::setFirstParentOnly(bool /*firstParent*/) {
    git_revwalk_simplify_first_parent(impl_->walker);
}

void RevWalk::reset() {
    git_revwalk_reset(impl_->walker);
}

Result<std::vector<CommitData>> RevWalk::next(size_t count) {
    std::vector<CommitData> commits;
    commits.reserve(count);
    git_oid oid;
    for (size_t i = 0; i < count; ++i) {
        int err = git_revwalk_next(&oid, impl_->walker);
        if (err == GIT_ITEROVER) break;
        if (err < 0) return GitError::fromLibgit2(err);
        commits.push_back(impl_->loadCommit(oid));
    }
    return commits;
}

Result<std::vector<CommitData>> RevWalk::all() {
    std::vector<CommitData> commits;
    git_oid oid;
    while (true) {
        int err = git_revwalk_next(&oid, impl_->walker);
        if (err == GIT_ITEROVER) break;
        if (err < 0) return GitError::fromLibgit2(err);
        commits.push_back(impl_->loadCommit(oid));
    }
    return commits;
}

Result<void> RevWalk::walk(WalkCallback callback) {
    git_oid oid;
    while (true) {
        int err = git_revwalk_next(&oid, impl_->walker);
        if (err == GIT_ITEROVER) break;
        if (err < 0) return GitError::fromLibgit2(err);
        if (!callback(impl_->loadCommit(oid))) break;
    }
    return Result<void>::success();
}

} // namespace gitbolt::core
