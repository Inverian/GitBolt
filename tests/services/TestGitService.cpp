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
//   - tag create / delete as the Commands menu drives them,
//   - branch checkout when a tag shares the branch's name,
//   - cancelRemoteOps(), which quitting during a fetch relies on, and
//     a fetch's own cancel flag, which drops an unwanted auto-fetch
//     (and which GitProcess ignores when it is null),
//   - "Checkout as local branch" on a remote branch, a slow one too,
//   - the rebase steps: the dialog's plan runs as listed, with its
//     rewords, squashes, fixups and edits, and messages kept whole;
//     git's failures (a conflict, nothing in progress) are reported,
//     and so is a rebase that is still in progress after a step, with
//     its state ahead of the outcome; a plan made for another HEAD (a
//     commit since, another branch) is refused; a step outlasts 30 s;
//     a step whose repository was swapped out reports nothing, one
//     whose repository was opened again does; and nothing else goes
//     to git while a step runs.
//
// Signal delivery: workers emit from pool threads, and QSignalSpy
// records those on the emitting thread, so the tests wait for worker
// signals through MainThreadSpy below. QSignalSpy remains for signals
// emitted on the main thread, and for counting after the pool drained.
//

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>

#include "../TestProcessHelper.h"
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
        record(svc, &GitService::rebaseStepFinished, "rebaseStep");
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

    // Index of the first entry `pattern` matches in, or -1. (QStringList::
    // indexOf wants the whole entry to match.)
    [[nodiscard]] qsizetype indexOfMatch(const QRegularExpression& pattern) const {
        for (qsizetype i = 0; i < size(); ++i)
            if (pattern.match(at(i)).hasMatch())
                return i;
        return -1;
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

    // The sidebar's "Checkout as local branch" on origin/feature asked
    // for a checkout of plain "feature", which only exists once there's
    // a local branch of that name: for a branch that's only on the
    // remote, the one case the entry is for, it failed. It now creates
    // the local branch, tracking origin/feature; an existing one is
    // checked out as it is.
    void checkoutRemoteBranchCreatesATrackingBranch() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        QTemporaryDir originDir;
        QVERIFY(originDir.isValid());
        auto origin = addBareOrigin(*repo, originDir);
        QVERIFY2(origin.ok(), origin ? "" : origin.error().message().c_str());
        auto& work = repo->repo();
        gitbolt::git::GitProcess git(repo->path().toStdString());
        // feature/x is on the server only, one commit ahead of main.
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "feature/x"})));
        QVERIFY(succeeded(git.run({"commit", "-q", "--allow-empty", "-m", "on feature"})));
        QVERIFY(succeeded(git.run({"push", "-q", "origin", "feature/x"})));
        const std::string featureTip = tip(work, "HEAD");
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
        QVERIFY(succeeded(git.run({"branch", "-q", "-D", "feature/x"})));
        QVERIFY(succeeded(git.run({"fetch", "-q", "origin"})));

        GitService svc;
        QSignalSpy failedSpy(&svc, &GitService::operationFailed);
        QVERIFY(svc.openRepository(repo->path()));
        svc.checkoutRemoteBranch(QStringLiteral("origin/feature/x"));
        QVERIFY2(failedSpy.isEmpty(),
                 failedSpy.isEmpty() ? "" : qPrintable(failedSpy.at(0).at(1).toString()));
        QCOMPARE(output(git.run({"branch", "--show-current"})), QStringLiteral("feature/x"));
        QCOMPARE(tip(work, "HEAD"), featureTip);
        QCOMPARE(upstreamOf(work, "feature/x"), std::string("refs/remotes/origin/feature/x"));

        // Already there: checked out, not recreated (nor moved).
        QVERIFY(succeeded(git.run({"commit", "-q", "--allow-empty", "-m", "local only"})));
        const std::string localTip = tip(work, "HEAD");
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
        svc.checkoutRemoteBranch(QStringLiteral("origin/feature/x"));
        QVERIFY(failedSpy.isEmpty());
        QCOMPARE(output(git.run({"branch", "--show-current"})), QStringLiteral("feature/x"));
        QCOMPARE(tip(work, "HEAD"), localTip);
    }

    // "Checkout as local branch" has git do the checkout, and a first
    // checkout of a branch can take minutes: LFS files downloaded on
    // the way, a slow post-checkout hook, a big tree. git was killed
    // after run()'s default 30 s: in the work tree update that left
    // index.lock behind and the work tree half switched, and in the
    // hook it reported a checkout that had gone through as failed.
    // Here the default is 1 s and the hook takes 3.
    void checkoutRemoteBranchWaitsForASlowCheckout() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        QTemporaryDir originDir;
        QVERIFY(originDir.isValid());
        auto origin = addBareOrigin(*repo, originDir);
        QVERIFY2(origin.ok(), origin ? "" : origin.error().message().c_str());
        gitbolt::git::GitProcess git(repo->path().toStdString());
        QVERIFY(succeeded(git.run({"push", "-q", "origin", "HEAD:refs/heads/slow"})));
        QVERIFY(succeeded(git.run({"fetch", "-q", "origin"})));

        QTemporaryDir hooks;
        QVERIFY(hooks.isValid());
        const QString ran = hooks.filePath(QStringLiteral("ran"));
        QFile hook(hooks.filePath(QStringLiteral("post-checkout")));
        QVERIFY(hook.open(QIODevice::WriteOnly));
        hook.write("#!/bin/sh\nsleep 3\n: > '" + QFile::encodeName(ran) + "'\n");
        hook.close();
        QVERIFY(hook.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(repo->repo().config().setString("core.hooksPath",
                                                hooks.path().toStdString()).ok());

        GitService svc;
        QSignalSpy failedSpy(&svc, &GitService::operationFailed);
        QVERIFY(svc.openRepository(repo->path()));
        const gitbolt::test::ScopedEnv shortDefault("GITBOLT_GIT_TIMEOUT_MS", "1000");
        svc.checkoutRemoteBranch(QStringLiteral("origin/slow"));
        QVERIFY2(failedSpy.isEmpty(),
                 failedSpy.isEmpty() ? "" : qPrintable(failedSpy.at(0).at(1).toString()));
        QVERIFY(QFileInfo::exists(ran));
        QCOMPARE(output(git.run({"branch", "--show-current"})), QStringLiteral("slow"));
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
        QVERIFY2(log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::None);

        QVERIFY(!succeeded(work.run({"rebase", "--exec", "false", "HEAD~1"})));
        log.clear();
        svc.rebaseAbort();
        QVERIFY2(log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::None);
    }

    // The rebase steps reported success whenever git could be started:
    // a Continue with no rebase in progress said "Rebase complete.".
    // git's failure must come back, before the step's unsuccessful
    // rebaseStepFinished. (Its wording varies: "No rebase in progress?"
    // in git 2.39, "no rebase in progress" in newer gits.)
    void rebaseStepsReportGitFailures() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        GitService svc;
        SignalLog log(svc);
        MainThreadSpy completions(&svc, &GitService::rebaseStepFinished);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));

        const auto failsWithoutARebase = [&](const std::function<void()>& step,
                                             const QString& name) {
            log.clear();
            completions.clear();
            step();
            if (!log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"}))
                return false;
            const qsizetype failure = log.indexOfMatch(QRegularExpression(
                QStringLiteral("^operationFailed\\(%1: .*no rebase in progress")
                    .arg(QRegularExpression::escape(name)),
                QRegularExpression::CaseInsensitiveOption));
            return failure >= 0 && failure < log.indexOf(QStringLiteral("rebaseStep"))
                && completions.size() == 1
                && completions.at(0) == QVariantList{name, false, false};
        };
        QVERIFY2(failsWithoutARebase([&] { svc.rebaseContinue(); },
                                     QStringLiteral("rebase --continue")),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(failsWithoutARebase([&] { svc.rebaseSkip(); },
                                     QStringLiteral("rebase --skip")),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(failsWithoutARebase([&] { svc.rebaseAbort(); },
                                     QStringLiteral("rebase --abort")),
                 qPrintable(log.join(QStringLiteral(", "))));
    }

    // The Rebase dialog's plan, which git never ran: GitService put the
    // todo text itself in GIT_SEQUENCE_EDITOR, git tried to run it as a
    // command and failed, and the dialog reported success. The plan
    // lists commits newest first, as the dialog shows them; reordering
    // and dropping must come out of git that way round.
    void interactiveRebaseRunsThePlan() {
        auto repo = repoWithCommits(4);     // commit 0 … commit 3
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        const auto id = [&](const char* spec) {
            auto r = repo->repo().resolveRef(spec);
            return r ? *r : gitbolt::git::ObjectId();
        };
        const auto op = [&](gitbolt::git::RebaseOperationType type, const char* spec,
                            const char* subject) {
            return gitbolt::git::RebaseOperation{type, id(spec), subject};
        };
        using Type = gitbolt::git::RebaseOperationType;
        gitbolt::git::RebasePlan plan;
        plan.onto = id("HEAD~3");
        plan.head = id("HEAD");
        plan.branch = branchOf(work);
        // Shown as: commit 3, commit 2, commit 1. Move commit 1 to the
        // top and drop commit 2.
        plan.operations = {op(Type::Pick, "HEAD~2", "commit 1"),
                           op(Type::Pick, "HEAD", "commit 3"),
                           op(Type::Drop, "HEAD~1", "commit 2")};

        GitService svc;
        SignalLog log(svc);
        MainThreadSpy completions(&svc, &GitService::rebaseStepFinished);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        log.clear();

        svc.interactiveRebase(plan);
        QVERIFY2(log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(completions.size(), 1);
        QVERIFY(completions.at(0).at(1).toBool());
        QCOMPARE(output(work.run({"log", "--format=%s"})),
                 QStringLiteral("commit 1\ncommit 3\ncommit 0"));
        QVERIFY(!QFileInfo::exists(repo->path() + QStringLiteral("/f2.txt")));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::None);
    }

    // A conflict stops the rebase: reported as a failure, with the
    // repository left mid-rebase for the resolver. Continue then fails
    // until the conflict is resolved, and succeeds after — without the
    // editor `git rebase --continue` opens for the commit message.
    // GitBolt has no terminal: that editor was vi waiting on a pipe
    // until the timeout, or whatever core.editor names (here a command
    // that fails, so running it would fail the continue).
    void rebaseConflictStopsThenContinuesWithoutAnEditor() {
        // A GIT_EDITOR exported where the tests run (some IDEs and
        // agents set one) would outrank core.editor and hide the bug.
        const bool hadEditor = qEnvironmentVariableIsSet("GIT_EDITOR");
        const QByteArray editor = qgetenv("GIT_EDITOR");
        qunsetenv("GIT_EDITOR");
        const auto restoreEditor = qScopeGuard([&] {
            if (hadEditor)
                qputenv("GIT_EDITOR", editor);
        });
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        QVERIFY(succeeded(work.run({"config", "core.editor", "false"})));
        QVERIFY(succeeded(work.run({"checkout", "-q", "-b", "side"})));
        auto sideTip = repo->writeAndCommit(QStringLiteral("f0.txt"),
                                            QByteArrayLiteral("side\n"),
                                            QStringLiteral("side change"));
        QVERIFY(sideTip.ok());
        QVERIFY(succeeded(work.run({"checkout", "-q", "-"})));
        auto mainTip = repo->writeAndCommit(QStringLiteral("f0.txt"),
                                            QByteArrayLiteral("main\n"),
                                            QStringLiteral("main change"));
        QVERIFY(mainTip.ok());
        QVERIFY(succeeded(work.run({"checkout", "-q", "side"})));

        gitbolt::git::RebasePlan plan;
        plan.onto = *mainTip;
        plan.head = *sideTip;
        plan.branch = branchOf(work);
        plan.operations = {{gitbolt::git::RebaseOperationType::Pick, *sideTip,
                            "side change"}};

        GitService svc;
        SignalLog log(svc);
        MainThreadSpy completions(&svc, &GitService::rebaseStepFinished);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));

        const auto step = [&](const std::function<void()>& run) {
            log.clear();
            completions.clear();
            run();
            return log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"})
                && completions.size() == 1;
        };
        QVERIFY2(step([&] { svc.interactiveRebase(plan); }),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY(!completions.at(0).at(1).toBool());
        QVERIFY2(log.indexOfMatch(QRegularExpression(
                     QStringLiteral("^operationFailed\\(rebase: .*could not apply"))) >= 0,
                 qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::Rebase);

        QVERIFY2(step([&] { svc.rebaseContinue(); }),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY(!completions.at(0).at(1).toBool());
        QVERIFY(log.hasFailure());
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::Rebase);

        repo->writeFile(QStringLiteral("f0.txt"), QByteArrayLiteral("resolved\n"));
        QVERIFY(succeeded(work.run({"add", "f0.txt"})));
        QVERIFY2(step([&] { svc.rebaseContinue(); }),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(completions.at(0).at(1).toBool(), qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::None);
        QCOMPARE(output(work.run({"log", "--format=%s"})),
                 QStringLiteral("side change\nmain change\ncommit 0"));
    }

    // The plan's other operations. The dialog offered them, but none
    // could be chosen, and a reword could only have kept the message:
    // git asks an editor for it, and GitBolt's never changes anything.
    // A reword now gets the message the plan gives it; squash keeps
    // both messages, as git's editor would show them; fixup keeps the
    // message it's folded into.
    void interactiveRebaseRewordsSquashesAndFixesUp() {
        auto repo = repoWithCommits(5);     // commit 0 … commit 4
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        const auto id = [&](const char* spec) {
            auto r = repo->repo().resolveRef(spec);
            return r ? *r : gitbolt::git::ObjectId();
        };
        using Type = gitbolt::git::RebaseOperationType;
        const std::string reworded =
            "reworded 3\n\nIt's got a body, 'quotes', \"more quotes\",\n$HOME and `backticks`.\n";
        gitbolt::git::RebasePlan plan;
        plan.onto = id("HEAD~4");
        plan.head = id("HEAD");
        plan.branch = branchOf(work);
        plan.operations = {{Type::Fixup, id("HEAD"), "commit 4"},
                           {Type::Reword, id("HEAD~1"), "commit 3", reworded},
                           {Type::Squash, id("HEAD~2"), "commit 2"},
                           {Type::Pick, id("HEAD~3"), "commit 1"}};

        GitService svc;
        SignalLog log(svc);
        MainThreadSpy steps(&svc, &GitService::rebaseStepFinished);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        log.clear();

        svc.interactiveRebase(plan);
        QVERIFY2(log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(steps.size(), 1);
        QCOMPARE(steps.at(0), (QVariantList{QStringLiteral("rebase"), true, false}));
        QCOMPARE(output(work.run({"log", "--format=%s"})),
                 QStringLiteral("reworded 3\ncommit 1\ncommit 0"));
        QCOMPARE(output(work.run({"log", "-1", "--format=%B", "HEAD"})),
                 QString::fromStdString(reworded).trimmed());
        QCOMPARE(output(work.run({"log", "-1", "--format=%B", "HEAD~1"})),
                 QStringLiteral("commit 1\n\ncommit 2"));
        // Nothing lost: every commit's file is still there.
        QCOMPARE(output(work.run({"ls-files"})),
                 QStringLiteral("f0.txt\nf1.txt\nf2.txt\nf3.txt\nf4.txt"));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::None);
    }

    // `edit` stops the rebase on purpose, and git says so with success.
    // That used to read as "Rebase complete." with the rebase still in
    // progress; the step now reports it, and the repository state goes
    // out with the refresh, for the in-progress bar.
    void editStopsTheRebaseUntilContinue() {
        auto repo = repoWithCommits(3);     // commit 0 … commit 2
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        const auto id = [&](const char* spec) {
            auto r = repo->repo().resolveRef(spec);
            return r ? *r : gitbolt::git::ObjectId();
        };
        using Type = gitbolt::git::RebaseOperationType;
        gitbolt::git::RebasePlan plan;
        plan.onto = id("HEAD~2");
        plan.head = id("HEAD");
        plan.branch = branchOf(work);
        plan.operations = {{Type::Pick, id("HEAD"), "commit 2"},
                           {Type::Edit, id("HEAD~1"), "commit 1"}};

        GitService svc;
        SignalLog log(svc);
        MainThreadSpy steps(&svc, &GitService::rebaseStepFinished);
        MainThreadSpy states(&svc, &GitService::repoStateReady);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        log.clear();
        states.clear();

        svc.interactiveRebase(plan);
        QVERIFY2(log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(steps.size(), 1);
        QCOMPARE(steps.at(0), (QVariantList{QStringLiteral("rebase"), true, true}));
        QCOMPARE(output(work.run({"log", "-1", "--format=%s"})), QStringLiteral("commit 1"));
        QVERIFY(states.waitForAny());
        QCOMPARE(states.last().at(0).value<gitbolt::git::RepoState>(),
                 gitbolt::git::RepoState::Rebase);
        QCOMPARE(states.last().at(1).toInt(), 0);

        log.clear();
        steps.clear();
        svc.rebaseContinue();
        QVERIFY2(log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(steps.at(0),
                 (QVariantList{QStringLiteral("rebase --continue"), true, false}));
        QCOMPARE(output(work.run({"log", "--format=%s"})),
                 QStringLiteral("commit 2\ncommit 1\ncommit 0"));
        QCOMPARE(states.last().at(0).value<gitbolt::git::RepoState>(),
                 gitbolt::git::RepoState::None);
    }

    // A reword whose commit stops on a conflict gets its new message
    // when Continue commits it. A skipped one gives its message to no
    // commit: a todo that amended "the commit just made" after the
    // pick would have renamed the commit before it.
    void rewordSurvivesAConflictButNotASkip() {
        for (const bool skip : {false, true}) {
            auto repo = repoWithCommits(1);
            QVERIFY(repo);
            gitbolt::git::GitProcess work(repo->path().toStdString());
            // Through git itself: TestRepo's commits reuse an index
            // that the checkouts below change behind its back.
            const auto commit = [&](const QString& file, const QByteArray& content,
                                    const char* message) {
                repo->writeFile(file, content);
                if (!succeeded(work.run({"add", file.toStdString()}))
                    || !succeeded(work.run({"commit", "-q", "-m", message})))
                    return gitbolt::git::ObjectId();
                auto head = repo->repo().resolveRef("HEAD");
                return head ? *head : gitbolt::git::ObjectId();
            };
            QVERIFY(succeeded(work.run({"checkout", "-q", "-b", "side"})));
            const auto clash = commit(QStringLiteral("f0.txt"), "side\n", "side clash");
            const auto clean = commit(QStringLiteral("b.txt"), "b\n", "side clean");
            QVERIFY(succeeded(work.run({"checkout", "-q", "-"})));
            const auto mainTip = commit(QStringLiteral("f0.txt"), "main\n", "main change");
            QVERIFY(!clash.isZero() && !clean.isZero() && !mainTip.isZero());
            QVERIFY(succeeded(work.run({"checkout", "-q", "side"})));

            using Type = gitbolt::git::RebaseOperationType;
            gitbolt::git::RebasePlan plan;
            plan.onto = mainTip;
            plan.head = clean;
            plan.branch = branchOf(work);
            plan.operations = {{Type::Reword, clean, "side clean", "clean, reworded\n"},
                               {Type::Reword, clash, "side clash", "clash, reworded\n"}};

            GitService svc;
            SignalLog log(svc);
            MainThreadSpy steps(&svc, &GitService::rebaseStepFinished);
            MainThreadSpy states(&svc, &GitService::repoStateReady);
            QVERIFY(svc.openRepository(repo->path()));
            QVERIFY(log.waitFor({"status", "log", "branches"}));
            log.clear();
            states.clear();

            svc.interactiveRebase(plan);
            QVERIFY2(log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"}),
                     qPrintable(log.join(QStringLiteral(", "))));
            QVERIFY2(steps.at(0) == (QVariantList{QStringLiteral("rebase"), false, true}),
                     qPrintable(log.join(QStringLiteral(", "))));
            QVERIFY(states.waitForAny());
            QCOMPARE(states.last().at(0).value<gitbolt::git::RepoState>(),
                     gitbolt::git::RepoState::Rebase);
            QCOMPARE(states.last().at(1).toInt(), 1);

            log.clear();
            steps.clear();
            if (skip) {
                svc.rebaseSkip();
            } else {
                repo->writeFile(QStringLiteral("f0.txt"), QByteArrayLiteral("resolved\n"));
                QVERIFY(succeeded(work.run({"add", "f0.txt"})));
                svc.rebaseContinue();
            }
            QVERIFY2(log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"}),
                     qPrintable(log.join(QStringLiteral(", "))));
            QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
            QCOMPARE(steps.at(0).at(1).toBool(), true);
            QCOMPARE(steps.at(0).at(2).toBool(), false);
            QCOMPARE(output(work.run({"log", "--format=%s"})),
                     skip ? QStringLiteral("clean, reworded\nmain change\ncommit 0")
                          : QStringLiteral("clean, reworded\nclash, reworded\nmain change\ncommit 0"));
        }
    }

    // git strips the lines that start with its comment character, '#',
    // from a message it has had edited: a reword's, and a squash's. A
    // reworded "#123 Fix login" came out empty, so git stopped and the
    // commit kept its old message; a body's "# Notes" or Markdown
    // heading went missing without a word. Every message now comes out
    // as the plan has it.
    void rebaseKeepsLinesThatLookLikeComments() {
        auto repo = repoWithCommits(1);     // commit 0
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        // Messages as GitBolt's own commits keep them (libgit2 cleans
        // nothing up).
        const auto commit = [&](const char* file, const char* message) {
            repo->writeFile(QString::fromLatin1(file), QByteArray(file) + '\n');
            if (!succeeded(work.run({"add", file}))
                || !succeeded(work.run({"commit", "-q", "--cleanup=verbatim", "-m", message})))
                return gitbolt::git::ObjectId();
            auto head = repo->repo().resolveRef("HEAD");
            return head ? *head : gitbolt::git::ObjectId();
        };
        const char* notes = "notes\n\n# Notes\nkeep this\n## Heading";
        const char* semicolon = "semicolon\n\n; not a comment either";
        const auto a = commit("a.txt", "a");
        const auto b = commit("b.txt", notes);
        const auto c = commit("c.txt", semicolon);
        const auto d = commit("d.txt", "d");
        QVERIFY(!a.isZero() && !b.isZero() && !c.isZero() && !d.isZero());

        using Type = gitbolt::git::RebaseOperationType;
        auto base = repo->repo().resolveRef("HEAD~4");
        QVERIFY(base.ok());
        gitbolt::git::RebasePlan plan;
        plan.onto = *base;
        plan.head = d;
        plan.branch = branchOf(work);
        plan.operations = {{Type::Reword, d, "d", "#123 Fix login\n"},
                           {Type::Squash, c, semicolon},
                           {Type::Reword, b, notes, "new notes\n\n# Notes\nkeep this\n## Heading\n"},
                           {Type::Pick, a, "a"}};

        GitService svc;
        SignalLog log(svc);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        log.clear();

        svc.interactiveRebase(plan);
        QVERIFY2(log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::None);
        QCOMPARE(output(work.run({"log", "-1", "--format=%B", "HEAD"})),
                 QStringLiteral("#123 Fix login"));
        // The squash: both messages whole, and nothing of git's own
        // notes in between.
        QCOMPARE(output(work.run({"log", "-1", "--format=%B", "HEAD~1"})),
                 QStringLiteral("new notes\n\n# Notes\nkeep this\n## Heading\n\n%1")
                     .arg(QLatin1String(semicolon)));
        QCOMPARE(output(work.run({"log", "--format=%s"})),
                 QStringLiteral("#123 Fix login\nnew notes\na\ncommit 0"));
    }

    // The same for a reword that stops on a conflict: Continue commits
    // it, and git edits its message then. That git needs the comment
    // character the rebase started with, or the "#42" line is cut and
    // git refuses to make the commit.
    void rewordAfterAConflictKeepsLinesThatLookLikeComments() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        QVERIFY(succeeded(work.run({"checkout", "-q", "-b", "side"})));
        auto clash = repo->writeAndCommit(QStringLiteral("f0.txt"), QByteArrayLiteral("side\n"),
                                          QStringLiteral("side clash"));
        QVERIFY(clash.ok());
        QVERIFY(succeeded(work.run({"checkout", "-q", "-"})));
        auto mainTip = repo->writeAndCommit(QStringLiteral("f0.txt"), QByteArrayLiteral("main\n"),
                                            QStringLiteral("main change"));
        QVERIFY(mainTip.ok());
        QVERIFY(succeeded(work.run({"checkout", "-q", "side"})));

        gitbolt::git::RebasePlan plan;
        plan.onto = *mainTip;
        plan.head = *clash;
        plan.branch = branchOf(work);
        plan.operations = {{gitbolt::git::RebaseOperationType::Reword, *clash, "side clash",
                            "#42 clash, reworded\n\n# Why\nbecause\n"}};

        GitService svc;
        SignalLog log(svc);
        MainThreadSpy steps(&svc, &GitService::rebaseStepFinished);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        log.clear();

        svc.interactiveRebase(plan);
        QVERIFY2(log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(steps.at(0), (QVariantList{QStringLiteral("rebase"), false, true}));

        repo->writeFile(QStringLiteral("f0.txt"), QByteArrayLiteral("resolved\n"));
        QVERIFY(succeeded(work.run({"add", "f0.txt"})));
        log.clear();
        steps.clear();
        svc.rebaseContinue();
        QVERIFY2(log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(steps.at(0), (QVariantList{QStringLiteral("rebase --continue"), true, false}));
        QCOMPARE(output(work.run({"log", "-1", "--format=%B"})),
                 QStringLiteral("#42 clash, reworded\n\n# Why\nbecause"));
    }

    // The Rebase dialog stays open while the repository goes on. A plan
    // made before a commit, or before another branch was checked out,
    // ran on the HEAD of the moment, its todo list in place of git's,
    // and git dropped every commit the plan didn't name, or rewrote the
    // other branch with them. Such a plan is refused now, and git never
    // runs. rebasePlanProblem() says so first, for the dialog to stay
    // open on it.
    void rebasePlanForAnotherHeadIsRefused_data() {
        QTest::addColumn<QString>("change");
        // A pattern; %1 is the branch the plan was made on.
        QTest::addColumn<QString>("reason");
        QTest::newRow("a commit since") << QStringLiteral("commit")
            << QStringLiteral("%1 has moved since the rebase was planned");
        QTest::newRow("another branch, same commit") << QStringLiteral("branch")
            << QStringLiteral("The rebase was planned on %1, but other is checked out now");
        QTest::newRow("detached, same commit") << QStringLiteral("detach")
            << QStringLiteral("The rebase was planned on %1, but a detached HEAD is "
                              "checked out now");
    }

    void rebasePlanForAnotherHeadIsRefused() {
        QFETCH(QString, change);
        QFETCH(QString, reason);
        auto repo = repoWithCommits(3);     // commit 0 … commit 2
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        const auto id = [&](const char* spec) {
            auto r = repo->repo().resolveRef(spec);
            return r ? *r : gitbolt::git::ObjectId();
        };
        using Type = gitbolt::git::RebaseOperationType;
        gitbolt::git::RebasePlan plan;
        plan.onto = id("HEAD~2");
        plan.head = id("HEAD");
        plan.branch = branchOf(work);
        QVERIFY(!plan.branch.empty());
        plan.operations = {{Type::Pick, id("HEAD"), "commit 2"},
                           {Type::Pick, id("HEAD~1"), "commit 1"}};

        GitService svc;
        SignalLog log(svc);
        MainThreadSpy steps(&svc, &GitService::rebaseStepFinished);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        QCOMPARE(svc.rebasePlanProblem(plan), QString());

        if (change == QLatin1String("commit"))
            QVERIFY(repo->writeAndCommit(QStringLiteral("late.txt"), QByteArrayLiteral("late\n"),
                                         QStringLiteral("made after the plan")).ok());
        else if (change == QLatin1String("branch"))
            QVERIFY(succeeded(work.run({"checkout", "-q", "-b", "other"})));
        else
            QVERIFY(succeeded(work.run({"checkout", "-q", "--detach"})));
        const QString branches = output(work.run({"for-each-ref", "--format=%(refname) %(objectname)"}));
        const QRegularExpression why(
            reason.arg(QRegularExpression::escape(QString::fromStdString(plan.branch))));
        QVERIFY2(why.match(svc.rebasePlanProblem(plan)).hasMatch(),
                 qPrintable(svc.rebasePlanProblem(plan)));
        log.clear();

        svc.interactiveRebase(plan);
        QVERIFY2(log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        const qsizetype failure = log.indexOfMatch(QRegularExpression(
            QStringLiteral("^operationFailed\\(rebase: ") + why.pattern()));
        QVERIFY2(failure >= 0 && failure < log.indexOf(QStringLiteral("rebaseStep")),
                 qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(steps.size(), 1);
        QCOMPARE(steps.at(0), (QVariantList{QStringLiteral("rebase"), false, false}));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::None);
        QCOMPARE(output(work.run({"for-each-ref", "--format=%(refname) %(objectname)"})),
                 branches);
    }

    // Continue, Skip and Abort ran with run()'s 30 s timeout, though one
    // step can replay the rest of a plan, the commit hooks of every
    // reword in it included: git was killed mid-rebase, its index.lock
    // left behind. They get as long as the rebase that started it.
    // Here the default is 1 s and the step takes 3.
    void rebaseStepsOutlastTheDefaultTimeout() {
        auto repo = repoWithCommits(2);
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        // The first exec stops the rebase; the second is left for
        // --continue.
        QVERIFY(!succeeded(work.run({"rebase", "--exec", "false", "--exec", "sleep 3",
                                     "HEAD~1"})));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::Rebase);

        GitService svc;
        SignalLog log(svc);
        MainThreadSpy steps(&svc, &GitService::rebaseStepFinished);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        log.clear();

        const gitbolt::test::ScopedEnv shortDefault("GITBOLT_GIT_TIMEOUT_MS", "1000");
        svc.rebaseContinue();
        QVERIFY2(log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"}, 20000),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(steps.at(0), (QVariantList{QStringLiteral("rebase --continue"), true, false}));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::None);
    }

    // The in-progress bar comes back to life on rebaseStepFinished. Its
    // state only came with the status refresh after that, seconds later
    // on a big tree: meanwhile it offered Continue for a rebase that had
    // just finished, or over conflicts it didn't know of yet. A step now
    // sends the state it left behind first.
    void rebaseStepReportsItsStateFirst() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        QVERIFY(succeeded(work.run({"checkout", "-q", "-b", "side"})));
        auto sideTip = repo->writeAndCommit(QStringLiteral("f0.txt"), QByteArrayLiteral("side\n"),
                                            QStringLiteral("side change"));
        QVERIFY(sideTip.ok());
        QVERIFY(succeeded(work.run({"checkout", "-q", "-"})));
        auto mainTip = repo->writeAndCommit(QStringLiteral("f0.txt"), QByteArrayLiteral("main\n"),
                                            QStringLiteral("main change"));
        QVERIFY(mainTip.ok());
        QVERIFY(succeeded(work.run({"checkout", "-q", "side"})));

        gitbolt::git::RebasePlan plan;
        plan.onto = *mainTip;
        plan.head = *sideTip;
        plan.branch = branchOf(work);
        plan.operations = {{gitbolt::git::RebaseOperationType::Pick, *sideTip, "side change"}};

        GitService svc;
        QStringList events;
        QObject context;
        connect(&svc, &GitService::repoStateReady, &context,
                [&](gitbolt::git::RepoState state, int conflicts) {
            events << QStringLiteral("state(%1, %2)").arg(static_cast<int>(state)).arg(conflicts);
        });
        connect(&svc, &GitService::rebaseStepFinished, &context,
                [&] { events << QStringLiteral("rebaseStep"); });
        SignalLog log(svc);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));

        // What arrives just before the step's outcome.
        const auto stateBeforeOutcome = [&](const std::function<void()>& step) {
            events.clear();
            step();
            if (!QTest::qWaitFor([&] { return events.contains(QStringLiteral("rebaseStep")); },
                                 10000))
                return QStringLiteral("no rebaseStep");
            const qsizetype at = events.indexOf(QStringLiteral("rebaseStep"));
            return at > 0 ? events.at(at - 1) : QStringLiteral("nothing");
        };
        const auto state = [](gitbolt::git::RepoState s, int conflicts) {
            return QStringLiteral("state(%1, %2)").arg(static_cast<int>(s)).arg(conflicts);
        };
        QCOMPARE(stateBeforeOutcome([&] { svc.interactiveRebase(plan); }),
                 state(gitbolt::git::RepoState::Rebase, 1));

        repo->writeFile(QStringLiteral("f0.txt"), QByteArrayLiteral("resolved\n"));
        QVERIFY(succeeded(work.run({"add", "f0.txt"})));
        QCOMPARE(stateBeforeOutcome([&] { svc.rebaseContinue(); }),
                 state(gitbolt::git::RepoState::None, 0));
    }

    // A step still running when another repository is opened finished
    // into that one: "Rebase complete." under its name, or, after a
    // conflict, an offer to resolve its conflicts. The outcome now goes
    // nowhere; the repository it belongs to shows its state when it is
    // opened again.
    void rebaseStepOfAReplacedRepositoryReportsNothing() {
        auto repo = repoWithCommits(2);
        auto other = repoWithCommits(1);
        QVERIFY(repo && other);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        // --continue waits at the second exec until there's a "go".
        QVERIFY(!succeeded(work.run({"rebase", "--exec", "false", "--exec",
                                     "while [ ! -f go ]; do sleep 0.05; done", "HEAD~1"})));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::Rebase);

        GitService svc;
        SignalLog log(svc);
        // A "go" on the way out too, should a check fail first:
        // ~GitService waits for the step.
        const auto release = qScopeGuard(
            [&] { repo->writeFile(QStringLiteral("go"), QByteArrayLiteral("")); });
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        svc.rebaseContinue();
        log.clear();
        QVERIFY(svc.openRepository(other->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));

        repo->writeFile(QStringLiteral("go"), QByteArrayLiteral(""));
        QVERIFY(QTest::qWaitFor(
            [&] { return repo->repo().state() == gitbolt::git::RepoState::None; }, 10000));
        // The step's worker returns right after git; let anything it
        // sent arrive.
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QTest::qWait(100);
        QVERIFY2(!log.contains(QStringLiteral("rebaseStep")) && !log.hasFailure(),
                 qPrintable(log.join(QStringLiteral(", "))));
    }

    // The same repository opened again while a step runs — from Recent,
    // the dashboard, a second launch handing its path over — is still
    // the one the step runs on. Its outcome was dropped as if another
    // repository had been opened, and its bar, cleared, offered
    // Continue under the running git. Away to another and back, too.
    void rebaseStepOfTheSameRepositoryOpenedAgainStillReports() {
        auto repo = repoWithCommits(2);
        auto other = repoWithCommits(1);
        QVERIFY(repo && other);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        // --continue waits at the second exec until there's a "go".
        QVERIFY(!succeeded(work.run({"rebase", "--exec", "false", "--exec",
                                     "while [ ! -f go ]; do sleep 0.05; done", "HEAD~1"})));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::Rebase);

        GitService svc;
        SignalLog log(svc);
        // A "go" on the way out too, should a check fail first:
        // ~GitService waits for the step.
        const auto release = qScopeGuard(
            [&] { repo->writeFile(QStringLiteral("go"), QByteArrayLiteral("")); });
        MainThreadSpy steps(&svc, &GitService::rebaseStepFinished);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        QVERIFY(!svc.rebaseStepRunning());
        svc.rebaseContinue();
        QVERIFY(svc.rebaseStepRunning());

        QVERIFY(svc.openRepository(other->path()));
        QVERIFY(!svc.rebaseStepRunning());
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(svc.rebaseStepRunning());
        // Spelled another way, the same repository still.
        QVERIFY(svc.openRepository(repo->path() + QStringLiteral("/./")));
        QVERIFY(svc.rebaseStepRunning());
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        log.clear();

        repo->writeFile(QStringLiteral("go"), QByteArrayLiteral(""));
        QVERIFY2(log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(steps.size(), 1);
        QCOMPARE(steps.at(0), (QVariantList{QStringLiteral("rebase --continue"), true, false}));
        QVERIFY(!svc.rebaseStepRunning());
    }

    // Nothing else goes to git while a rebase step runs on the
    // repository: not another step (two sequencers on one todo list),
    // not the resolver's abort (`git rebase --abort` reset the branch
    // while the step went on picking onto it), nor its staging. Each
    // is refused, and the running step's outcome is the only one.
    void nothingElseRunsWhileARebaseStepDoes() {
        auto repo = repoWithCommits(2);
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        QVERIFY(!succeeded(work.run({"rebase", "--exec", "false", "--exec",
                                     "while [ ! -f go ]; do sleep 0.05; done", "HEAD~1"})));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::Rebase);

        GitService svc;
        SignalLog log(svc);
        // A "go" on the way out too, should a check fail first:
        // ~GitService waits for the step.
        const auto release = qScopeGuard(
            [&] { repo->writeFile(QStringLiteral("go"), QByteArrayLiteral("")); });
        MainThreadSpy steps(&svc, &GitService::rebaseStepFinished);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        log.clear();

        svc.rebaseContinue();
        // git at the exec that waits (git lists a command as done as it
        // starts it).
        QFile done(repo->path() + QLatin1Char('/')
                   + output(work.run({"rev-parse", "--git-path", "rebase-merge/done"})));
        QVERIFY(QTest::qWaitFor([&] {
            if (!done.open(QIODevice::ReadOnly))
                return false;
            const bool waiting = done.readAll().contains("while [ ! -f go ]");
            done.close();
            return waiting;
        }, 10000));
        svc.rebaseSkip();
        svc.rebaseAbort();
        QVERIFY(!svc.abortConflictState());
        QVERIFY(!svc.resolveConflicts({{QStringLiteral("f1.txt"), QStringLiteral("resolved\n")}}));
        for (const char* op : {"rebase --skip", "rebase --abort", "abort", "resolve conflicts"})
            QVERIFY2(log.contains(QStringLiteral("operationFailed(%1: A rebase step is still "
                                                 "running; try again when it's done.)")
                                      .arg(QLatin1String(op))),
                     qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::Rebase);
        QFile f1(repo->path() + QStringLiteral("/f1.txt"));
        QVERIFY(f1.open(QIODevice::ReadOnly));
        QCOMPARE(f1.readAll(), QByteArrayLiteral("content 1\n"));
        QVERIFY(steps.isEmpty());

        repo->writeFile(QStringLiteral("go"), QByteArrayLiteral(""));
        QVERIFY2(log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QTest::qWait(100);
        QCOMPARE(steps.size(), 1);
        QCOMPARE(steps.at(0), (QVariantList{QStringLiteral("rebase --continue"), true, false}));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::None);
        QCOMPARE(output(work.run({"log", "--format=%s"})), QStringLiteral("commit 1\ncommit 0"));
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

    // Quitting while a fetch, pull or push runs: MainWindow cancels the
    // remote ops, then waits for its pool thread, which runs inside
    // this service, before the service is destroyed. Against a server
    // that never answers, that wait used to last until git's 2-minute
    // timeout. The cancel must end the op promptly, take down git's
    // whole process tree, and make a later op fail without starting git.
    void cancelRemoteOpsStopsAStalledFetch() {
#ifndef GITBOLT_TEST_STALL_TRANSPORT
        QSKIP("StallTransport not built");
#else
        using gitbolt::test::processAlive;
        using gitbolt::test::readStallPids;
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        QVERIFY(succeeded(work.run({"remote", "add", "origin",
                                    gitbolt::test::StallTransportEnv::url()})));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString pidFile = dir.filePath(QStringLiteral("stall.pid"));
        gitbolt::test::StallTransportEnv env(pidFile);

        GitService svc;
        SignalLog log(svc);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        log.clear();

        // On a pool thread, as MainWindow runs it. Joined on every exit
        // path: the job references svc.
        QFuture<void> fetch = QtConcurrent::run([&svc] { svc.fetch(); });
        const auto join = qScopeGuard([&] {
            svc.cancelRemoteOps();
            fetch.waitForFinished();
        });
        QTRY_VERIFY_WITH_TIMEOUT(readStallPids(pidFile).stub > 0, 15000);
        const qint64 stub = readStallPids(pidFile).stub;
        QVERIFY(processAlive(stub));
        QVERIFY(!fetch.isFinished());

        QElapsedTimer timer;
        timer.start();
        svc.cancelRemoteOps();
        QVERIFY(QTest::qWaitFor([&] { return fetch.isFinished(); }, 10000));
        QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
        const QString cancelled = QStringLiteral("operationFailed(fetch: Cancelled)");
        QVERIFY2(log.waitFor({cancelled}), qPrintable(log.join(QStringLiteral(", "))));
        // git's grandchild, where ssh and git-remote-https live, went
        // down with it.
        QTRY_VERIFY_WITH_TIMEOUT(!processAlive(stub), 5000);

        QVERIFY(QFile::remove(pidFile));
        log.clear();
        svc.fetch();
        QVERIFY2(log.waitFor({cancelled}), qPrintable(log.join(QStringLiteral(", "))));
        QTest::qWait(500);
        QVERIFY(!QFileInfo::exists(pidFile));
#endif
    }

    // A fetch given its own cancel flag (MainWindow's auto-fetch, which
    // is dropped when its repository leaves the screen): setting the
    // flag ends that fetch promptly, as cancelRemoteOps() would, without
    // cancelling later ops; and cancelRemoteOps() still stops a fetch
    // that has a flag of its own.
    void fetchStopsOnItsOwnCancelFlag() {
#ifndef GITBOLT_TEST_STALL_TRANSPORT
        QSKIP("StallTransport not built");
#else
        using gitbolt::test::processAlive;
        using gitbolt::test::readStallPids;
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        QVERIFY(succeeded(work.run({"remote", "add", "origin",
                                    gitbolt::test::StallTransportEnv::url()})));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString pidFile = dir.filePath(QStringLiteral("stall.pid"));
        gitbolt::test::StallTransportEnv env(pidFile);

        GitService svc;
        SignalLog log(svc);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        log.clear();
        const QString cancelled = QStringLiteral("operationFailed(fetch: Cancelled)");

        // Starts a fetch with its own flag on a pool thread and waits
        // until the transport runs: git is connected and stuck.
        const auto stalledFetch = [&](const std::shared_ptr<std::atomic<bool>>& flag) {
            QFile::remove(pidFile);
            QFuture<void> fetch = QtConcurrent::run([&svc, flag] {
                svc.fetch(QString(), flag);
            });
            if (!QTest::qWaitFor([&] { return readStallPids(pidFile).stub > 0; }, 15000))
                qWarning("the fetch never reached StallTransport");
            return fetch;
        };

        auto own = std::make_shared<std::atomic<bool>>(false);
        QFuture<void> fetch = stalledFetch(own);
        // Joined on every exit path: the job references svc.
        const auto join = qScopeGuard([&] {
            svc.cancelRemoteOps();
            fetch.waitForFinished();
        });
        qint64 stub = readStallPids(pidFile).stub;
        QVERIFY(stub > 0);
        QVERIFY(!fetch.isFinished());

        QElapsedTimer timer;
        timer.start();
        own->store(true);
        QVERIFY(QTest::qWaitFor([&] { return fetch.isFinished(); }, 10000));
        QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
        QVERIFY2(log.waitFor({cancelled}), qPrintable(log.join(QStringLiteral(", "))));
        QTRY_VERIFY_WITH_TIMEOUT(!processAlive(stub), 5000);

        // Only that fetch: the next one runs, and cancelRemoteOps()
        // stops it though it has an unset flag of its own.
        log.clear();
        fetch = stalledFetch(std::make_shared<std::atomic<bool>>(false));
        stub = readStallPids(pidFile).stub;
        QVERIFY2(stub > 0, qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY(!fetch.isFinished());

        timer.restart();
        svc.cancelRemoteOps();
        QVERIFY(QTest::qWaitFor([&] { return fetch.isFinished(); }, 10000));
        QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
        QVERIFY2(log.waitFor({cancelled}), qPrintable(log.join(QStringLiteral(", "))));
        QTRY_VERIFY_WITH_TIMEOUT(!processAlive(stub), 5000);
#endif
    }

    // That flag is optional, and GitService passes it on as given, so
    // GitProcess ignores a null one: run() stays the plain one (it
    // used to crash reading the null flag), and a real flag added
    // next to it still stops it.
    void nullCancelFlagIsIgnored() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess git(repo->path().toStdString());
        git.addCancelFlag(nullptr);
        QVERIFY(succeeded(git.run({"rev-parse", "HEAD"})));

        git.addCancelFlag(std::make_shared<std::atomic<bool>>(true));
        const auto stopped = git.run({"rev-parse", "HEAD"});
        QVERIFY(!stopped.ok());
        QCOMPARE(stopped.error().code(), gitbolt::git::GitErrorCode::User);
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

    // -----------------------------------------------------------------
    // checkoutBranch is what the sidebar, the toolbar's branch combo
    // and Commands > Checkout branch call, with a bare branch name. A
    // tag of the same name used to supply the files while HEAD went to
    // the branch, so the switch showed up as uncommitted changes.
    // -----------------------------------------------------------------
    void checkoutBranchSharingATagName() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        auto atTag = repo->repo().head();
        QVERIFY(atTag.ok());
        QVERIFY(repo->repo().createTag("v1", atTag.value(), "release").ok());
        auto onBranch = repo->writeAndCommit(QStringLiteral("f0.txt"),
                                             QByteArrayLiteral("on branch\n"),
                                             QStringLiteral("branch tip"));
        QVERIFY(onBranch.ok());
        QVERIFY(repo->repo().createBranch("v1", onBranch.value()).ok());
        QVERIFY(repo->writeAndCommit(QStringLiteral("f0.txt"),
                                     QByteArrayLiteral("on main\n"),
                                     QStringLiteral("main tip")).ok());

        GitService svc;
        QSignalSpy failedSpy(&svc, &GitService::operationFailed);
        QVERIFY(svc.openRepository(repo->path()));

        svc.checkoutBranch(QStringLiteral("v1"));
        QCOMPARE(failedSpy.count(), 0);
        auto branch = repo->repo().headBranchName();
        QVERIFY(branch.ok());
        QCOMPARE(branch.value(), std::string("v1"));
        QCOMPARE(tip(repo->repo(), "HEAD"), onBranch->toHex());
        auto status = repo->repo().status();
        QVERIFY(status.ok());
        QCOMPARE(status->size(), size_t(0));
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

    // The branch checked out, as a RebasePlan records it ("" when HEAD
    // is detached).
    static std::string branchOf(const gitbolt::git::GitProcess& work) {
        return output(work.run({"branch", "--show-current"})).toStdString();
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
