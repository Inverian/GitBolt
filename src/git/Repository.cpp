#include "git/Repository.h"
#include <QFile>
#include <QString>
#include <git2.h>
#include <cstring>
#include <sstream>
#include <string_view>

namespace gitbolt::git {

std::string libgit2Version() {
    int major = 0, minor = 0, rev = 0;
    git_libgit2_version(&major, &minor, &rev);
    std::ostringstream os;
    os << major << '.' << minor << '.' << rev;
    return os.str();
}

std::string libgit2Features() {
    const int f = git_libgit2_features();
    std::string out;
    auto append = [&](const char* name) {
        if (!out.empty())
            out += ", ";
        out += name;
    };
    if (f & GIT_FEATURE_THREADS)    append("threads");
    if (f & GIT_FEATURE_HTTPS)      append("https");
    if (f & GIT_FEATURE_SSH)        append("ssh");
    if (f & GIT_FEATURE_NSEC)       append("nsec");
    if (out.empty())
        out = "none";
    return out;
}


namespace {
// libgit2 lifecycle owner.
//
// The GitBolt application ALSO calls git_libgit2_init/shutdown from
// main.cpp — these two mechanisms cooperate via libgit2's internal
// refcount. The static's job is to make sure libgit2 is initialised
// even in test binaries and other embedders that don't go through
// main.cpp. Keeping the static is what makes TestRepository,
// TestRevWalk, TestDiff, and TestStatus work out of the box.
//
// Caveat: on macOS 15.6 + Qt 6.11, a test binary that links
// gitbolt_git but never references any Repository symbol can still
// crash with SIGTRAP at static teardown because the Qt threading
// subsystem ends up calling into libgit2 during its own cleanup
// (via TLS key destructors) *after* our static has already shut
// libgit2 down. The workaround is for such test binaries to touch
// libgit2 early so the refcount stays above zero for the full
// duration of the test run — see tests/models/TestCommitLogModel.cpp
// initTestCase() for the pattern.
struct LibGit2Init {
    LibGit2Init() { git_libgit2_init(); }
    ~LibGit2Init() { git_libgit2_shutdown(); }
};
static LibGit2Init s_libgit2Init;
} // namespace

Repository::Repository(git_repository* repo, const std::string& path)
    : repo_(repo), repoPath_(path) {}

Repository::~Repository() {
    if (repo_) git_repository_free(repo_);
}

Repository::Repository(Repository&& other) noexcept
    : repo_(other.repo_), repoPath_(std::move(other.repoPath_)) {
    other.repo_ = nullptr;
}

Repository& Repository::operator=(Repository&& other) noexcept {
    if (this != &other) {
        if (repo_) git_repository_free(repo_);
        repo_ = other.repo_;
        repoPath_ = std::move(other.repoPath_);
        other.repo_ = nullptr;
    }
    return *this;
}

Result<Repository> Repository::open(const std::string& path) {
    git_repository* repo = nullptr;
    int err = git_repository_open(&repo, path.c_str());
    if (err < 0) return GitError::fromLibgit2(err);
    return Repository(repo, path);
}

Result<Repository> Repository::init(const std::string& path, bool bare) {
    git_repository* repo = nullptr;
    int err = git_repository_init(&repo, path.c_str(), bare ? 1 : 0);
    if (err < 0) return GitError::fromLibgit2(err);
    return Repository(repo, path);
}

std::string Repository::path() const {
    const char* p = git_repository_path(repo_);
    return p ? p : "";
}

std::string Repository::workdir() const {
    const char* p = git_repository_workdir(repo_);
    return p ? p : "";
}

bool Repository::isBare() const { return git_repository_is_bare(repo_) != 0; }
bool Repository::isHeadDetached() const { return git_repository_head_detached(repo_) != 0; }
bool Repository::isHeadUnborn() const { return git_repository_head_unborn(repo_) != 0; }
bool Repository::isEmpty() const { return git_repository_is_empty(repo_) != 0; }

Result<ObjectId> Repository::head() const {
    git_reference* ref = nullptr;
    int err = git_repository_head(&ref, repo_);
    if (err < 0) return GitError::fromLibgit2(err);
    const git_oid* oid = git_reference_target(ref);
    ObjectId result(oid);
    git_reference_free(ref);
    return result;
}

Result<std::string> Repository::headBranchName() const {
    git_reference* ref = nullptr;
    int err = git_repository_head(&ref, repo_);
    if (err < 0) return GitError::fromLibgit2(err);
    const char* name = git_reference_shorthand(ref);
    std::string result = name ? name : "";
    git_reference_free(ref);
    return result;
}

Result<ObjectId> Repository::resolveRef(const std::string& spec) const {
    // git_revparse_single handles every flavor of revision spec we
    // care about: full / short SHAs, branch names, tag names, HEAD,
    // HEAD~3, refs/heads/foo, etc. It returns a generic git_object,
    // which we narrow down to its OID — for non-commit refs (a tag
    // pointing at a tree, say) the returned OID is still meaningful
    // for the caller; Cherry pick and Go to commit lookup the OID as
    // a commit downstream and surface a clean error if it isn't one.
    if (spec.empty())
        return GitError(GitErrorCode::InvalidSpec,
                        "empty revision spec");
    git_object* obj = nullptr;
    int err = git_revparse_single(&obj, repo_, spec.c_str());
    if (err < 0) return GitError::fromLibgit2(err);
    ObjectId result(git_object_id(obj));
    git_object_free(obj);
    return result;
}

Result<CommitData> Repository::lookupCommit(const ObjectId& id) const {
    git_oid oid;
    std::memcpy(oid.id, id.raw().data(), ObjectId::RAW_SIZE);
    git_commit* commit = nullptr;
    int err = git_commit_lookup(&commit, repo_, &oid);
    if (err < 0) return GitError::fromLibgit2(err);

    CommitData data;
    data.id = id;
    data.treeId = ObjectId(git_commit_tree_id(commit));
    data.author = Signature::fromGit(git_commit_author(commit));
    data.committer = Signature::fromGit(git_commit_committer(commit));
    const char* msg = git_commit_message(commit);
    data.message = msg ? msg : "";
    const char* summary = git_commit_summary(commit);
    data.summary = summary ? summary : "";

    size_t parentCount = git_commit_parentcount(commit);
    data.parentIds.reserve(parentCount);
    for (size_t i = 0; i < parentCount; ++i)
        data.parentIds.emplace_back(git_commit_parent_id(commit, static_cast<unsigned int>(i)));

    git_commit_free(commit);
    return data;
}

Result<std::vector<StatusEntry>> Repository::status() const {
    git_status_options opts;
    git_status_options_init(&opts, GIT_STATUS_OPTIONS_VERSION);
    opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX
               | GIT_STATUS_OPT_SORT_CASE_SENSITIVELY;

    git_status_list* list = nullptr;
    int err = git_status_list_new(&list, repo_, &opts);
    if (err < 0) return GitError::fromLibgit2(err);

    std::vector<StatusEntry> entries;
    size_t count = git_status_list_entrycount(list);
    entries.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        const git_status_entry* entry = git_status_byindex(list, i);
        StatusEntry se;
        se.status = static_cast<FileStatus>(entry->status);
        if (entry->head_to_index) {
            se.path = entry->head_to_index->new_file.path ? entry->head_to_index->new_file.path : "";
            se.oldPath = entry->head_to_index->old_file.path ? entry->head_to_index->old_file.path : "";
        } else if (entry->index_to_workdir) {
            se.path = entry->index_to_workdir->new_file.path ? entry->index_to_workdir->new_file.path : "";
            se.oldPath = entry->index_to_workdir->old_file.path ? entry->index_to_workdir->old_file.path : "";
        }
        entries.push_back(std::move(se));
    }

    git_status_list_free(list);
    return entries;
}

// Helper to convert a git_diff_delta status
static DiffStatus convertDeltaStatus(git_delta_t status) {
    switch (status) {
        case GIT_DELTA_ADDED:      return DiffStatus::Added;
        case GIT_DELTA_DELETED:    return DiffStatus::Deleted;
        case GIT_DELTA_MODIFIED:   return DiffStatus::Modified;
        case GIT_DELTA_RENAMED:    return DiffStatus::Renamed;
        case GIT_DELTA_COPIED:     return DiffStatus::Copied;
        case GIT_DELTA_IGNORED:    return DiffStatus::Ignored;
        case GIT_DELTA_UNTRACKED:  return DiffStatus::Untracked;
        case GIT_DELTA_TYPECHANGE: return DiffStatus::TypeChanged;
        case GIT_DELTA_CONFLICTED: return DiffStatus::Conflicted;
        default:                   return DiffStatus::Unmodified;
    }
}

// Helper to load hunks and lines from a diff
static DiffResult loadDiffResult(git_diff* diff) {
    DiffResult result;
    size_t numDeltas = git_diff_num_deltas(diff);
    result.files.reserve(numDeltas);

    for (size_t i = 0; i < numDeltas; ++i) {
        const git_diff_delta* delta = git_diff_get_delta(diff, i);
        DiffFileEntry entry;
        entry.oldPath = delta->old_file.path ? delta->old_file.path : "";
        entry.newPath = delta->new_file.path ? delta->new_file.path : "";
        entry.oldId = ObjectId(&delta->old_file.id);
        entry.newId = ObjectId(&delta->new_file.id);
        entry.isBinary = (delta->flags & GIT_DIFF_FLAG_BINARY) != 0;
        entry.similarity = delta->similarity;
        entry.status = convertDeltaStatus(delta->status);

        git_patch* patch = nullptr;
        if (git_patch_from_diff(&patch, diff, i) == 0 && patch) {
            size_t numHunks = git_patch_num_hunks(patch);
            for (size_t h = 0; h < numHunks; ++h) {
                const git_diff_hunk* hunk = nullptr;
                size_t numLines = 0;
                if (git_patch_get_hunk(&hunk, &numLines, patch, h) == 0) {
                    DiffHunk dh;
                    dh.header = hunk->header;
                    dh.oldStart = hunk->old_start;
                    dh.oldLines = hunk->old_lines;
                    dh.newStart = hunk->new_start;
                    dh.newLines = hunk->new_lines;

                    for (size_t l = 0; l < numLines; ++l) {
                        const git_diff_line* line = nullptr;
                        if (git_patch_get_line_in_hunk(&line, patch, h, l) == 0) {
                            DiffLine dl;
                            dl.content = std::string(line->content, static_cast<size_t>(line->content_len));
                            dl.oldLineno = line->old_lineno;
                            dl.newLineno = line->new_lineno;
                            switch (line->origin) {
                                case GIT_DIFF_LINE_ADDITION: dl.type = DiffLineType::Addition; result.totalAdditions++; break;
                                case GIT_DIFF_LINE_DELETION: dl.type = DiffLineType::Deletion; result.totalDeletions++; break;
                                case GIT_DIFF_LINE_CONTEXT:  dl.type = DiffLineType::Context; break;
                                case GIT_DIFF_LINE_FILE_HDR: dl.type = DiffLineType::FileHeader; break;
                                case GIT_DIFF_LINE_HUNK_HDR: dl.type = DiffLineType::HunkHeader; break;
                                case GIT_DIFF_LINE_BINARY:   dl.type = DiffLineType::Binary; break;
                                default: dl.type = DiffLineType::Context; break;
                            }
                            dh.lines.push_back(std::move(dl));
                        }
                    }
                    entry.hunks.push_back(std::move(dh));
                }
            }
            git_patch_free(patch);
        }
        result.files.push_back(std::move(entry));
    }
    return result;
}

Result<DiffResult> Repository::diffIndexToWorkdir() const {
    git_diff* diff = nullptr;
    git_diff_options opts;
    git_diff_options_init(&opts, GIT_DIFF_OPTIONS_VERSION);
    opts.flags = GIT_DIFF_INCLUDE_UNTRACKED;
    int err = git_diff_index_to_workdir(&diff, repo_, nullptr, &opts);
    if (err < 0) return GitError::fromLibgit2(err);
    auto result = loadDiffResult(diff);
    git_diff_free(diff);
    return result;
}

Result<DiffResult> Repository::diffHeadToIndex() const {
    git_object* obj = nullptr;
    git_tree* tree = nullptr;
    int err = git_revparse_single(&obj, repo_, "HEAD^{tree}");
    if (err == 0) tree = reinterpret_cast<git_tree*>(obj);

    git_diff* diff = nullptr;
    err = git_diff_tree_to_index(&diff, repo_, tree, nullptr, nullptr);
    if (tree) git_tree_free(tree);
    if (err < 0) return GitError::fromLibgit2(err);

    auto result = loadDiffResult(diff);
    git_diff_free(diff);
    return result;
}

Result<DiffResult> Repository::diffTreeToTree(const ObjectId& oldTreeId, const ObjectId& newTreeId) const {
    git_oid oldOid, newOid;
    std::memcpy(oldOid.id, oldTreeId.raw().data(), ObjectId::RAW_SIZE);
    std::memcpy(newOid.id, newTreeId.raw().data(), ObjectId::RAW_SIZE);

    // Failed lookups must be errors, not silently-null trees — a
    // bad id otherwise produced a misleading "everything added/
    // deleted" diff instead of a diagnosable failure.
    git_tree* oldTree = nullptr;
    int err = git_tree_lookup(&oldTree, repo_, &oldOid);
    if (err < 0)
        return GitError::fromLibgit2(err);
    git_tree* newTree = nullptr;
    err = git_tree_lookup(&newTree, repo_, &newOid);
    if (err < 0) {
        git_tree_free(oldTree);
        return GitError::fromLibgit2(err);
    }

    git_diff* diff = nullptr;
    err = git_diff_tree_to_tree(&diff, repo_, oldTree, newTree, nullptr);
    if (oldTree) git_tree_free(oldTree);
    if (newTree) git_tree_free(newTree);
    if (err < 0) return GitError::fromLibgit2(err);

    auto result = loadDiffResult(diff);
    git_diff_free(diff);
    return result;
}

Result<DiffResult> Repository::diffCommit(const ObjectId& commitId) const {
    git_oid oid;
    std::memcpy(oid.id, commitId.raw().data(), ObjectId::RAW_SIZE);

    git_commit* commit = nullptr;
    int err = git_commit_lookup(&commit, repo_, &oid);
    if (err < 0) return GitError::fromLibgit2(err);

    git_tree* commitTree = nullptr;
    err = git_commit_tree(&commitTree, commit);
    if (err < 0) { git_commit_free(commit); return GitError::fromLibgit2(err); }

    git_tree* parentTree = nullptr;
    if (git_commit_parentcount(commit) > 0) {
        git_commit* parent = nullptr;
        if (git_commit_parent(&parent, commit, 0) == 0) {
            err = git_commit_tree(&parentTree, parent);
            git_commit_free(parent);
            if (err < 0) {
                git_tree_free(commitTree);
                git_commit_free(commit);
                return GitError::fromLibgit2(err);
            }
        }
    }

    git_diff* diff = nullptr;
    err = git_diff_tree_to_tree(&diff, repo_, parentTree, commitTree, nullptr);
    if (parentTree) git_tree_free(parentTree);
    git_tree_free(commitTree);
    git_commit_free(commit);
    if (err < 0) return GitError::fromLibgit2(err);

    auto result = loadDiffResult(diff);
    git_diff_free(diff);
    return result;
}

// ---------------------------------------------------------------------------
// Tree walking — used by the File tree tab to render the full
// directory listing for a selected commit. We do a single
// pre-order depth-first walk via git_tree_walk and append every
// entry (file or directory) into a flat vector. The caller can
// rebuild the hierarchy in O(n) by indexing on parent path.
//
// Sizes are populated for blobs by looking up each blob's
// rawsize via git_blob_lookup; trees report size 0. The blob
// lookup is fast (libgit2 has an in-memory ODB cache) and the
// alternative — leaving sizes as 0 — would force the FileTreeWidget
// to do its own per-row blob lookups during scroll, which is
// strictly worse.
// ---------------------------------------------------------------------------

namespace {

struct TreeWalkPayload {
    git_repository*        repo;
    std::vector<TreeEntry>* out;
};

int treeWalkCallback(const char* root, const git_tree_entry* entry, void* payload)
{
    auto* ctx = static_cast<TreeWalkPayload*>(payload);

    TreeEntry e;
    e.name   = git_tree_entry_name(entry);
    e.path   = std::string(root) + e.name;
    e.oid    = ObjectId(git_tree_entry_id(entry));
    e.mode   = git_tree_entry_filemode(entry);
    e.isTree = (git_tree_entry_type(entry) == GIT_OBJECT_TREE);

    if (!e.isTree) {
        // Look up the blob just to read its size. This is a hash-
        // table lookup against libgit2's ODB cache — cheap.
        git_blob* blob = nullptr;
        if (git_blob_lookup(&blob, ctx->repo, git_tree_entry_id(entry)) == 0) {
            e.size = static_cast<uint64_t>(git_blob_rawsize(blob));
            git_blob_free(blob);
        }
    }

    ctx->out->push_back(std::move(e));
    return 0;  // continue walk
}

} // namespace

Result<std::vector<TreeEntry>> Repository::walkTreeAtCommit(const ObjectId& commitId) const
{
    git_oid oid;
    std::memcpy(oid.id, commitId.raw().data(), ObjectId::RAW_SIZE);

    git_commit* commit = nullptr;
    int err = git_commit_lookup(&commit, repo_, &oid);
    if (err < 0) return GitError::fromLibgit2(err);

    git_tree* tree = nullptr;
    err = git_commit_tree(&tree, commit);
    git_commit_free(commit);
    if (err < 0) return GitError::fromLibgit2(err);

    std::vector<TreeEntry> entries;
    TreeWalkPayload payload{ repo_, &entries };
    err = git_tree_walk(tree, GIT_TREEWALK_PRE, treeWalkCallback, &payload);
    git_tree_free(tree);
    if (err < 0) return GitError::fromLibgit2(err);

    return entries;
}

Result<std::vector<char>> Repository::readBlob(const ObjectId& blobId) const
{
    git_oid oid;
    std::memcpy(oid.id, blobId.raw().data(), ObjectId::RAW_SIZE);

    git_blob* blob = nullptr;
    int err = git_blob_lookup(&blob, repo_, &oid);
    if (err < 0) return GitError::fromLibgit2(err);

    const void*    data = git_blob_rawcontent(blob);
    const git_object_size_t size = git_blob_rawsize(blob);

    std::vector<char> bytes;
    if (data && size > 0) {
        bytes.assign(static_cast<const char*>(data),
                     static_cast<const char*>(data) + static_cast<size_t>(size));
    }
    git_blob_free(blob);
    return bytes;
}

namespace {
// Rereads libgit2's copy of the index from the index file (see
// freshIndex), dropping whatever a libgit2 operation that failed
// partway left in it: a checkout (a merge's, a cherry-pick's and a
// stash's too) updates the copy file by file and writes it only once
// every file is written.
void reloadIndex(git_repository* repo) {
    git_index* index = nullptr;
    if (freshIndex(&index, repo) == 0)
        git_index_free(index);
}
} // namespace

Result<void> Repository::stageFile(const std::string& path) {
    Index idx(repo_);
    return idx.addPath(path);
}

Result<void> Repository::unstageFile(const std::string& path) {
    git_reference* headRef = nullptr;
    git_object* headCommit = nullptr;
    if (git_repository_head(&headRef, repo_) == 0) {
        git_reference_peel(&headCommit, headRef, GIT_OBJECT_COMMIT);
        git_reference_free(headRef);
    }
    // git_reset_default works on libgit2's copy of the index; bring
    // it up to date first (see freshIndex).
    reloadIndex(repo_);
    const char* paths[] = {path.c_str()};
    git_strarray arr = {const_cast<char**>(paths), 1};
    int err = git_reset_default(repo_, headCommit, &arr);
    if (headCommit) git_object_free(headCommit);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> Repository::stageAll() {
    Index idx(repo_);
    return idx.addAll();
}

Result<void> Repository::discardWorkdirChanges(const std::string& path) {
    git_checkout_options opts;
    git_checkout_options_init(&opts, GIT_CHECKOUT_OPTIONS_VERSION);
    opts.checkout_strategy = GIT_CHECKOUT_FORCE;
    const char* paths[] = {path.c_str()};
    opts.paths.strings = const_cast<char**>(paths);
    opts.paths.count = 1;
    int err = git_checkout_head(repo_, &opts);
    if (err < 0) {
        const GitError error = GitError::fromLibgit2(err);
        reloadIndex(repo_);
        return error;
    }
    return Result<void>::success();
}

Result<ObjectId> Repository::commit(const std::string& message, bool amend) {
    git_index* index = nullptr;
    int err = freshIndex(&index, repo_);
    if (err < 0) return GitError::fromLibgit2(err);

    git_oid treeOid;
    err = git_index_write_tree(&treeOid, index);
    git_index_free(index);
    if (err < 0) return GitError::fromLibgit2(err);

    git_tree* tree = nullptr;
    err = git_tree_lookup(&tree, repo_, &treeOid);
    if (err < 0) return GitError::fromLibgit2(err);

    git_signature* sig = nullptr;
    err = git_signature_default(&sig, repo_);
    if (err < 0) { git_tree_free(tree); return GitError::fromLibgit2(err); }

    git_oid commitOid;
    if (amend) {
        // An unborn HEAD (fresh repo, no commits) has nothing to
        // amend. The old code ignored this error and handed a null
        // reference straight to git_reference_peel — a guaranteed
        // crash on the very first commit with "Amend" ticked.
        git_reference* headRef = nullptr;
        err = git_repository_head(&headRef, repo_);
        if (err < 0) {
            git_signature_free(sig);
            git_tree_free(tree);
            return GitError::fromLibgit2(err);
        }
        git_commit* headCommit = nullptr;
        err = git_reference_peel(reinterpret_cast<git_object**>(&headCommit), headRef, GIT_OBJECT_COMMIT);
        git_reference_free(headRef);
        if (err < 0) {
            git_signature_free(sig);
            git_tree_free(tree);
            return GitError::fromLibgit2(err);
        }
        err = git_commit_amend(&commitOid, headCommit, "HEAD", sig, sig, nullptr, message.c_str(), tree);
        git_commit_free(headCommit);
    } else {
        std::vector<git_commit*> parents;
        git_reference* headRef = nullptr;
        if (git_repository_head(&headRef, repo_) == 0) {
            git_commit* parent = nullptr;
            git_reference_peel(reinterpret_cast<git_object**>(&parent), headRef, GIT_OBJECT_COMMIT);
            git_reference_free(headRef);
            if (parent) parents.push_back(parent);
        }

        // Concluding an in-progress merge: MERGE_HEAD's commits are
        // additional parents, exactly like `git commit` after a
        // conflicted merge. Without this the resulting commit had a
        // single parent — the merge silently flattened into a
        // normal commit and the merged-from branch appeared
        // unmerged forever.
        std::vector<git_oid> mergeHeads;
        git_repository_mergehead_foreach(
            repo_,
            [](const git_oid* oid, void* payload) -> int {
                static_cast<std::vector<git_oid>*>(payload)
                    ->push_back(*oid);
                return 0;
            },
            &mergeHeads);
        for (const auto& oid : mergeHeads) {
            git_commit* mergeParent = nullptr;
            if (git_commit_lookup(&mergeParent, repo_, &oid) == 0)
                parents.push_back(mergeParent);
        }

        // Concluding a cherry-pick: the commit is still the picked
        // commit's work, so it keeps that commit's author (name,
        // email and date), as `git commit` does; the user is the
        // committer. libgit2's cherry-pick never commits, so every
        // cherry-pick made in GitBolt ends here, and each one went
        // into the history as the user's own. A merge and a revert
        // are the user's work and keep the user as author.
        git_commit* picked = nullptr;
        git_oid pickedId;
        if (git_reference_name_to_id(&pickedId, repo_, "CHERRY_PICK_HEAD") == 0)
            git_commit_lookup(&picked, repo_, &pickedId);
        const git_signature* author = picked ? git_commit_author(picked) : sig;

        std::vector<const git_commit*> parentPtrs(parents.begin(),
                                                  parents.end());
        err = git_commit_create(&commitOid, repo_, "HEAD", author, sig,
                                nullptr, message.c_str(), tree,
                                parentPtrs.size(),
                                parentPtrs.empty() ? nullptr
                                                   : parentPtrs.data());
        git_commit_free(picked);
        for (auto* p : parents)
            git_commit_free(p);

        // Clear MERGE_HEAD / MERGE_MSG (or cherry-pick state) so
        // the repository leaves the "merging" state once the
        // concluding commit lands — those files only, as `git
        // commit` does. git_repository_state_cleanup() deletes a
        // rebase's state too (and a bisect's, and the sequencer's
        // list of cherry-picks still to come): a commit made while a
        // rebase was stopped, as the conflict resolver advised,
        // threw the rest of the rebase away and left HEAD detached
        // halfway through it.
        if (err == 0) {
            const QString gitDir = QString::fromUtf8(git_repository_path(repo_));
            for (const char* name : {"MERGE_HEAD", "MERGE_MODE", "MERGE_MSG", "SQUASH_MSG",
                                     "CHERRY_PICK_HEAD", "REVERT_HEAD"})
                QFile::remove(gitDir + QLatin1String(name));
        }
    }

    git_signature_free(sig);
    git_tree_free(tree);
    if (err < 0) return GitError::fromLibgit2(err);
    return ObjectId(&commitOid);
}

Result<std::vector<BranchInfo>> Repository::branches(BranchType filter) const {
    git_branch_iterator* iter = nullptr;
    git_branch_t gitFilter = (filter == BranchType::Local) ? GIT_BRANCH_LOCAL : GIT_BRANCH_REMOTE;
    int err = git_branch_iterator_new(&iter, repo_, gitFilter);
    if (err < 0) return GitError::fromLibgit2(err);

    std::vector<BranchInfo> result;
    git_reference* ref = nullptr;
    git_branch_t type;
    while (git_branch_next(&ref, &type, iter) == 0) {
        BranchInfo info;
        const char* name = nullptr;
        git_branch_name(&name, ref);
        info.name = name ? name : "";
        info.fullRefName = git_reference_name(ref);
        info.type = (type == GIT_BRANCH_LOCAL) ? BranchType::Local : BranchType::Remote;
        const git_oid* oid = git_reference_target(ref);
        if (oid) info.tipId = ObjectId(oid);
        // git_branch_is_head returns 1 for the ref that HEAD points at,
        // 0 for every other branch, and <0 on error. A negative return
        // would leave isHead defaulted to false which would hide HEAD
        // highlighting in the UI, so clamp explicitly.
        const int headCheck = git_branch_is_head(ref);
        info.isHead = (headCheck == 1);
        git_reference_free(ref);
        result.push_back(std::move(info));
    }
    git_branch_iterator_free(iter);
    return result;
}

Result<std::vector<BranchInfo>> Repository::allBranches() const {
    auto local = branches(BranchType::Local);
    if (!local) return local.error();
    auto remote = branches(BranchType::Remote);
    if (!remote) return remote.error();
    auto& all = local.value();
    auto& rem = remote.value();
    all.insert(all.end(), std::make_move_iterator(rem.begin()), std::make_move_iterator(rem.end()));
    return all;
}

Result<void> Repository::createBranch(const std::string& name, const ObjectId& target) {
    git_oid oid;
    std::memcpy(oid.id, target.raw().data(), ObjectId::RAW_SIZE);
    git_commit* commit = nullptr;
    int err = git_commit_lookup(&commit, repo_, &oid);
    if (err < 0) return GitError::fromLibgit2(err);
    git_reference* ref = nullptr;
    err = git_branch_create(&ref, repo_, name.c_str(), commit, 0);
    git_commit_free(commit);
    if (ref) git_reference_free(ref);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> Repository::deleteBranch(const std::string& name) {
    git_reference* ref = nullptr;
    int err = git_branch_lookup(&ref, repo_, name.c_str(), GIT_BRANCH_LOCAL);
    if (err < 0) return GitError::fromLibgit2(err);
    err = git_branch_delete(ref);
    git_reference_free(ref);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> Repository::renameBranch(const std::string& oldName, const std::string& newName) {
    git_reference* ref = nullptr;
    int err = git_branch_lookup(&ref, repo_, oldName.c_str(), GIT_BRANCH_LOCAL);
    if (err < 0) return GitError::fromLibgit2(err);
    git_reference* newRef = nullptr;
    err = git_branch_move(&newRef, ref, newName.c_str(), 0);
    git_reference_free(ref);
    if (newRef) git_reference_free(newRef);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

namespace {
// The working tree of a worktree whose HEAD is the branch `branchRef`
// ("refs/heads/..."), or "" if no worktree that opens has it, for
// saying where a branch is checked out. Walks them as
// git_branch_is_checked_out does: the main worktree, then the linked
// ones. The caller rules out `repo` itself.
std::string worktreeOnBranch(git_repository* repo, const char* branchRef) {
    const auto workdirIfOnBranch = [branchRef](git_repository* wt) {
        std::string workdir;
        git_reference* head = nullptr;
        if (!git_repository_is_bare(wt)
            && git_reference_lookup(&head, wt, "HEAD") == 0
            && git_reference_type(head) == GIT_REFERENCE_SYMBOLIC
            && std::strcmp(git_reference_symbolic_target(head), branchRef) == 0)
            workdir = git_repository_workdir(wt);
        git_reference_free(head);
        // libgit2 ends it with a '/', which git's messages don't.
        if (workdir.size() > 1 && workdir.back() == '/')
            workdir.pop_back();
        return workdir;
    };

    std::string found;
    git_repository* mainRepo = nullptr;
    if (git_repository_open(&mainRepo, git_repository_commondir(repo)) == 0) {
        found = workdirIfOnBranch(mainRepo);
        git_repository_free(mainRepo);
    }
    git_strarray names = {nullptr, 0};
    if (found.empty() && git_worktree_list(&names, repo) == 0) {
        for (size_t i = 0; i < names.count && found.empty(); ++i) {
            git_worktree* wt = nullptr;
            git_repository* wtRepo = nullptr;
            if (git_worktree_lookup(&wt, repo, names.strings[i]) == 0
                && git_repository_open_from_worktree(&wtRepo, wt) == 0)
                found = workdirIfOnBranch(wtRepo);
            git_repository_free(wtRepo);
            git_worktree_free(wt);
        }
        git_strarray_dispose(&names);
    }
    return found;
}
} // namespace

Result<void> Repository::checkout(const std::string& branchOrRef) {
    // git's rule for `git checkout <name>`: a LOCAL BRANCH name means
    // that branch, even when a tag (or anything else) has the same
    // name. The tree used to come from git_revparse_single, where
    // refs/tags/<name> beats refs/heads/<name>, while HEAD went to
    // the branch: the tag's files under the branch, showing up as
    // phantom changes. Full refs ("refs/tags/x", "refs/heads/x") are
    // no branch name, so they resolve as written.
    git_reference* branchRef = nullptr;
    if (git_branch_lookup(&branchRef, repo_, branchOrRef.c_str(),
                          GIT_BRANCH_LOCAL) < 0)
        branchRef = nullptr;

    // Annotated tags peel to their commit; commits peel to
    // themselves. Checking out a non-committish fails right here.
    git_object* peeled = nullptr;
    int err = 0;
    if (branchRef) {
        err = git_reference_peel(&peeled, branchRef, GIT_OBJECT_COMMIT);
    } else {
        git_object* target = nullptr;
        err = git_revparse_single(&target, repo_, branchOrRef.c_str());
        if (err == 0) {
            err = git_object_peel(&peeled, target, GIT_OBJECT_COMMIT);
            git_object_free(target);
        }
    }
    if (err < 0) {
        git_reference_free(branchRef);
        return GitError::fromLibgit2(err);
    }

    // git won't check out a branch that another worktree has checked
    // out. git_repository_set_head won't either, but it refuses only
    // once git_checkout_tree has written the branch's files and
    // index: HEAD stayed on the old branch under the new one's files,
    // all of them staged for the next commit. And from a detached
    // HEAD it doesn't check at all, so HEAD went onto another
    // worktree's branch. So ask first, before anything is touched.
    if (branchRef && git_branch_is_head(branchRef) == 0
        && git_branch_is_checked_out(branchRef) == 1) {
        const std::string where = worktreeOnBranch(repo_, git_reference_name(branchRef));
        git_reference_free(branchRef);
        git_object_free(peeled);
        return GitError(GitErrorCode::GenericError,
                        "'" + branchOrRef + "' is already checked out "
                            + (where.empty() ? std::string("in another worktree")
                                             : "at '" + where + "'"));
    }

    git_checkout_options opts;
    git_checkout_options_init(&opts, GIT_CHECKOUT_OPTIONS_VERSION);
    opts.checkout_strategy = GIT_CHECKOUT_SAFE;
    err = git_checkout_tree(repo_, peeled, &opts);
    if (err < 0) {
        // A checkout that failed partway (another program has a file
        // open, another git holds the index's lock) has written some
        // of the files, and libgit2's copy of the index has them
        // while the index file doesn't. Status listed them as staged;
        // after the reload they are changes in the working tree, as
        // git leaves them.
        const GitError error = GitError::fromLibgit2(err);
        reloadIndex(repo_);
        git_reference_free(branchRef);
        git_object_free(peeled);
        return error;
    }

    // HEAD update: only an actual LOCAL BRANCH name gets a symbolic
    // HEAD; tags, remote-tracking refs, full refs, raw SHAs and
    // relative specs detach, as in git. The old code wrote
    // "refs/heads/<input>" for all of them, leaving HEAD as a broken
    // symbolic ref to a branch that doesn't exist while the working
    // tree showed the checkout.
    err = branchRef
        ? git_repository_set_head(repo_, git_reference_name(branchRef))
        : git_repository_set_head_detached(repo_, git_object_id(peeled));
    if (err < 0) {
        // HEAD stays where it was (another git holds its lock, say),
        // but the files and the index are the target's already. Put
        // HEAD's back, or the switch shows up as staged changes on the
        // old branch. The baseline is the target's tree, so files still
        // as the checkout left them count as its work to undo, not as
        // changes of the user's to keep. An unborn HEAD has no tree to
        // put back.
        const GitError error = GitError::fromLibgit2(err);
        git_object* headTree = nullptr;
        git_object* targetTree = nullptr;
        if (git_revparse_single(&headTree, repo_, "HEAD^{tree}") == 0
            && git_object_peel(&targetTree, peeled, GIT_OBJECT_TREE) == 0) {
            git_checkout_options undo;
            git_checkout_options_init(&undo, GIT_CHECKOUT_OPTIONS_VERSION);
            undo.checkout_strategy = GIT_CHECKOUT_SAFE;
            undo.baseline = reinterpret_cast<git_tree*>(targetTree);
            if (git_checkout_tree(repo_, headTree, &undo) < 0)
                reloadIndex(repo_);
        }
        git_object_free(targetTree);
        git_object_free(headTree);
        git_reference_free(branchRef);
        git_object_free(peeled);
        return error;
    }
    git_reference_free(branchRef);
    git_object_free(peeled);
    return Result<void>::success();
}

Result<MergeResult> Repository::analyzeMerge(const ObjectId& theirHead) const {
    git_oid oid;
    std::memcpy(oid.id, theirHead.raw().data(), ObjectId::RAW_SIZE);
    git_annotated_commit* annotated = nullptr;
    int err = git_annotated_commit_lookup(&annotated, repo_, &oid);
    if (err < 0) return GitError::fromLibgit2(err);

    git_merge_analysis_t analysis;
    git_merge_preference_t preference;
    const git_annotated_commit* heads[] = {annotated};
    err = git_merge_analysis(&analysis, &preference, repo_, heads, 1);
    git_annotated_commit_free(annotated);
    if (err < 0) return GitError::fromLibgit2(err);

    MergeResult result;
    if (analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE) result.analysis = MergeAnalysis::UpToDate;
    else if (analysis & GIT_MERGE_ANALYSIS_FASTFORWARD) result.analysis = MergeAnalysis::FastForward;
    else if (analysis & GIT_MERGE_ANALYSIS_UNBORN) result.analysis = MergeAnalysis::Unborn;
    else result.analysis = MergeAnalysis::Normal;
    return result;
}

Result<MergeResult> Repository::merge(const ObjectId& theirHead, MergePreference /*pref*/) {
    git_oid oid;
    std::memcpy(oid.id, theirHead.raw().data(), ObjectId::RAW_SIZE);
    git_annotated_commit* annotated = nullptr;
    int err = git_annotated_commit_lookup(&annotated, repo_, &oid);
    if (err < 0) return GitError::fromLibgit2(err);

    git_merge_options merge_opts;
    git_merge_options_init(&merge_opts, GIT_MERGE_OPTIONS_VERSION);
    git_checkout_options checkout_opts;
    git_checkout_options_init(&checkout_opts, GIT_CHECKOUT_OPTIONS_VERSION);
    checkout_opts.checkout_strategy = GIT_CHECKOUT_FORCE | GIT_CHECKOUT_ALLOW_CONFLICTS;

    const git_annotated_commit* heads[] = {annotated};
    err = git_merge(repo_, heads, 1, &merge_opts, &checkout_opts);
    git_annotated_commit_free(annotated);
    if (err < 0) {
        // As with checkout(): a merge whose checkout failed partway
        // (a file another program has open, one that can't be
        // written) has written some of the merged files, and
        // libgit2's copy of the index has them while the index file
        // doesn't. libgit2 removes MERGE_HEAD and the rest of the
        // merge's state but leaves the copy as it is, and status
        // listed those files as staged.
        const GitError error = GitError::fromLibgit2(err);
        reloadIndex(repo_);
        return error;
    }

    git_index* index = nullptr;
    err = git_repository_index(&index, repo_);
    if (err < 0)
        return GitError::fromLibgit2(err);
    MergeResult result;
    result.analysis = MergeAnalysis::Normal;
    result.hasConflicts = git_index_has_conflicts(index) != 0;
    git_index_free(index);
    return result;
}

RepoState Repository::state() const {
    switch (git_repository_state(repo_)) {
    case GIT_REPOSITORY_STATE_NONE:
        return RepoState::None;
    case GIT_REPOSITORY_STATE_MERGE:
        return RepoState::Merge;
    case GIT_REPOSITORY_STATE_REVERT:
    case GIT_REPOSITORY_STATE_REVERT_SEQUENCE:
        return RepoState::Revert;
    case GIT_REPOSITORY_STATE_CHERRYPICK:
    case GIT_REPOSITORY_STATE_CHERRYPICK_SEQUENCE:
        return RepoState::CherryPick;
    case GIT_REPOSITORY_STATE_REBASE:
    case GIT_REPOSITORY_STATE_REBASE_INTERACTIVE:
    case GIT_REPOSITORY_STATE_REBASE_MERGE:
        return RepoState::Rebase;
    default:
        return RepoState::Other;
    }
}

Result<std::vector<MergeConflictEntry>> Repository::conflictEntries() const {
    git_index* index = nullptr;
    int err = git_repository_index(&index, repo_);
    if (err < 0) return GitError::fromLibgit2(err);

    // Reload from disk: libgit2 caches the git_index object, so a
    // merge/cherry-pick/rebase performed by an external process —
    // or by GitBolt's own CLI-backed merge — leaves this handle's
    // in-memory copy stale, and the conflict entries written to
    // .git/index would be invisible. git_index_read(force=1)
    // re-reads unconditionally.
    git_index_read(index, /*force=*/1);

    // Reads a conflict side's blob into a string. A null entry is
    // normal (add/add conflicts have no ancestor; delete/modify
    // has only one side) and yields empty content so the three-way
    // panes can render "nothing on this side".
    auto readSide = [this](const git_index_entry* e,
                           std::string& content, ObjectId& id) {
        if (!e) return;
        id = ObjectId(&e->id);
        git_blob* blob = nullptr;
        if (git_blob_lookup(&blob, repo_, &e->id) == 0) {
            const auto* data =
                static_cast<const char*>(git_blob_rawcontent(blob));
            content.assign(data, git_blob_rawsize(blob));
            git_blob_free(blob);
        }
    };

    git_index_conflict_iterator* it = nullptr;
    err = git_index_conflict_iterator_new(&it, index);
    if (err < 0) {
        git_index_free(index);
        return GitError::fromLibgit2(err);
    }

    std::vector<MergeConflictEntry> result;
    const git_index_entry* ancestor = nullptr;
    const git_index_entry* ours = nullptr;
    const git_index_entry* theirs = nullptr;
    while (git_index_conflict_next(&ancestor, &ours, &theirs, it)
           == 0) {
        MergeConflictEntry entry;
        if (ours && ours->path)            entry.path = ours->path;
        else if (theirs && theirs->path)   entry.path = theirs->path;
        else if (ancestor && ancestor->path) entry.path = ancestor->path;
        readSide(ancestor, entry.ancestorContent, entry.ancestorId);
        readSide(ours, entry.oursContent, entry.oursId);
        readSide(theirs, entry.theirsContent, entry.theirsId);
        result.push_back(std::move(entry));
    }
    git_index_conflict_iterator_free(it);
    git_index_free(index);
    return result;
}

Result<int> Repository::conflictCount() const {
    git_index* index = nullptr;
    int err = git_repository_index(&index, repo_);
    if (err < 0) return GitError::fromLibgit2(err);
    // Fresh from disk, as in conflictEntries(): git wrote it.
    git_index_read(index, /*force=*/1);

    git_index_conflict_iterator* it = nullptr;
    err = git_index_conflict_iterator_new(&it, index);
    if (err < 0) {
        git_index_free(index);
        return GitError::fromLibgit2(err);
    }
    int count = 0;
    const git_index_entry* ancestor = nullptr;
    const git_index_entry* ours = nullptr;
    const git_index_entry* theirs = nullptr;
    while (git_index_conflict_next(&ancestor, &ours, &theirs, it) == 0)
        ++count;
    git_index_conflict_iterator_free(it);
    git_index_free(index);
    return count;
}

Result<std::vector<RemoteInfo>> Repository::remotes() const {
    git_strarray names = {nullptr, 0};
    int err = git_remote_list(&names, repo_);
    if (err < 0) return GitError::fromLibgit2(err);

    std::vector<RemoteInfo> result;
    result.reserve(names.count);
    for (size_t i = 0; i < names.count; ++i) {
        git_remote* remote = nullptr;
        if (git_remote_lookup(&remote, repo_, names.strings[i]) == 0) {
            RemoteInfo info;
            info.name = names.strings[i];
            const char* url = git_remote_url(remote);
            info.url = url ? url : "";
            const char* pushUrl = git_remote_pushurl(remote);
            info.pushUrl = pushUrl ? pushUrl : "";
            git_remote_free(remote);
            result.push_back(std::move(info));
        }
    }
    git_strarray_dispose(&names);
    return result;
}

Result<void> Repository::addRemote(const std::string& name, const std::string& url) {
    git_remote* remote = nullptr;
    int err = git_remote_create(&remote, repo_, name.c_str(), url.c_str());
    if (remote) git_remote_free(remote);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> Repository::removeRemote(const std::string& name) {
    int err = git_remote_delete(repo_, name.c_str());
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

namespace {
constexpr std::string_view kTagRefPrefix = "refs/tags/";
} // namespace

Result<std::vector<TagInfo>> Repository::tags() const {
    struct Payload {
        git_repository* repo;
        std::vector<TagInfo> tags;
    } payload{repo_, {}};
    int err = git_tag_foreach(repo_, [](const char* name, git_oid* oid, void* p) -> int {
        auto* payload = static_cast<Payload*>(p);
        TagInfo info;
        // git_tag_foreach hands over the FULL ref name, while
        // git_tag_delete and git_tag_create take the short one.
        // Passing this through as `name` made Delete tag fail for
        // every tag and put "refs/tags/" all over the UI.
        info.fullRefName = name;
        std::string_view shortName = info.fullRefName;
        if (shortName.starts_with(kTagRefPrefix))
            shortName.remove_prefix(kTagRefPrefix.size());
        info.name = shortName;
        info.targetId = ObjectId(oid);
        info.type = TagType::Lightweight;

        // `oid` is the ref's target: the commit for a lightweight
        // tag, but the tag OBJECT for an annotated one. Reporting it
        // as-is gave annotated tags a target that is in no log (the
        // sidebar tooltip's "Target:") and dropped their tagger and
        // message. An object we can't read stays a lightweight tag
        // at the ref's target rather than failing the whole list.
        git_object* obj = nullptr;
        if (git_object_lookup(&obj, payload->repo, oid, GIT_OBJECT_ANY) == 0) {
            if (git_object_type(obj) == GIT_OBJECT_TAG) {
                auto* tag = reinterpret_cast<git_tag*>(obj);
                info.type = TagType::Annotated;
                info.tagId = ObjectId(oid);
                info.targetId = ObjectId(git_tag_target_id(tag));
                // A tag of a tag: peel through to what it finally
                // names (normally the commit).
                git_object* peeled = nullptr;
                if (git_tag_peel(&peeled, tag) == 0) {
                    info.targetId = ObjectId(git_object_id(peeled));
                    git_object_free(peeled);
                }
                const char* message = git_tag_message(tag);
                info.message = message ? message : "";
                info.tagger = Signature::fromGit(git_tag_tagger(tag));
            }
            git_object_free(obj);
        }
        payload->tags.push_back(std::move(info));
        return 0;
    }, &payload);
    // Enumeration failure (corrupt packed-refs etc.) must not be
    // reported as "no tags".
    if (err < 0)
        return GitError::fromLibgit2(err);
    return std::move(payload.tags);
}

Result<void> Repository::createTag(const std::string& name, const ObjectId& target, const std::string& message) {
    git_oid oid;
    std::memcpy(oid.id, target.raw().data(), ObjectId::RAW_SIZE);
    git_object* obj = nullptr;
    int err = git_object_lookup(&obj, repo_, &oid, GIT_OBJECT_COMMIT);
    if (err < 0) return GitError::fromLibgit2(err);
    git_signature* sig = nullptr;
    err = git_signature_default(&sig, repo_);
    if (err < 0) { git_object_free(obj); return GitError::fromLibgit2(err); }
    git_oid tagOid;
    err = git_tag_create(&tagOid, repo_, name.c_str(), obj, sig, message.c_str(), 0);
    git_signature_free(sig);
    git_object_free(obj);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> Repository::createLightweightTag(const std::string& name, const ObjectId& target) {
    git_oid oid;
    std::memcpy(oid.id, target.raw().data(), ObjectId::RAW_SIZE);
    git_object* obj = nullptr;
    int err = git_object_lookup(&obj, repo_, &oid, GIT_OBJECT_COMMIT);
    if (err < 0) return GitError::fromLibgit2(err);
    git_oid tagOid;
    err = git_tag_create_lightweight(&tagOid, repo_, name.c_str(), obj, 0);
    git_object_free(obj);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> Repository::deleteTag(const std::string& name) {
    // git_tag_delete prepends "refs/tags/" itself, so a full ref name
    // would look up refs/tags/refs/tags/<tag> and fail as not found.
    // Accept both forms: callers holding a TagInfo may pass either.
    std::string_view shortName = name;
    if (shortName.starts_with(kTagRefPrefix))
        shortName.remove_prefix(kTagRefPrefix.size());
    int err = git_tag_delete(repo_, std::string(shortName).c_str());
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<BlameResult> Repository::blame(const std::string& path,
                                      const ObjectId& newestCommit) const {
    git_blame_options opts;
    git_blame_options_init(&opts, GIT_BLAME_OPTIONS_VERSION);
    // Blame "as of" a specific commit when one is given (used by
    // the blame view's "Blame Before" action to re-blame at the
    // parent of a selected commit). Zero/default = HEAD, which is
    // libgit2's default behavior.
    if (!newestCommit.isZero())
        std::memcpy(opts.newest_commit.id, newestCommit.raw().data(),
                    ObjectId::RAW_SIZE);

    git_blame* bl = nullptr;
    int err = git_blame_file(&bl, repo_, path.c_str(), &opts);
    if (err < 0) return GitError::fromLibgit2(err);

    BlameResult result;
    result.path = path;
    uint32_t hunkCount = git_blame_get_hunk_count(bl);
    result.hunks.reserve(hunkCount);

    for (uint32_t i = 0; i < hunkCount; ++i) {
        const git_blame_hunk* hunk = git_blame_get_hunk_byindex(bl, i);
        BlameHunk bh;
        bh.commitId = ObjectId(&hunk->final_commit_id);
        bh.origCommitId = ObjectId(&hunk->orig_commit_id);
        bh.signature = Signature::fromGit(hunk->final_signature);
        bh.origPath = hunk->orig_path ? hunk->orig_path : "";
        // BlameHunk stores uint32_t (as does libgit2's hunk count);
        // exceeding it takes a file of > 4G lines, i.e. > 4 GiB.
        bh.startLine = static_cast<uint32_t>(hunk->final_start_line_number);
        bh.lineCount = static_cast<uint32_t>(hunk->lines_in_hunk);
        bh.boundary = hunk->boundary != 0;
        result.hunks.push_back(std::move(bh));
    }
    git_blame_free(bl);

    // File content at the blamed revision. The hunks only carry
    // line RANGES; BlameModel zips them with these lines to render
    // the table, so without this the blame view shows zero rows.
    // (Workdir content would be wrong here: blame attributes lines
    // as of <rev>, so the text must come from <rev>:<path> too.)
    const std::string spec =
        (newestCommit.isZero() ? std::string("HEAD")
                               : newestCommit.toHex())
        + ":" + path;
    git_object* obj = nullptr;
    err = git_revparse_single(&obj, repo_, spec.c_str());
    if (err < 0) return GitError::fromLibgit2(err);
    if (git_object_type(obj) != GIT_OBJECT_BLOB) {
        git_object_free(obj);
        return GitError(GitErrorCode::GenericError,
                        path + " is not a file at " + spec);
    }
    auto* blob = reinterpret_cast<git_blob*>(obj);
    if (git_blob_is_binary(blob)) {
        git_object_free(obj);
        return GitError(GitErrorCode::GenericError,
                        path + " is a binary file");
    }
    const auto* data =
        static_cast<const char*>(git_blob_rawcontent(blob));
    const size_t size = git_blob_rawsize(blob);
    std::string current;
    for (size_t i = 0; i < size; ++i) {
        if (data[i] == '\n') {
            result.lines.push_back(std::move(current));
            current.clear();
        } else if (data[i] != '\r') {
            current.push_back(data[i]);
        }
    }
    if (!current.empty())
        result.lines.push_back(std::move(current));
    git_object_free(obj);

    return result;
}

Result<std::vector<ReflogEntry>> Repository::reflog(
        const std::string& refName) const {
    git_reflog* log = nullptr;
    int err = git_reflog_read(&log, repo_, refName.c_str());
    if (err < 0) return GitError::fromLibgit2(err);

    const size_t count = git_reflog_entrycount(log);
    std::vector<ReflogEntry> result;
    result.reserve(count);

    // libgit2 indexes reflog entries newest-first (index 0 == most
    // recent). The UI wants oldest-first for chronological "first
    // X happened, then Y" reading; iterate backwards so the result
    // vector ends up in chronological order.
    for (size_t i = count; i-- > 0;) {
        const git_reflog_entry* entry =
            git_reflog_entry_byindex(log, i);
        if (!entry) continue;
        ReflogEntry e;
        const git_oid* oldId = git_reflog_entry_id_old(entry);
        const git_oid* newId = git_reflog_entry_id_new(entry);
        if (oldId) e.oldId = ObjectId(oldId);
        if (newId) e.newId = ObjectId(newId);
        const git_signature* sig = git_reflog_entry_committer(entry);
        if (sig) e.committer = Signature::fromGit(sig);
        const char* msg = git_reflog_entry_message(entry);
        e.message = msg ? msg : "";
        result.push_back(std::move(e));
    }
    git_reflog_free(log);
    return result;
}

Result<std::vector<StashEntry>> Repository::stashes() const {
    std::vector<StashEntry> result;
    git_stash_foreach(repo_, [](size_t index, const char* message, const git_oid* stash_id, void* payload) -> int {
        auto* s = static_cast<std::vector<StashEntry>*>(payload);
        StashEntry entry;
        entry.index = index;
        entry.id = ObjectId(stash_id);
        entry.message = message ? message : "";
        s->push_back(std::move(entry));
        return 0;
    }, &result);
    return result;
}

Result<ObjectId> Repository::stashSave(const std::string& message, bool includeUntracked,
                                       bool keepIndex) {
    git_signature* sig = nullptr;
    int err = git_signature_default(&sig, repo_);
    if (err < 0) return GitError::fromLibgit2(err);
    uint32_t flags = GIT_STASH_DEFAULT;
    if (includeUntracked) flags |= GIT_STASH_INCLUDE_UNTRACKED;
    if (keepIndex) flags |= GIT_STASH_KEEP_INDEX;
    git_oid oid;
    err = git_stash_save(&oid, repo_, sig, message.empty() ? nullptr : message.c_str(), flags);
    git_signature_free(sig);
    if (err < 0) {
        // The stash commits can be made and the work tree reset before
        // the index write fails (a held index.lock): don't let status
        // read libgit2's copy as if it were the file.
        const GitError error = GitError::fromLibgit2(err);
        reloadIndex(repo_);
        return error;
    }
    return ObjectId(&oid);
}

Result<void> Repository::stashApply(size_t index) {
    int err = git_stash_apply(repo_, index, nullptr);
    if (err < 0) {
        // libgit2 writes the stash's files, stages the ones the stash
        // added in its copy of the index, and writes the index file
        // last. When that write failed (another git holds index.lock),
        // the copy kept them staged while the index file didn't, and
        // status listed them as staged. After the reload they are new
        // files in the work tree.
        const GitError error = GitError::fromLibgit2(err);
        reloadIndex(repo_);
        return error;
    }
    return Result<void>::success();
}

Result<void> Repository::stashPop(size_t index) {
    int err = git_stash_pop(repo_, index, nullptr);
    if (err < 0) {
        // As in stashApply(); a stash that wasn't applied isn't dropped.
        const GitError error = GitError::fromLibgit2(err);
        reloadIndex(repo_);
        return error;
    }
    return Result<void>::success();
}

Result<void> Repository::stashDrop(size_t index) {
    int err = git_stash_drop(repo_, index);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<CherryPickResult> Repository::cherryPick(const ObjectId& commitId) {
    git_oid oid;
    std::memcpy(oid.id, commitId.raw().data(), ObjectId::RAW_SIZE);
    git_commit* commit = nullptr;
    int err = git_commit_lookup(&commit, repo_, &oid);
    if (err < 0) return GitError::fromLibgit2(err);
    git_cherrypick_options opts;
    git_cherrypick_options_init(&opts, GIT_CHERRYPICK_OPTIONS_VERSION);
    err = git_cherrypick(repo_, commit, &opts);
    git_commit_free(commit);
    // A real failure (bad object, bare repo, OOM) is an ERROR with
    // git_error_last context; a conflicted index is the normal
    // "needs resolution" outcome and returns 0. The old code
    // collapsed both into hasConflicts, so genuine failures
    // surfaced as "produced conflicts" with the message discarded.
    // A failure can come partway through the checkout, too: as with
    // merge(), libgit2 drops CHERRY_PICK_HEAD and MERGE_MSG but not
    // the files its copy of the index had picked up by then.
    if (err < 0) {
        const GitError error = GitError::fromLibgit2(err);
        reloadIndex(repo_);
        return error;
    }

    git_index* index = nullptr;
    err = git_repository_index(&index, repo_);
    if (err < 0)
        return GitError::fromLibgit2(err);
    CherryPickResult result;
    result.hasConflicts = git_index_has_conflicts(index) != 0;
    git_index_free(index);
    return result;
}

Result<std::vector<SubmoduleInfo>> Repository::submodules() const {
    std::vector<SubmoduleInfo> result;
    git_submodule_foreach(repo_, [](git_submodule* sm, const char* name, void* payload) -> int {
        auto* subs = static_cast<std::vector<SubmoduleInfo>*>(payload);
        SubmoduleInfo info;
        info.name = name ? name : "";
        info.path = git_submodule_path(sm) ? git_submodule_path(sm) : "";
        info.url = git_submodule_url(sm) ? git_submodule_url(sm) : "";
        const git_oid* headOid = git_submodule_head_id(sm);
        if (headOid) info.headId = ObjectId(headOid);
        const git_oid* indexOid = git_submodule_index_id(sm);
        if (indexOid) info.indexId = ObjectId(indexOid);
        info.status = SubmoduleStatus::Clean;
        subs->push_back(std::move(info));
        return 0;
    }, &result);
    return result;
}

Result<std::vector<WorktreeInfo>> Repository::worktrees() const {
    git_strarray names = {nullptr, 0};
    int err = git_worktree_list(&names, repo_);
    if (err < 0) return GitError::fromLibgit2(err);

    std::vector<WorktreeInfo> result;
    result.reserve(names.count);
    for (size_t i = 0; i < names.count; ++i) {
        git_worktree* wt = nullptr;
        if (git_worktree_lookup(&wt, repo_, names.strings[i]) == 0) {
            WorktreeInfo info;
            info.name = names.strings[i];
            info.path = git_worktree_path(wt) ? git_worktree_path(wt) : "";
            info.isLocked = git_worktree_is_locked(nullptr, wt) != 0;
            info.isPrunable = git_worktree_is_prunable(wt, nullptr) != 0;

            // Checked-out branch of the worktree. The table's
            // Branch column was previously always empty because
            // this was never resolved. Detached HEAD shows the
            // short SHA instead of a ref name.
            git_reference* head = nullptr;
            if (git_repository_head_for_worktree(
                    &head, repo_, names.strings[i]) == 0) {
                if (git_reference_is_branch(head)) {
                    const char* shorthand =
                        git_reference_shorthand(head);
                    info.branch = shorthand ? shorthand : "";
                } else if (const git_oid* oid =
                               git_reference_target(head)) {
                    info.branch =
                        ObjectId(oid).toShortHex() + " (detached)";
                }
                git_reference_free(head);
            }

            git_worktree_free(wt);
            result.push_back(std::move(info));
        }
    }
    git_strarray_dispose(&names);
    return result;
}

Result<void> Repository::addWorktree(const std::string& name, const std::string& path,
                                     const std::string& branch, bool createBranch) {
    git_worktree_add_options opts;
    git_worktree_add_options_init(&opts, GIT_WORKTREE_ADD_OPTIONS_VERSION);
    git_reference* ref = nullptr;
    if (!branch.empty()) {
        if (createBranch) {
            // Mirror `git worktree add -b <branch>`: new branch at
            // the current HEAD commit, checked out in the worktree.
            git_reference* headRef = nullptr;
            int err = git_repository_head(&headRef, repo_);
            if (err < 0) return GitError::fromLibgit2(err);
            git_commit* headCommit = nullptr;
            err = git_reference_peel(
                reinterpret_cast<git_object**>(&headCommit), headRef,
                GIT_OBJECT_COMMIT);
            git_reference_free(headRef);
            if (err < 0) return GitError::fromLibgit2(err);
            err = git_branch_create(&ref, repo_, branch.c_str(),
                                    headCommit, 0);
            git_commit_free(headCommit);
            if (err < 0) return GitError::fromLibgit2(err);
        } else {
            // Surface a bad branch name instead of silently letting
            // git_worktree_add invent a branch named after the
            // worktree (the old behavior when lookup failed).
            int err = git_branch_lookup(&ref, repo_, branch.c_str(),
                                        GIT_BRANCH_LOCAL);
            if (err < 0) return GitError::fromLibgit2(err);
        }
    }
    opts.ref = ref;

    git_worktree* wt = nullptr;
    int err = git_worktree_add(&wt, repo_, name.c_str(), path.c_str(), &opts);
    if (ref) git_reference_free(ref);
    if (wt) git_worktree_free(wt);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> Repository::removeWorktree(const std::string& name) {
    git_worktree* wt = nullptr;
    int err = git_worktree_lookup(&wt, repo_, name.c_str());
    if (err < 0) return GitError::fromLibgit2(err);

    // Default prune options refuse to touch a valid (i.e. normal,
    // still-on-disk) worktree — they only clean up already-broken
    // ones, so "Remove" from the UI would always fail. VALID lets
    // a healthy worktree be pruned; WORKING_TREE also deletes its
    // directory from disk (the UI confirms with the user first).
    // A LOCKED worktree is still refused — unlock first, which is
    // the same protection `git worktree remove` gives.
    git_worktree_prune_options opts = GIT_WORKTREE_PRUNE_OPTIONS_INIT;
    opts.flags = GIT_WORKTREE_PRUNE_VALID
               | GIT_WORKTREE_PRUNE_WORKING_TREE;
    err = git_worktree_prune(wt, &opts);
    git_worktree_free(wt);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> Repository::lockWorktree(const std::string& name,
                                      const std::string& reason) {
    git_worktree* wt = nullptr;
    int err = git_worktree_lookup(&wt, repo_, name.c_str());
    if (err < 0) return GitError::fromLibgit2(err);
    err = git_worktree_lock(wt, reason.empty() ? nullptr
                                               : reason.c_str());
    git_worktree_free(wt);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> Repository::unlockWorktree(const std::string& name) {
    git_worktree* wt = nullptr;
    int err = git_worktree_lookup(&wt, repo_, name.c_str());
    if (err < 0) return GitError::fromLibgit2(err);
    err = git_worktree_unlock(wt);
    git_worktree_free(wt);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<RevWalk> Repository::createRevWalk() const {
    return RevWalk(repo_);
}

Config Repository::config() const {
    return Config(repo_);
}

GitProcess Repository::process() const {
    return GitProcess(workdir());
}

} // namespace gitbolt::git
