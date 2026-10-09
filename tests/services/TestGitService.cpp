//
// TestGitService — exercises the orchestration layer against real
// on-disk repositories built by TestRepoHelper.
//
// GitService is the threading-heavy core: every operation either
// runs on a QtConcurrent worker under repoMutex_ or mutates the repo
// synchronously and re-kicks async refreshes. These tests pin down
// the contract pieces that the UI depends on and that broke (or
// nearly broke) in the past:
//
//   - open/close lifecycle and its signals,
//   - async open success AND failure delivery,
//   - statusReady / logReady payloads for a real repo,
//   - commitChanges round trip,
//   - stashSave's keep-index flag (wired in the same change set
//     that added these tests),
//   - the cherry-pick close race: a queued cherry-pick must neither
//     crash nor emit once the repository it was queued for is gone
//     (regression test for the raw-pointer capture UAF),
//   - tag create / delete as the Commands menu drives them.
//
// Signal delivery: workers emit from pool threads, and QSignalSpy
// records those on the emitting thread, so the tests wait for worker
// signals through MainThreadSpy below. QSignalSpy remains for signals
// emitted on the main thread, and for counting after the pool drained.
//

#include <QCoreApplication>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "../TestRepoHelper.h"
#include "git/GitProcess.h"
#include "services/GitService.h"

#include <algorithm>
#include <functional>
#include <type_traits>

using gitbolt::services::GitService;

namespace {

// QSignalSpy for signals a pool worker emits. QSignalSpy connects
// directly, so it records a worker's emit on the worker, the moment
// it happens; QSignalSpy::wait() only counts emits after it starts,
// so one that lands between kicking the job and calling wait() is
// missed, and wait() sits out its whole timeout and fails. That is
// the likely cause of this binary's Windows CI failures, which took
// a passing run's time plus 5 s. Here the context object lives on
// the main thread, so the connection is queued: emits arrive only
// while the test spins the event loop, as they do in MainWindow, and
// no other thread touches the list.
class MainThreadSpy : public QList<QVariantList> {
public:
    template <typename Sender, typename... Args>
    MainThreadSpy(const Sender* sender, void (Sender::*signal)(Args...)) {
        QObject::connect(sender, signal, &context_,
                         [this](const std::decay_t<Args>&... args) {
            append(QVariantList{QVariant::fromValue(args)...});
        });
    }

    // Spins the event loop until at least one emit has arrived.
    // Unlike QSignalSpy::wait(), emits from before the call count.
    [[nodiscard]] bool waitForAny(int timeoutMs = 5000) {
        return QTest::qWaitFor([this] { return !isEmpty(); }, timeoutMs);
    }

private:
    QObject context_;
};

// Convenience: build a TestRepo with `n` commits on main, files
// f0.txt..f(n-1).txt.
std::unique_ptr<gitbolt::test::TestRepo> repoWithCommits(int n) {
    auto repo = std::make_unique<gitbolt::test::TestRepo>();
    for (int i = 0; i < n; ++i) {
        auto res = repo->writeAndCommit(
            QStringLiteral("f%1.txt").arg(i),
            QByteArrayLiteral("content ") + QByteArray::number(i) + '\n',
            QStringLiteral("commit %1").arg(i));
        if (!res.ok())
            return nullptr;
    }
    return repo;
}

// The order in which the main thread receives GitService's completion
// and refresh signals, by short name. The context object lives on the
// main thread, so a worker's emit is queued here exactly as it is to
// MainWindow, and only the main thread touches the list. Failures are
// recorded with their message, so a QVERIFY2 on the joined log says
// what went wrong.
class SignalLog : public QStringList {
public:
    explicit SignalLog(const GitService& svc) {
        record(svc, &GitService::statusReady, "status");
        record(svc, &GitService::logReady, "log");
        record(svc, &GitService::branchesReady, "branches");
        record(svc, &GitService::commitComplete, "commitComplete");
        record(svc, &GitService::rebaseComplete, "rebaseComplete");
        record(svc, &GitService::cherryPickComplete, "cherryPickComplete");
        QObject::connect(&svc, &GitService::operationFailed, &context_,
                         [this](const QString& op, const QString& err) {
            append(QStringLiteral("operationFailed(%1: %2)").arg(op, err));
        });
    }

    // Spins the event loop until every one of `names` has arrived.
    [[nodiscard]] bool waitFor(const QStringList& names, int timeoutMs = 10000) {
        return QTest::qWaitFor([&] {
            return std::all_of(names.begin(), names.end(),
                               [this](const QString& n) { return contains(n); });
        }, timeoutMs);
    }

    // Spins the event loop until `first` has arrived and, after it,
    // every one of `then`. Order-aware because the repository watcher
    // can slip in a status refresh of its own at any point.
    [[nodiscard]] bool waitForAfter(const QString& first, const QStringList& then,
                                    int timeoutMs = 10000) {
        return QTest::qWaitFor([&] {
            const qsizetype at = indexOf(first);
            return at >= 0
                && std::all_of(then.begin(), then.end(),
                               [&](const QString& n) { return indexOf(n, at + 1) > at; });
        }, timeoutMs);
    }

    [[nodiscard]] bool hasFailure() const {
        return std::any_of(begin(), end(), [](const QString& e) {
            return e.startsWith(QLatin1String("operationFailed"));
        });
    }

private:
    template <typename Signal>
    void record(const GitService& svc, Signal sig, const char* name) {
        QObject::connect(&svc, sig, &context_,
                         [this, name] { append(QString::fromLatin1(name)); });
    }

    QObject context_;
};

} // namespace

class TestGitService : public QObject {
    Q_OBJECT

private slots:
    // Touch libgit2 once so its static init/shutdown lifecycle runs
    // in a known order (same SIGTRAP-at-teardown workaround as
    // TestCommitLogModel — see the comment there).
    void initTestCase() {
        gitbolt::test::TestRepo touch;
        QVERIFY(!touch.path().isEmpty());
    }

    // Every test fails on Qt's cross-thread parenting warning and on
    // any AsyncRunner warning (an off-thread submission, or a worker
    // that threw). The first is what a pool thread driving GitService
    // state printed: "QObject: Cannot create children for a parent
    // that is in a different thread".
    void init() {
        QTest::failOnWarning(
            QRegularExpression(QStringLiteral("Cannot create children")));
        QTest::failOnWarning(QRegularExpression(QStringLiteral("AsyncRunner")));
    }

    // -----------------------------------------------------------------
    // Every git child must carry the askpass wiring: GIT_ASKPASS /
    // SSH_ASKPASS point back at this binary so auth questions become
    // GUI prompts instead of instant failures. A shell alias executed
    // BY git prints what's really in git's child environment — `git
    // var GIT_ASKPASS` would be more direct but only exists in newer
    // gits (Apple's 2.39 lacks it).
    // -----------------------------------------------------------------
    void gitChildrenGetAskpassEnvironment() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);

        gitbolt::git::GitProcess proc(repo->path().toStdString());
        auto result = proc.run(
            {"-c", "alias.echo-askpass=!printf '%s' \"$GIT_ASKPASS\"",
             "echo-askpass"});
        QVERIFY(result.ok());
        QCOMPARE(result.value().exitCode, 0);
        QCOMPARE(QString::fromStdString(result.value().stdoutData).trimmed(),
                 QCoreApplication::applicationFilePath());
    }

    // -----------------------------------------------------------------
    // Synchronous open + close: state flags and signals.
    // -----------------------------------------------------------------
    void openAndCloseLifecycle() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);

        GitService svc;
        QSignalSpy closedSpy(&svc, &GitService::repositoryClosed);

        QVERIFY(!svc.isOpen());
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(svc.isOpen());
        QVERIFY(svc.repository() != nullptr);

        svc.closeRepository();
        QVERIFY(!svc.isOpen());
        QVERIFY(svc.repository() == nullptr);
        QCOMPARE(closedSpy.count(), 1);
    }

    // -----------------------------------------------------------------
    // Async open: repositoryOpened with the requested path.
    // -----------------------------------------------------------------
    void asyncOpenEmitsOpened() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);

        GitService svc;
        QSignalSpy openedSpy(&svc, &GitService::repositoryOpened);
        QSignalSpy failedSpy(&svc, &GitService::repositoryOpenFailed);

        svc.openRepositoryAsync(repo->path());
        QVERIFY(openedSpy.wait(5000));
        QCOMPARE(openedSpy.count(), 1);
        QCOMPARE(openedSpy.first().at(0).toString(), repo->path());
        QCOMPARE(failedSpy.count(), 0);
        QVERIFY(svc.isOpen());
    }

    // -----------------------------------------------------------------
    // Async open of a directory that is not a repository: failure
    // signal, no open state, and the previous state is untouched.
    // -----------------------------------------------------------------
    void asyncOpenFailureEmitsFailed() {
        QTemporaryDir notARepo;
        QVERIFY(notARepo.isValid());

        GitService svc;
        QSignalSpy openedSpy(&svc, &GitService::repositoryOpened);
        QSignalSpy failedSpy(&svc, &GitService::repositoryOpenFailed);

        svc.openRepositoryAsync(notARepo.path());
        QVERIFY(failedSpy.wait(5000));
        QCOMPARE(failedSpy.count(), 1);
        QCOMPARE(failedSpy.first().at(0).toString(), notARepo.path());
        QVERIFY(!failedSpy.first().at(1).toString().isEmpty());
        QCOMPARE(openedSpy.count(), 0);
        QVERIFY(!svc.isOpen());
    }

    // -----------------------------------------------------------------
    // refreshStatus: a dirty working tree shows up in statusReady.
    // -----------------------------------------------------------------
    void refreshStatusReportsDirtyFile() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);

        GitService svc;
        QVERIFY(svc.openRepository(repo->path()));

        repo->writeFile(QStringLiteral("dirty.txt"),
                        QByteArrayLiteral("uncommitted\n"));

        MainThreadSpy statusSpy(&svc, &GitService::statusReady);
        svc.refreshStatus();

        // openRepository's own refresh can land here too, with a
        // status taken before dirty.txt existed: look at every
        // delivery, not just the last one.
        const auto sawDirty = [&statusSpy] {
            for (const auto& args : statusSpy) {
                const auto entries = args.at(0)
                    .value<std::vector<gitbolt::git::StatusEntry>>();
                for (const auto& e : entries)
                    if (e.path == "dirty.txt") return true;
            }
            return false;
        };
        QVERIFY2(QTest::qWaitFor(sawDirty, 5000),
                 "statusReady did not include the dirty file");
    }

    // -----------------------------------------------------------------
    // refreshLog: all commits arrive, newest first, offset 0.
    // -----------------------------------------------------------------
    void refreshLogDeliversCommits() {
        auto repo = repoWithCommits(3);
        QVERIFY(repo);

        GitService svc;
        QVERIFY(svc.openRepository(repo->path()));

        MainThreadSpy logSpy(&svc, &GitService::logReady);
        svc.refreshLog();
        QVERIFY(logSpy.waitForAny());

        const auto commits = logSpy.last().at(0)
            .value<std::vector<gitbolt::git::CommitData>>();
        QCOMPARE(logSpy.last().at(1).toInt(), 0);
        QCOMPARE(commits.size(), size_t(3));
        QCOMPARE(commits.front().summary, std::string("commit 2"));
        QCOMPARE(commits.back().summary, std::string("commit 0"));
    }

    // -----------------------------------------------------------------
    // commitChanges: staged content becomes a commit; commitComplete
    // reports success; the on-disk repo really has the new commit.
    // -----------------------------------------------------------------
    void commitChangesCreatesCommit() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);

        GitService svc;
        QVERIFY(svc.openRepository(repo->path()));

        repo->writeFile(QStringLiteral("staged.txt"),
                        QByteArrayLiteral("to be committed\n"));
        QVERIFY(repo->stageFile(QStringLiteral("staged.txt")).ok());

        QSignalSpy doneSpy(&svc, &GitService::commitComplete);
        svc.commitChanges(QStringLiteral("service-made commit"));
        // commitComplete may arrive synchronously (the commit itself
        // is not on a worker) — QSignalSpy::wait only sees NEW
        // signals, so poll instead of wait.
        QTRY_VERIFY_WITH_TIMEOUT(!doneSpy.isEmpty(), 5000);
        QVERIFY2(doneSpy.last().at(0).toBool(),
                 qPrintable(doneSpy.last().at(1).toString()));

        // Assert against the repository, not the service: the commit
        // must exist on disk with the right summary.
        auto head = repo->repo().head();
        QVERIFY(head.ok());
        auto commit = repo->repo().lookupCommit(head.value());
        QVERIFY(commit.ok());
        QCOMPARE(commit.value().summary, std::string("service-made commit"));
    }

    // -----------------------------------------------------------------
    // stashSave with keepIndex: the staged change survives in the
    // index, the unstaged change is stashed away.
    // -----------------------------------------------------------------
    void stashSaveKeepIndexPreservesStagedEntry() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);

        GitService svc;
        QVERIFY(svc.openRepository(repo->path()));

        // One staged modification, one unstaged modification.
        repo->writeFile(QStringLiteral("f0.txt"),
                        QByteArrayLiteral("staged edit\n"));
        QVERIFY(repo->stageFile(QStringLiteral("f0.txt")).ok());
        repo->writeFile(QStringLiteral("loose.txt"),
                        QByteArrayLiteral("unstaged edit\n"));

        MainThreadSpy stashesSpy(&svc, &GitService::stashesReady);
        QSignalSpy failSpy(&svc, &GitService::operationFailed);
        svc.stashSave(QStringLiteral("keep-index stash"),
                      /*includeUntracked=*/true, /*keepIndex=*/true);
        QVERIFY(stashesSpy.waitForAny());
        QCOMPARE(failSpy.count(), 0);

        // The unstaged file is gone from the working tree (stashed);
        // the staged edit is still in the index.
        QVERIFY(!QFile::exists(
            QDir(repo->path()).absoluteFilePath(QStringLiteral("loose.txt"))));
        auto status = repo->repo().status();
        QVERIFY(status.ok());
        bool stagedSurvived = false;
        for (const auto& e : status.value()) {
            if (e.path == "f0.txt" && e.isStaged())
                stagedSurvived = true;
        }
        QVERIFY2(stagedSurvived,
                 "keep-index stash should leave the staged edit staged");
    }

    // -----------------------------------------------------------------
    // Regression test for the cherry-pick close race (the worker used
    // to capture a raw Repository* with no staleness re-check — a
    // use-after-free once close destroyed the repo). The fixed worker
    // pins a shared_ptr and drops superseded work: no crash, and no
    // completion signal for a repository that is no longer open.
    // -----------------------------------------------------------------
    void closeDuringQueuedCherryPickIsDropped() {
        auto repo = repoWithCommits(2);
        QVERIFY(repo);

        auto head = repo->repo().head();
        QVERIFY(head.ok());

        GitService svc;
        QVERIFY(svc.openRepository(repo->path()));

        QSignalSpy doneSpy(&svc, &GitService::cherryPickComplete);

        // Force the ordering the old raw-pointer code crashed on:
        // the close must complete BEFORE the cherry-pick worker
        // starts. Shrink the global pool to one thread and park it
        // on a gate; the cherry-pick job queues behind the gate,
        // close swaps the repo out, then the gate opens.
        auto* pool = QThreadPool::globalInstance();
        const int savedThreads = pool->maxThreadCount();
        pool->setMaxThreadCount(1);
        // A failed QVERIFY returns early; the later tests in this
        // binary must not inherit a one-thread global pool.
        const auto restoreThreads =
            qScopeGuard([&] { pool->setMaxThreadCount(savedThreads); });
        QSemaphore gate;
        auto gateJob = QtConcurrent::run([&gate] { gate.acquire(); });

        svc.cherryPick({head.value()});
        svc.closeRepository();
        gate.release();
        gateJob.waitForFinished();

        // Let the (now unblocked) cherry-pick job run to its
        // staleness check and drop. Waiting for the pool to drain
        // (not a fixed sleep) guarantees the job really ran before
        // the assertions below, however slow the runner.
        QVERIFY(pool->waitForDone(5000));

        QCOMPARE(doneSpy.count(), 0);
        QVERIFY(!svc.isOpen());
        // Reaching this line without a crash is the other half of
        // the assertion — the worker must not touch the destroyed
        // repository.
    }

    // -----------------------------------------------------------------
    // Remote branches against a local bare "origin". push() of a branch
    // that has never been pushed publishes it with --set-upstream (it
    // used to fail with "has no upstream branch"); once tracked, a
    // later push is an ordinary one; deleteRemoteBranch() removes it
    // on the server and drops the local remote-tracking ref.
    // -----------------------------------------------------------------
    void pushPublishesNewBranchThenDeleteRemoteRemovesIt() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        QTemporaryDir originDir;
        QVERIFY(originDir.isValid());
        auto origin = addBareOrigin(*repo, originDir);
        QVERIFY2(origin.ok(), origin ? "" : origin.error().message().c_str());
        auto& work = repo->repo();
        auto head = work.head();
        QVERIFY(head.ok());
        QVERIFY(work.createBranch("feature", head.value()).ok());
        QVERIFY(work.checkout("feature").ok());

        GitService svc;
        QSignalSpy failedSpy(&svc, &GitService::operationFailed);
        QVERIFY(svc.openRepository(repo->path()));

        // First push: publish + upstream.
        svc.push(QStringLiteral("origin"), QString());
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(tip(*origin, "refs/heads/feature"), tip(work, "HEAD"));
        QCOMPARE(upstreamOf(work, "feature"),
                 std::string("refs/remotes/origin/feature"));
        QCOMPARE(tip(work, "refs/remotes/origin/feature"), tip(work, "HEAD"));

        // Tracked now: a new commit goes up with an ordinary push.
        QVERIFY(repo->writeAndCommit(QStringLiteral("more.txt"),
                                     QByteArrayLiteral("more\n"),
                                     QStringLiteral("more")).ok());
        svc.push(QStringLiteral("origin"), QString());
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(tip(*origin, "refs/heads/feature"), tip(work, "HEAD"));

        // Delete on the server; the tracking ref goes with it.
        svc.deleteRemoteBranch(QStringLiteral("origin"), QStringLiteral("feature"));
        QCOMPARE(failedSpy.count(), 0);
        QVERIFY(!origin->resolveRef("refs/heads/feature").ok());
        QVERIFY(!work.resolveRef("refs/remotes/origin/feature").ok());
    }

    // A failed remote delete must surface as operationFailed under the
    // op name MainWindow keys its remote-op feedback on.
    void deleteRemoteBranchFailureIsReported() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        QTemporaryDir originDir;
        QVERIFY(originDir.isValid());
        auto origin = addBareOrigin(*repo, originDir);
        QVERIFY2(origin.ok(), origin ? "" : origin.error().message().c_str());

        GitService svc;
        QSignalSpy failedSpy(&svc, &GitService::operationFailed);
        QVERIFY(svc.openRepository(repo->path()));
        svc.deleteRemoteBranch(QStringLiteral("origin"),
                               QStringLiteral("no-such-branch"));
        QCOMPARE(failedSpy.count(), 1);
        QCOMPARE(failedSpy.at(0).at(0).toString(),
                 QStringLiteral("delete remote branch"));
        QVERIFY(!failedSpy.at(0).at(1).toString().isEmpty());
    }

    // -----------------------------------------------------------------
    // Refreshes requested from a pool thread. The rebase and cherry-
    // pick workers, and fetch / pull on the pool thread MainWindow's
    // runRemoteOp uses, ask for refreshStatus() & co. when they finish.
    // Those used to run right there: unlocked reads of repo_ and the
    // log scope racing the GUI thread, and AsyncRunner driven off its
    // thread ("Cannot create children for a parent that is in a
    // different thread" from its watcher, plus an unsynchronized drain
    // list). init() fails on that warning; these tests drive each kind
    // of worker and check that the refreshes still arrive, after the
    // completion signal, carrying the post-operation state.
    // -----------------------------------------------------------------
    void rebaseWorkersRefreshAfterCompletion() {
        auto repo = repoWithCommits(2);
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());

        GitService svc;
        SignalLog log(svc);
        QVERIFY(svc.openRepository(repo->path()));
        // The open's own refreshes, out of the way before log.clear().
        QVERIFY(log.waitFor({"status", "log", "branches"}));

        // An exec that fails stops the rebase; --continue finishes it.
        QVERIFY(!succeeded(work.run({"rebase", "--exec", "false", "HEAD~1"})));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::Rebase);
        log.clear();
        svc.rebaseContinue();
        QVERIFY2(log.waitForAfter(QStringLiteral("rebaseComplete"), {"status", "log"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::None);

        // --abort emits no completion signal, just the refreshes.
        QVERIFY(!succeeded(work.run({"rebase", "--exec", "false", "HEAD~1"})));
        log.clear();
        svc.rebaseAbort();
        QVERIFY2(log.waitFor({"status", "log"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::None);
    }

    // The cherry-pick worker asks for its refreshes while it still
    // holds repoMutex_; the refresh workers must queue behind it, not
    // deadlock, and must see what it applied.
    void cherryPickWorkerRefreshesAfterCompletion() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        QVERIFY(succeeded(work.run({"checkout", "-q", "-b", "side"})));
        QVERIFY(repo->writeAndCommit(QStringLiteral("side.txt"),
                                     QByteArrayLiteral("side\n"),
                                     QStringLiteral("side commit")).ok());
        auto picked = repo->repo().head();
        QVERIFY(picked.ok());
        QVERIFY(succeeded(work.run({"checkout", "-q", "-"})));

        GitService svc;
        SignalLog log(svc);
        QObject receiver;
        bool sideStaged = false;
        QObject::connect(&svc, &GitService::statusReady, &receiver,
                         [&](const std::vector<gitbolt::git::StatusEntry>& entries) {
            sideStaged = std::any_of(entries.begin(), entries.end(),
                [](const auto& e) { return e.path == "side.txt" && e.isStaged(); });
        });
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        QVERIFY(!sideStaged);

        log.clear();
        svc.cherryPick({picked.value()});
        QVERIFY2(log.waitForAfter(QStringLiteral("cherryPickComplete"), {"status", "log"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(sideStaged, "the status refresh ran before the cherry-pick applied");
    }

    // fetch / pull / push the way MainWindow::runRemoteOp runs them: on
    // a pool thread, against a local bare origin that is one commit
    // ahead, so the refreshes have something new to show.
    void remoteOpsOffThreadRefreshOnServiceThread() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        QTemporaryDir originDir;
        QVERIFY(originDir.isValid());
        gitbolt::git::GitProcess work(repo->path().toStdString());
        gitbolt::git::GitProcess origin(originDir.path().toStdString());
        QVERIFY(succeeded(origin.run({"init", "--bare", "-q"})));
        QVERIFY(succeeded(work.run({"remote", "add", "origin",
                                    originDir.path().toStdString()})));
        QVERIFY(succeeded(work.run({"push", "-q", "-u", "origin", "HEAD"})));
        // Put "ahead" on the origin only: push it, then rewind the local
        // branch and its remote-tracking ref to the commit before.
        QVERIFY(repo->writeAndCommit(QStringLiteral("ahead.txt"),
                                     QByteArrayLiteral("ahead\n"),
                                     QStringLiteral("ahead")).ok());
        QVERIFY(succeeded(work.run({"push", "-q", "origin", "HEAD"})));
        const QString aheadSha = output(work.run({"rev-parse", "HEAD"}));
        const QString tracking = output(work.run({"rev-parse", "--symbolic-full-name",
                                                  "@{upstream}"}));
        QVERIFY(!tracking.isEmpty());
        QVERIFY(succeeded(work.run({"reset", "-q", "--hard", "HEAD~1"})));
        QVERIFY(succeeded(work.run({"update-ref", tracking.toStdString(), "HEAD"})));

        GitService svc;
        SignalLog log(svc);
        QObject receiver;
        QString newestLogged;
        QString trackingTip;
        QObject::connect(&svc, &GitService::logReady, &receiver,
                         [&](const std::vector<gitbolt::git::CommitData>& commits,
                             int offset) {
            if (offset == 0 && !commits.empty())
                newestLogged = QString::fromStdString(commits.front().summary);
        });
        QObject::connect(&svc, &GitService::branchesReady, &receiver,
                         [&](const std::vector<gitbolt::git::BranchInfo>& branches) {
            for (const auto& b : branches)
                if (b.type == gitbolt::git::BranchType::Remote)
                    trackingTip = QString::fromStdString(b.tipId.toHex());
        });
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        QCOMPARE(newestLogged, QStringLiteral("commit 0"));

        // Runs `op` on a pool thread and spins this thread's event loop
        // until it has finished and `refreshes` have arrived. Polls the
        // future rather than calling waitForFinished(), which runs a job
        // that has not started yet on the calling thread — and the op
        // must not run on the service's thread here.
        const auto offThread = [&](const std::function<void()>& op,
                                   const QStringList& refreshes) {
            log.clear();
            QFuture<void> done = QtConcurrent::run(op);
            // On a timeout, still join before returning: the op holds
            // references to this test's locals (svc, log).
            const auto join = qScopeGuard([&done] { done.waitForFinished(); });
            if (!QTest::qWaitFor([&] { return done.isFinished(); }, 30000))
                return false;
            // Deliver what the op queued before it finished: failure
            // signals and refresh requests.
            QCoreApplication::processEvents();
            return log.waitFor(refreshes, 30000) && !log.hasFailure();
        };

        QVERIFY2(offThread([&] { svc.fetch(); }, {"branches"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(trackingTip, aheadSha);

        QVERIFY2(offThread([&] { svc.pull(QStringLiteral("origin"), QString()); },
                           {"status", "log", "branches"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(output(work.run({"rev-parse", "HEAD"})), aheadSha);
        QCOMPARE(newestLogged, QStringLiteral("ahead"));

        // push() asks for no refresh itself (MainWindow's completion
        // handler does); it must still work from the pool thread.
        QVERIFY(repo->writeAndCommit(QStringLiteral("pushed.txt"),
                                     QByteArrayLiteral("pushed\n"),
                                     QStringLiteral("pushed")).ok());
        QVERIFY2(offThread([&] { svc.push(QStringLiteral("origin"), QString()); }, {}),
                 qPrintable(log.join(QStringLiteral(", "))));
        const QString branch = output(work.run({"symbolic-ref", "--short", "HEAD"}));
        QVERIFY(!branch.isEmpty());
        QCOMPARE(output(origin.run({"rev-parse", "refs/heads/" + branch.toStdString()})),
                 output(work.run({"rev-parse", "HEAD"})));
    }

    // commitChanges runs on the GUI thread and refreshes directly; the
    // UI (CommitDialog, MainWindow) relies on commitComplete reaching it
    // before the refreshed status.
    void commitCompleteArrivesBeforeItsRefresh() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);

        GitService svc;
        SignalLog log(svc);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));

        repo->writeFile(QStringLiteral("staged.txt"),
                        QByteArrayLiteral("to be committed\n"));
        QVERIFY(repo->stageFile(QStringLiteral("staged.txt")).ok());
        log.clear();
        svc.commitChanges(QStringLiteral("ordered commit"));
        QVERIFY2(log.waitForAfter(QStringLiteral("commitComplete"), {"status", "log"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
    }

    // -----------------------------------------------------------------
    // Commands > Delete tag hands deleteTag the short name its picker
    // lists; a full ref name works too. It used to get the full ref
    // from tags() and fail for every tag. The refresh that follows
    // reports short names.
    // -----------------------------------------------------------------
    void deleteTagRemovesTagAndRefreshes() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        auto head = repo->repo().head();
        QVERIFY(head.ok());
        QVERIFY(repo->repo().createLightweightTag("v0.1", head.value()).ok());
        QVERIFY(repo->repo().createTag("v1.0", head.value(), "release").ok());

        GitService svc;
        QSignalSpy failedSpy(&svc, &GitService::operationFailed);
        MainThreadSpy tagsSpy(&svc, &GitService::tagsReady);
        QVERIFY(svc.openRepository(repo->path()));

        svc.deleteTag(QStringLiteral("v1.0"));
        QCOMPARE(failedSpy.count(), 0);
        QVERIFY(tagsSpy.waitForAny());
        const auto tags = tagsSpy.last().at(0)
            .value<std::vector<gitbolt::git::TagInfo>>();
        QCOMPARE(tags.size(), size_t(1));
        QCOMPARE(tags.front().name, std::string("v0.1"));
        QCOMPARE(tags.front().fullRefName, std::string("refs/tags/v0.1"));

        svc.deleteTag(QStringLiteral("refs/tags/v0.1"));
        QCOMPARE(failedSpy.count(), 0);
        auto left = repo->repo().tags();
        QVERIFY(left.ok());
        QVERIFY(left->empty());
    }

    void deleteMissingTagReportsFailure() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);

        GitService svc;
        QSignalSpy failedSpy(&svc, &GitService::operationFailed);
        QVERIFY(svc.openRepository(repo->path()));
        svc.deleteTag(QStringLiteral("no-such-tag"));
        QCOMPARE(failedSpy.count(), 1);
        QCOMPARE(failedSpy.at(0).at(0).toString(), QStringLiteral("deleteTag"));
        QVERIFY(!failedSpy.at(0).at(1).toString().isEmpty());
    }

    // -----------------------------------------------------------------
    // createTag gets TagDialog's target as picked or typed — a branch
    // name by default, or any ref / short hash — not a full hex id.
    // "Push after create" pushes the tag's full ref, so a branch with
    // the same name can't make the refspec ambiguous.
    // -----------------------------------------------------------------
    void createTagResolvesTargetAndPushesTag() {
        auto repo = repoWithCommits(2);
        QVERIFY(repo);
        QTemporaryDir originDir;
        QVERIFY(originDir.isValid());
        gitbolt::git::GitProcess work(repo->path().toStdString());
        gitbolt::git::GitProcess origin(originDir.path().toStdString());
        QVERIFY(succeeded(origin.run({"init", "--bare", "-q"})));
        QVERIFY(succeeded(work.run({"remote", "add", "origin",
                                    originDir.path().toStdString()})));
        QVERIFY(succeeded(work.run({"branch", "v1"})));
        auto branch = repo->repo().headBranchName();
        QVERIFY(branch.ok());
        const QString head = output(work.run({"rev-parse", "HEAD"}));
        const QString parent = output(work.run({"rev-parse", "HEAD~1"}));
        QVERIFY(!parent.isEmpty());

        GitService svc;
        QSignalSpy failedSpy(&svc, &GitService::operationFailed);
        QVERIFY(svc.openRepository(repo->path()));

        // Annotated, on the branch the dialog preselects, pushed.
        svc.createTag(QStringLiteral("v1"), QString::fromStdString(branch.value()),
                      QStringLiteral("release"), /*annotated=*/true,
                      /*pushAfter=*/true);
        QVERIFY2(failedSpy.isEmpty(),
                 qPrintable(failedSpy.isEmpty() ? QString()
                            : failedSpy.first().at(1).toString()));
        QCOMPARE(output(work.run({"cat-file", "-t", "refs/tags/v1"})),
                 QStringLiteral("tag"));
        QCOMPARE(output(work.run({"rev-parse", "refs/tags/v1^{commit}"})), head);
        QCOMPARE(output(origin.run({"rev-parse", "refs/tags/v1"})),
                 output(work.run({"rev-parse", "refs/tags/v1"})));
        QVERIFY(!succeeded(origin.run({"rev-parse", "--verify", "-q",
                                       "refs/heads/v1"})));

        // Lightweight, on a short hash typed into "Or target ref".
        svc.createTag(QStringLiteral("light"), parent.left(8), QString(),
                      /*annotated=*/false, /*pushAfter=*/false);
        QVERIFY(failedSpy.isEmpty());
        QCOMPARE(output(work.run({"rev-parse", "refs/tags/light"})), parent);

        // An unresolvable target fails loudly instead of tagging
        // something arbitrary.
        svc.createTag(QStringLiteral("bad"), QStringLiteral("no-such-ref"),
                      QString(), /*annotated=*/false, /*pushAfter=*/false);
        QCOMPARE(failedSpy.count(), 1);
        QCOMPARE(failedSpy.at(0).at(0).toString(), QStringLiteral("createTag"));
        QVERIFY(!succeeded(work.run({"rev-parse", "--verify", "-q",
                                     "refs/tags/bad"})));
    }

private:
    // The remote tests set up and check through libgit2, so the only
    // git processes are the ones GitService itself runs: a process
    // start costs ~50 ms on the Windows runners, and git CLI setup and
    // checks were over a third of these tests' processes.

    // A bare repository in `dir`, added to `repo` as its "origin".
    static gitbolt::git::Result<gitbolt::git::Repository> addBareOrigin(
            gitbolt::test::TestRepo& repo, const QTemporaryDir& dir) {
        const std::string path = dir.path().toStdString();
        auto origin = gitbolt::git::Repository::init(path, /*bare=*/true);
        if (!origin) return origin;
        // No receive-side `gc --auto` (`maintenance run --auto` in newer
        // gits) after each push: one more process, nothing to collect.
        if (auto r = origin->config().setBool("receive.autogc", false); !r)
            return r.error();
        if (auto r = repo.repo().addRemote("origin", path); !r)
            return r.error();
        return origin;
    }

    // The commit `spec` resolves to, or "<spec>: <error>", which never
    // equals a hash, so comparing two failed lookups still fails.
    static std::string tip(const gitbolt::git::Repository& r,
                           const std::string& spec) {
        auto id = r.resolveRef(spec);
        return id ? id->toHex() : spec + ": " + id.error().message();
    }

    // The ref `branch` tracks (what `git push --set-upstream` records),
    // or "" when it has none.
    static std::string upstreamOf(const gitbolt::git::Repository& r,
                                  const std::string& branch) {
        const std::string ref = "refs/heads/" + branch;
        git_buf buf = GIT_BUF_INIT;
        std::string name;
        if (git_branch_upstream_name(&buf, r.raw(), ref.c_str()) == 0)
            name.assign(buf.ptr, buf.size);
        git_buf_dispose(&buf);
        return name;
    }

    static bool succeeded(
        const gitbolt::git::Result<gitbolt::git::ProcessOutput>& r) {
        return r.ok() && r.value().success();
    }
    // Trimmed stdout, or an empty string when git failed (so a QCOMPARE
    // against an expected value fails instead of crashing).
    static QString output(
        const gitbolt::git::Result<gitbolt::git::ProcessOutput>& r) {
        return succeeded(r) ? QString::fromStdString(r.value().stdoutData).trimmed()
                            : QString();
    }
};

QTEST_GUILESS_MAIN(TestGitService)
#include "TestGitService.moc"
