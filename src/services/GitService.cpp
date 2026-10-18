#include "services/GitService.h"

#include <QDir>
#include <QFile>
#include <QScopeGuard>

#include <algorithm>

namespace gitbolt::services {

namespace {
// Why a git command that would work on the index or the operation's
// state isn't run: a step (a rebase's, a sequence's, an abort, a
// commit) is at them already (see rebaseStepRunning()).
QString rebaseStepStillRunning() {
    return GitService::tr("Another git operation is still running on this repository; "
                          "try again when it's done.");
}

// Which repository a rebase step runs on: its git dir, which libgit2
// resolves however the path was typed. Windows paths ignore case, and
// libgit2 keeps the case a path was typed in there, so "c:\repo" and
// "C:\repo" must not count as two repositories.
std::string rebaseStepKey(const git::Repository& repo) {
#ifdef Q_OS_WIN
    return QString::fromStdString(repo.path()).toLower().toStdString();
#else
    return repo.path();
#endif
}

// Where the search for the repository a folder is in stops: before
// $HOME. A dotfiles repository there would otherwise claim every folder
// in the home directory that is in no repository of its own. $HOME
// itself still opens if it is a repository, as any repository does.
std::string searchCeiling() {
    return QDir::homePath().toStdString();
}
} // namespace

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
    // And stop the git of every job runner_ is about to wait for: a
    // rebase step's hook, a slow checkout, gc. Quitting waited for the
    // whole of it, and a second launch meanwhile found nobody to hand
    // its path to and gave up.
    cancelGitOps();
}

bool GitService::openRepository(const QString& path) {
    // A sync open supersedes any pending async open (and its
    // enumeration) the same way a newer async open would.
    ++openGeneration_;
    if (watchEnumCancel_)
        watchEnumCancel_->store(true);

    openPending_ = false;

    // A folder inside a repository opens that repository.
    std::string found;
    auto result = git::Repository::openContaining(path.toStdString(), searchCeiling(), found);
    if (!result) {
        // What was open stays so; an outcome that waited for the
        // superseded async open is its, if it is for it.
        settleReports();
        return false;
    }
    const QString root = QString::fromStdString(found);
    const QString gitDir = QString::fromStdString(result->path());
    const QString commonDir = QString::fromStdString(result->commonDir());
    {
        // Swap under the libgit2 mutex so an in-flight refresh
        // worker can't have the old repo destroyed mid-call.
        std::lock_guard<std::mutex> lock(repoMutex_);
        repo_ = std::make_shared<git::Repository>(std::move(*result));
        openKey_ = rebaseStepKey(*repo_);
    }
    watcher_.watchRepository(root, gitDir, commonDir);
    emit repositoryOpened(root);
    refreshStatus();
    refreshLog();
    refreshBranches();
    settleReports();
    return true;
}

quint64 GitService::openRepositoryAsync(const QString& path) {
    const quint64 gen = ++openGeneration_;
    if (watchEnumCancel_)
        watchEnumCancel_->store(true);
    openPending_ = true;

    // Worker outcome: exactly one of repo / error is set. Must be
    // copyable + default-constructible for QFutureWatcher::result().
    struct OpenOutcome {
        std::shared_ptr<git::Repository> repo;
        QString error;
        int errorCode = 0;   // git::GitErrorCode
        QString root;        // what opened: `path`, or the repository it is in
        QString gitDir;      // its git dir and common dir, for the watcher
        QString commonDir;
    };

    runner_.runWithResult<OpenOutcome>(
        [path, ceiling = searchCeiling()]() -> OpenOutcome {
            // Fresh git_repository* — no repoMutex_ needed; nothing
            // else can touch this object until we publish it below.
            // A folder inside a repository opens that repository.
            std::string found;
            auto result = git::Repository::openContaining(path.toStdString(), ceiling, found);
            if (!result)
                return { nullptr,
                         QString::fromStdString(result.error().message()),
                         static_cast<int>(result.error().code()), QString(),
                         QString(), QString() };
            const QString gitDir = QString::fromStdString(result->path());
            const QString commonDir = QString::fromStdString(result->commonDir());
            return { std::make_shared<git::Repository>(std::move(*result)),
                     QString(), 0, QString::fromStdString(found), gitDir, commonDir };
        },
        [this, path, gen](OpenOutcome outcome) {
            // Main thread. A newer open/close superseded us — drop
            // everything silently (the Repository, if any, is
            // destroyed with the outcome).
            if (gen != openGeneration_)
                return;
            openPending_ = false;

            if (!outcome.repo) {
                emit repositoryOpenFailed(path, outcome.error, outcome.errorCode);
                // The repository open before is still the open one.
                settleReports();
                return;
            }

            {
                std::lock_guard<std::mutex> lock(repoMutex_);
                repo_ = std::move(outcome.repo);
                openKey_ = rebaseStepKey(*repo_);
            }

            // Cheap, immediate watches (.git internals + repo root)
            // now; the expensive working-tree walk goes to a worker.
            // From here on the path is the repository's: a subfolder
            // asked for is not what opened.
            const QString root = outcome.root;
            watcher_.watchGitInternals(root, outcome.gitDir, outcome.commonDir);
            startWatchEnumeration(root, gen);

            // Emit BEFORE kicking refreshes: onRepositoryOpened
            // handlers read repository() (headBranchName etc.) on
            // the main thread, and direct-connection slots running
            // synchronously inside this emit are guaranteed to
            // finish before any refresh worker grabs repoMutex_.
            emit repositoryOpened(root);
            refreshStatus();
            loadLog(0, kLogPageSize, gen);   // tagged: initialLogLoaded
            refreshBranches();
            // Behind the window's own handling of the open: a step of
            // this repository's that ended meanwhile finds its bar.
            settleReports();
        });
    return gen;
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
    openPending_ = false;

    watcher_.stop();
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        repo_.reset();
        openKey_.clear();
    }
    waitingReports_.clear();
    emit repositoryClosed();
}

// The index, HEAD and the work tree are a running step's (a commit and
// its hooks, a rebase step, an abort): git commit reads the index again
// after the pre-commit hook, and writes HEAD through the branch it names
// when it is done. A branch switched meanwhile from the toolbar got the
// commit; a file staged went into it unchecked. MainWindow and the
// Commit dialog keep these off while a step runs; this turns away
// whatever gets past them.
bool GitService::refusedWhileAStepRuns(const QString& operation) {
    if (!rebaseStepRunning())
        return false;
    emit operationFailed(operation, rebaseStepStillRunning());
    return true;
}

void GitService::reportTo(std::string key, std::function<void()> report) {
    if (openPending_) {
        waitingReports_.emplace_back(std::move(key), std::move(report));
        return;
    }
    if (!key.empty() && key == openKey_)
        report();
}

void GitService::postReport(std::string key, std::function<void()> report) {
    QMetaObject::invokeMethod(this, [this, key = std::move(key), report = std::move(report)]() mutable {
        reportTo(std::move(key), std::move(report));
    }, Qt::QueuedConnection);
}

void GitService::settleReports() {
    if (openPending_)
        return;     // a newer open is on its way: they wait for it in turn
    auto waiting = std::exchange(waitingReports_, {});
    for (auto& [key, report] : waiting) {
        if (!key.empty() && key == openKey_)
            report();
    }
}

bool GitService::isOpen() const { return repo_ != nullptr; }
git::Repository* GitService::repository() const { return repo_.get(); }

git::GitProcess GitService::process() const {
    std::lock_guard<std::mutex> lock(repoMutex_);
    git::GitProcess proc = repo_->process();
    proc.addCancelFlag(opsCancelled_);
    return proc;
}

std::optional<git::GitProcess> GitService::processIfOpen(
    std::shared_ptr<std::atomic<bool>> cancel) const {
    std::lock_guard<std::mutex> lock(repoMutex_);
    if (!repo_) return std::nullopt;
    git::GitProcess proc = repo_->process();
    proc.addCancelFlag(opsCancelled_);
    proc.addCancelFlag(std::move(cancel));
    return proc;
}

void GitService::cancelGitOps() {
    opsCancelled_->store(true);
}

// Refresh workers capture repo_ as a shared_ptr (pinning the object
// alive across a concurrent repo swap) and re-check `r == repo_`
// under repoMutex_ before doing any work: if the user opened a
// different repository while this job sat in the thread-pool queue,
// the job must not touch the swapped-out repo. Reading repo_ from the
// worker is safe because every mutation of repo_ happens under
// repoMutex_ (openRepository / openRepositoryAsync / closeRepository).
// Their results go out through deliverFor, which checks again where
// the swap happens, so none is misattributed to the next repository.
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
        int conflicts = 0;
        if (result) {
            conflicts = static_cast<int>(std::count_if(
                result->begin(), result->end(),
                [](const git::StatusEntry& e) { return e.isConflicted(); }));
        }
        deliverFor(r, [this, result = std::move(result), state = r->state(),
                       conflicts, paused = r->sequencePaused(),
                       skippable = r->sequenceSkippable()]() mutable {
            if (result)
                emit statusReady(std::move(*result));
            emit repoStateReady(state, conflicts, paused, skippable);
        });
    });
}

void GitService::refreshLog(int offset, int count) {
    if (deferToOwnerThread([this, offset, count] { refreshLog(offset, count); }))
        return;
    loadLog(offset, count, 0);
}

void GitService::loadLog(int offset, int count, quint64 openGeneration) {
    if (!repo_) return;
    std::shared_ptr<git::Repository> r = repo_;
    const LogScope scope = logScope_;
    // Snapshot so the worker lambda doesn't race with a UI-thread
    // setSelectedBranches() during the walk.
    const QStringList selected = selectedBranches_;
    runner_.run([this, r, offset, count, scope, selected, openGeneration]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        if (r != repo_) return;   // superseded by a repo switch
        // A page goes out, and right behind it, for the first page of
        // an open, initialLogLoaded, so a receiver sees the page first.
        const auto deliverPage = [this, r, offset, openGeneration](
                                     std::vector<git::CommitData> commits) {
            deliverFor(r, [this, commits = std::move(commits), offset,
                           openGeneration]() mutable {
                const int size = static_cast<int>(commits.size());
                emit logReady(std::move(commits), offset);
                if (offset == 0 && openGeneration != 0)
                    emit initialLogLoaded(openGeneration, size);
            });
        };
        auto walk = r->createRevWalk();
        if (!walk) {
            // Still emit for the initial page: the UI uses the first
            // logReady(offset==0) as "loading finished" (it closes
            // the open-repo spinner), so error paths must produce
            // closure too — an empty log, same end state as today.
            if (offset == 0)
                deliverPage({});
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
        // Each page is a fresh walk from the tips, so step over the
        // rows the model already has; without this every page after
        // the first repeated it. A walk that ends inside the skip
        // yields an empty page, which tells the model there's no more.
        if (offset > 0) {
            auto skipped = walk->skip(static_cast<size_t>(offset));
            if (!skipped) return;
        }
        auto commits = walk->next(static_cast<size_t>(count));
        if (commits)
            deliverPage(std::move(*commits));
        else if (offset == 0)
            deliverPage({});
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
        if (result)
            deliverFor(r, [this, branches = std::move(*result)]() mutable {
                emit branchesReady(std::move(branches));
            });
    });
}

void GitService::refreshConflicts() {
    if (deferToOwnerThread([this] { refreshConflicts(); })) return;
    if (!repo_) return;
    std::shared_ptr<git::Repository> r = repo_;
    runner_.run([this, r]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        if (r != repo_) return;   // superseded by a repo switch
        deliverFor(r, [this, result = r->conflictEntries()]() mutable {
            if (result)
                emit conflictsReady(std::move(*result));
            else
                emit operationFailed(QStringLiteral("conflicts"),
                    QString::fromStdString(result.error().message()));
        });
    });
}

bool GitService::resolveConflicts(
        const std::vector<std::pair<QString, QString>>& resolutions) {
    if (!repo_) return true;
    // A resolver left open from an earlier stop, while Continue runs:
    // staged now, its files went into whatever git was committing.
    if (rebaseStepRunning()) {
        emit operationFailed(QStringLiteral("resolve conflicts"), rebaseStepStillRunning());
        return false;
    }
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
    return true;
}

bool GitService::abortConflictState() {
    if (!repo_) return true;
    // `git rebase --abort` under a running step reset the branch while
    // the step went on picking onto it.
    if (rebaseStepRunning()) {
        emit operationFailed(QStringLiteral("abort"), rebaseStepStillRunning());
        return false;
    }

    git::RepoState state = git::RepoState::None;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        state = repo_->state();
    }

    std::vector<std::string> args;
    switch (state) {
    case git::RepoState::Merge:
        args = {"merge", "--abort"};
        break;
    case git::RepoState::CherryPick:
        args = {"cherry-pick", "--abort"};
        break;
    case git::RepoState::Rebase:
        // A step like the in-progress bar's Abort, so that nothing else
        // goes to the rebase's state while it runs.
        rebaseAbort();
        return true;
    case git::RepoState::Revert:
        args = {"revert", "--abort"};
        break;
    case git::RepoState::None:
    case git::RepoState::Other:
        return true;   // nothing in progress to abort
    }

    // On a worker: an abort puts the work tree back, which can take as
    // long as a checkout, and it froze the window meanwhile. As a step
    // (runRebaseStep), one git at a time: a Continue, a second Abort or
    // a commit started while it ran was a second git on the same
    // operation, and a second Abort failed on index.lock with git's
    // advice to delete it. Reported on rebaseStepFinished as
    // "merge --abort", "cherry-pick --abort" or "revert --abort".
    const QString step = QString::fromStdString(args[0]) + QStringLiteral(" --abort");
    runRebaseStep(step, [args](const git::GitProcess& proc) {
        return proc.run(args, /*timeoutMs=*/300000);
    }, {}, state);
    return true;
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
                deliverFor(r, [this, message = resolved.error().message()] {
                    emit operationFailed(QStringLiteral("blame"),
                                         QString::fromStdString(message));
                });
                return;
            }
            newest = *resolved;
        }

        deliverFor(r, [this, result = r->blame(p, newest)]() mutable {
            if (result)
                emit blameReady(std::move(*result));
            else
                emit operationFailed(QStringLiteral("blame"),
                    QString::fromStdString(result.error().message()));
        });
    });
}

// Synchronous mutators below (all but commitChanges, which runs git on
// a worker): they run on the main thread but schedule async refreshes
// that touch repo_ on a worker. Locking here serializes them against
// any in-flight worker — without this, a stage on the main thread can
// race with a refreshStatus worker that's mid-libgit2-call and
// corrupt the heap. Same lock as the
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
    if (refusedWhileAStepRuns(QStringLiteral("stage"))) return;
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
    if (refusedWhileAStepRuns(QStringLiteral("unstage"))) return;
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
    if (refusedWhileAStepRuns(QStringLiteral("stage all"))) return;
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
    if (refusedWhileAStepRuns(QStringLiteral("stage hunk"))) return;

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
    if (refusedWhileAStepRuns(QStringLiteral("unstageAll"))) return;
    // `git reset -- :/` unstages everything: it rewrites the index to
    // match HEAD. This is the cleanest way to do "un-add all."
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
    // With a pathspec (the whole tree, ":/"): with none, git's reset
    // also ends a merge or a cherry-pick in progress (it removes
    // MERGE_HEAD, CHERRY_PICK_HEAD), and the stopped pick of a
    // `git cherry-pick A B C` was then dropped by Continue, which said
    // "Cherry-pick complete." with its resolution left uncommitted.
    auto result = proc.run({"reset", "-q", "--", ":/"});
    if (!result) {
        emit operationFailed(QStringLiteral("unstageAll"),
            QString::fromStdString(result.error().message()));
    } else if (!result->success()) {
        // git ran, and unstaged nothing: another git holds index.lock,
        // say. Unreported, the click just did nothing.
        QString detail =
            QString::fromStdString(result->stderrData).trimmed();
        if (detail.isEmpty())
            detail = QString::fromStdString(result->stdoutData).trimmed();
        emit operationFailed(QStringLiteral("unstageAll"), detail);
    }
    refreshStatus();
}

void GitService::discardFile(const QString& path) {
    if (!repo_) return;
    if (refusedWhileAStepRuns(QStringLiteral("discardFile"))) return;
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

namespace {
std::string rebaseStepKey(const git::Repository& repo);   // below

// Why `git commit` failed: git's own words as a terminal would show
// them, a hook's among them; with nothing on stderr (git says "nothing
// to commit" on stdout, after the whole status), stdout's last line; or
// why git couldn't run at all.
QString commitFailure(const git::Result<git::ProcessOutput>& result) {
    if (!result)
        return QString::fromStdString(result.error().message());
    const QString errors = QString::fromStdString(result->stderrData).trimmed();
    if (!errors.isEmpty())
        return errors;
    const QString last = QString::fromStdString(result->stdoutData)
                             .trimmed().section(QLatin1Char('\n'), -1).trimmed();
    return last.isEmpty()
        ? GitService::tr("git commit exited with code %1").arg(result->exitCode)
        : last;
}
} // namespace

// Through git, not libgit2, which runs no hooks: a pre-commit that
// guards against secrets, a commit-msg that adds Gerrit's Change-Id, a
// post-commit were all skipped without a word, and so were
// commit.gpgSign and a merge's autostash (merge.autoStash), whose
// changes stayed stashed away. On a worker, as the hooks may take
// minutes, and unlocked while git runs, as a rebase step is.
void GitService::commitChanges(const QString& message, bool amend) {
    if (!repo_) return;
    std::shared_ptr<git::Repository> r = repo_;
    std::string gitDir;
    std::optional<git::GitProcess> process;
    {
        // A step of its own (rebaseSteps_): a rebase step, a sequence's
        // or an abort started while git commits, or a commit while one
        // of them runs, was a second git on the same index. A Continue
        // clicked during an amend's hook finished the rebase, and the
        // amend then found nothing to amend: its message was lost.
        std::lock_guard<std::mutex> lock(repoMutex_);
        gitDir = rebaseStepKey(*r);
        if (std::lock_guard<std::mutex> steps(stepsMutex_); rebaseSteps_.insert(gitDir).second)
            process = r->process();
    }
    if (!process) {
        emit commitComplete(false, rebaseStepStillRunning());
        return;
    }
    emit stepStarted(QStringLiteral("commit"));
    // Quitting stops git, and a hook that hangs with it: ~GitService
    // waits for this worker.
    process->addCancelFlag(opsCancelled_);
    runner_.run([this, r, gitDir, proc = std::move(*process), message, amend]() {
        // Whatever happens, the mark goes; the normal paths take it out
        // themselves, before they report, so that a step started on the
        // strength of the report is never refused.
        bool cleared = false;
        const auto clearMark = [&] {
            if (cleared) return;
            std::lock_guard<std::mutex> steps(stepsMutex_);
            rebaseSteps_.erase(gitDir);
            cleared = true;
        };
        const auto clearOnExit = qScopeGuard([&] {
            std::lock_guard<std::mutex> lock(repoMutex_);
            clearMark();
        });
        // The repository the commit is for, opened again or not. A
        // result reported while another is open reads as its own.
        const auto stillOpen = [&] { return repo_ && rebaseStepKey(*repo_) == gitDir; };
        QString refusal;
        std::optional<git::ObjectId> autostash;
        {
            std::lock_guard<std::mutex> lock(repoMutex_);
            if (!stillOpen())
                return;
            if (amend)
                refusal = QString::fromStdString(r->amendProblem());
            if (auto stash = r->resolveRef("MERGE_AUTOSTASH"))
                autostash = *stash;
            if (!refusal.isEmpty())
                clearMark();
        }
        // Reported on the service's thread, to this repository if it is
        // the open one then (reportTo), as a rebase step's outcome is.
        // Emitted here, a commit that ended while another repository
        // was opening said "Commit created: …" (or a hook's refusal)
        // over that one's "Opening…", and one posted just before the
        // swap arrived after it. Held while an open is pending, a Commit
        // & Push of this repository, opened again meanwhile, still
        // pushes: its Push is back by the time the outcome is in.
        const auto report = [this, gitDir](bool ok, QString text, bool autostashKept) {
            postReport(gitDir, [this, ok, text, autostashKept] {
                emit commitComplete(ok, text);
                if (!ok)
                    return;
                // After the commit's own note, which it would cover: the
                // work tree has conflict markers in it now.
                if (autostashKept) {
                    emit operationFailed(QStringLiteral("autostash"),
                                         tr("Applying the autostash resulted in "
                                            "conflicts. Your changes are safe in "
                                            "the stash."));
                    refreshStashes();
                }
                refreshStatus();
                refreshLog();
            });
        };
        if (!refusal.isEmpty()) {
            report(false, refusal, false);
            return;
        }

        const auto result = proc.commit(message.toStdString(), amend);
        const bool ok = result && result->success();
        QString shortHex;
        bool autostashKept = false;
        {
            std::lock_guard<std::mutex> lock(repoMutex_);
            clearMark();
            // `r` is the repository committed to, whether or not it is
            // the open one now; reportTo decides where this goes.
            if (ok) {
                if (auto head = r->head())
                    shortHex = QString::fromStdString(head->toShortHex());
                // git keeps them in the stash when putting them back
                // conflicts.
                if (autostash) {
                    if (auto stashes = r->stashes())
                        autostashKept = std::any_of(stashes->begin(), stashes->end(),
                            [&](const git::StashEntry& s) { return s.id == *autostash; });
                }
            }
        }
        if (!ok) {
            report(false, commitFailure(result), false);
            return;
        }
        report(true,
               autostash && !autostashKept
                   ? tr("Commit created: %1. Applied autostash.").arg(shortHex)
                   : tr("Commit created: %1").arg(shortHex),
               autostashKept);
    });
}

void GitService::createBranch(const QString& name, const QString& startPoint) {
    if (!repo_) return;
    if (refusedWhileAStepRuns(QStringLiteral("create branch"))) return;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        // The branch named, as a branch: a tag of the same name is
        // another commit.
        auto headResult = startPoint.isEmpty()
            ? repo_->head()
            : repo_->resolveRef("refs/heads/" + startPoint.toStdString() + "^{commit}");
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
    if (refusedWhileAStepRuns(QStringLiteral("delete branch"))) return;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        auto result = repo_->deleteBranch(name.toStdString());
        if (!result)
            emit operationFailed(QStringLiteral("delete branch"),
                QString::fromStdString(result.error().message()));
    }
    refreshBranches();
}

bool GitService::checkoutBranch(const QString& name) {
    if (!repo_) return false;
    if (refusedWhileAStepRuns(QStringLiteral("checkout"))) return false;
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
        // is already showing the correct current branch). Status too:
        // a checkout that failed partway has written some of the
        // target's files, and those show up as changes.
        refreshStatus();
        refreshBranches();
        return false;
    }
    refreshStatus();
    refreshLog();
    refreshBranches();
    return true;
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

// git's "warning:" lines from a step that succeeded, without the
// prefix: an abort of a cherry-pick or revert sequence that didn't
// rewind says so there alone, and exits 0. Empty for one that failed,
// whose message is rebaseFailure()'s.
QString stepWarnings(const git::Result<git::ProcessOutput>& result) {
    if (!result || !result->success())
        return {};
    QStringList warnings;
    for (const QString& raw : QString::fromStdString(result->stderrData).split(QLatin1Char('\n'))) {
        const QString line = raw.mid(raw.lastIndexOf(QLatin1Char('\r')) + 1).trimmed();
        if (line.startsWith(QLatin1String("warning:")))
            warnings.append(line.mid(8).trimmed());
    }
    return warnings.join(QLatin1Char('\n'));
}

// See GitService::rebasePlanProblem().
QString outdatedPlan(const git::Repository& repo, const git::RebasePlan& plan) {
    const auto head = repo.head();
    if (!head)
        return QString::fromStdString(head.error().message());
    std::string branch;
    if (!repo.isHeadDetached()) {
        auto name = repo.headBranchName();
        if (!name)
            return QString::fromStdString(name.error().message());
        branch = std::move(*name);
    }
    const auto where = [](const std::string& b) {
        return b.empty() ? GitService::tr("a detached HEAD") : QString::fromStdString(b);
    };
    // Even at the same commit: git would rewrite the branch checked
    // out now, which the plan wasn't made for.
    if (branch != plan.branch)
        return GitService::tr("The rebase was planned on %1, but %2 is checked out now. "
                              "Pick the target again.")
            .arg(where(plan.branch), where(branch));
    if (*head != plan.head)
        return GitService::tr("%1 has moved since the rebase was planned. "
                              "Pick the target again.")
            .arg(branch.empty() ? QStringLiteral("HEAD") : QString::fromStdString(branch));
    return {};
}
} // namespace

bool GitService::localBranchExists(const QString& name) const {
    std::lock_guard<std::mutex> lock(repoMutex_);
    return repo_ && repo_->resolveRef("refs/heads/" + name.toStdString()).ok();
}

// push/pull/fetch shell out via GitProcess (QProcess) instead of
// libgit2, so the actual git command is safe to run while a libgit2
// worker is active. processIfOpen() still locks around
// `repo_->process()`, because that calls libgit2's
// git_repository_workdir() to get the path, and because these run on
// MainWindow's pool thread, where even the "is a repo open" check
// must not read repo_ unlocked. Once GitProcess is constructed (a
// plain string + QProcess), it's independent of repo_ and runs
// unlocked. Their follow-up refreshes hop to the GUI thread.
bool GitService::push(const QString& remote, const QString& branch) {
    const auto snapshot = processIfOpen();
    if (!snapshot) return false;
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
        return false;
    }
    if (result.value().exitCode != 0) {
        emit operationFailed("push", stderrOrFallback(result.value(),
            tr("git push exited with code %1").arg(result.value().exitCode)));
        return false;
    }
    return true;
}

bool GitService::deleteRemoteBranch(const QString& remote, const QString& branch) {
    const auto snapshot = processIfOpen();
    if (!snapshot) return false;
    const git::GitProcess& proc = *snapshot;
    auto result = proc.run({"push", remote.toStdString(), "--delete",
                            branch.toStdString()}, 120000);
    if (!result) {
        emit operationFailed(QStringLiteral("delete remote branch"),
                             QString::fromStdString(result.error().message()));
        return false;
    }
    if (result.value().exitCode != 0) {
        emit operationFailed(QStringLiteral("delete remote branch"),
            stderrOrFallback(result.value(),
                tr("git push --delete exited with code %1").arg(result.value().exitCode)));
        return false;
    }
    return true;
}

bool GitService::pull(const QString& remote, const QString& branch) {
    const auto snapshot = processIfOpen();
    if (!snapshot) return false;
    const git::GitProcess& proc = *snapshot;
    auto result = proc.pull(remote.toStdString(), branch.toStdString());
    if (!result) {
        emit operationFailed("pull", QString::fromStdString(result.error().message()));
        return false;
    }
    if (result.value().exitCode != 0) {
        emit operationFailed("pull", stderrOrFallback(result.value(),
            tr("git pull exited with code %1").arg(result.value().exitCode)));
        return false;
    }
    refreshStatus();
    refreshLog();
    refreshBranches();
    return true;
}

namespace {
// The config keys that decide where a pruning fetch reaches, for
// `git config --get-regexp`. fetch.prune is there for the submodules:
// the superproject's own is read before (GitService::fetch).
const char* const kPruneKeys =
    "^(fetch\\.(prune|prunetags)|remote\\..*\\.(prunetags|fetch))$";

// Whether, by the `git config --get-regexp kPruneKeys` lines in
// `config`, a pruning fetch could delete nothing but remote-tracking
// refs (refs/remotes/…). git prunes every ref a fetch refspec writes
// to, and a plain `git fetch` prunes nothing: a tags refspec
// (+refs/tags/*:refs/tags/*), a mirror's +refs/*:refs/*, or
// fetch.pruneTags / remote.<name>.pruneTags made GitBolt's Fetch, and
// the silent auto-fetch, delete local tags and branches the server
// doesn't have (unpushed ones), which `git fetch` keeps. Every remote
// counts, as a fetch with no remote named may pick any of them. A
// fetch.prune of a submodule's own, either way, is its say.
bool pruneStaysInRemoteTracking(const QString& config) {
    const QStringList lines = config.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const qsizetype space = line.indexOf(QLatin1Char(' '));
        const QString key = (space < 0 ? line : line.left(space)).toLower();
        const QString value = space < 0 ? QString() : line.mid(space + 1).trimmed();
        if (key == QLatin1String("fetch.prune"))
            return false;
        if (key.endsWith(QLatin1String(".prunetags"))) {
            // A key with no value is true, as git reads it.
            static const QStringList no = {QStringLiteral("false"), QStringLiteral("no"),
                                           QStringLiteral("off"), QStringLiteral("0")};
            if (space < 0 || !no.contains(value.toLower()))
                return false;
            continue;
        }
        // remote.<name>.fetch: [+]<src>[:<dst>]. One with no
        // destination, or a negative one (^…), writes no ref to prune.
        QString refspec = value;
        if (refspec.startsWith(QLatin1Char('+')))
            refspec.remove(0, 1);
        if (refspec.startsWith(QLatin1Char('^')))
            continue;
        const qsizetype colon = refspec.indexOf(QLatin1Char(':'));
        if (colon < 0)
            continue;
        const QString destination = refspec.mid(colon + 1);
        if (!destination.isEmpty() && !destination.startsWith(QLatin1String("refs/remotes/")))
            return false;
    }
    return true;
}

// pruneStaysInRemoteTracking() for the repository and for every
// submodule its fetch can go on into. `-c fetch.prune=true` reaches
// them all: git hands -c settings to the fetches it runs in the
// submodules (GIT_CONFIG_PARAMETERS), and there they outrank the
// submodule's own config. So a submodule's tags refspec lost it its
// unpushed tags, and its fetch.prune=false was overridden. Initialized
// submodules only, nested ones too (git fetches no others); a listing
// that fails prunes nothing, except in a bare repository, which has
// none.
bool pruneOnlyTouchesRemoteTracking(const git::GitProcess& proc) {
    const auto config = proc.run({"config", "--get-regexp", kPruneKeys});
    if (!config || (config->exitCode != 0 && config->exitCode != 1))   // 1: none set
        return false;
    if (!pruneStaysInRemoteTracking(QString::fromStdString(config->stdoutData)))
        return false;
    // Each submodule's matching keys, one after the other; `test` keeps
    // git config's 1 (none set) from stopping the walk.
    const auto submodules = proc.run(
        {"submodule", "foreach", "--quiet", "--recursive",
         std::string("git config --get-regexp '") + kPruneKeys + "'; test $? -le 1"});
    if (!submodules || !submodules->success()) {
        const auto bare = proc.run({"rev-parse", "--is-bare-repository"});
        return bare && bare->success()
            && QString::fromStdString(bare->stdoutData).trimmed() == QLatin1String("true");
    }
    return pruneStaysInRemoteTracking(QString::fromStdString(submodules->stdoutData));
}
} // namespace

bool GitService::fetch(const QString& remote,
                       std::shared_ptr<std::atomic<bool>> cancel,
                       bool allowPrompts) {
    auto snapshot = processIfOpen(std::move(cancel));
    if (!snapshot) return false;
    if (!allowPrompts)
        snapshot->forbidPrompts();
    const git::GitProcess& proc = *snapshot;
    // fetch.prune unset: prune, as most Git GUIs' Fetch does, so a
    // branch deleted on the server leaves the sidebar too. Set either
    // way, it is the user's call; remote.<name>.prune outranks both,
    // as in git. Only where pruning can delete nothing but
    // remote-tracking refs, though, here and in every submodule the
    // fetch goes on into (pruneOnlyTouchesRemoteTracking).
    const auto pruneSetting = proc.run({"config", "--get", "fetch.prune"});
    const bool pruneByDefault = pruneSetting && pruneSetting->exitCode == 1  // 1 = not set
                                && pruneOnlyTouchesRemoteTracking(proc);
    auto result = proc.fetch(remote.toStdString(), pruneByDefault);
    if (!result) {
        emit operationFailed("fetch", QString::fromStdString(result.error().message()));
        return false;
    }
    if (result.value().exitCode != 0) {
        emit operationFailed("fetch", stderrOrFallback(result.value(),
            tr("git fetch exited with code %1").arg(result.value().exitCode)));
        return false;
    }
    refreshBranches();
    return true;
}

// ---------------------------------------------------------------------------
// Interactive Rebase
//
// From here on, the operation workers (rebase, cherry-pick, Git Flow)
// call refresh*() on a pool thread when they finish. Those calls queue
// onto the GUI thread behind the completion signal emitted just before
// them, so e.g. rebaseStepFinished still reaches MainWindow before
// the statusReady it causes.
// ---------------------------------------------------------------------------

void GitService::interactiveRebase(const git::RebasePlan& plan) {
    // git's todo list runs oldest first; the plan lists newest first,
    // as the log does. Full hashes, so no abbreviation can turn
    // ambiguous; the subject is only there for `git status` to show.
    std::string todo;
    std::map<std::string, std::string> newMessages;
    std::vector<std::string> messages;
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
        messages.push_back(op->message);
        if (op->type == git::RebaseOperationType::Reword && !op->newMessage.empty())
            newMessages[op->commitId.toHex()] = op->newMessage;
    }

    // The Rebase dialog stays open while the repository goes on: a
    // commit made meanwhile, or another branch checked out, and git
    // would rebase that HEAD with this todo list, dropping every
    // commit the list doesn't name. The dialog asks first; this is
    // for whatever moved HEAD since.
    runRebaseStep(QStringLiteral("rebase"),
                  [onto = plan.onto.toHex(), todo, newMessages, messages](
                      const git::GitProcess& proc) {
        return proc.interactiveRebase(onto, todo, newMessages, messages);
    }, [plan](const git::Repository& repo) { return outdatedPlan(repo, plan); });
}

QString GitService::rebasePlanProblem(const git::RebasePlan& plan) const {
    std::lock_guard<std::mutex> lock(repoMutex_);
    if (!repo_)
        return tr("No repository is open.");
    return outdatedPlan(*repo_, plan);
}

// Not under repoMutex_: a refresh worker holds that for as long as its
// libgit2 walk takes (seconds, for the status of a big tree), and the
// window asks this each time it turns its actions on or off. openKey_
// is the open repository's key, on this, the service's, thread.
bool GitService::rebaseStepRunning() const {
    std::lock_guard<std::mutex> steps(stepsMutex_);
    return !openKey_.empty() && rebaseSteps_.contains(openKey_);
}

void GitService::rebaseContinue() {
    runRebaseStep(QStringLiteral("rebase --continue"),
                  [](const git::GitProcess& proc) { return proc.rebaseContinue(); });
}

void GitService::rebaseAbort() {
    runRebaseStep(QStringLiteral("rebase --abort"),
                  [](const git::GitProcess& proc) { return proc.rebaseAbort(); });
}

void GitService::rebaseSkip() {
    runRebaseStep(QStringLiteral("rebase --skip"),
                  [](const git::GitProcess& proc) { return proc.rebaseSkip(); });
}

QString GitService::committedRebaseStop() const {
    const auto proc = processIfOpen();
    return proc ? QString::fromStdString(proc->committedRebaseStop()) : QString();
}

bool GitService::hasTrackedChanges() const {
    const auto proc = processIfOpen();
    return proc && proc->hasTrackedChanges();
}

void GitService::inspectStop(std::function<void(const StopFacts&)> done, bool continuing) {
    auto proc = processIfOpen();
    if (!proc) return;
    runner_.run([this, key = openKey_, proc = std::move(*proc), done = std::move(done),
                 continuing]() mutable {
        StopFacts facts;
        if (continuing) {
            facts.unpickedSequenceStop = QString::fromStdString(proc.unpickedSequenceStop());
        } else {
            facts.committedRebaseStop = QString::fromStdString(proc.committedRebaseStop());
            facts.trackedChanges = proc.hasTrackedChanges();
            facts.stagedWithoutPick = proc.stagedWorkWithoutPick();
        }
        postReport(std::move(key), [done = std::move(done), facts] { done(facts); });
    });
}

// These all reported success whenever git could be started: a rebase
// that stopped on a conflict, or never began, said "Rebase complete."
// A step that succeeds can still leave the rebase in progress — an
// `edit` stops on purpose — so the repository's state goes with the
// outcome. Either way the repository may have changed: both refresh.
//
// The state also goes out as repoStateReady, ahead of the outcome. The
// in-progress bar comes back to life on rebaseStepFinished, and until
// the status refresh came in (seconds, on a big tree) it offered
// Continue for a rebase that had just finished, or with the conflict
// count from before the step.
//
// One step at a time per repository. A second git on top of a running
// one — an abort resetting the branch while the first still picked
// onto it, two sequencers on one todo list — left the branch half
// rebased. The in-progress bar holds its buttons back while a step
// runs; this refuses whatever gets past it. The step that runs still
// reports, so a refused one doesn't: its rebaseStepFinished would
// bring the bar back to life under the running one.
void GitService::runRebaseStep(
    const QString& step,
    std::function<git::Result<git::ProcessOutput>(const git::GitProcess&)> command,
    std::function<QString(const git::Repository&)> refusal,
    git::RepoState operation) {
    if (!repo_) return;
    std::shared_ptr<git::Repository> r = repo_;
    std::string gitDir;
    std::optional<git::GitProcess> process;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        gitDir = rebaseStepKey(*r);
        if (std::lock_guard<std::mutex> steps(stepsMutex_); rebaseSteps_.insert(gitDir).second)
            process = r->process();
    }
    // Quitting stops the step as Ctrl-C would: git and its hooks get
    // SIGINT, and the rebase stays in progress, to be carried on from
    // the in-progress bar next time. Quitting used to wait for the
    // whole step, a hook that never ended included.
    if (process)
        process->addCancelFlag(opsCancelled_);
    if (!process) {
        emit operationFailed(step, rebaseStepStillRunning());
        return;
    }
    emit stepStarted(step);
    runner_.run([this, r, gitDir, step, proc = std::move(*process), command = std::move(command),
                 refusal = std::move(refusal), operation]() {
        // Whatever happens, the repository mustn't stay marked busy:
        // every later step on it would be refused until a restart.
        // The normal path clears the mark itself, under the same lock
        // as the state it reports, so that a step started on the
        // strength of that report is never refused.
        bool cleared = false;
        const auto clearMark = qScopeGuard([&] {
            if (cleared) return;
            std::lock_guard<std::mutex> lock(repoMutex_);
            std::lock_guard<std::mutex> steps(stepsMutex_);
            rebaseSteps_.erase(gitDir);
        });
        QString failure;
        QString warning;
        if (refusal) {
            std::lock_guard<std::mutex> lock(repoMutex_);
            failure = refusal(*r);
        }
        if (failure.isEmpty()) {
            const auto result = command(proc);
            failure = rebaseFailure(result);
            warning = stepWarnings(result);
        }
        git::RepoState state = git::RepoState::None;
        int conflicts = 0;
        bool paused = false;
        bool skippable = false;
        {
            std::lock_guard<std::mutex> lock(repoMutex_);
            {
                std::lock_guard<std::mutex> steps(stepsMutex_);
                rebaseSteps_.erase(gitDir);
            }
            cleared = true;
            state = r->state();
            paused = r->sequencePaused();
            skippable = r->sequenceSkippable();
            // Counted from the index alone: the resolver reads the
            // blobs, if it's asked to.
            if (state != git::RepoState::None) {
                if (auto count = r->conflictCount())
                    conflicts = *count;
            }
        }
        // Reported on the service's thread, if another repository
        // hasn't been opened meanwhile (checked there, where the swap
        // happens: checked here, an outcome posted just before an open
        // landed reached the next repository). What the step did, it
        // did to this one; reported then, it would read as the open
        // one's ("Rebase complete.", or an offer to resolve its
        // conflicts). The same repository opened again (from Recent, or
        // handed over by a second launch) is still this one, a new
        // Repository for it or not: the outcome is its own, and its bar
        // waits for it. While an open is on its way the outcome waits
        // for it (reportTo): reported at once, a conflict stop offered
        // its resolver over the next repository's "Opening…", and a Yes
        // given once that had landed opened the resolver on it. An open
        // that fails leaves this one open, and it gets the outcome then.
        QMetaObject::invokeMethod(this, [this, gitDir, step, failure, warning, state, conflicts,
                                         paused, skippable, operation]() {
            reportTo(gitDir, [this, step, failure, warning, state, conflicts, paused, skippable,
                              operation] {
                if (!failure.isEmpty())
                    emit operationFailed(step, failure);
                if (!warning.isEmpty())
                    emit stepWarning(step, warning);
                emit repoStateReady(state, conflicts, paused, skippable);
                emit rebaseStepFinished(step, failure.isEmpty(), state == operation);
                refreshStatus();
                refreshLog();
                // A rebase moves the branch, and an abort puts it back.
                refreshBranches();
            });
        }, Qt::QueuedConnection);
    });
}

// A cherry-pick or revert of several commits (`git cherry-pick A B C`,
// from a terminal) paused between two of them: nothing in GitBolt ran
// git's --continue, so the bar's Commit… was all there was, and with
// nothing to commit the rest of the sequence never came. Steps like the
// rebase's: one at a time, with the bar busy until the outcome.
void GitService::sequenceContinue() {
    runSequenceStep(/*skip=*/false);
}

void GitService::sequenceSkip() {
    runSequenceStep(/*skip=*/true);
}

void GitService::runSequenceStep(bool skip) {
    if (!repo_) return;
    // Anything but a revert gets the cherry-pick's command, and git's
    // own "no cherry-pick or revert in progress" if that's the case.
    const bool revert = withRepository(
        [](const git::Repository& r) { return r.state() == git::RepoState::Revert; });
    const QString step = (revert ? QStringLiteral("revert") : QStringLiteral("cherry-pick"))
                       + (skip ? QStringLiteral(" --skip") : QStringLiteral(" --continue"));
    runRebaseStep(step, [revert, skip](const git::GitProcess& proc) {
        return skip ? proc.sequenceSkip(revert) : proc.sequenceContinue(revert);
    }, {}, revert ? git::RepoState::Revert : git::RepoState::CherryPick);
}

// ---------------------------------------------------------------------------
// Cherry-pick
// ---------------------------------------------------------------------------

// One commit: this took a list, but libgit2's pick only stages, so the
// second one met the first one's uncommitted changes, failed, and took
// the first pick's CHERRY_PICK_HEAD and MERGE_MSG down with it.
void GitService::cherryPick(const git::ObjectId& commit) {
    if (!repo_) return;
    if (refusedWhileAStepRuns(QStringLiteral("cherryPick"))) return;
    // Pin the repository like every other async op does: the
    // shared_ptr keeps the object alive across a close/switch while
    // this job sits in the thread pool, and the staleness check
    // drops the work instead of cherry-picking onto a repository
    // that is no longer the open one. (This used to capture the raw
    // pointer with no re-check — a use-after-free.)
    std::shared_ptr<git::Repository> r = repo_;
    runner_.run([this, r, commit]() {
        std::lock_guard<std::mutex> lock(repoMutex_);
        if (r != repo_)
            return;  // superseded: repo closed or swapped mid-flight
        // Refused while another operation is in progress (see
        // Repository::cherryPick), with a failure alone: a
        // cherryPickComplete(false) offers to resolve the conflicts
        // the pick stopped on, and these would be the merge's, or the
        // earlier pick's.
        const bool refused = r->state() != git::RepoState::None;
        auto result = r->cherryPick(commit);
        QString error;
        bool conflicts = false;
        bool alreadyApplied = false;
        if (!result) {
            error = QString::fromStdString(result.error().message());
        } else {
            conflicts = result->hasConflicts;
            alreadyApplied = result->alreadyApplied;
        }
        // Reported on the service's thread, to this repository if it is
        // the open one then (reportTo), as a rebase step's outcome is.
        // Emitted here, a pick that stopped on conflicts just before
        // another repository was opened said so over that one, and
        // offered to resolve its conflicts.
        QMetaObject::invokeMethod(this, [this, key = rebaseStepKey(*r), refused, error,
                                         conflicts, alreadyApplied, commit]() {
            reportTo(key, [this, refused, error, conflicts, alreadyApplied, commit] {
                if (!error.isEmpty()) {
                    emit operationFailed(QStringLiteral("cherryPick"), error);
                    if (!refused)
                        emit cherryPickComplete(false, error);
                    return;
                }
                if (conflicts) {
                    emit cherryPickComplete(
                        false,
                        tr("Cherry-pick produced conflicts for commit %1")
                            .arg(QString::fromStdString(commit.toShortHex())));
                    refreshStatus();
                    return;
                }
                if (alreadyApplied) {
                    emit cherryPickComplete(
                        true,
                        tr("Commit %1 is already on this branch: nothing to pick.")
                            .arg(QString::fromStdString(commit.toShortHex())));
                    refreshStatus();
                    return;
                }
                emit cherryPickComplete(true, tr("Cherry-pick completed successfully."));
                refreshStatus();
                refreshLog();
            });
        }, Qt::QueuedConnection);
    });
}

// ---------------------------------------------------------------------------
// Stash
// ---------------------------------------------------------------------------

void GitService::stashSave(const QString& message, bool includeUntracked,
                           bool keepIndex) {
    if (!repo_) return;
    if (refusedWhileAStepRuns(QStringLiteral("stashSave"))) return;
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
    if (refusedWhileAStepRuns(QStringLiteral("stashApply"))) return;
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
    if (refusedWhileAStepRuns(QStringLiteral("stashPop"))) return;
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
    if (refusedWhileAStepRuns(QStringLiteral("stashDrop"))) return;
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
        if (result)
            deliverFor(r, [this, stashes = std::move(*result)]() mutable {
                emit stashesReady(std::move(stashes));
            });
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
        if (result)
            deliverFor(r, [this, tags = std::move(*result)]() mutable {
                emit tagsReady(std::move(tags));
            });
    });
}

bool GitService::createTag(const QString& name, const QString& target,
                            const QString& message, bool annotated,
                            bool pushAfter) {
    if (!repo_) return false;

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
        return false;
    }
    refreshTags();
    if (pushAfter) {
        // `git push origin refs/tags/<tag>` on the same synchronous CLI
        // path as push(remote, branch); failures surface via
        // operationFailed("push"). The full ref, because the short
        // name is ambiguous whenever a branch shares it.
        return push(QStringLiteral("origin"), QStringLiteral("refs/tags/") + name);
    }
    return true;
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
        if (result)
            deliverFor(r, [this, submodules = std::move(*result)]() mutable {
                emit submodulesReady(std::move(submodules));
            });
    });
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
        if (result)
            deliverFor(r, [this, worktrees = std::move(*result)]() mutable {
                emit worktreesReady(std::move(worktrees));
            });
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

void GitService::removeWorktree(const QString& name, const QString& folder) {
    if (!repo_) return;
    bool ok = false;
    QString err;
    {
        std::lock_guard<std::mutex> lock(repoMutex_);
        auto result = repo_->removeWorktree(name.toStdString(), folder.toStdString());
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
    git::GitProcess proc = process();
    runner_.run([this, key = openKey_, proc = std::move(proc)]() mutable {
        auto result = proc.gitFlowInit();
        // To the repository it ran on, if that is the open one then
        // (reportTo): emitted here, it reached the next one.
        postReport(key, [this, result] {
            if (!result) {
                emit operationFailed("gitFlowInit",
                    QString::fromStdString(result.error().message()));
                emit gitFlowOperationComplete(false,
                    QString::fromStdString(result.error().message()));
            } else {
                emit gitFlowOperationComplete(true, tr("Git Flow initialized."));
            }
        });
    });
}

void GitService::featureStart(const QString& name) {
    if (!repo_) return;
    git::GitProcess proc = process();
    runner_.run([this, key = openKey_, proc = std::move(proc), n = name.toStdString()]() mutable {
        auto result = proc.gitFlowFeatureStart(n);
        // To the repository it ran on, if that is the open one then
        // (reportTo): emitted here, it reached the next one.
        postReport(key, [this, result, n] {
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
    });
}

void GitService::featureFinish(const QString& name) {
    if (!repo_) return;
    git::GitProcess proc = process();
    runner_.run([this, key = openKey_, proc = std::move(proc), n = name.toStdString()]() mutable {
        auto result = proc.gitFlowFeatureFinish(n);
        // To the repository it ran on, if that is the open one then
        // (reportTo): emitted here, it reached the next one.
        postReport(key, [this, result, n] {
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
    });
}

void GitService::releaseStart(const QString& version) {
    if (!repo_) return;
    git::GitProcess proc = process();
    runner_.run([this, key = openKey_, proc = std::move(proc), v = version.toStdString()]() mutable {
        auto result = proc.gitFlowReleaseStart(v);
        // To the repository it ran on, if that is the open one then
        // (reportTo): emitted here, it reached the next one.
        postReport(key, [this, result, v] {
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
    });
}

void GitService::releaseFinish(const QString& version) {
    if (!repo_) return;
    git::GitProcess proc = process();
    runner_.run([this, key = openKey_, proc = std::move(proc), v = version.toStdString()]() mutable {
        auto result = proc.gitFlowReleaseFinish(v);
        // To the repository it ran on, if that is the open one then
        // (reportTo): emitted here, it reached the next one.
        postReport(key, [this, result, v] {
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
    });
}

void GitService::hotfixStart(const QString& version) {
    if (!repo_) return;
    git::GitProcess proc = process();
    runner_.run([this, key = openKey_, proc = std::move(proc), v = version.toStdString()]() mutable {
        auto result = proc.gitFlowHotfixStart(v);
        // To the repository it ran on, if that is the open one then
        // (reportTo): emitted here, it reached the next one.
        postReport(key, [this, result, v] {
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
    });
}

void GitService::hotfixFinish(const QString& version) {
    if (!repo_) return;
    git::GitProcess proc = process();
    runner_.run([this, key = openKey_, proc = std::move(proc), v = version.toStdString()]() mutable {
        auto result = proc.gitFlowHotfixFinish(v);
        // To the repository it ran on, if that is the open one then
        // (reportTo): emitted here, it reached the next one.
        postReport(key, [this, result, v] {
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
    git::GitProcess proc = process();
    runner_.run([this, key = openKey_, proc = std::move(proc)]() mutable {
        auto result = proc.gc();
        postReport(key, [this, result] {
            if (!result) {
                emit operationFailed("gc",
                    QString::fromStdString(result.error().message()));
            } else {
                emit maintenanceComplete(
                    QString::fromStdString(result->stdoutData + result->stderrData));
            }
        });
    });
}

void GitService::runPrune() {
    if (!repo_) return;
    git::GitProcess proc = process();
    runner_.run([this, key = openKey_, proc = std::move(proc)]() mutable {
        auto result = proc.prune();
        postReport(key, [this, result] {
            if (!result) {
                emit operationFailed("prune",
                    QString::fromStdString(result.error().message()));
            } else {
                emit maintenanceComplete(
                    QString::fromStdString(result->stdoutData + result->stderrData));
            }
        });
    });
}

void GitService::runFsck() {
    if (!repo_) return;
    git::GitProcess proc = process();
    runner_.run([this, key = openKey_, proc = std::move(proc)]() mutable {
        auto result = proc.fsck();
        postReport(key, [this, result] {
            if (!result) {
                emit operationFailed("fsck",
                    QString::fromStdString(result.error().message()));
            } else {
                emit maintenanceComplete(
                    QString::fromStdString(result->stdoutData + result->stderrData));
            }
        });
    });
}

void GitService::runRepack() {
    if (!repo_) return;
    git::GitProcess proc = process();
    runner_.run([this, key = openKey_, proc = std::move(proc)]() mutable {
        auto result = proc.run({"repack", "-a", "-d"});
        postReport(key, [this, result] {
            if (!result) {
                emit operationFailed("repack",
                    QString::fromStdString(result.error().message()));
            } else {
                emit maintenanceComplete(
                    QString::fromStdString(result->stdoutData + result->stderrData));
            }
        });
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
