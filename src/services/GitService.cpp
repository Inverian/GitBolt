#include "services/GitService.h"

#include <QDir>
#include <QFile>

namespace gitbolt::services {

GitService::GitService(QObject* parent)
    : QObject(parent), watcher_(this), runner_(this) {
    connect(&watcher_, &watcher::FileWatcher::repositoryChanged, this, [this]() {
        refreshStatus();
        emit repositoryChanged();
    });
}

GitService::~GitService() {
    // Abort any in-flight watch-directory walk so app shutdown
    // isn't held up by a worker stat()ing a huge tree for nothing.
    if (watchEnumCancel_)
        watchEnumCancel_->store(true);
}

bool GitService::openRepository(const QString& path) {
    // A sync open supersedes any pending async open (and its
    // enumeration) the same way a newer async open would.
    ++openGeneration_;
    if (watchEnumCancel_)
        watchEnumCancel_->store(true);

    auto result = git::Repository::open(path.toStdString());
    if (!result) return false;
    {
        // Swap under the libgit2 mutex so an in-flight refresh
        // worker can't have the old repo destroyed mid-call.
        std::lock_guard<std::mutex> lock(repoMutex_);
        repo_ = std::make_shared<git::Repository>(std::move(*result));
    }
    watcher_.watchRepository(path);
    emit repositoryOpened(path);
    refreshStatus();
    refreshLog();
    refreshBranches();
    return true;
}

void GitService::openRepositoryAsync(const QString& path) {
    const quint64 gen = ++openGeneration_;
    if (watchEnumCancel_)
        watchEnumCancel_->store(true);

    // Worker outcome: exactly one of repo / error is set. Must be
    // copyable + default-constructible for QFutureWatcher::result().
    struct OpenOutcome {
        std::shared_ptr<git::Repository> repo;
        QString error;
    };

    runner_.runWithResult<OpenOutcome>(
        [path]() -> OpenOutcome {
            // Fresh git_repository* — no repoMutex_ needed; nothing
            // else can touch this object until we publish it below.
            auto result = git::Repository::open(path.toStdString());
            if (!result)
                return { nullptr,
                         QString::fromStdString(result.error().message()) };
            return { std::make_shared<git::Repository>(std::move(*result)),
                     QString() };
        },
        [this, path, gen](OpenOutcome outcome) {
            // Main thread. A newer open/close superseded us — drop
            // everything silently (the Repository, if any, is
            // destroyed with the outcome).
            if (gen != openGeneration_)
                return;

            if (!outcome.repo) {
                emit repositoryOpenFailed(path, outcome.error);
                return;
            }

            {
                std::lock_guard<std::mutex> lock(repoMutex_);
                repo_ = std::move(outcome.repo);
            }

            // Cheap, immediate watches (.git internals + repo root)
            // now; the expensive working-tree walk goes to a worker.
            watcher_.watchGitInternals(path);
            startWatchEnumeration(path, gen);

            // Emit BEFORE kicking refreshes: onRepositoryOpened
            // handlers read repository() (headBranchName etc.) on
            // the main thread, and direct-connection slots running
            // synchronously inside this emit are guaranteed to
            // finish before any refresh worker grabs repoMutex_.
            emit repositoryOpened(path);
            refreshStatus();
            refreshLog();
            refreshBranches();
        });
}

void GitService::startWatchEnumeration(const QString& path, quint64 gen) {
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    watchEnumCancel_ = cancel;

    runner_.runWithResult<QStringList>(
        [path, cancel]() {
            // Pure filesystem walk — no libgit2, no QFileSystemWatcher,
            // so no mutex. Captures only value copies; safe even if
            // the service is torn down while we run.
            return watcher::FileWatcher::enumerateWatchDirs(
                path, 4096, [cancel]() { return cancel->load(); });
        },
        [this, path, gen](QStringList dirs) {
            if (gen != openGeneration_)
                return;
            // Applied on the main thread (QFileSystemWatcher's
            // thread); FileWatcher chunks the registrations across
            // event-loop ticks internally.
            watcher_.addWatchPaths(path, dirs);
        });
}

void GitService::closeRepository() {
    // Invalidate any in-flight async open + watch enumeration.
    ++openGeneration_;
    if (watchEnumCancel_)
        watchEnumCancel_->store(true);

    watcher_.stop();
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        repo_.reset();
    }
    emit repositoryClosed();
}

bool GitService::isOpen() const { return repo_ != nullptr; }
git::Repository* GitService::repository() const { return repo_.get(); }

git::GitProcess GitService::process() const {
    std::lock_guard<std::mutex> lock(repoMutex_);
    return repo_->process();
}

std::optional<git::GitProcess> GitService::processIfOpen() const {
    std::lock_guard<std::mutex> lock(repoMutex_);
    if (!repo_) return std::nullopt;
    git::GitProcess proc = repo_->process();
    proc.setCancelFlag(remoteOpsCancelled_);
    return proc;
}

void GitService::cancelRemoteOps() {
    remoteOpsCancelled_->store(true);
}

// Refresh workers capture repo_ as a shared_ptr (pinning the object
// alive across a concurrent repo swap) and re-check `r == repo_`
// under repoMutex_ before doing any work: if the user opened a
// different repository while this job sat in the thread-pool queue,
// the job must neither touch the swapped-out repo nor emit results
// the UI would misattribute to the new one. Reading repo_ from the
// worker is safe because every mutation of repo_ happens under
// repoMutex_ (openRepository / openRepositoryAsync / closeRepository).
//
// Each entry point first hops to the service's thread (the threading
// rule in GitService.h): the operation workers, and MainWindow's pool
// thread via pull/fetch, call these when they finish. Run there, the
// unlocked repo_ / logScope_ / selectedBranches_ reads below raced
// the GUI thread, and runner_ was driven from a pool thread.
void GitService::refreshStatus() {
    if (deferToOwnerThread([this] { refreshStatus(); })) return;
    if (!repo_) return;
    std::shared_ptr<git::Repository> r = repo_;
    runner_.run([this, r]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        if (r != repo_) return;   // superseded by a repo switch
        auto result = r->status();
        if (result) emit statusReady(std::move(*result));
    });
}

void GitService::refreshLog(int offset, int count) {
    if (deferToOwnerThread([this, offset, count] { refreshLog(offset, count); }))
        return;
    if (!repo_) return;
    std::shared_ptr<git::Repository> r = repo_;
    const LogScope scope = logScope_;
    // Snapshot so the worker lambda doesn't race with a UI-thread
    // setSelectedBranches() during the walk.
    const QStringList selected = selectedBranches_;
    runner_.run([this, r, offset, count, scope, selected]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        if (r != repo_) return;   // superseded by a repo switch
        auto walk = r->createRevWalk();
        if (!walk) {
            // Still emit for the initial page: the UI uses the first
            // logReady(offset==0) as "loading finished" (it closes
            // the open-repo spinner), so error paths must produce
            // closure too — an empty log, same end state as today.
            if (offset == 0)
                emit logReady(std::vector<git::CommitData>{}, 0);
            return;
        }
        walk->setSorting(git::SortOrder::TopologicalTime);
        if (scope == LogScope::AllLocalBranches) {
            // Push every local branch tip so the walk includes
            // commits reachable from any branch (mirrors
            // `git log --branches`). RevWalk dedupes commits
            // visited via multiple paths.
            if (auto br = r->branches(git::BranchType::Local); br.ok()) {
                for (const auto& b : br.value())
                    walk->push(b.tipId);
            } else {
                // Fall back to HEAD if branch enumeration fails.
                walk->pushHead();
            }
        } else if (scope == LogScope::SelectedBranches) {
            // Push only the user-picked branches. We look up each
            // by short name to get its tip OID; unknown names are
            // skipped silently (the picker only offers existing
            // branches, but a branch could be deleted between the
            // pick and the refresh). If the resulting set is empty
            // we fall back to HEAD so the log isn't blank.
            int pushed = 0;
            if (auto br = r->branches(git::BranchType::Local); br.ok()) {
                // Build a name -> tipId index once instead of
                // calling resolveRef per name (the repo may have
                // hundreds of branches; the picker dedupes selection).
                const auto& branches = br.value();
                for (const QString& name : selected) {
                    const std::string n = name.toStdString();
                    for (const auto& b : branches) {
                        // BranchInfo::name is the short name from
                        // git_branch_name (no "refs/heads/" prefix),
                        // which matches what the picker stored.
                        if (b.name == n) {
                            walk->push(b.tipId);
                            ++pushed;
                            break;
                        }
                    }
                }
            }
            if (pushed == 0) walk->pushHead();
        } else {
            walk->pushHead();
        }
        auto commits = walk->next(static_cast<size_t>(count));
        if (commits)
            emit logReady(std::move(*commits), offset);
        else if (offset == 0)
            emit logReady(std::vector<git::CommitData>{}, 0);
    });
}

void GitService::setLogScope(LogScope scope) {
    if (logScope_ == scope) return;
    logScope_ = scope;
    if (repo_) refreshLog();
}

void GitService::setSelectedBranches(const QStringList& branchNames) {
    selectedBranches_ = branchNames;
    if (logScope_ == LogScope::SelectedBranches && repo_)
        refreshLog();
}

void GitService::refreshBranches() {
    if (deferToOwnerThread([this] { refreshBranches(); })) return;
    if (!repo_) return;
    std::shared_ptr<git::Repository> r = repo_;
    runner_.run([this, r]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        if (r != repo_) return;   // superseded by a repo switch
        auto result = r->allBranches();
        if (result) emit branchesReady(std::move(*result));
    });
}

void GitService::refreshConflicts() {
    if (deferToOwnerThread([this] { refreshConflicts(); })) return;
    if (!repo_) return;
    std::shared_ptr<git::Repository> r = repo_;
    runner_.run([this, r]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        if (r != repo_) return;   // superseded by a repo switch
        auto result = r->conflictEntries();
        if (result)
            emit conflictsReady(std::move(*result));
        else
            emit operationFailed(QStringLiteral("conflicts"),
                QString::fromStdString(result.error().message()));
    });
}

void GitService::resolveConflicts(
        const std::vector<std::pair<QString, QString>>& resolutions) {
    if (!repo_) return;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        const QString workdir =
            QString::fromStdString(repo_->workdir());
        for (const auto& [path, content] : resolutions) {
            QFile f(QDir(workdir).filePath(path));
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                emit operationFailed(
                    QStringLiteral("resolve conflicts"),
                    tr("Could not write %1: %2")
                        .arg(path, f.errorString()));
                continue;
            }
            f.write(content.toUtf8());
            f.close();

            // Staging a conflicted path collapses its conflict
            // entries to a normal stage-0 entry — same as
            // `git add` during manual resolution.
            auto res = repo_->stageFile(path.toStdString());
            if (!res)
                emit operationFailed(
                    QStringLiteral("resolve conflicts"),
                    QString::fromStdString(res.error().message()));
        }
    }
    refreshStatus();
}

void GitService::abortConflictState() {
    if (!repo_) return;

    git::RepoState state = git::RepoState::None;
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        state = repo_->state();
        return repo_->process();
    }()};

    std::vector<std::string> args;
    switch (state) {
    case git::RepoState::Merge:
        args = {"merge", "--abort"};
        break;
    case git::RepoState::CherryPick:
        args = {"cherry-pick", "--abort"};
        break;
    case git::RepoState::Rebase:
        args = {"rebase", "--abort"};
        break;
    case git::RepoState::Revert:
        args = {"revert", "--abort"};
        break;
    case git::RepoState::None:
    case git::RepoState::Other:
        return;   // nothing in progress to abort
    }

    auto result = proc.run(args);
    if (!result) {
        emit operationFailed(QStringLiteral("abort"),
            QString::fromStdString(result.error().message()));
    } else if (!result->success()) {
        QString detail =
            QString::fromStdString(result->stderrData).trimmed();
        if (detail.isEmpty())
            detail = QString::fromStdString(result->stdoutData).trimmed();
        emit operationFailed(QStringLiteral("abort"), detail);
    }
    refreshStatus();
    refreshLog();
    refreshBranches();
}

void GitService::blameFile(const QString& path,
                           const QString& newestCommitSpec) {
    if (deferToOwnerThread([this, path, newestCommitSpec] {
            blameFile(path, newestCommitSpec);
        }))
        return;
    if (!repo_) return;
    std::shared_ptr<git::Repository> r = repo_;
    const std::string p = path.toStdString();
    const std::string spec = newestCommitSpec.toStdString();
    runner_.run([this, r, p, spec]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        if (r != repo_) return;   // superseded by a repo switch

        git::ObjectId newest;     // zero = blame at HEAD
        if (!spec.empty()) {
            auto resolved = r->resolveRef(spec);
            if (!resolved) {
                // Typical case: "<root-commit>^" from Blame Before
                // on the initial commit — there is no parent.
                emit operationFailed(QStringLiteral("blame"),
                    QString::fromStdString(resolved.error().message()));
                return;
            }
            newest = *resolved;
        }

        auto result = r->blame(p, newest);
        if (result)
            emit blameReady(std::move(*result));
        else
            emit operationFailed(QStringLiteral("blame"),
                QString::fromStdString(result.error().message()));
    });
}

// Synchronous mutators below: they all run on the main thread but
// schedule async refreshes that touch repo_ on a worker. Locking here
// serializes them against any in-flight worker — without this, a
// commit on the main thread can race with a refreshStatus worker
// that's mid-libgit2-call and corrupt the heap. Same lock as the
// worker lambdas, so a worker waits for the mutator to finish, and
// vice versa.
// Each mutator surfaces libgit2 failures via operationFailed —
// MainWindow has a global handler (and CommitDialog its own), so a
// failed click produces visible feedback instead of the button
// silently doing nothing. The refresh that follows runs on both
// paths: even after a failure the on-disk state may have shifted
// and a resync is cheap.
void GitService::stageFile(const QString& path) {
    if (!repo_) return;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        auto result = repo_->stageFile(path.toStdString());
        if (!result)
            emit operationFailed(QStringLiteral("stage"),
                QString::fromStdString(result.error().message()));
    }
    refreshStatus();
}

void GitService::unstageFile(const QString& path) {
    if (!repo_) return;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        auto result = repo_->unstageFile(path.toStdString());
        if (!result)
            emit operationFailed(QStringLiteral("unstage"),
                QString::fromStdString(result.error().message()));
    }
    refreshStatus();
}

void GitService::stageAll() {
    if (!repo_) return;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        auto result = repo_->stageAll();
        if (!result)
            emit operationFailed(QStringLiteral("stage all"),
                QString::fromStdString(result.error().message()));
    }
    refreshStatus();
}

void GitService::applyPatchToIndex(const QString& patchText,
                                   bool reverse) {
    if (!repo_ || patchText.isEmpty()) return;

    // Only the repo_->process() call needs the mutex (it reads the
    // workdir via libgit2); `git apply` itself is a subprocess and
    // independent of libgit2 state — same pattern as unstageAll.
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};

    std::vector<std::string> args{"apply", "--cached"};
    if (reverse)
        args.push_back("--reverse");
    args.push_back("-");   // read the patch from stdin

    auto result = proc.runWithInput(args, patchText.toStdString());
    if (!result) {
        emit operationFailed(QStringLiteral("stage hunk"),
            QString::fromStdString(result.error().message()));
    } else if (!result->success()) {
        QString detail =
            QString::fromStdString(result->stderrData).trimmed();
        if (detail.isEmpty())
            detail = QString::fromStdString(result->stdoutData).trimmed();
        emit operationFailed(QStringLiteral("stage hunk"), detail);
    }
    refreshStatus();
}

void GitService::unstageAll() {
    if (!repo_) return;
    // `git reset` with no paths unstages everything: it rewrites the
    // index to match HEAD. This is the cleanest way to do "un-add all."
    // We use the CLI rather than libgit2 here because libgit2's
    // git_reset_default requires an explicit pathspec list, and building
    // one from the current status just to undo everything is wasted work.
    //
    // Only the `repo_->process()` call needs the mutex (it queries libgit2
    // for the workdir). The proc.run() that follows is QProcess and
    // independent of repo_, so we release the lock before it. (Earlier
    // versions of this method held the mutex through the full git reset,
    // which deadlocked when a bulk-replace produced nested
    // `std::lock_guard` calls on the same non-recursive mutex — the
    // unstageAll click then silently froze the main thread.)
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};
    auto result = proc.run({"reset"});
    if (!result) {
        emit operationFailed(QStringLiteral("unstageAll"),
            QString::fromStdString(result.error().message()));
    }
    refreshStatus();
}

void GitService::discardFile(const QString& path) {
    if (!repo_) return;
    // Discard working-tree changes for a single file by checking it out
    // from the index. The caller is expected to have already confirmed
    // with the user — this is a destructive operation.
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        auto result = repo_->discardWorkdirChanges(path.toStdString());
        if (!result) {
            emit operationFailed(QStringLiteral("discardFile"),
                QString::fromStdString(result.error().message()));
        }
    }
    refreshStatus();
}

void GitService::commitChanges(const QString& message, bool amend) {
    if (!repo_) return;
    bool ok = false;
    QString shortHex;
    QString err;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        auto result = repo_->commit(message.toStdString(), amend);
        if (result) {
            ok = true;
            shortHex = QString::fromStdString(result->toShortHex());
        } else {
            err = QString::fromStdString(result.error().message());
        }
    }
    if (ok) {
        emit commitComplete(true, "Commit created: " + shortHex);
        refreshStatus();
        refreshLog();
    } else {
        emit commitComplete(false, err);
    }
}

void GitService::createBranch(const QString& name) {
    if (!repo_) return;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        auto headResult = repo_->head();
        if (!headResult) {
            // Most common cause: unborn HEAD (fresh repo, no
            // commits) — nothing for the branch to point at yet.
            // Without this signal the New Branch button appeared
            // to simply swallow the click.
            emit operationFailed(QStringLiteral("create branch"),
                QString::fromStdString(headResult.error().message()));
            return;
        }
        auto result = repo_->createBranch(name.toStdString(), *headResult);
        if (!result)
            emit operationFailed(QStringLiteral("create branch"),
                QString::fromStdString(result.error().message()));
    }
    refreshBranches();
}

void GitService::deleteBranch(const QString& name) {
    if (!repo_) return;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        auto result = repo_->deleteBranch(name.toStdString());
        if (!result)
            emit operationFailed(QStringLiteral("delete branch"),
                QString::fromStdString(result.error().message()));
    }
    refreshBranches();
}

void GitService::checkoutBranch(const QString& name) {
    if (!repo_) return;
    bool ok = false;
    QString err;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        auto result = repo_->checkout(name.toStdString());
        ok = result.ok();
        if (!ok) err = QString::fromStdString(result.error().message());
    }
    if (!ok) {
        // Most common cause is a dirty working tree that conflicts
        // with the target branch. We surface the libgit2 error so the
        // toolbar indicator can render the red ✗ state instead of
        // misleadingly showing "✓ Switched" when nothing happened.
        emit operationFailed(QStringLiteral("checkout"), err);
        // Still kick a refresh so the UI matches reality (branchesReady
        // will fire with the unchanged list, which is fine — the combo
        // is already showing the correct current branch).
        refreshBranches();
        return;
    }
    refreshStatus();
    refreshLog();
    refreshBranches();
}

// GitProcess::run() returns a successful Result whenever the QProcess
// started and finished — the actual git exit code lives inside the
// returned ProcessOutput. So "Result ok" only means "we managed to
// invoke git", NOT "git did the thing". We have to check exitCode != 0
// ourselves and surface stderr as the failure message; otherwise a
// non-zero exit (Repository not found, push rejected, no upstream, …)
// silently looks like success in the toolbar indicator.
namespace {
inline QString stderrOrFallback(const git::ProcessOutput& out, const QString& fallback) {
    QString s = QString::fromStdString(out.stderrData).trimmed();
    return s.isEmpty() ? fallback : s;
}

// Why a rebase step failed, or an empty string if it didn't: git's
// own words when it exited non-zero, as a terminal would show them
// (a "Rebasing (1/2)" progress line is overwritten via '\r'), minus
// its "hint:" lines (advice for the command line), or why git
// couldn't run at all.
QString rebaseFailure(const git::Result<git::ProcessOutput>& result) {
    if (!result)
        return QString::fromStdString(result.error().message());
    const git::ProcessOutput& out = result.value();
    if (out.success())
        return {};
    QStringList lines;
    const QString text = stderrOrFallback(out, QString::fromStdString(out.stdoutData));
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.mid(raw.lastIndexOf(QLatin1Char('\r')) + 1).trimmed();
        if (!line.isEmpty() && !line.startsWith(QLatin1String("hint:")))
            lines.append(line);
    }
    const QString message = lines.join(QLatin1Char('\n'));
    return message.isEmpty()
        ? GitService::tr("git rebase exited with code %1").arg(out.exitCode)
        : message;
}
} // namespace

// push/pull/fetch shell out via GitProcess (QProcess) instead of
// libgit2, so the actual git command is safe to run while a libgit2
// worker is active. processIfOpen() still locks around
// `repo_->process()`, because that calls libgit2's
// git_repository_workdir() to get the path, and because these run on
// MainWindow's pool thread, where even the "is a repo open" check
// must not read repo_ unlocked. Once GitProcess is constructed (a
// plain string + QProcess), it's independent of repo_ and runs
// unlocked. Their follow-up refreshes hop to the GUI thread.
void GitService::push(const QString& remote, const QString& branch) {
    const auto snapshot = processIfOpen();
    if (!snapshot) return;
    const git::GitProcess& proc = *snapshot;
    // A branch that has never been pushed has no upstream, and the bare
    // `git push <remote>` then fails with "has no upstream branch" — so
    // the first push of every new branch failed. Publish it instead,
    // which is what a first push means in every Git GUI. A detached
    // HEAD has no branch to publish; git's own error explains that.
    std::string target = branch.toStdString();
    if (target.empty()) {
        auto head = proc.run({"symbolic-ref", "--quiet", "--short", "HEAD"});
        if (head && head.value().success())
            target = QString::fromStdString(head.value().stdoutData)
                         .trimmed().toStdString();
    }
    bool publish = false;
    if (!target.empty()) {
        auto upstream = proc.run({"rev-parse", "--abbrev-ref",
                                  "--symbolic-full-name", target + "@{upstream}"});
        publish = upstream && !upstream.value().success();
    }

    auto result = publish
        ? proc.push(remote.toStdString(), target, /*force=*/false, /*setUpstream=*/true)
        : proc.push(remote.toStdString(), branch.toStdString());
    if (!result) {
        emit operationFailed("push", QString::fromStdString(result.error().message()));
    } else if (result.value().exitCode != 0) {
        emit operationFailed("push", stderrOrFallback(result.value(),
            tr("git push exited with code %1").arg(result.value().exitCode)));
    }
}

void GitService::deleteRemoteBranch(const QString& remote, const QString& branch) {
    const auto snapshot = processIfOpen();
    if (!snapshot) return;
    const git::GitProcess& proc = *snapshot;
    auto result = proc.run({"push", remote.toStdString(), "--delete",
                            branch.toStdString()}, 120000);
    if (!result) {
        emit operationFailed(QStringLiteral("delete remote branch"),
                             QString::fromStdString(result.error().message()));
    } else if (result.value().exitCode != 0) {
        emit operationFailed(QStringLiteral("delete remote branch"),
            stderrOrFallback(result.value(),
                tr("git push --delete exited with code %1").arg(result.value().exitCode)));
    }
}

void GitService::pull(const QString& remote, const QString& branch) {
    const auto snapshot = processIfOpen();
    if (!snapshot) return;
    const git::GitProcess& proc = *snapshot;
    auto result = proc.pull(remote.toStdString(), branch.toStdString());
    if (!result) {
        emit operationFailed("pull", QString::fromStdString(result.error().message()));
    } else if (result.value().exitCode != 0) {
        emit operationFailed("pull", stderrOrFallback(result.value(),
            tr("git pull exited with code %1").arg(result.value().exitCode)));
    } else {
        refreshStatus();
        refreshLog();
        refreshBranches();
    }
}

void GitService::fetch(const QString& remote) {
    const auto snapshot = processIfOpen();
    if (!snapshot) return;
    const git::GitProcess& proc = *snapshot;
    auto result = proc.fetch(remote.toStdString());
    if (!result) {
        emit operationFailed("fetch", QString::fromStdString(result.error().message()));
    } else if (result.value().exitCode != 0) {
        emit operationFailed("fetch", stderrOrFallback(result.value(),
            tr("git fetch exited with code %1").arg(result.value().exitCode)));
    } else {
        refreshBranches();
    }
}

// ---------------------------------------------------------------------------
// Interactive Rebase
//
// From here on, the operation workers (rebase, cherry-pick, Git Flow)
// call refresh*() on a pool thread when they finish. Those calls queue
// onto the GUI thread behind the completion signal emitted just before
// them, so e.g. rebaseComplete still reaches MainWindow before the
// statusReady it causes.
// ---------------------------------------------------------------------------

void GitService::interactiveRebase(const git::RebasePlan& plan) {
    if (!repo_) return;

    // git's todo list runs oldest first; the plan lists newest first,
    // as the log does. Full hashes, so no abbreviation can turn
    // ambiguous; the subject is only there for `git status` to show.
    std::string todo;
    for (auto op = plan.operations.rbegin(); op != plan.operations.rend(); ++op) {
        const char* verb = "pick";
        switch (op->type) {
        case git::RebaseOperationType::Pick:    verb = "pick";   break;
        case git::RebaseOperationType::Reword:  verb = "reword"; break;
        case git::RebaseOperationType::Edit:    verb = "edit";   break;
        case git::RebaseOperationType::Squash:  verb = "squash"; break;
        case git::RebaseOperationType::Fixup:   verb = "fixup";  break;
        case git::RebaseOperationType::Drop:    verb = "drop";   break;
        }
        todo += verb;
        todo += " ";
        todo += op->commitId.toHex();
        todo += " ";
        todo += op->message.substr(0, op->message.find('\n'));
        todo += "\n";
    }

    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};
    runner_.run([this, proc = std::move(proc),
                 onto = plan.onto.toHex(), todo]() mutable {
        finishRebaseStep(QStringLiteral("rebase"), proc.interactiveRebase(onto, todo));
    });
}

void GitService::rebaseContinue() {
    if (!repo_) return;
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};
    runner_.run([this, proc = std::move(proc)]() mutable {
        finishRebaseStep(QStringLiteral("rebase --continue"), proc.rebaseContinue());
    });
}

void GitService::rebaseAbort() {
    if (!repo_) return;
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};
    runner_.run([this, proc = std::move(proc)]() mutable {
        const QString failure = rebaseFailure(proc.rebaseAbort());
        if (!failure.isEmpty())
            emit operationFailed(QStringLiteral("rebase --abort"), failure);
        refreshStatus();
        refreshLog();
    });
}

void GitService::rebaseSkip() {
    if (!repo_) return;
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};
    runner_.run([this, proc = std::move(proc)]() mutable {
        finishRebaseStep(QStringLiteral("rebase --skip"), proc.rebaseSkip());
    });
}

// These all reported success whenever git could be started: a rebase
// that stopped on a conflict, or never began, said "Rebase complete."
// Either way the repository may have changed, so both refresh.
void GitService::finishRebaseStep(const QString& step,
                                  const git::Result<git::ProcessOutput>& result) {
    const QString failure = rebaseFailure(result);
    if (!failure.isEmpty())
        emit operationFailed(step, failure);
    emit rebaseComplete(failure.isEmpty());
    refreshStatus();
    refreshLog();
}

// ---------------------------------------------------------------------------
// Cherry-pick
// ---------------------------------------------------------------------------

void GitService::cherryPick(const std::vector<git::ObjectId>& commits) {
    if (!repo_) return;
    // Pin the repository like every other async op does: the
    // shared_ptr keeps the object alive across a close/switch while
    // this job sits in the thread pool, and the staleness check
    // drops the work instead of cherry-picking onto a repository
    // that is no longer the open one. (This used to capture the raw
    // pointer with no re-check — a use-after-free.)
    std::shared_ptr<git::Repository> r = repo_;
    runner_.run([this, r, commits]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        if (r != repo_)
            return;  // superseded: repo closed or swapped mid-flight
        for (const auto& commitId : commits) {
            auto result = r->cherryPick(commitId);
            if (!result) {
                emit operationFailed(
                    QStringLiteral("cherryPick"),
                    QString::fromStdString(result.error().message()));
                emit cherryPickComplete(false, QString::fromStdString(result.error().message()));
                return;
            }
            if (result->hasConflicts) {
                emit cherryPickComplete(
                    false,
                    tr("Cherry-pick produced conflicts for commit %1")
                        .arg(QString::fromStdString(commitId.toShortHex())));
                refreshStatus();
                return;
            }
        }
        emit cherryPickComplete(true, tr("Cherry-pick completed successfully."));
        refreshStatus();
        refreshLog();
    });
}

// ---------------------------------------------------------------------------
// Stash
// ---------------------------------------------------------------------------

void GitService::stashSave(const QString& message, bool includeUntracked,
                           bool keepIndex) {
    if (!repo_) return;
    bool ok = false;
    QString err;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        auto result = repo_->stashSave(message.toStdString(), includeUntracked,
                                       keepIndex);
        ok = result.ok();
        if (!ok) err = QString::fromStdString(result.error().message());
    }
    if (!ok) {
        emit operationFailed(QStringLiteral("stashSave"), err);
    } else {
        refreshStatus();
        refreshStashes();
    }
}

void GitService::stashApply(int index) {
    if (!repo_) return;
    bool ok = false;
    QString err;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        auto result = repo_->stashApply(static_cast<size_t>(index));
        ok = result.ok();
        if (!ok) err = QString::fromStdString(result.error().message());
    }
    if (!ok) emit operationFailed(QStringLiteral("stashApply"), err);
    else     refreshStatus();
}

void GitService::stashPop(int index) {
    if (!repo_) return;
    bool ok = false;
    QString err;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        auto result = repo_->stashPop(static_cast<size_t>(index));
        ok = result.ok();
        if (!ok) err = QString::fromStdString(result.error().message());
    }
    if (!ok) {
        emit operationFailed(QStringLiteral("stashPop"), err);
    } else {
        refreshStatus();
        refreshStashes();
    }
}

void GitService::stashDrop(int index) {
    if (!repo_) return;
    bool ok = false;
    QString err;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        auto result = repo_->stashDrop(static_cast<size_t>(index));
        ok = result.ok();
        if (!ok) err = QString::fromStdString(result.error().message());
    }
    if (!ok) emit operationFailed(QStringLiteral("stashDrop"), err);
    else     refreshStashes();
}

void GitService::refreshStashes() {
    if (deferToOwnerThread([this] { refreshStashes(); })) return;
    if (!repo_) return;
    std::shared_ptr<git::Repository> r = repo_;
    runner_.run([this, r]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        if (r != repo_) return;   // superseded by a repo switch
        auto result = r->stashes();
        if (result) emit stashesReady(std::move(*result));
    });
}

// ---------------------------------------------------------------------------
// Tags
// ---------------------------------------------------------------------------

void GitService::refreshTags() {
    if (deferToOwnerThread([this] { refreshTags(); })) return;
    if (!repo_) return;
    std::shared_ptr<git::Repository> r = repo_;
    runner_.run([this, r]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        if (r != repo_) return;   // superseded by a repo switch
        auto result = r->tags();
        if (result) emit tagsReady(std::move(*result));
    });
}

void GitService::createTag(const QString& name, const QString& target,
                            const QString& message, bool annotated,
                            bool pushAfter) {
    if (!repo_) return;

    bool ok = false;
    QString err;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        // TagDialog's target is a branch name by default, or whatever
        // ref / short hash the user typed. Parsing it as a full hex id
        // turned every branch target into the null OID, so Create tag
        // failed unless given a complete SHA.
        git::Result<void> result = git::Result<void>::success();
        if (auto targetId = repo_->resolveRef(target.toStdString()); !targetId.ok())
            result = targetId.error();
        else if (annotated)
            result = repo_->createTag(name.toStdString(), targetId.value(),
                                      message.toStdString());
        else
            result = repo_->createLightweightTag(name.toStdString(), targetId.value());
        ok = result.ok();
        if (!ok) err = QString::fromStdString(result.error().message());
    }
    if (!ok) {
        emit operationFailed("createTag", err);
        return;
    }
    refreshTags();
    if (pushAfter) {
        // `git push origin refs/tags/<tag>` on the same synchronous CLI
        // path as push(remote, branch); failures surface via
        // operationFailed("push"). The full ref, because the short
        // name is ambiguous whenever a branch shares it.
        push(QStringLiteral("origin"), QStringLiteral("refs/tags/") + name);
    }
}

void GitService::deleteTag(const QString& name) {
    if (!repo_) return;
    bool ok = false;
    QString err;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        auto result = repo_->deleteTag(name.toStdString());
        ok = result.ok();
        if (!ok) err = QString::fromStdString(result.error().message());
    }
    if (!ok) emit operationFailed("deleteTag", err);
    else     refreshTags();
}

// ---------------------------------------------------------------------------
// Submodules
// ---------------------------------------------------------------------------

void GitService::refreshSubmodules() {
    if (deferToOwnerThread([this] { refreshSubmodules(); })) return;
    if (!repo_) return;
    std::shared_ptr<git::Repository> r = repo_;
    runner_.run([this, r]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        if (r != repo_) return;   // superseded by a repo switch
        auto result = r->submodules();
        if (result) emit submodulesReady(std::move(*result));
    });
}

void GitService::submoduleInit(const QString& name) {
    if (!repo_) return;
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};
    auto result = proc.run({"submodule", "init", name.toStdString()});
    if (!result)
        emit operationFailed("submoduleInit", QString::fromStdString(result.error().message()));
    else
        refreshSubmodules();
}

void GitService::submoduleUpdate(const QString& name) {
    if (!repo_) return;
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};
    auto result = proc.run({"submodule", "update", "--init", name.toStdString()});
    if (!result)
        emit operationFailed("submoduleUpdate", QString::fromStdString(result.error().message()));
    else
        refreshSubmodules();
}

// ---------------------------------------------------------------------------
// Worktrees
// ---------------------------------------------------------------------------

void GitService::refreshWorktrees() {
    if (deferToOwnerThread([this] { refreshWorktrees(); })) return;
    if (!repo_) return;
    std::shared_ptr<git::Repository> r = repo_;
    runner_.run([this, r]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        if (r != repo_) return;   // superseded by a repo switch
        auto result = r->worktrees();
        if (result) emit worktreesReady(std::move(*result));
    });
}

void GitService::addWorktree(const QString& name, const QString& path,
                              const QString& branch, bool createBranch) {
    if (!repo_) return;
    bool ok = false;
    QString err;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        auto result = repo_->addWorktree(name.toStdString(), path.toStdString(),
                                          branch.toStdString(), createBranch);
        ok = result.ok();
        if (!ok) err = QString::fromStdString(result.error().message());
    }
    if (!ok) emit operationFailed("addWorktree", err);
    else     refreshWorktrees();
}

void GitService::removeWorktree(const QString& name) {
    if (!repo_) return;
    bool ok = false;
    QString err;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        auto result = repo_->removeWorktree(name.toStdString());
        ok = result.ok();
        if (!ok) err = QString::fromStdString(result.error().message());
    }
    if (!ok) emit operationFailed("removeWorktree", err);
    else     refreshWorktrees();
}

void GitService::lockWorktree(const QString& name) {
    if (!repo_) return;
    bool ok = false;
    QString err;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        auto result = repo_->lockWorktree(name.toStdString());
        ok = result.ok();
        if (!ok) err = QString::fromStdString(result.error().message());
    }
    if (!ok) emit operationFailed("lockWorktree", err);
    else     refreshWorktrees();
}

void GitService::unlockWorktree(const QString& name) {
    if (!repo_) return;
    bool ok = false;
    QString err;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        auto result = repo_->unlockWorktree(name.toStdString());
        ok = result.ok();
        if (!ok) err = QString::fromStdString(result.error().message());
    }
    if (!ok) emit operationFailed("unlockWorktree", err);
    else     refreshWorktrees();
}

// ---------------------------------------------------------------------------
// Git Flow
// ---------------------------------------------------------------------------

bool GitService::isGitFlowInitialized() {
    if (!repo_) return false;
    std::lock_guard<std::mutex> lock(repoMutex_);
    auto cfg = repo_->config();
    auto master = cfg.getString("gitflow.branch.master");
    return master.ok();
}

void GitService::gitFlowInit(const QString& master, const QString& develop,
                             const QString& featurePrefix,
                             const QString& releasePrefix,
                             const QString& hotfixPrefix) {
    if (!repo_) return;
    {
        // `git flow init -d` answers every prompt with its default,
        // and (AVH git-flow) each prompt's default is the existing
        // gitflow.* config value when one is set. Write the caller's
        // names first so the non-interactive init adopts them
        // instead of silently using master/develop.
        std::lock_guard<std::mutex> lock(repoMutex_);
        auto cfg = repo_->config();
        cfg.setString("gitflow.branch.master", master.toStdString());
        cfg.setString("gitflow.branch.develop", develop.toStdString());
        cfg.setString("gitflow.prefix.feature", featurePrefix.toStdString());
        cfg.setString("gitflow.prefix.release", releasePrefix.toStdString());
        cfg.setString("gitflow.prefix.hotfix", hotfixPrefix.toStdString());
        cfg.setString("gitflow.prefix.support", "support/");
        cfg.setString("gitflow.prefix.versiontag", "");
    }
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};
    runner_.run([this, proc = std::move(proc)]() mutable {
        auto result = proc.gitFlowInit();
        if (!result) {
            emit operationFailed("gitFlowInit",
                QString::fromStdString(result.error().message()));
            emit gitFlowOperationComplete(false,
                QString::fromStdString(result.error().message()));
        } else {
            emit gitFlowOperationComplete(true, tr("Git Flow initialized."));
        }
    });
}

void GitService::featureStart(const QString& name) {
    if (!repo_) return;
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};
    runner_.run([this, proc = std::move(proc), n = name.toStdString()]() mutable {
        auto result = proc.gitFlowFeatureStart(n);
        if (!result) {
            emit operationFailed("featureStart",
                QString::fromStdString(result.error().message()));
            emit gitFlowOperationComplete(false,
                QString::fromStdString(result.error().message()));
        } else {
            emit gitFlowOperationComplete(true,
                tr("Feature '%1' started.").arg(QString::fromStdString(n)));
            refreshBranches();
        }
    });
}

void GitService::featureFinish(const QString& name) {
    if (!repo_) return;
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};
    runner_.run([this, proc = std::move(proc), n = name.toStdString()]() mutable {
        auto result = proc.gitFlowFeatureFinish(n);
        if (!result) {
            emit operationFailed("featureFinish",
                QString::fromStdString(result.error().message()));
            emit gitFlowOperationComplete(false,
                QString::fromStdString(result.error().message()));
        } else {
            emit gitFlowOperationComplete(true,
                tr("Feature '%1' finished.").arg(QString::fromStdString(n)));
            refreshBranches();
            refreshLog();
        }
    });
}

void GitService::releaseStart(const QString& version) {
    if (!repo_) return;
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};
    runner_.run([this, proc = std::move(proc), v = version.toStdString()]() mutable {
        auto result = proc.gitFlowReleaseStart(v);
        if (!result) {
            emit operationFailed("releaseStart",
                QString::fromStdString(result.error().message()));
            emit gitFlowOperationComplete(false,
                QString::fromStdString(result.error().message()));
        } else {
            emit gitFlowOperationComplete(true,
                tr("Release '%1' started.").arg(QString::fromStdString(v)));
            refreshBranches();
        }
    });
}

void GitService::releaseFinish(const QString& version) {
    if (!repo_) return;
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};
    runner_.run([this, proc = std::move(proc), v = version.toStdString()]() mutable {
        auto result = proc.gitFlowReleaseFinish(v);
        if (!result) {
            emit operationFailed("releaseFinish",
                QString::fromStdString(result.error().message()));
            emit gitFlowOperationComplete(false,
                QString::fromStdString(result.error().message()));
        } else {
            emit gitFlowOperationComplete(true,
                tr("Release '%1' finished.").arg(QString::fromStdString(v)));
            refreshBranches();
            refreshLog();
            refreshTags();
        }
    });
}

void GitService::hotfixStart(const QString& version) {
    if (!repo_) return;
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};
    runner_.run([this, proc = std::move(proc), v = version.toStdString()]() mutable {
        auto result = proc.gitFlowHotfixStart(v);
        if (!result) {
            emit operationFailed("hotfixStart",
                QString::fromStdString(result.error().message()));
            emit gitFlowOperationComplete(false,
                QString::fromStdString(result.error().message()));
        } else {
            emit gitFlowOperationComplete(true,
                tr("Hotfix '%1' started.").arg(QString::fromStdString(v)));
            refreshBranches();
        }
    });
}

void GitService::hotfixFinish(const QString& version) {
    if (!repo_) return;
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};
    runner_.run([this, proc = std::move(proc), v = version.toStdString()]() mutable {
        auto result = proc.gitFlowHotfixFinish(v);
        if (!result) {
            emit operationFailed("hotfixFinish",
                QString::fromStdString(result.error().message()));
            emit gitFlowOperationComplete(false,
                QString::fromStdString(result.error().message()));
        } else {
            emit gitFlowOperationComplete(true,
                tr("Hotfix '%1' finished.").arg(QString::fromStdString(v)));
            refreshBranches();
            refreshLog();
            refreshTags();
        }
    });
}

QStringList GitService::activeFeatures() {
    if (!repo_) return {};
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};
    auto result = proc.run({"branch", "--list", "feature/*"});
    if (!result) return {};
    QStringList names;
    const auto lines = QString::fromStdString(result->stdoutData).split('\n', Qt::SkipEmptyParts);
    for (const auto& line : lines) {
        QString trimmed = line.trimmed().remove(0, line.indexOf("feature/") >= 0 ? 0 : 0);
        // Strip leading "* " for current branch indicator
        if (trimmed.startsWith("* ")) trimmed = trimmed.mid(2);
        // Extract just the name after "feature/"
        qsizetype idx = trimmed.indexOf("feature/");
        if (idx >= 0)
            names.append(trimmed.mid(idx + 8));
    }
    return names;
}

QStringList GitService::activeReleases() {
    if (!repo_) return {};
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};
    auto result = proc.run({"branch", "--list", "release/*"});
    if (!result) return {};
    QStringList names;
    const auto lines = QString::fromStdString(result->stdoutData).split('\n', Qt::SkipEmptyParts);
    for (const auto& line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith("* ")) trimmed = trimmed.mid(2);
        qsizetype idx = trimmed.indexOf("release/");
        if (idx >= 0)
            names.append(trimmed.mid(idx + 8));
    }
    return names;
}

QStringList GitService::activeHotfixes() {
    if (!repo_) return {};
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};
    auto result = proc.run({"branch", "--list", "hotfix/*"});
    if (!result) return {};
    QStringList names;
    const auto lines = QString::fromStdString(result->stdoutData).split('\n', Qt::SkipEmptyParts);
    for (const auto& line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith("* ")) trimmed = trimmed.mid(2);
        qsizetype idx = trimmed.indexOf("hotfix/");
        if (idx >= 0)
            names.append(trimmed.mid(idx + 7));
    }
    return names;
}

// ---------------------------------------------------------------------------
// Repository Maintenance
// ---------------------------------------------------------------------------

void GitService::runGc() {
    if (!repo_) return;
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};
    runner_.run([this, proc = std::move(proc)]() mutable {
        auto result = proc.gc();
        if (!result) {
            emit operationFailed("gc",
                QString::fromStdString(result.error().message()));
        } else {
            emit maintenanceComplete(
                QString::fromStdString(result->stdoutData + result->stderrData));
        }
    });
}

void GitService::runPrune() {
    if (!repo_) return;
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};
    runner_.run([this, proc = std::move(proc)]() mutable {
        auto result = proc.prune();
        if (!result) {
            emit operationFailed("prune",
                QString::fromStdString(result.error().message()));
        } else {
            emit maintenanceComplete(
                QString::fromStdString(result->stdoutData + result->stderrData));
        }
    });
}

void GitService::runFsck() {
    if (!repo_) return;
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};
    runner_.run([this, proc = std::move(proc)]() mutable {
        auto result = proc.fsck();
        if (!result) {
            emit operationFailed("fsck",
                QString::fromStdString(result.error().message()));
        } else {
            emit maintenanceComplete(
                QString::fromStdString(result->stdoutData + result->stderrData));
        }
    });
}

void GitService::runRepack() {
    if (!repo_) return;
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};
    runner_.run([this, proc = std::move(proc)]() mutable {
        auto result = proc.run({"repack", "-a", "-d"});
        if (!result) {
            emit operationFailed("repack",
                QString::fromStdString(result.error().message()));
        } else {
            emit maintenanceComplete(
                QString::fromStdString(result->stdoutData + result->stderrData));
        }
    });
}

QString GitService::repositoryDiskUsage() {
    if (!repo_) return {};
    git::GitProcess proc{[&]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        return repo_->process();
    }()};
    auto result = proc.run({"count-objects", "-vH"});
    if (!result) return tr("Unable to determine disk usage.");
    return QString::fromStdString(result->stdoutData);
}

} // namespace gitbolt::services
