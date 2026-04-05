#include "git/Repository.h"
#include <git2.h>
#include <cstring>

namespace gitbolt::git {

namespace {
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

Result<Repository> Repository::clone(const std::string& url, const std::string& path) {
    git_repository* repo = nullptr;
    int err = git_clone(&repo, url.c_str(), path.c_str(), nullptr);
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
    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
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
    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
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

    git_tree* oldTree = nullptr;
    git_tree* newTree = nullptr;
    git_tree_lookup(&oldTree, repo_, &oldOid);
    git_tree_lookup(&newTree, repo_, &newOid);

    git_diff* diff = nullptr;
    int err = git_diff_tree_to_tree(&diff, repo_, oldTree, newTree, nullptr);
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
            git_commit_tree(&parentTree, parent);
            git_commit_free(parent);
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
    git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
    opts.checkout_strategy = GIT_CHECKOUT_FORCE;
    const char* paths[] = {path.c_str()};
    opts.paths.strings = const_cast<char**>(paths);
    opts.paths.count = 1;
    int err = git_checkout_head(repo_, &opts);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<ObjectId> Repository::commit(const std::string& message, bool amend) {
    git_index* index = nullptr;
    int err = git_repository_index(&index, repo_);
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
        git_reference* headRef = nullptr;
        git_commit* headCommit = nullptr;
        git_repository_head(&headRef, repo_);
        git_reference_peel(reinterpret_cast<git_object**>(&headCommit), headRef, GIT_OBJECT_COMMIT);
        git_reference_free(headRef);
        err = git_commit_amend(&commitOid, headCommit, "HEAD", sig, sig, nullptr, message.c_str(), tree);
        git_commit_free(headCommit);
    } else {
        git_commit* parent = nullptr;
        const git_commit* parents[1] = {nullptr};
        int parentCount = 0;
        git_reference* headRef = nullptr;
        if (git_repository_head(&headRef, repo_) == 0) {
            git_reference_peel(reinterpret_cast<git_object**>(&parent), headRef, GIT_OBJECT_COMMIT);
            git_reference_free(headRef);
            parents[0] = parent;
            parentCount = 1;
        }
        err = git_commit_create(&commitOid, repo_, "HEAD", sig, sig, nullptr,
                                message.c_str(), tree, parentCount, parents);
        if (parent) git_commit_free(parent);
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
        info.isHead = git_branch_is_head(ref) != 0;
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

Result<void> Repository::checkout(const std::string& branchOrRef) {
    git_object* target = nullptr;
    int err = git_revparse_single(&target, repo_, branchOrRef.c_str());
    if (err < 0) return GitError::fromLibgit2(err);
    git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
    opts.checkout_strategy = GIT_CHECKOUT_SAFE;
    err = git_checkout_tree(repo_, target, &opts);
    git_object_free(target);
    if (err < 0) return GitError::fromLibgit2(err);
    std::string refName = "refs/heads/" + branchOrRef;
    err = git_repository_set_head(repo_, refName.c_str());
    if (err < 0) return GitError::fromLibgit2(err);
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

    git_merge_options merge_opts = GIT_MERGE_OPTIONS_INIT;
    git_checkout_options checkout_opts = GIT_CHECKOUT_OPTIONS_INIT;
    checkout_opts.checkout_strategy = GIT_CHECKOUT_FORCE | GIT_CHECKOUT_ALLOW_CONFLICTS;

    const git_annotated_commit* heads[] = {annotated};
    err = git_merge(repo_, heads, 1, &merge_opts, &checkout_opts);
    git_annotated_commit_free(annotated);
    if (err < 0) return GitError::fromLibgit2(err);

    git_index* index = nullptr;
    git_repository_index(&index, repo_);
    MergeResult result;
    result.analysis = MergeAnalysis::Normal;
    result.hasConflicts = git_index_has_conflicts(index) != 0;
    git_index_free(index);
    return result;
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

Result<std::vector<TagInfo>> Repository::tags() const {
    std::vector<TagInfo> result;
    git_tag_foreach(repo_, [](const char* name, git_oid* oid, void* payload) -> int {
        auto* tags = static_cast<std::vector<TagInfo>*>(payload);
        TagInfo info;
        info.name = name;
        info.targetId = ObjectId(oid);
        info.type = TagType::Lightweight;
        tags->push_back(std::move(info));
        return 0;
    }, &result);
    return result;
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
    int err = git_tag_delete(repo_, name.c_str());
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<BlameResult> Repository::blame(const std::string& path) const {
    git_blame_options opts = GIT_BLAME_OPTIONS_INIT;
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
        bh.startLine = hunk->final_start_line_number;
        bh.lineCount = hunk->lines_in_hunk;
        bh.boundary = hunk->boundary != 0;
        result.hunks.push_back(std::move(bh));
    }
    git_blame_free(bl);
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

Result<ObjectId> Repository::stashSave(const std::string& message, bool includeUntracked) {
    git_signature* sig = nullptr;
    int err = git_signature_default(&sig, repo_);
    if (err < 0) return GitError::fromLibgit2(err);
    uint32_t flags = GIT_STASH_DEFAULT;
    if (includeUntracked) flags |= GIT_STASH_INCLUDE_UNTRACKED;
    git_oid oid;
    err = git_stash_save(&oid, repo_, sig, message.empty() ? nullptr : message.c_str(), flags);
    git_signature_free(sig);
    if (err < 0) return GitError::fromLibgit2(err);
    return ObjectId(&oid);
}

Result<void> Repository::stashApply(size_t index) {
    int err = git_stash_apply(repo_, index, nullptr);
    if (err < 0) return GitError::fromLibgit2(err);
    return Result<void>::success();
}

Result<void> Repository::stashPop(size_t index) {
    int err = git_stash_pop(repo_, index, nullptr);
    if (err < 0) return GitError::fromLibgit2(err);
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
    git_cherrypick_options opts = GIT_CHERRYPICK_OPTIONS_INIT;
    err = git_cherrypick(repo_, commit, &opts);
    git_commit_free(commit);
    CherryPickResult result;
    result.hasConflicts = (err < 0);
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
            git_worktree_free(wt);
            result.push_back(std::move(info));
        }
    }
    git_strarray_dispose(&names);
    return result;
}

Result<void> Repository::addWorktree(const std::string& name, const std::string& path, const std::string& branch) {
    git_worktree_add_options opts = GIT_WORKTREE_ADD_OPTIONS_INIT;
    git_reference* ref = nullptr;
    if (!branch.empty())
        git_branch_lookup(&ref, repo_, branch.c_str(), GIT_BRANCH_LOCAL);
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
    err = git_worktree_prune(wt, nullptr);
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
