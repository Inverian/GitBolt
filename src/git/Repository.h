#pragma once

#include "git/Blame.h"
#include "git/Branch.h"
#include "git/Cherrypick.h"
#include "git/Commit.h"
#include "git/Config.h"
#include "git/Diff.h"
#include "git/Error.h"
#include "git/GitProcess.h"
#include "git/Index.h"
#include "git/Merge.h"
#include "git/ObjectId.h"
#include "git/Rebase.h"
#include "git/Remote.h"
#include "git/Revwalk.h"
#include "git/Stash.h"
#include "git/Status.h"
#include "git/Submodule.h"
#include "git/Tag.h"
#include "git/Tree.h"
#include "git/Worktree.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct git_repository;

namespace gitbolt::git {

/// Incremental progress report emitted by `Repository::clone()` via
/// its optional callback. libgit2 surfaces two separate metric sets
/// during a clone — the fetch/indexing phase (objects, bytes, deltas)
/// and the checkout phase (files written) — and this struct flattens
/// both into one shape that the caller can render into a single
/// progress UI without caring which phase is active.
struct CloneProgress {
    enum class Phase {
        Receiving,   ///< downloading pack objects from the remote
        Resolving,   ///< indexing / resolving deltas after fetch
        CheckingOut  ///< writing files to the working directory
    };
    Phase    phase = Phase::Receiving;

    // Fetch-phase counters (valid in Receiving / Resolving phases).
    uint32_t receivedObjects = 0;
    uint32_t indexedObjects  = 0;
    uint32_t totalObjects    = 0;
    uint32_t indexedDeltas   = 0;
    uint32_t totalDeltas     = 0;
    uint64_t receivedBytes   = 0;

    // Checkout-phase counters (valid in CheckingOut phase).
    uint32_t completedSteps  = 0;
    uint32_t totalSteps      = 0;
};

/// Callback type invoked from the libgit2 worker thread during
/// `Repository::clone()`. The callback is called MANY times per
/// second — if the receiver lives on a different thread (e.g. the
/// GUI thread), the lambda must marshal the update safely via
/// QMetaObject::invokeMethod or similar.
using CloneProgressCallback = std::function<void(const CloneProgress&)>;

/// Returns the runtime libgit2 version as "MAJOR.MINOR.PATCH" (e.g.
/// "1.9.2"). This reads from libgit2's own reported version rather
/// than the LIBGIT2_VERSION macro, so it reflects the actual shared
/// library loaded at runtime — useful for AboutDialog and for bug
/// reports where the user might be running against a different
/// libgit2 than the one we built against.
std::string libgit2Version();

/// Returns the libgit2 features compiled in, as a comma-separated
/// string: "threads, https, ssh" etc. Parses the libgit2 feature
/// flags at runtime.
std::string libgit2Features();

class Repository {
public:
    ~Repository();
    Repository(const Repository&) = delete;
    Repository& operator=(const Repository&) = delete;
    Repository(Repository&& other) noexcept;
    Repository& operator=(Repository&& other) noexcept;

    static Result<Repository> open(const std::string& path);
    static Result<Repository> init(const std::string& path, bool bare = false);
    static Result<Repository> clone(const std::string& url, const std::string& path);

    /// Overload that reports fetch / checkout progress through
    /// `onProgress`. The callback is invoked on the libgit2 worker
    /// thread (typically a background thread); it must not touch UI
    /// state directly — marshal across threads first.
    static Result<Repository> clone(const std::string& url,
                                    const std::string& path,
                                    CloneProgressCallback onProgress);

    std::string path() const;
    std::string workdir() const;
    bool isBare() const;
    bool isHeadDetached() const;
    bool isHeadUnborn() const;
    bool isEmpty() const;

    Result<ObjectId> head() const;
    Result<std::string> headBranchName() const;
    Result<CommitData> lookupCommit(const ObjectId& id) const;

    // Status
    Result<std::vector<StatusEntry>> status() const;

    // Diffing
    Result<DiffResult> diffIndexToWorkdir() const;
    Result<DiffResult> diffHeadToIndex() const;
    Result<DiffResult> diffTreeToTree(const ObjectId& oldTree, const ObjectId& newTree) const;
    Result<DiffResult> diffCommit(const ObjectId& commitId) const;

    // Tree browsing — flat depth-first walk from a commit's root
    // tree, plus blob content reads. Used by the File tree tab.
    // walkTreeAtCommit returns entries in pre-order so callers can
    // build hierarchical models in a single pass; readBlob returns
    // the raw bytes (binary-safe) and is bounded by libgit2's own
    // size limits — call sites should still impose a sensible
    // upper bound before showing untrusted blobs in the UI.
    Result<std::vector<TreeEntry>> walkTreeAtCommit(const ObjectId& commitId) const;
    Result<std::vector<char>>      readBlob(const ObjectId& blobId) const;

    // Staging
    Result<void> stageFile(const std::string& path);
    Result<void> unstageFile(const std::string& path);
    Result<void> stageAll();
    Result<void> discardWorkdirChanges(const std::string& path);

    // Commits
    Result<ObjectId> commit(const std::string& message, bool amend = false);

    // Branches
    Result<std::vector<BranchInfo>> branches(BranchType filter = BranchType::Local) const;
    Result<std::vector<BranchInfo>> allBranches() const;
    Result<void> createBranch(const std::string& name, const ObjectId& target);
    Result<void> deleteBranch(const std::string& name);
    Result<void> renameBranch(const std::string& oldName, const std::string& newName);
    Result<void> checkout(const std::string& branchOrRef);

    // Merge
    Result<MergeResult> analyzeMerge(const ObjectId& theirHead) const;
    Result<MergeResult> merge(const ObjectId& theirHead, MergePreference pref = MergePreference::Normal);

    // Remotes
    Result<std::vector<RemoteInfo>> remotes() const;
    Result<void> addRemote(const std::string& name, const std::string& url);
    Result<void> removeRemote(const std::string& name);

    // Tags
    Result<std::vector<TagInfo>> tags() const;
    Result<void> createTag(const std::string& name, const ObjectId& target, const std::string& message = "");
    Result<void> createLightweightTag(const std::string& name, const ObjectId& target);
    Result<void> deleteTag(const std::string& name);

    // Blame
    Result<BlameResult> blame(const std::string& path) const;

    // Stash
    Result<std::vector<StashEntry>> stashes() const;
    Result<ObjectId> stashSave(const std::string& message = "", bool includeUntracked = false);
    Result<void> stashApply(size_t index = 0);
    Result<void> stashPop(size_t index = 0);
    Result<void> stashDrop(size_t index = 0);

    // Cherry-pick
    Result<CherryPickResult> cherryPick(const ObjectId& commitId);

    // Submodules
    Result<std::vector<SubmoduleInfo>> submodules() const;

    // Worktrees
    Result<std::vector<WorktreeInfo>> worktrees() const;
    Result<void> addWorktree(const std::string& name, const std::string& path, const std::string& branch);
    Result<void> removeWorktree(const std::string& name);

    // Revwalk
    Result<RevWalk> createRevWalk() const;

    // Config
    Config config() const;

    // CLI fallback
    GitProcess process() const;

    // Low-level access
    git_repository* raw() const { return repo_; }

private:
    Repository(git_repository* repo, const std::string& path);
    git_repository* repo_ = nullptr;
    std::string repoPath_;
};

} // namespace gitbolt::git
