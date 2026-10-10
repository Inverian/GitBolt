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
#include "git/Reflog.h"
#include "git/Remote.h"
#include "git/Revwalk.h"
#include "git/Stash.h"
#include "git/Status.h"
#include "git/Submodule.h"
#include "git/Tag.h"
#include "git/Tree.h"
#include "git/Worktree.h"

#include <cstdint>
#include <string>
#include <vector>

struct git_repository;

namespace gitbolt::git {

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

    std::string path() const;
    std::string workdir() const;
    bool isBare() const;
    bool isHeadDetached() const;
    bool isHeadUnborn() const;
    bool isEmpty() const;

    Result<ObjectId> head() const;
    Result<std::string> headBranchName() const;
    Result<CommitData> lookupCommit(const ObjectId& id) const;

    /// Resolve any revision spec — full sha, short sha, branch name,
    /// tag, HEAD, HEAD~3, refs/foo/bar, etc. — to a concrete ObjectId.
    /// Wraps `git_revparse_single`. Useful when you want to accept
    /// flexible user input in dialogs (Cherry pick, Go to commit) and
    /// resolve down to a single commit ID before mutating the repo.
    Result<ObjectId> resolveRef(const std::string& spec) const;

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

    /// In-progress operation state (merging, rebasing, …) — wraps
    /// git_repository_state. Drives which abort command applies
    /// and whether the conflict resolver has anything to do.
    RepoState state() const;

    /// Three-way payloads for every conflicted index entry:
    /// ancestor / ours / theirs content (empty for sides that
    /// don't exist, e.g. add/add has no ancestor). Works for any
    /// conflict source — merge, cherry-pick, rebase, revert.
    Result<std::vector<MergeConflictEntry>> conflictEntries() const;

    /// How many paths conflictEntries() would return, from the index
    /// alone: no blob is read.
    Result<int> conflictCount() const;

    // Remotes
    Result<std::vector<RemoteInfo>> remotes() const;
    Result<void> addRemote(const std::string& name, const std::string& url);
    Result<void> removeRemote(const std::string& name);

    // Tags
    Result<std::vector<TagInfo>> tags() const;
    Result<void> createTag(const std::string& name, const ObjectId& target, const std::string& message = "");
    Result<void> createLightweightTag(const std::string& name, const ObjectId& target);
    /// `name` may be short ("v1.0") or the full "refs/tags/v1.0".
    Result<void> deleteTag(const std::string& name);

    // Blame
    /// Per-line commit attribution for `path`, plus the file's
    /// content at the blamed revision (BlameResult::lines — the
    /// blame table zips hunk ranges with these lines). Blames at
    /// HEAD by default; pass `newestCommit` to blame as of that
    /// commit instead (the blame view's "Blame Before" re-blames
    /// at the parent of a selected commit this way — no checkout
    /// involved).
    Result<BlameResult> blame(const std::string& path,
                              const ObjectId& newestCommit = ObjectId()) const;

    // Reflog
    /// Read the reflog for `refName` (e.g. "HEAD",
    /// "refs/heads/main"). Returns entries in chronological
    /// order — oldest first — flipping libgit2's natural
    /// newest-first iteration so the UI can show "what
    /// happened, in order" without rewriting the list. Empty
    /// vector if the ref has no reflog (a ref freshly created
    /// without `core.logAllRefUpdates` won't have one).
    Result<std::vector<ReflogEntry>> reflog(
        const std::string& refName = "HEAD") const;

    // Stash
    Result<std::vector<StashEntry>> stashes() const;
    Result<ObjectId> stashSave(const std::string& message = "",
                               bool includeUntracked = false,
                               bool keepIndex = false);
    Result<void> stashApply(size_t index = 0);
    Result<void> stashPop(size_t index = 0);
    Result<void> stashDrop(size_t index = 0);

    // Cherry-pick
    Result<CherryPickResult> cherryPick(const ObjectId& commitId);

    // Submodules
    Result<std::vector<SubmoduleInfo>> submodules() const;

    // Worktrees
    Result<std::vector<WorktreeInfo>> worktrees() const;
    Result<void> addWorktree(const std::string& name, const std::string& path,
                             const std::string& branch, bool createBranch = false);
    /// Prunes the worktree's administrative files AND deletes its
    /// working directory from disk. Refuses locked worktrees —
    /// unlock first. Caller is expected to confirm with the user.
    Result<void> removeWorktree(const std::string& name);
    Result<void> lockWorktree(const std::string& name,
                              const std::string& reason = "");
    Result<void> unlockWorktree(const std::string& name);

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
