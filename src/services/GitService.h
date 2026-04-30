#pragma once
#include "git/Repository.h"
#include "util/AsyncRunner.h"
#include "watcher/FileWatcher.h"
#include <QObject>
#include <QStringList>
#include <memory>
#include <mutex>

namespace gitbolt::services {

class GitService : public QObject {
    Q_OBJECT
public:
    explicit GitService(QObject* parent = nullptr);

    bool openRepository(const QString& path);
    void closeRepository();
    bool isOpen() const;
    git::Repository* repository() const;

    /// Mutex that serializes all libgit2 access through this service.
    /// UI code that calls Repository methods directly (via
    /// `gitService->repository()->...`) should take this lock for the
    /// duration of those calls — otherwise it races against the
    /// background workers that GitService spawns for refreshStatus /
    /// refreshLog / refreshBranches. libgit2 is not safe for
    /// concurrent access on a single git_repository*. The seen-in-the-
    /// wild crash was a malloc_zone_error inside git_pool_clear under
    /// git_status_list_new while another worker was reading the log.
    std::mutex& repoMutex() const { return repoMutex_; }

    /// Thread-safe construction of a GitProcess for the open repository.
    /// Locks repoMutex_ briefly while libgit2 reports the workdir, then
    /// returns by value. Once you have the GitProcess object you can
    /// run shell commands on it without holding the lock — QProcess is
    /// independent of libgit2 state.
    git::GitProcess process() const;

    void refreshStatus();
    void refreshLog(int offset = 0, int count = 256);
    void refreshBranches();

    /// Choose which refs the log walk starts from. Default is
    /// `Head` (just the current branch and its ancestors).
    /// `AllLocalBranches` pushes every local branch tip onto the
    /// walk so the log shows commits across the whole repo.
    /// `SelectedBranches` pushes only the branches named in
    /// `selectedBranches_` (set via setSelectedBranches). The
    /// scope persists across subsequent refreshLog() calls until
    /// changed again.
    enum class LogScope {
        Head,
        AllLocalBranches,
        SelectedBranches,
    };
    void setLogScope(LogScope scope);
    LogScope logScope() const { return logScope_; }

    /// Configure which branches feed the walk when `LogScope` is
    /// `SelectedBranches`. Names are short refs (e.g. "main"), no
    /// "refs/heads/" prefix needed. Setting the list while the
    /// active scope is already SelectedBranches triggers an
    /// immediate refreshLog().
    void setSelectedBranches(const QStringList& branchNames);
    QStringList selectedBranches() const { return selectedBranches_; }

    void stageFile(const QString& path);
    void unstageFile(const QString& path);
    void stageAll();
    void unstageAll();
    void discardFile(const QString& path);
    void commitChanges(const QString& message, bool amend = false);

    void createBranch(const QString& name);
    void deleteBranch(const QString& name);
    void checkoutBranch(const QString& name);

    void push(const QString& remote, const QString& branch);
    void pull(const QString& remote, const QString& branch);
    void fetch(const QString& remote = "");

    // Interactive Rebase
    void interactiveRebase(const git::RebasePlan& plan);
    void rebaseContinue();
    void rebaseAbort();
    void rebaseSkip();

    // Cherry-pick
    void cherryPick(const std::vector<git::ObjectId>& commits);

    // Stash
    void stashSave(const QString& message, bool includeUntracked = false);
    void stashApply(int index);
    void stashPop(int index);
    void stashDrop(int index);
    void refreshStashes();

    // Tags
    void refreshTags();
    void createTag(const QString& name, const QString& target,
                   const QString& message, bool annotated);
    void deleteTag(const QString& name);

    // Submodules
    void refreshSubmodules();
    void submoduleInit(const QString& name);
    void submoduleUpdate(const QString& name);

    // Worktrees
    void refreshWorktrees();
    void addWorktree(const QString& name, const QString& path, const QString& branch);
    void removeWorktree(const QString& name);

    // Git Flow
    bool isGitFlowInitialized();
    void gitFlowInit();
    void featureStart(const QString& name);
    void featureFinish(const QString& name);
    void releaseStart(const QString& version);
    void releaseFinish(const QString& version);
    void hotfixStart(const QString& version);
    void hotfixFinish(const QString& version);
    QStringList activeFeatures();
    QStringList activeReleases();
    QStringList activeHotfixes();

    // Repository maintenance
    void runGc();
    void runPrune();
    void runFsck();
    void runRepack();
    QString repositoryDiskUsage();

signals:
    void repositoryOpened(const QString& path);
    void repositoryClosed();
    void statusReady(std::vector<gitbolt::git::StatusEntry> entries);
    void logReady(std::vector<gitbolt::git::CommitData> commits, int offset);
    void branchesReady(std::vector<gitbolt::git::BranchInfo> branches);
    void commitComplete(bool success, const QString& message);
    void operationFailed(const QString& operation, const QString& error);
    void repositoryChanged();

    // Phase 6 signals — Rebase / Cherry-pick / Stash
    void rebaseComplete(bool success);
    void cherryPickComplete(bool success, const QString& message);
    void stashesReady(std::vector<gitbolt::git::StashEntry> stashes);

    // Phase 7 signals
    void tagsReady(std::vector<gitbolt::git::TagInfo> tags);
    void submodulesReady(std::vector<gitbolt::git::SubmoduleInfo> submodules);
    void worktreesReady(std::vector<gitbolt::git::WorktreeInfo> worktrees);

    // Phase 11 signals — Git Flow & Maintenance
    void gitFlowOperationComplete(bool success, const QString& message);
    void maintenanceComplete(const QString& output);

private:
    std::unique_ptr<git::Repository> repo_;
    util::AsyncRunner runner_;
    watcher::FileWatcher watcher_;
    LogScope logScope_ = LogScope::Head;
    QStringList selectedBranches_;

    // libgit2 is not safe for concurrent access on a single
    // git_repository*. AsyncRunner submits to the global QThreadPool,
    // so two refreshStatus / refreshLog / refreshBranches calls can
    // land on different worker threads and corrupt libgit2's internal
    // pool state. Real crash seen in the wild was a malloc_zone_error
    // inside git_pool_clear under git_status_list_new while another
    // worker was reading the log. Every Repository call from worker
    // threads (and from the main-thread mutators that compete with
    // them) acquires this mutex first.
    mutable std::mutex repoMutex_;
};

} // namespace gitbolt::services
