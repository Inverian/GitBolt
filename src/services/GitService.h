#pragma once
#include "git/Repository.h"
#include "util/AsyncRunner.h"
#include "watcher/FileWatcher.h"
#include <QObject>
#include <QStringList>
#include <QThread>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

namespace gitbolt::services {

/// Threading rule. A GitService's state belongs to the thread it lives
/// on (the GUI thread): repo_ is swapped there, the log scope and
/// branch selection are set there, and only that thread submits work
/// to runner_. Any other thread may only
///   - read repo_ while holding repoMutex_ (the refresh workers),
///   - emit signals (receivers on the GUI thread get them queued),
///   - call push / pull / fetch / deleteRemoteBranch, which MainWindow
///     runs on a pool thread, and
///   - call refresh*() and blameFile(), which queue themselves onto
///     the service's thread before reading any state.
/// So a worker that wants fresh data after its operation simply calls
/// refreshStatus() & co.; the request lands behind any completion
/// signal the worker emitted first, so handlers see e.g.
/// rebaseStepFinished before the statusReady it causes.
///
/// What the refresh workers and the rebase steps read goes out from
/// the service's thread (deliverFor), and only while their repository
/// is still the open one: the check on the worker alone let a result
/// posted just before a swap reach the GUI after it, as the next
/// repository's.
class GitService : public QObject {
    Q_OBJECT
public:
    explicit GitService(QObject* parent = nullptr);
    ~GitService() override;

    /// Synchronous open — blocks the calling thread on libgit2 open
    /// AND the working-tree watch walk. Kept for RepoManager and
    /// tests; interactive UI paths should use openRepositoryAsync()
    /// so large repositories don't freeze the window.
    bool openRepository(const QString& path);

    /// Non-blocking open: git_repository_open runs on a worker, the
    /// working-tree watch enumeration runs on another worker, and
    /// completion is reported via repositoryOpened (success) or
    /// repositoryOpenFailed (failure). A newer openRepositoryAsync /
    /// openRepository / closeRepository call supersedes an in-flight
    /// one — the stale result is discarded without any signal.
    /// Returns this open's generation: initialLogLoaded carries it
    /// with the open's first history page.
    quint64 openRepositoryAsync(const QString& path);

    /// The generation of the latest open or close: equal to what
    /// openRepositoryAsync returned while that open is the latest.
    quint64 openGeneration() const { return openGeneration_; }

    void closeRepository();
    bool isOpen() const;
    git::Repository* repository() const;

    /// Mutex that serializes all libgit2 access through this service.
    /// libgit2 is not safe for concurrent access on a single
    /// git_repository*, and GitService's background workers
    /// (refreshStatus / refreshLog / refreshBranches / the watcher-
    /// triggered refreshes) hold this lock while they run. The
    /// seen-in-the-wild crash was a malloc_zone_error inside
    /// git_pool_clear under git_status_list_new while another worker
    /// was reading the log.
    ///
    /// UI code should NOT lock this directly — use withRepository(),
    /// which can't be forgotten at one of two dozen call sites.
    std::mutex& repoMutex() const { return repoMutex_; }

    /// The only safe way for UI code to call git::Repository methods
    /// directly: runs `fn(repo)` while holding repoMutex_, so the call
    /// cannot race the background refresh workers. Returns whatever
    /// the callable returns. When no repository is open:
    ///   - git::Result<T> returns carry a "no repository open" error
    ///     (so existing `.ok()` / boolean checks just work),
    ///   - void callables are skipped,
    ///   - anything else returns a value-initialized default.
    /// Keep the callable small — the lock starves refresh workers
    /// while it runs. Never call GitService methods from inside `fn`
    /// (deadlock: the mutex is not recursive), and never let the raw
    /// Repository& escape the lambda.
    template <typename Fn>
    auto withRepository(Fn&& fn) const
        -> std::invoke_result_t<Fn&, git::Repository&>
    {
        using R = std::invoke_result_t<Fn&, git::Repository&>;
        std::lock_guard<std::mutex> lock(repoMutex_);
        if (!repo_) {
            if constexpr (std::is_void_v<R>) {
                return;
            } else if constexpr (IsGitResult<R>::value) {
                return git::GitError(git::GitErrorCode::GenericError,
                                     "no repository open");
            } else {
                return R{};
            }
        }
        return std::forward<Fn>(fn)(*repo_);
    }

    /// Thread-safe construction of a GitProcess for the open repository.
    /// Locks repoMutex_ briefly while libgit2 reports the workdir, then
    /// returns by value. Once you have the GitProcess object you can
    /// run shell commands on it without holding the lock — QProcess is
    /// independent of libgit2 state. Its commands stop on
    /// cancelGitOps(), so a command run on a pool thread can't hold up
    /// quitting.
    git::GitProcess process() const;

    /// Whether a job the service runs (a refresh, a rebase step, a
    /// checkout, Git Flow, maintenance) is still running. MainWindow
    /// hides itself while quitting waits for them.
    bool workRunning() const { return runner_.busy(); }

    void refreshStatus();
    /// Commits per history page (CommitLogModel pages by the same).
    static constexpr int kLogPageSize = 256;
    void refreshLog(int offset = 0, int count = kLogPageSize);
    void refreshBranches();

    /// Async per-line blame for a repo-relative path. Result lands
    /// via blameReady; failures via operationFailed("blame", …).
    /// `newestCommitSpec` is any revspec ("", "HEAD", "<sha>^", …);
    /// empty blames at HEAD. "<sha>^" is how the blame view's
    /// "Blame Before" re-blames at the parent of a commit.
    void blameFile(const QString& path,
                   const QString& newestCommitSpec = QString());

    /// Async read of the index's conflicted entries (three-way
    /// payloads). Result lands via conflictsReady — empty when the
    /// repository has no conflicts. Works for conflicts from any
    /// source: merge, cherry-pick, rebase, revert.
    void refreshConflicts();

    /// Write each (repo-relative path, resolved content) pair to
    /// the working tree and stage it, collapsing the conflict
    /// entries. Synchronous (file writes + index adds are fast);
    /// failures surface per-file via operationFailed. Ends with a
    /// status refresh. The caller then concludes the merge with a
    /// normal commit — git commit (commitChanges) picks up MERGE_HEAD
    /// as a second parent automatically.
    /// Returns false, having written nothing, while a rebase step runs
    /// on the repository (see rebaseStepRunning()), reported as an
    /// operationFailed.
    bool resolveConflicts(
        const std::vector<std::pair<QString, QString>>& resolutions);

    /// Abort whatever conflicted operation is in progress, using
    /// the command that matches the repository state: merge /
    /// cherry-pick / rebase / revert --abort. No-op when the
    /// repository is in a normal state. git runs on a worker (a rebase
    /// as a rebase step, reported on rebaseStepFinished); a failure
    /// comes as operationFailed("abort", …), and the refreshes follow.
    /// Returns false, running nothing, while a rebase step runs on the
    /// repository, reported as an operationFailed.
    bool abortConflictState();

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

    /// Apply a unified patch to the INDEX only (`git apply --cached`),
    /// reversed when `reverse` is true. This is how hunk- and
    /// line-level staging work: the diff pane builds a minimal patch
    /// for the chosen hunk/lines (git::buildHunkPatch /
    /// buildLinesPatch) and stages it without touching the working
    /// tree. Failures (e.g. the diff went stale because the file
    /// changed since it was rendered) surface via operationFailed.
    void applyPatchToIndex(const QString& patchText, bool reverse);
    void discardFile(const QString& path);
    /// Commits the index through git itself (GitProcess::commit), on a
    /// worker: the hooks may take minutes. Reports on commitComplete,
    /// with git's own words (a hook's too) when it fails, then
    /// refreshes. An amend that Repository::amendProblem() refuses
    /// never reaches git, a revert's included, which git would allow.
    /// When the merge concluded had stashed changes away
    /// (merge.autoStash), git puts them back; if that conflicts, they
    /// stay in the stash, reported as operationFailed("autostash").
    /// Nothing is reported if another repository was opened meanwhile.
    void commitChanges(const QString& message, bool amend = false);

    void createBranch(const QString& name);
    void deleteBranch(const QString& name);
    /// False when the checkout failed (reported as an operationFailed)
    /// or no repository is open.
    bool checkoutBranch(const QString& name);
    /// The sidebar's "Checkout as local branch" on `remoteBranch`
    /// ("origin/feature/x"): the local branch named for it
    /// ("feature/x"), created to track it unless it exists. git runs on
    /// a worker: a first checkout can take minutes. Reports failure as
    /// operationFailed("checkout", …).
    void checkoutRemoteBranch(const QString& remoteBranch);

    // The remote ops return false exactly when they reported a failure
    // (operationFailed) or no repository is open: the outcome of the op
    // MainWindow ran, whatever else failed meanwhile.

    /// Pushes `branch` (empty = the checked-out branch). A branch with
    /// no upstream yet is published with --set-upstream instead of
    /// failing. Safe off the GUI thread, like pull/fetch.
    bool push(const QString& remote, const QString& branch);
    /// `git push <remote> --delete <branch>`: removes the branch on the
    /// server and, on success, the local remote-tracking ref. Failures
    /// surface as operationFailed("delete remote branch", …). Safe off
    /// the GUI thread.
    bool deleteRemoteBranch(const QString& remote, const QString& branch);
    bool pull(const QString& remote, const QString& branch);
    /// Prunes the remote-tracking refs of branches deleted on the
    /// server where git config doesn't say: with fetch.prune or
    /// remote.<name>.prune set, git's own rules decide.
    /// `cancel`, when given, stops this fetch alone: set from any
    /// thread, it ends git the way cancelGitOps() does (which stops
    /// this fetch as well). MainWindow drops an auto-fetch with it.
    /// Without `allowPrompts` git and ssh ask the user nothing, and a
    /// fetch that needs an answer fails (GitProcess::forbidPrompts):
    /// for the auto-fetch, which nobody is waiting on.
    bool fetch(const QString& remote = "",
               std::shared_ptr<std::atomic<bool>> cancel = nullptr,
               bool allowPrompts = true);
    /// Stops every git command the service, or anyone with a process()
    /// of it, runs: a push, pull, fetch or remote-branch delete, a
    /// rebase step, a commit and its hooks, a checkout, Git Flow,
    /// maintenance, a local op MainWindow runs on a pool thread. They fail with "Cancelled",
    /// and later ones fail at once. git and its hooks get SIGTERM, as
    /// on Ctrl-C, so a rebase stopped mid-step stays in progress, to be
    /// carried on later. For shutdown: MainWindow calls it before
    /// waiting for its pool threads, and the destructor before waiting
    /// for the service's own. Thread-safe.
    void cancelGitOps();

    // Interactive Rebase. Each step reports on rebaseStepFinished.
    // One step at a time per repository: another asked for while one
    // runs is refused, with an operationFailed and no
    // rebaseStepFinished (the running one's still comes).
    void interactiveRebase(const git::RebasePlan& plan);
    void rebaseContinue();
    void rebaseAbort();
    void rebaseSkip();

    /// Why `plan` can't run on the open repository as it is now, or an
    /// empty string when it can: HEAD has moved, or another branch is
    /// checked out, since the plan was made (or no repository is
    /// open). interactiveRebase() refuses such a plan; asking first
    /// lets the Rebase dialog stay open with it.
    QString rebasePlanProblem(const git::RebasePlan& plan) const;

    /// Whether a rebase step is still running on the open repository —
    /// started on it, or on an earlier open of it (Recent, a hand-off
    /// of the same path), whose outcome still comes.
    bool rebaseStepRunning() const;

    /// A cherry-pick or revert of several commits paused between them
    /// (Repository::sequencePaused()): `git cherry-pick --continue`
    /// picks the rest (revert's reverts it), Skip leaves out the commit
    /// it stopped at first. Steps as the rebase's are, one at a time,
    /// reported on rebaseStepFinished as "cherry-pick --continue",
    /// "revert --skip", ….
    void sequenceContinue();
    void sequenceSkip();

    /// Stages `commit`'s changes: libgit2's cherry-pick never commits,
    /// and the user's commit then concludes it, keeping its author.
    /// Picking several this way can't work: the next one meets the
    /// first one's uncommitted changes. While another operation is in
    /// progress (a merge, a pick not committed yet) it is refused with
    /// an operationFailed alone, as Repository::cherryPick refuses it.
    void cherryPick(const git::ObjectId& commit);

    // Stash
    void stashSave(const QString& message, bool includeUntracked = false,
                   bool keepIndex = false);
    void stashApply(int index);
    void stashPop(int index);
    void stashDrop(int index);
    void refreshStashes();

    // Tags
    void refreshTags();
    /// `target` is any revision spec (branch, short or full SHA, …).
    /// `pushAfter` pushes the new tag to origin once creation
    /// succeeds (same synchronous CLI path as push()); MainWindow
    /// pushes it as a remote op of its own instead. False when the tag
    /// (or the push) failed, reported as an operationFailed.
    bool createTag(const QString& name, const QString& target,
                   const QString& message, bool annotated,
                   bool pushAfter = false);
    /// `name` may be short ("v1.0") or the full "refs/tags/v1.0".
    void deleteTag(const QString& name);

    // Submodules. Init and update run git on a worker: an update
    // clones and checks out.
    void refreshSubmodules();
    void submoduleInit(const QString& name);
    void submoduleUpdate(const QString& name);

    // Worktrees
    void refreshWorktrees();
    /// `createBranch` mirrors `git worktree add -b`: create `branch`
    /// at HEAD and check it out in the new worktree, instead of
    /// requiring an existing branch.
    void addWorktree(const QString& name, const QString& path, const QString& branch,
                     bool createBranch = false);
    void removeWorktree(const QString& name);
    void lockWorktree(const QString& name);
    void unlockWorktree(const QString& name);

    // Git Flow
    bool isGitFlowInitialized();
    /// Initialize git-flow non-interactively. The names are written
    /// to gitflow.* config first; `git flow init -d` then adopts the
    /// configured values as its defaults (AVH behavior), so the
    /// wizard's answers actually take effect.
    void gitFlowInit(const QString& master = QStringLiteral("master"),
                     const QString& develop = QStringLiteral("develop"),
                     const QString& featurePrefix = QStringLiteral("feature/"),
                     const QString& releasePrefix = QStringLiteral("release/"),
                     const QString& hotfixPrefix = QStringLiteral("hotfix/"));
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
    /// `errorCode`: the git::GitErrorCode, for usage statistics'
    /// repo_open_failed, which never sends the message.
    void repositoryOpenFailed(const QString& path, const QString& error, int errorCode);
    void repositoryClosed();
    void statusReady(std::vector<gitbolt::git::StatusEntry> entries);
    /// Right after each statusReady (and in its place when status
    /// couldn't be read): the operation the repository is in the
    /// middle of, if any, and how many files are still conflicted.
    /// Also just before each rebaseStepFinished, with the state the
    /// step left behind. `sequencePaused`: `state` is a cherry-pick or
    /// revert of several commits, paused between two of them
    /// (Repository::sequencePaused()).
    void repoStateReady(gitbolt::git::RepoState state, int conflicts, bool sequencePaused);
    void logReady(std::vector<gitbolt::git::CommitData> commits, int offset);
    /// Right behind the logReady(…, 0) of the first history page a
    /// successful openRepositoryAsync loads: that open's generation and
    /// the page's size. A refresh of the previous repository that was
    /// still walking when the open landed delivers its page after
    /// repositoryOpened too, so only this tells the new repository's
    /// first page apart (MainWindow times opens by it).
    void initialLogLoaded(quint64 openGeneration, int commits);
    void branchesReady(std::vector<gitbolt::git::BranchInfo> branches);
    void blameReady(gitbolt::git::BlameResult result);
    void conflictsReady(std::vector<gitbolt::git::MergeConflictEntry> conflicts);
    void commitComplete(bool success, const QString& message);
    void operationFailed(const QString& operation, const QString& error);
    void repositoryChanged();

    // Phase 6 signals — Rebase / Cherry-pick / Stash
    /// After each rebase step — "rebase" (interactiveRebase),
    /// "rebase --continue", "--skip" or "--abort" — and each step of a
    /// sequence of cherry-picks or reverts ("cherry-pick --continue",
    /// "revert --skip", …). `success` is false when git failed or
    /// stopped on a conflict, right after an operationFailed with git's
    /// message. `inProgress`: the repository is still in the middle of
    /// the step's operation — after a conflict, or after a successful
    /// rebase step that stopped where the plan said `edit`. Not emitted
    /// for a step whose repository was closed or swapped for another
    /// while git ran: the result would land on another one. (Opened
    /// again, the same repository still gets it.)
    void rebaseStepFinished(const QString& step, bool success, bool inProgress);
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
    // Trait backing withRepository's closed-repo fallback: detects
    // git::Result<T> so it can synthesize an error instead of a
    // default-constructed value.
    template <typename>
    struct IsGitResult : std::false_type {};
    template <typename T>
    struct IsGitResult<git::Result<T>> : std::true_type {};

    /// Kicks the working-tree watch enumeration onto a worker and
    /// applies the result on the main thread (QFileSystemWatcher is
    /// not thread-safe). `gen` ties the apply to the open that
    /// requested it.
    void startWatchEnumeration(const QString& path, quint64 gen);

    /// refreshLog's worker. `openGeneration`, when not 0, marks the
    /// first page of that open, which then also emits
    /// initialLogLoaded.
    void loadLog(int offset, int count, quint64 openGeneration);

    /// The threading rule's hop. Returns false on the service's own
    /// thread. Anywhere else it queues `retry` onto that thread, with
    /// this as the context so the retry is dropped if the service is
    /// destroyed first, and returns true: the caller must then return
    /// without touching any state.
    template <typename Fn>
    bool deferToOwnerThread(Fn&& retry) {
        if (QThread::currentThread() == thread())
            return false;
        QMetaObject::invokeMethod(this, std::forward<Fn>(retry),
                                  Qt::QueuedConnection);
        return true;
    }

    /// A refresh worker's way out: `emitResult` runs on the service's
    /// thread, and only if `r` is still the open repository and no open
    /// is pending. The swap happens on that thread too, so nothing can
    /// come in between. Checked on the worker alone, a result posted
    /// just before an open landed was delivered right after it: the
    /// previous repository's history, status or branches under the next
    /// one's name, and its first page let the loading overlay go.
    /// While an open is pending the window shows neither repository,
    /// and a failed one asks for every refresh again.
    template <typename Fn>
    void deliverFor(std::shared_ptr<git::Repository> r, Fn&& emitResult) {
        QMetaObject::invokeMethod(
            this,
            [this, r = std::move(r), fn = std::forward<Fn>(emitResult)]() mutable {
                if (repo_ == r && !openPending_)
                    fn();
            },
            Qt::QueuedConnection);
    }

    /// Runs one rebase step on a worker (`step` names it in
    /// operationFailed and rebaseStepFinished): reports a failure,
    /// emits repoStateReady and rebaseStepFinished, and asks for the
    /// refreshes. `refusal`, run first under repoMutex_, says why the
    /// step must not run, if it mustn't; the step then fails with that
    /// and git never starts. Refused at once, with operationFailed
    /// alone, while another step runs on the repository. `operation`
    /// is what the step carries on (a sequence's steps run here too):
    /// rebaseStepFinished's `inProgress` says the repository is still
    /// in it.
    void runRebaseStep(
        const QString& step,
        std::function<git::Result<git::ProcessOutput>(const git::GitProcess&)> command,
        std::function<QString(const git::Repository&)> refusal = {},
        git::RepoState operation = git::RepoState::Rebase);

    /// sequenceContinue() and sequenceSkip(): `skip` picks which.
    void runSequenceStep(bool skip);

    /// A GitProcess for the open repository, or nullopt when none is
    /// open. Checks and reads repo_ under repoMutex_, so the network
    /// ops can use it from MainWindow's pool thread. Its commands stop
    /// on cancelGitOps(), and once `cancel` is set when given.
    std::optional<git::GitProcess> processIfOpen(
        std::shared_ptr<std::atomic<bool>> cancel = nullptr) const;

    /// Set by cancelGitOps(); added to every GitProcess the service
    /// hands out or runs, a commit's included.
    const std::shared_ptr<std::atomic<bool>> opsCancelled_ =
        std::make_shared<std::atomic<bool>>(false);

    // An openRepositoryAsync is in flight (service thread only): no
    // refresh result is delivered meanwhile (deliverFor).
    bool openPending_ = false;
    // rebaseStepKey() of the open repository, empty when none is
    // (service thread only): a rebase step's outcome is delivered only
    // while it matches the step's.
    std::string openKey_;

    // DESTRUCTION ORDER MATTERS in this section. ~AsyncRunner blocks
    // until every worker finishes, and those workers lock repoMutex_
    // and compare against repo_ — so runner_ is declared LAST (and
    // therefore destroyed first, draining the workers while the
    // mutex and repo are still alive).

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

    // shared_ptr (not unique_ptr) for two reasons: the async-open
    // worker hands a freshly opened Repository back through a
    // QFuture, which requires a copyable result type; and refresh
    // workers capture a shared_ptr so a repo swap mid-refresh can't
    // destroy the object under their feet (they finish against the
    // old repo, then notice it's stale and drop their result).
    std::shared_ptr<git::Repository> repo_;

    // The repositories a rebase step is running on, by git dir
    // (Repository::path(): the same for every open of one). Under
    // repoMutex_: a step's worker takes its own out when git is done.
    std::set<std::string> rebaseSteps_;

    LogScope logScope_ = LogScope::Head;
    QStringList selectedBranches_;

    // Monotonic token for open/close operations. Bumped on every
    // openRepository / openRepositoryAsync / closeRepository call
    // (main thread only); async completions capture the value at
    // start and discard themselves if it moved on — so a rapid
    // second open cleanly supersedes the first with no stale
    // signals.
    quint64 openGeneration_ = 0;

    // Cancellation flag for the in-flight watch-directory walk.
    // Replaced (not just toggled) per enumeration so each walk has
    // its own flag; the old walk sees its flag flip and bails.
    std::shared_ptr<std::atomic<bool>> watchEnumCancel_;

    watcher::FileWatcher watcher_;

    // Declared last on purpose: ~AsyncRunner drains every in-flight
    // worker, and it must do so while repoMutex_ / repo_ above are
    // still alive (members destruct in reverse declaration order).
    util::AsyncRunner runner_;
};

} // namespace gitbolt::services
