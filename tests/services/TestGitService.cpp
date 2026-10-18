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
//   - commitChanges round trip, through `git commit`: the repository's
//     hooks run (core.hooksPath too) and can refuse it, a slow hook is
//     waited for and a quit stops it, a merge's autostash comes back,
//     an amend during a revert is refused, an amend keeps the commit's
//     author, a commit of nothing staged is refused, and the message
//     goes in as the UTF-8 it is (no i18n.commitEncoding label), a
//     reword's too,
//   - Unstage All with the index locked is reported, and it leaves a
//     merge or a stopped cherry-pick in progress (and works on an
//     unborn branch),
//   - stashSave's keep-index flag (wired in the same change set
//     that added these tests),
//   - the cherry-pick close race: a queued cherry-pick must neither
//     crash nor emit once the repository it was queued for is gone
//     (regression test for the raw-pointer capture UAF), and a pick
//     on top of another is refused with a failure alone, and one whose
//     change is on the branch already says so and leaves nothing in
//     progress,
//   - a cherry-pick of several commits paused between two: its state
//     goes out as such, and Continue / Skip carry it on,
//   - tag create / delete as the Commands menu drives them,
//   - branch checkout when a tag shares the branch's name, and a new
//     branch starts at the branch it is made from (HEAD by default),
//   - the askpass git's children get, which an auto-fetch's get in a
//     mode that never asks, and the kinds of question it tells apart,
//   - Fetch prunes, unless fetch.prune or remote.<name>.prune says not,
//     and not where a prune could reach past refs/remotes/, a
//     submodule's config included (git hands the prune on to it),
//   - cancelGitOps(), which quitting during a fetch relies on, and
//     a fetch's own cancel flag, which drops an unwanted auto-fetch
//     (and which GitProcess ignores when it is null),
//   - the rebase steps: the dialog's plan runs as listed, with its
//     rewords, squashes, fixups and edits, and messages kept whole; a
//     reword a hook turned down is tried again on Continue (a
//     prepare-commit-msg hook's additions notwithstanding), and one
//     stopped on a conflict keeps its new message for a terminal's git;
//     Skip at an `edit` stop with changes made there is refused (after
//     a rebase onto a moved base, an amend or a split too), and so is
//     a cherry-pick sequence's Skip over staged work with nothing
//     picked; the reword retry knows a pick onto a moved base, leaves
//     the user's own amend alone (with no reflog, and when it keeps the
//     old message and adds to it, too), and stands aside for a pick that
//     came out empty; a sequence's stop that a commit concluded counts
//     as picked, whatever the commit's message;
//     git's failures (a conflict, nothing in progress) are reported,
//     and so is a rebase that is still in progress after a step, with
//     its state ahead of the outcome; a plan made for another HEAD (a
//     commit since, another branch) is refused; a step outlasts 30 s;
//     a step whose repository was swapped out reports nothing, one
//     whose repository was opened again does; and nothing else goes
//     to git while a step runs,
//   - quitting (cancelGitOps()) stops a running rebase step, as Ctrl-C
//     would, and the rebase stays in progress,
//   - no refresh result of a repository reaches the GUI after another
//     one has been opened, nor a rebase step's, a cherry-pick's, a
//     commit's or git maintenance's outcome, which also waits for an
//     open on its way (and reaches its own repository, opened again),
//   - a commit waiting on its hook leaves the thread pool room for an
//     open (a job waiting on git lends its thread back),
//   - the remote ops and checkoutBranch return their own outcome,
//   - a conflicted merge is aborted off the GUI thread,
//   - the resolver's results are staged and then written from the
//     index: a symbolic link's conflict to a side's link alone, never
//     through the link, and a text conflict as it always was,
//   - a commit in a submodule and an edit of .git/info/exclude
//     refresh, and the watcher's walk goes into the directories it
//     leaves out (.git, node_modules, build, …).
//
// Signal delivery: workers emit from pool threads, and QSignalSpy
// records those on the emitting thread, so the tests wait for worker
// signals through MainThreadSpy below. QSignalSpy remains for signals
// emitted on the main thread, and for counting after the pool drained.
//

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>

#include "../TestProcessHelper.h"
#include "../TestRepoHelper.h"
#include "git/Askpass.h"
#include "git/Error.h"
#include "git/GitProcess.h"
#include "services/GitService.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <thread>
#include <type_traits>

#ifndef Q_OS_WIN
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

// git's own configuration, for the tests that commit through git: the
// repository's alone. A developer's or CI machine's global
// commit.gpgSign or core.hooksPath would otherwise sign the commits, or
// run other hooks. (TestRepo isolates libgit2's configuration only.)
class IsolatedGitConfig {
public:
    IsolatedGitConfig()
        : global_("GIT_CONFIG_GLOBAL", QFile::encodeName(emptyFile())),
          noSystem_("GIT_CONFIG_NOSYSTEM", "1") {}

private:
    static QString emptyFile() {
        static QTemporaryDir dir;   // lives for the process
        const QString path = dir.filePath(QStringLiteral("gitconfig"));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            qFatal("cannot create %s", qPrintable(path));
        return path;
    }

    gitbolt::test::ScopedEnv global_;
    gitbolt::test::ScopedEnv noSystem_;
};

// A hook script `name` in `dir`, run by sh as git runs every hook (Git
// for Windows too, which needs no executable bit).
bool writeHook(const QString& dir, const char* name, const QByteArray& body) {
    QDir().mkpath(dir);
    QFile hook(QDir(dir).filePath(QLatin1String(name)));
    if (!hook.open(QIODevice::WriteOnly))
        return false;
    hook.write("#!/bin/sh\n" + body);
    hook.close();
    return hook.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

// A file's bytes, or "<missing>".
QByteArray fileBytes(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArrayLiteral("<missing>");
}

#ifndef Q_OS_WIN
// Makes the work tree's `path` a symbolic link to `target` and stages
// it, as a link.
bool stageLink(gitbolt::test::TestRepo& repo, const QString& path, const QString& target) {
    const QString link = QDir(repo.path()).filePath(path);
    QFile::remove(link);
    return QFile::link(target, link) && repo.stageFile(path).ok();
}

// Where the symbolic link `path` points, as the link has it (QFileInfo
// makes it absolute), or "" when `path` is no link.
QString linkTarget(const QString& path) {
    char target[4096];
    const ssize_t size = ::readlink(QFile::encodeName(path).constData(), target, sizeof target);
    return size < 0 ? QString() : QFile::decodeName(QByteArray(target, size));
}
#endif

} // namespace

// cherryPick() takes one commit. It took a list, but libgit2's pick
// only stages: the second met the first one's uncommitted changes,
// failed, and took the first pick's state files down with it.
static_assert(std::is_same_v<decltype(&GitService::cherryPick),
                             void (GitService::*)(const gitbolt::git::ObjectId&)>);

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

    // Except where nobody is there to answer: the auto-fetch popped a
    // "Username for …" dialog at every tick. A GitProcess with prompts
    // forbidden gives git an empty GIT_ASKPASS, which outranks
    // core.askPass and SSH_ASKPASS, so git fails at once with "terminal
    // prompts disabled"; ssh gets GitBolt's askpass in refuse mode,
    // which says no without a window; and Git Credential Manager is
    // told not to open its own. ${GIT_ASKPASS-unset} tells an empty
    // value, which survived the trip, from a missing one. Through the
    // cancellable run() too, which is the one the auto-fetch takes.
    void gitChildrenOfAPromptlessProcessCannotAsk() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        // Whatever GitBolt was started with goes through where prompts
        // are allowed. Set here, not inherited: GitHub's Windows
        // runners export GCM_INTERACTIVE=Never to every step.
        const gitbolt::test::ScopedEnv gcm("GCM_INTERACTIVE", "auto");
        const std::vector<std::string> echoEnv = {
            "-c", "alias.echo-env=!printf '%s|%s|%s|%s' \"${GIT_ASKPASS-unset}\" "
                  "\"$SSH_ASKPASS_REQUIRE\" \"$GITBOLT_ASKPASS_MODE\" \"$GCM_INTERACTIVE\"",
            "echo-env"};
        const QString asking = QCoreApplication::applicationFilePath()
                               + QStringLiteral("|force|1|auto");
        const QString promptless = QStringLiteral("|force|refuse|never");

        gitbolt::git::GitProcess proc(repo->path().toStdString());
        QCOMPARE(output(proc.run(echoEnv)), asking);
        proc.forbidPrompts();
        QCOMPARE(output(proc.run(echoEnv)), promptless);
        proc.addCancelFlag(std::make_shared<std::atomic<bool>>(false));
        QCOMPARE(output(proc.run(echoEnv)), promptless);

        gitbolt::git::GitProcess cancellable(repo->path().toStdString());
        cancellable.addCancelFlag(std::make_shared<std::atomic<bool>>(false));
        QCOMPARE(output(cancellable.run(echoEnv)), asking);
    }

    // The same through GitService, down to ssh: the auto-fetch's
    // fetch(…, allowPrompts=false) and the toolbar Fetch's fetch().
    // core.sshCommand stands in for ssh, and writes down what it got.
    void autoFetchNeverAsks() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        // As above: the inherited value goes through where prompts are
        // allowed, so the test sets it rather than reading the runner's.
        const gitbolt::test::ScopedEnv gcm("GCM_INTERACTIVE", "auto");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString envFile = dir.filePath(QStringLiteral("env.txt"));
        const QString sshCommand = QStringLiteral(
            "printf '%s|%s|%s|%s' \"${GIT_ASKPASS-unset}\" \"$SSH_ASKPASS_REQUIRE\" "
            "\"$GITBOLT_ASKPASS_MODE\" \"$GCM_INTERACTIVE\" > '%1'; false").arg(envFile);
        auto config = repo->repo().config();
        QVERIFY(repo->repo().addRemote("origin", "ssh://gitbolt.invalid/r.git").ok());
        QVERIFY(config.setString("core.sshCommand", sshCommand.toStdString()).ok());
        // No `<command> -G` first to find out which ssh it is.
        QVERIFY(config.setString("ssh.variant", "simple").ok());
        const auto sshGot = [&envFile] {
            QFile file(envFile);
            return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                                  : QStringLiteral("(no %1)").arg(envFile);
        };

        GitService svc;
        QSignalSpy failedSpy(&svc, &GitService::operationFailed);
        QVERIFY(svc.openRepository(repo->path()));

        svc.fetch(QString(), nullptr, /*allowPrompts=*/false);
        QCOMPARE(failedSpy.count(), 1);
        QCOMPARE(failedSpy.at(0).at(0).toString(), QStringLiteral("fetch"));
        QCOMPARE(sshGot(), QStringLiteral("|force|refuse|never"));

        QVERIFY(QFile::remove(envFile));
        svc.fetch();
        QCOMPARE(failedSpy.count(), 2);
        QCOMPARE(sshGot(), QCoreApplication::applicationFilePath()
                           + QStringLiteral("|force|1|auto"));
    }

    // What the askpass shows for a question: a field that shows what is
    // typed for a user name, a masked one for a secret, and Yes / No
    // for ssh's yes-or-no questions. A new host key's was a masked
    // field, so the user had to type "yes" blind. ssh gives no hint
    // for that one (SSH_ASKPASS_PROMPT is unset), so its text decides.
    void askpassKindTellsTheQuestionsApart_data() {
        QTest::addColumn<QString>("prompt");
        QTest::addColumn<QByteArray>("sshHint");
        QTest::addColumn<int>("kind");
        using gitbolt::git::AskpassKind;
        // OpenSSH 9.9, verbatim.
        QTest::newRow("host key")
            << QStringLiteral(
                   "The authenticity of host 'fakehost (<no hostip for proxy command>)' "
                   "can't be established.\n"
                   "ED25519 key fingerprint is SHA256:i+zYdBS33CCsmOP/uQ3ovFF4uD74kMFd9bDRbPhXuQ0.\n"
                   "This key is not known by any other names.\n"
                   "Are you sure you want to continue connecting (yes/no/[fingerprint])? ")
            << QByteArray() << int(AskpassKind::YesNo);
        // OpenSSH before 8.0.
        QTest::newRow("host key, older ssh")
            << QStringLiteral(
                   "The authenticity of host 'example.com (192.0.2.1)' can't be established.\n"
                   "RSA key fingerprint is SHA256:nThbg6kXUpJWGl7E1IGOCspRomTxdCARLviKw6E5SY8.\n"
                   "Are you sure you want to continue connecting (yes/no)? ")
            << QByteArray() << int(AskpassKind::YesNo);
        QTest::newRow("ssh confirm")
            << QStringLiteral("Allow shared connection to example.com? ")
            << QByteArray("confirm") << int(AskpassKind::YesNo);
        QTest::newRow("user name")
            << QStringLiteral("Username for 'https://github.com': ")
            << QByteArray() << int(AskpassKind::Text);
        QTest::newRow("password")
            << QStringLiteral("Password for 'https://u@github.com': ")
            << QByteArray() << int(AskpassKind::Secret);
        QTest::newRow("passphrase")
            << QStringLiteral("Enter passphrase for key '/x/id_ed25519': ")
            << QByteArray() << int(AskpassKind::Secret);
    }

    void askpassKindTellsTheQuestionsApart() {
        QFETCH(QString, prompt);
        QFETCH(QByteArray, sshHint);
        QFETCH(int, kind);
        QCOMPARE(int(gitbolt::git::askpassKind(prompt, sshHint)), kind);
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

    // A commit in a submodule, whether its git dir is under .git/modules
    // or is the submodule's own .git directory, moves nothing but its
    // HEAD or a ref there, and an edit of .git/info/exclude changes what
    // status shows: each refreshes. A watcher that stopped going into
    // git dirs left the status as it was until something else changed.
    void submoduleCommitAndExcludeEditRefresh_data() {
        QTest::addColumn<QString>("change");
        QTest::newRow("a commit in a submodule (.git/modules)") << QStringLiteral("absorbed");
        QTest::newRow("a commit in a submodule with its own .git")
            << QStringLiteral("own .git");
        QTest::newRow("an edit of .git/info/exclude") << QStringLiteral("exclude");
    }

    void submoduleCommitAndExcludeEditRefresh() {
        QFETCH(QString, change);
        const IsolatedGitConfig isolated;
        const gitbolt::test::ScopedEnv authorName("GIT_AUTHOR_NAME", "T");
        const gitbolt::test::ScopedEnv authorEmail("GIT_AUTHOR_EMAIL", "t@example.com");
        const gitbolt::test::ScopedEnv committerName("GIT_COMMITTER_NAME", "T");
        const gitbolt::test::ScopedEnv committerEmail("GIT_COMMITTER_EMAIL", "t@example.com");
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        const QString subPath = repo->path() + QStringLiteral("/sub");
        gitbolt::git::GitProcess super(repo->path().toStdString());
        gitbolt::git::GitProcess sub(subPath.toStdString());
        QVERIFY(succeeded(super.run({"init", "-q", "sub"})));
        QVERIFY(succeeded(sub.run({"commit", "-q", "--allow-empty", "-m", "s1"})));
        // An existing repository added where it is keeps its .git
        // directory; absorbgitdirs moves it under .git/modules.
        QVERIFY(succeeded(super.run({"submodule", "add", "-q", "./sub", "sub"})));
        QVERIFY(succeeded(super.run({"commit", "-q", "-m", "add sub"})));
        if (change == QLatin1String("absorbed")) {
            QVERIFY(succeeded(super.run({"submodule", "-q", "absorbgitdirs"})));
            QVERIFY(QFileInfo(repo->path() + QStringLiteral("/.git/modules/sub/HEAD")).isFile());
        } else {
            QVERIFY(QFileInfo(subPath + QStringLiteral("/.git/HEAD")).isFile());
        }

        gitbolt::watcher::FileWatcher watcher;
        QSignalSpy changed(&watcher, &gitbolt::watcher::FileWatcher::repositoryChanged);
        watcher.watchRepository(repo->path());
        // Whatever the setup itself still has on its way.
        QTest::qWait(1000);
        changed.clear();

        if (change == QLatin1String("exclude")) {
            // Written as git and most editors write a file: a new one
            // renamed over it.
            QSaveFile exclude(repo->path() + QStringLiteral("/.git/info/exclude"));
            QVERIFY(exclude.open(QIODevice::WriteOnly));
            exclude.write("*.log\n");
            QVERIFY(exclude.commit());
        } else {
            QVERIFY(succeeded(sub.run({"commit", "-q", "--allow-empty", "-m", "s2"})));
        }
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 10000);
    }

    // A linked worktree, or a submodule opened on its own, has a .git
    // file pointing at its git dir, and keeps its refs in a common dir
    // elsewhere: the watcher looked for <repo>/.git as a directory and
    // watched none of it, so a commit made outside GitBolt there never
    // refreshed the window.
    void worktreeAndSubmoduleCommitsRefresh_data() {
        QTest::addColumn<QString>("kind");
        QTest::newRow("a linked worktree") << QStringLiteral("worktree");
        QTest::newRow("a submodule opened on its own") << QStringLiteral("submodule");
    }

    void worktreeAndSubmoduleCommitsRefresh() {
        QFETCH(QString, kind);
        const IsolatedGitConfig isolated;
        const gitbolt::test::ScopedEnv authorName("GIT_AUTHOR_NAME", "T");
        const gitbolt::test::ScopedEnv authorEmail("GIT_AUTHOR_EMAIL", "t@example.com");
        const gitbolt::test::ScopedEnv committerName("GIT_COMMITTER_NAME", "T");
        const gitbolt::test::ScopedEnv committerEmail("GIT_COMMITTER_EMAIL", "t@example.com");
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess main(repo->path().toStdString());
        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        QString opened;
        if (kind == QLatin1String("worktree")) {
            opened = outside.filePath(QStringLiteral("wt"));
            QVERIFY(succeeded(main.run({"worktree", "add", "-q", "-b", "topic",
                                        opened.toStdString()})));
        } else {
            opened = repo->path() + QStringLiteral("/sub");
            gitbolt::git::GitProcess sub(opened.toStdString());
            QVERIFY(succeeded(main.run({"init", "-q", "sub"})));
            QVERIFY(succeeded(sub.run({"commit", "-q", "--allow-empty", "-m", "s1"})));
            QVERIFY(succeeded(main.run({"submodule", "add", "-q", "./sub", "sub"})));
            QVERIFY(succeeded(main.run({"submodule", "-q", "absorbgitdirs"})));
        }
        QVERIFY2(QFileInfo(opened + QStringLiteral("/.git")).isFile(), qPrintable(opened));

        GitService svc;
        QVERIFY(svc.openRepository(opened));
        QSignalSpy changed(&svc, &GitService::repositoryChanged);
        QTest::qWait(1000);
        changed.clear();

        gitbolt::git::GitProcess there(opened.toStdString());
        QVERIFY(succeeded(there.run({"commit", "-q", "--allow-empty", "-m", "outside"})));
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 10000);
    }

    // The walk leaves out .git, node_modules, build and the like
    // themselves, but still goes into them: what refreshes the status
    // above, and a tracked file in a subdirectory of build/, is watched
    // there. One directly in build/ is not: build/ itself has no watch.
    void enumerateWatchDirsGoesIntoLeftOutDirs() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        const QDir root(repo->path());
        for (const char* dir : {"src", ".git/info", "node_modules/pkg/lib", "build/obj"})
            QVERIFY(root.mkpath(QString::fromLatin1(dir)));
        QStringList dirs;
        for (const QString& dir :
             gitbolt::watcher::FileWatcher::enumerateWatchDirs(repo->path()))
            dirs << root.relativeFilePath(dir);
        for (const char* in : {"src", ".git/info", "node_modules/pkg", "node_modules/pkg/lib",
                               "build/obj"})
            QVERIFY2(dirs.contains(QString::fromLatin1(in)), in);
        for (const char* out : {".git", "node_modules", "build"})
            QVERIFY2(!dirs.contains(QString::fromLatin1(out)), out);
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
    // A folder inside a repository opens that repository, and the
    // signal names the repository's top (what the title, Recent and
    // the watchers want), not the folder asked for.
    // -----------------------------------------------------------------
    void asyncOpenOfASubfolderEmitsTheRepository() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        QVERIFY(QDir(repo->path()).mkpath(QStringLiteral("src/app")));

        GitService svc;
        QSignalSpy openedSpy(&svc, &GitService::repositoryOpened);
        QSignalSpy failedSpy(&svc, &GitService::repositoryOpenFailed);

        svc.openRepositoryAsync(QDir(repo->path()).filePath(QStringLiteral("src/app")));
        QVERIFY(openedSpy.wait(5000));
        QCOMPARE(openedSpy.count(), 1);
        // Spelled as the folder was: Recent keeps one entry for it.
        QCOMPARE(openedSpy.first().at(0).toString(), QDir::cleanPath(repo->path()));
        QCOMPARE(failedSpy.count(), 0);
        QVERIFY(svc.isOpen());
    }

    void syncOpenOfASubfolderEmitsTheRepository() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        QVERIFY(QDir(repo->path()).mkpath(QStringLiteral("docs")));

        GitService svc;
        QSignalSpy openedSpy(&svc, &GitService::repositoryOpened);
        QVERIFY(svc.openRepository(QDir(repo->path()).filePath(QStringLiteral("docs"))));
        QCOMPARE(openedSpy.count(), 1);
        QCOMPARE(QFileInfo(openedSpy.first().at(0).toString()).canonicalFilePath(),
                 QFileInfo(repo->path()).canonicalFilePath());
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
        // The code, for usage statistics' repo_open_failed, which sends
        // its name and never the message.
        QCOMPARE(failedSpy.first().at(2).toInt(),
                 int(gitbolt::git::GitErrorCode::NotFound));
        QCOMPARE(openedSpy.count(), 0);
        QVERIFY(!svc.isOpen());
    }

    // -----------------------------------------------------------------
    // initialLogLoaded: the first history page of an async open, tagged
    // with the generation openRepositoryAsync returned and the page's
    // size, right behind that page's logReady. A plain refresh, and an
    // open superseded by a newer one, emit none — so a page of the
    // previous repository is never taken for the new one's (usage
    // statistics time an open to it).
    // -----------------------------------------------------------------
    void asyncOpenTagsItsFirstLogPage() {
        auto first = repoWithCommits(2);
        auto second = repoWithCommits(3);
        QVERIFY(first && second);

        GitService svc;
        MainThreadSpy initialSpy(&svc, &GitService::initialLogLoaded);
        QStringList order;
        QObject context;
        QObject::connect(&svc, &GitService::logReady, &context,
                         [&order](const std::vector<gitbolt::git::CommitData>&, int offset) {
            order << QStringLiteral("log(%1)").arg(offset);
        });
        QObject::connect(&svc, &GitService::initialLogLoaded, &context,
                         [&order](quint64, int) { order << QStringLiteral("initial"); });

        // Superseded at once: only the second open's page is tagged.
        const quint64 superseded = svc.openRepositoryAsync(first->path());
        const quint64 gen = svc.openRepositoryAsync(second->path());
        QVERIFY(gen > superseded);
        QCOMPARE(svc.openGeneration(), gen);
        QVERIFY(initialSpy.waitForAny());
        QCOMPARE(initialSpy.size(), 1);
        QCOMPARE(initialSpy.first().at(0).value<quint64>(), gen);
        QCOMPARE(initialSpy.first().at(1).toInt(), 3);
        QCOMPARE(order.mid(order.indexOf(QStringLiteral("initial")) - 1, 2),
                 (QStringList{QStringLiteral("log(0)"), QStringLiteral("initial")}));

        // A refresh of the open repository is no open's first page.
        const qsizetype logs = order.count(QStringLiteral("log(0)"));
        svc.refreshLog();
        QVERIFY(QTest::qWaitFor([&] { return order.count(QStringLiteral("log(0)")) > logs; },
                                5000));
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
        QCOMPARE(initialSpy.size(), 1);
    }

    // -----------------------------------------------------------------
    // A refresh of repository A whose worker was done just before B was
    // opened: its result was checked on the worker, while A was still
    // open, and reached the GUI after B's repositoryOpened — A's status
    // and history under B's name (and A's first page let B's loading
    // overlay go). The check is now made where the swap happens, so
    // nothing of A's arrives once B is open.
    // -----------------------------------------------------------------
    void resultOfAReplacedRepositoryIsNotDelivered() {
        auto a = repoWithCommits(2);
        auto b = repoWithCommits(1);
        QVERIFY(a && b);

        GitService svc;
        SignalLog log(svc);
        // What arrives on this thread, in order. A's results are told
        // apart by what only A has: an untracked a-only.txt, and its
        // second commit.
        QStringList events;
        QObject context;
        connect(&svc, &GitService::repositoryOpened, &context, [&](const QString& path) {
            events << (path == b->path() ? QStringLiteral("opened(b)") : QStringLiteral("opened"));
        });
        connect(&svc, &GitService::statusReady, &context,
                [&](const std::vector<gitbolt::git::StatusEntry>& entries) {
            const bool ofA = std::any_of(entries.begin(), entries.end(),
                [](const auto& e) { return e.path == "a-only.txt"; });
            events << (ofA ? QStringLiteral("status(a)") : QStringLiteral("status"));
        });
        connect(&svc, &GitService::logReady, &context,
                [&](const std::vector<gitbolt::git::CommitData>& commits, int) {
            const bool ofA = std::any_of(commits.begin(), commits.end(),
                [](const auto& c) { return c.summary == "commit 1"; });
            events << (ofA ? QStringLiteral("log(a)") : QStringLiteral("log"));
        });
        QVERIFY(svc.openRepository(a->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));

        a->writeFile(QStringLiteral("a-only.txt"), QByteArrayLiteral("a\n"));
        svc.refreshStatus();
        svc.refreshLog();
        // The workers are done, their results on their way; none has
        // arrived, as the event loop hasn't run. Then B is opened.
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QVERIFY(svc.openRepository(b->path()));
        const qsizetype opened = events.indexOf(QStringLiteral("opened(b)"));
        QVERIFY(opened >= 0);
        // B's own refreshes, then anything still on its way.
        QVERIFY(QTest::qWaitFor([&] {
            return events.indexOf(QStringLiteral("status"), opened) > opened
                && events.indexOf(QStringLiteral("log"), opened) > opened;
        }, 10000));
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QTest::qWait(100);
        const QStringList afterB = events.mid(opened);
        QVERIFY2(!afterB.contains(QStringLiteral("status(a)"))
                     && !afterB.contains(QStringLiteral("log(a)")),
                 qPrintable(events.join(QStringLiteral(", "))));
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
    // refreshLog(offset, count): a later page continues where the
    // previous one stopped instead of repeating the newest commits,
    // and a page past the end arrives empty, which ends paging.
    // -----------------------------------------------------------------
    void refreshLogPagesFromOffset() {
        auto repo = repoWithCommits(5);
        QVERIFY(repo);

        GitService svc;
        QVERIFY(svc.openRepository(repo->path()));

        // openRepository's own offset-0 refresh can land in the spy
        // too, so wait for the page with the offset asked for.
        MainThreadSpy logSpy(&svc, &GitService::logReady);
        const auto pageAt = [&logSpy](int offset)
            -> std::optional<std::vector<gitbolt::git::CommitData>> {
            for (const auto& args : logSpy)
                if (args.at(1).toInt() == offset)
                    return args.at(0).value<std::vector<gitbolt::git::CommitData>>();
            return std::nullopt;
        };

        svc.refreshLog(2, 2);
        QVERIFY(QTest::qWaitFor([&] { return pageAt(2).has_value(); }, 5000));
        const auto page = *pageAt(2);
        QCOMPARE(page.size(), size_t(2));
        QCOMPARE(page.front().summary, std::string("commit 2"));
        QCOMPARE(page.back().summary, std::string("commit 1"));

        svc.refreshLog(5, 2);
        QVERIFY(QTest::qWaitFor([&] { return pageAt(5).has_value(); }, 5000));
        QVERIFY(pageAt(5)->empty());
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

        // git makes the commit, on a worker.
        MainThreadSpy doneSpy(&svc, &GitService::commitComplete);
        svc.commitChanges(QStringLiteral("service-made commit"));
        QVERIFY(doneSpy.waitForAny(15000));
        QVERIFY2(doneSpy.last().at(0).toBool(),
                 qPrintable(doneSpy.last().at(1).toString()));
        QVERIFY(doneSpy.last().at(1).toString().startsWith(QStringLiteral("Commit created: ")));

        // Assert against the repository, not the service: the commit
        // must exist on disk with the right summary.
        auto head = repo->repo().head();
        QVERIFY(head.ok());
        auto commit = repo->repo().lookupCommit(head.value());
        QVERIFY(commit.ok());
        QCOMPARE(commit.value().summary, std::string("service-made commit"));
    }

    // -----------------------------------------------------------------
    // Commits go through `git commit`, so the repository's hooks run.
    // libgit2 made every commit and runs none: a pre-commit that guards
    // against secrets, a commit-msg that adds Gerrit's Change-Id, a
    // post-commit were all skipped without a word. A hook's refusal
    // comes back with what it said, and nothing is committed.
    // -----------------------------------------------------------------
    void commitRunsTheRepositorysHooks_data() {
        QTest::addColumn<bool>("hooksPath");
        QTest::newRow(".git/hooks") << false;
        QTest::newRow("core.hooksPath") << true;
    }

    void commitRunsTheRepositorysHooks() {
        QFETCH(bool, hooksPath);
        const IsolatedGitConfig config;
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess git(repo->path().toStdString());
        QTemporaryDir elsewhere;
        QVERIFY(elsewhere.isValid());
        QString hooks = repo->path() + QStringLiteral("/.git/hooks");
        if (hooksPath) {
            hooks = elsewhere.path();
            QVERIFY(succeeded(git.run({"config", "core.hooksPath", hooks.toStdString()})));
        }
        // Hooks run at the top of the work tree.
        QVERIFY(writeHook(hooks, "pre-commit",
                          "if [ -f .git/BLOCK ]; then echo 'pre-commit: no' >&2; exit 1; fi\n"));
        QVERIFY(writeHook(hooks, "commit-msg", "echo 'Change-Id: I123' >> \"$1\"\n"));
        QVERIFY(writeHook(hooks, "post-commit", ": > .git/post-commit-ran\n"));
        repo->writeFile(QStringLiteral(".git/BLOCK"), QByteArray());
        repo->writeFile(QStringLiteral("staged.txt"), QByteArrayLiteral("to be committed\n"));
        QVERIFY(repo->stageFile(QStringLiteral("staged.txt")).ok());
        const QString head = output(git.run({"rev-parse", "HEAD"}));

        GitService svc;
        QVERIFY(svc.openRepository(repo->path()));
        MainThreadSpy done(&svc, &GitService::commitComplete);
        svc.commitChanges(QStringLiteral("hooked"));
        QVERIFY(done.waitForAny(15000));
        QCOMPARE(done.size(), 1);
        QVERIFY(!done.at(0).at(0).toBool());
        QVERIFY2(done.at(0).at(1).toString().contains(QStringLiteral("pre-commit: no")),
                 qPrintable(done.at(0).at(1).toString()));
        QCOMPARE(output(git.run({"rev-parse", "HEAD"})), head);
        QVERIFY(!QFileInfo::exists(repo->path() + QStringLiteral("/.git/post-commit-ran")));

        QVERIFY(QFile::remove(repo->path() + QStringLiteral("/.git/BLOCK")));
        done.clear();
        svc.commitChanges(QStringLiteral("hooked"));
        QVERIFY(done.waitForAny(15000));
        QVERIFY2(done.at(0).at(0).toBool(), qPrintable(done.at(0).at(1).toString()));
        QCOMPARE(output(git.run({"rev-parse", "HEAD~1"})), head);
        QCOMPARE(output(git.run({"log", "-1", "--format=%B"})),
                 QStringLiteral("hooked\nChange-Id: I123"));
        QVERIFY(QFileInfo::exists(repo->path() + QStringLiteral("/.git/post-commit-ran")));
    }

    // A pre-commit hook may take a while (it may run the tests): the
    // commit waits for it past run()'s default limit, here 1 s for a
    // hook of 3. Quitting doesn't: GitService's quit cancel stops git,
    // the hook with it, at once, and the commit fails "Cancelled".
    void commitWaitsForASlowHookButNotForAQuit() {
        const IsolatedGitConfig config;
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess git(repo->path().toStdString());
        const QString hooks = repo->path() + QStringLiteral("/.git/hooks");
        // Slow while .git/SLOW exists; stuck, its pid written down,
        // while .git/STUCK does.
        QVERIFY(writeHook(hooks, "pre-commit",
                          "if [ -f .git/SLOW ]; then sleep 3; fi\n"
                          "if [ -f .git/STUCK ]; then"
                          // Git for Windows' sh: the Windows pid.
                          " { cat /proc/$$/winpid 2>/dev/null || echo $$; } > .git/hook.pid;"
                          " while :; do sleep 0.05; done; fi\n"));
        repo->writeFile(QStringLiteral(".git/SLOW"), QByteArray());
        repo->writeFile(QStringLiteral("a.txt"), QByteArrayLiteral("a\n"));
        QVERIFY(repo->stageFile(QStringLiteral("a.txt")).ok());

        GitService svc;
        QVERIFY(svc.openRepository(repo->path()));
        MainThreadSpy done(&svc, &GitService::commitComplete);
        {
            const gitbolt::test::ScopedEnv shortDefault("GITBOLT_GIT_TIMEOUT_MS", "1000");
            svc.commitChanges(QStringLiteral("slow"));
            QVERIFY(done.waitForAny(20000));
        }
        QVERIFY2(done.at(0).at(0).toBool(), qPrintable(done.at(0).at(1).toString()));
        QCOMPARE(output(git.run({"log", "-1", "--format=%s"})), QStringLiteral("slow"));

        QVERIFY(QFile::remove(repo->path() + QStringLiteral("/.git/SLOW")));
        repo->writeFile(QStringLiteral(".git/STUCK"), QByteArray());
        repo->writeFile(QStringLiteral("b.txt"), QByteArrayLiteral("b\n"));
        QVERIFY(repo->stageFile(QStringLiteral("b.txt")).ok());
        const QString head = output(git.run({"rev-parse", "HEAD"}));
        const QString pidFile = repo->path() + QStringLiteral("/.git/hook.pid");
        done.clear();
        svc.commitChanges(QStringLiteral("stuck"));
        QVERIFY(QTest::qWaitFor([&] { return QFileInfo(pidFile).size() > 0; }, 15000));
        QFile pid(pidFile);
        QVERIFY(pid.open(QIODevice::ReadOnly));
        const qint64 hook = pid.readAll().trimmed().toLongLong();
        QVERIFY(hook > 0);

        QElapsedTimer timer;
        timer.start();
        svc.cancelGitOps();
        QVERIFY(done.waitForAny(10000));
        QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
        QCOMPARE(done.at(0), (QVariantList{false, QStringLiteral("Cancelled")}));
        QCOMPARE(output(git.run({"rev-parse", "HEAD"})), head);
        QTRY_VERIFY_WITH_TIMEOUT(!gitbolt::test::processAlive(hook), 5000);
    }

    // A repository whose commits are to be in another encoding
    // (i18n.commitEncoding): git labels a message with it and converts
    // nothing, and GitBolt's messages are UTF-8. "Café crème" went into
    // history labelled ISO-8859-1, and read "CafÃ© crÃ¨me" wherever the
    // label is honoured. A commit's message, and a reword's, go in as
    // UTF-8 and unlabelled now, as libgit2's commits did.
    void commitMessagesAreLabelledAsTheUtf8TheyAre() {
        const IsolatedGitConfig config;
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess git(repo->path().toStdString());
        QVERIFY(succeeded(git.run({"config", "i18n.commitEncoding", "ISO-8859-1"})));
        const QString cafe = QStringLiteral("Caf\u00e9 cr\u00e8me");
        const auto header = [&](const char* rev) {
            const QString object = output(git.run({"cat-file", "-p", rev}));
            return object.section(QStringLiteral("\n\n"), 0, 0);
        };
        // The message's bytes, as they are in the commit object.
        const auto rawMessage = [&](const char* rev) {
            const auto out = git.run({"cat-file", "commit", rev});
            const QByteArray object = out ? QByteArray::fromStdString(out->stdoutData) : QByteArray();
            return object.mid(object.indexOf("\n\n") + 2).trimmed();
        };
        repo->writeFile(QStringLiteral("x.txt"), QByteArrayLiteral("x\n"));
        QVERIFY(repo->stageFile(QStringLiteral("x.txt")).ok());

        GitService svc;
        SignalLog log(svc);
        MainThreadSpy done(&svc, &GitService::commitComplete);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        svc.commitChanges(cafe);
        QVERIFY(done.waitForAny(15000));
        QVERIFY2(done.at(0).at(0).toBool(), qPrintable(done.at(0).at(1).toString()));
        QVERIFY2(!header("HEAD").contains(QStringLiteral("\nencoding ")), qPrintable(header("HEAD")));
        QCOMPARE(rawMessage("HEAD"), cafe.toUtf8());

        // A reword through the rebase, and the commit it picks anew
        // after it.
        repo->writeFile(QStringLiteral("y.txt"), QByteArrayLiteral("y\n"));
        QVERIFY(succeeded(git.run({"add", "y.txt"})));
        QVERIFY(succeeded(git.run({"-c", "i18n.commitEncoding=UTF-8", "commit", "-q", "-m", "y"})));
        const auto id = [&](const char* spec) {
            auto r = repo->repo().resolveRef(spec);
            return r ? *r : gitbolt::git::ObjectId();
        };
        using Type = gitbolt::git::RebaseOperationType;
        gitbolt::git::RebasePlan plan;
        plan.onto = id("HEAD~2");
        plan.head = id("HEAD");
        plan.branch = branchOf(git);
        plan.operations = {{Type::Pick, id("HEAD"), "y"},
                           {Type::Reword, id("HEAD~1"), cafe.toStdString(),
                            (cafe + QStringLiteral(", na\u00efve\n")).toStdString()}};
        MainThreadSpy steps(&svc, &GitService::rebaseStepFinished);
        log.clear();
        svc.interactiveRebase(plan);
        QVERIFY2(log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(steps.at(0), (QVariantList{QStringLiteral("rebase"), true, false}));
        QVERIFY2(!header("HEAD~1").contains(QStringLiteral("\nencoding ")),
                 qPrintable(header("HEAD~1")));
        QCOMPARE(rawMessage("HEAD~1"), (cafe + QStringLiteral(", na\u00efve")).toUtf8());
        QVERIFY2(!header("HEAD").contains(QStringLiteral("\nencoding ")), qPrintable(header("HEAD")));
    }

    // -----------------------------------------------------------------
    // Long git jobs share the global thread pool with the opens and the
    // refreshes, and the pool is as big as the machine has CPUs. A
    // commit waiting on its pre-commit hook held a thread throughout:
    // on a 2-CPU machine two such jobs, here one in a pool of one, left
    // none, and an open of another repository came in only once the
    // hook was done. A job waiting on git lends its thread back now.
    // -----------------------------------------------------------------
    void aCommitWaitingOnItsHookLeavesRoomForAnOpen() {
        const IsolatedGitConfig config;
        auto a = repoWithCommits(1);
        auto b = repoWithCommits(1);
        QVERIFY(a && b);
        QVERIFY(writeHook(a->path() + QStringLiteral("/.git/hooks"), "pre-commit",
                          ": > .git/hook-started\n"
                          "i=0; while [ ! -f .git/GO ] && [ $i -lt 300 ]; do"
                          " sleep 0.05; i=$((i+1)); done\n"));
        a->writeFile(QStringLiteral("x.txt"), QByteArrayLiteral("x\n"));
        QVERIFY(a->stageFile(QStringLiteral("x.txt")).ok());

        QThreadPool* pool = QThreadPool::globalInstance();
        QVERIFY(pool->waitForDone(10000));
        const int threads = pool->maxThreadCount();
        const auto restore = qScopeGuard([&] { pool->setMaxThreadCount(threads); });
        pool->setMaxThreadCount(1);

        GitService committing;
        QVERIFY(committing.openRepository(a->path()));
        QVERIFY(pool->waitForDone(10000));   // its refreshes
        MainThreadSpy done(&committing, &GitService::commitComplete);
        committing.commitChanges(QStringLiteral("held"));
        QVERIFY(QTest::qWaitFor([&] {
            return QFileInfo::exists(a->path() + QStringLiteral("/.git/hook-started"));
        }, 10000));

        GitService opening;
        MainThreadSpy opened(&opening, &GitService::repositoryOpened);
        QElapsedTimer timer;
        timer.start();
        opening.openRepositoryAsync(b->path());
        const bool openedInTime = opened.waitForAny(5000);
        const qint64 openMs = timer.elapsed();
        const bool hookStillRan = done.isEmpty();
        a->writeFile(QStringLiteral(".git/GO"), QByteArray());
        QVERIFY(done.waitForAny(20000));
        QVERIFY2(openedInTime && hookStillRan,
                 qPrintable(QStringLiteral("opened after %1 ms, the commit %2")
                                .arg(openMs)
                                .arg(hookStillRan ? QStringLiteral("still running")
                                                  : QStringLiteral("done first"))));
        QVERIFY2(done.first().at(0).toBool(), qPrintable(done.first().at(1).toString()));
        QVERIFY(pool->waitForDone(10000));
    }

    // -----------------------------------------------------------------
    // A merge that stashed the user's changes away (merge.autoStash) and
    // stopped on conflicts: git puts them back when the merge's commit
    // is made, and git commit does. libgit2's commit didn't: the change
    // was gone from the file, the stash list empty, and git's
    // MERGE_AUTOSTASH left to turn up at the next merge. When putting
    // it back conflicts, git keeps it in the stash, and says so.
    // -----------------------------------------------------------------
    void commitConcludingAnAutostashedMergeRestoresTheChanges_data() {
        QTest::addColumn<bool>("conflicts");
        QTest::newRow("applies") << false;
        QTest::newRow("conflicts") << true;
    }

    void commitConcludingAnAutostashedMergeRestoresTheChanges() {
        QFETCH(bool, conflicts);
        const IsolatedGitConfig config;
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess git(repo->path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        QVERIFY(succeeded(git.run({"config", "merge.autoStash", "true"})));
        QVERIFY(repo->writeAndCommit(QStringLiteral("notes.txt"), QByteArrayLiteral("base\n"),
                                     QStringLiteral("notes")).ok());
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "side"})));
        repo->writeFile(QStringLiteral("f0.txt"), QByteArrayLiteral("side\n"));
        QVERIFY(succeeded(git.run({"commit", "-q", "-am", "side"})));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));
        repo->writeFile(QStringLiteral("f0.txt"), QByteArrayLiteral("main\n"));
        QVERIFY(succeeded(git.run({"commit", "-q", "-am", "main"})));
        // The user's change: in the file the merge conflicts in, or not.
        const QString dirty = conflicts ? QStringLiteral("f0.txt") : QStringLiteral("notes.txt");
        repo->writeFile(dirty, QByteArrayLiteral("mine\n"));
        QVERIFY(!succeeded(git.run({"merge", "side"})));
        const QString autostash = repo->path() + QStringLiteral("/.git/MERGE_AUTOSTASH");
        QVERIFY(QFileInfo::exists(autostash));
        repo->writeFile(QStringLiteral("f0.txt"), QByteArrayLiteral("resolved\n"));
        QVERIFY(repo->stageFile(QStringLiteral("f0.txt")).ok());

        GitService svc;
        SignalLog log(svc);
        MainThreadSpy done(&svc, &GitService::commitComplete);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        log.clear();
        svc.commitChanges(QStringLiteral("merge side"));
        QVERIFY(done.waitForAny(15000));
        QVERIFY2(done.at(0).at(0).toBool(), qPrintable(done.at(0).at(1).toString()));
        QVERIFY(log.waitForAfter(QStringLiteral("commitComplete"), {"status", "log"}));
        QCOMPARE(output(git.run({"rev-list", "--parents", "-1", "HEAD"})).split(' ').size(), 3);
        QVERIFY(!QFileInfo::exists(autostash));

        QFile file(repo->path() + QLatin1Char('/') + dirty);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();
        if (!conflicts) {
            QCOMPARE(content, QByteArrayLiteral("mine\n"));
            QCOMPARE(output(git.run({"stash", "list"})), QString());
            QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
            QCOMPARE(done.at(0).at(1).toString().endsWith(QStringLiteral("Applied autostash.")),
                     true);
            return;
        }
        // Conflict markers in the file; the change itself in the stash.
        QVERIFY2(content.contains("<<<<<<<"), content.constData());
        QCOMPARE(output(git.run({"stash", "list"})), QStringLiteral("stash@{0}: autostash"));
        const QString kept = QStringLiteral("operationFailed(autostash: Applying the autostash "
                                            "resulted in conflicts. Your changes are safe in "
                                            "the stash.)");
        QVERIFY2(log.indexOf(kept) > log.indexOf(QStringLiteral("commitComplete")),
                 qPrintable(log.join(QStringLiteral(", "))));
    }

    // An amend during a revert: git would fold the revert into the
    // commit before it, and finish the revert, while the in-progress
    // bar says "Commit to finish it". Refused, as the Commit dialog's
    // Amend box is, before git runs.
    void amendDuringARevertIsRefused() {
        const IsolatedGitConfig config;
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess git(repo->path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        QVERIFY(repo->writeAndCommit(QStringLiteral("f0.txt"), QByteArrayLiteral("changed\n"),
                                     QStringLiteral("change")).ok());
        QVERIFY(repo->writeAndCommit(QStringLiteral("f0.txt"), QByteArrayLiteral("later\n"),
                                     QStringLiteral("later")).ok());
        QVERIFY(!succeeded(git.run({"revert", "--no-edit", "HEAD~1"})));
        repo->writeFile(QStringLiteral("f0.txt"), QByteArrayLiteral("resolved\n"));
        QVERIFY(repo->stageFile(QStringLiteral("f0.txt")).ok());
        const QString head = output(git.run({"rev-parse", "HEAD"}));

        GitService svc;
        QVERIFY(svc.openRepository(repo->path()));
        MainThreadSpy done(&svc, &GitService::commitComplete);
        svc.commitChanges(QStringLiteral("folded"), /*amend=*/true);
        QVERIFY(done.waitForAny(15000));
        QCOMPARE(done.at(0), (QVariantList{false, QStringLiteral(
                                 "You are in the middle of a revert -- cannot amend.")}));
        QCOMPARE(output(git.run({"rev-parse", "HEAD"})), head);
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::Revert);
    }

    // An amend through git (`commit --amend`) keeps the commit's author,
    // name, email and date: only the committer is the user, as in a
    // terminal. With --reset-author, amending a teammate's commit to fix
    // its message would have made it the user's, dated now.
    void amendKeepsTheAuthor() {
        const IsolatedGitConfig config;
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess git(repo->path().toStdString());
        const QString base = output(git.run({"rev-parse", "HEAD"}));
        repo->writeFile(QStringLiteral("theirs.txt"), QByteArrayLiteral("their work\n"));
        QVERIFY(succeeded(git.run({"add", "theirs.txt"})));
        QVERIFY(succeeded(git.run({"commit", "-q", "-m", "their work",
                                   "--author=Team Mate <mate@example.com>",
                                   "--date=2001-02-03T04:05:06+01:00"})));

        GitService svc;
        QVERIFY(svc.openRepository(repo->path()));
        MainThreadSpy done(&svc, &GitService::commitComplete);
        svc.commitChanges(QStringLiteral("their work, reworded"), /*amend=*/true);
        QVERIFY(done.waitForAny(15000));
        QVERIFY2(done.at(0).at(0).toBool(), qPrintable(done.at(0).at(1).toString()));
        QCOMPARE(output(git.run({"log", "-1", "--date=iso-strict",
                                 "--format=%an <%ae> %ad, %cn <%ce>"})),
                 QStringLiteral("Team Mate <mate@example.com> 2001-02-03T04:05:06+01:00, "
                                "Test User <test@gitbolt.local>"));
        QCOMPARE(output(git.run({"log", "-1", "--format=%s"})),
                 QStringLiteral("their work, reworded"));
        QCOMPARE(output(git.run({"rev-parse", "HEAD^"})), base);
    }

    // A commit of nothing (nothing staged, whether or not the work tree
    // has changes) is git's to refuse, as a terminal's `git commit` is:
    // it fails with git's reason, and HEAD stays where it was. With
    // --allow-empty, a Commit clicked before staging made an empty
    // commit.
    void commitOfNothingIsRefused_data() {
        QTest::addColumn<bool>("unstaged");
        QTest::addColumn<QString>("reason");
        QTest::newRow("a clean work tree") << false << QStringLiteral("nothing to commit");
        QTest::newRow("a change not staged") << true
                                             << QStringLiteral("no changes added to commit");
    }

    void commitOfNothingIsRefused() {
        QFETCH(bool, unstaged);
        QFETCH(QString, reason);
        const IsolatedGitConfig config;
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess git(repo->path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        if (unstaged)
            repo->writeFile(QStringLiteral("f0.txt"), QByteArrayLiteral("changed, not staged\n"));
        const QString head = output(git.run({"rev-parse", "HEAD"}));

        GitService svc;
        QVERIFY(svc.openRepository(repo->path()));
        MainThreadSpy done(&svc, &GitService::commitComplete);
        svc.commitChanges(QStringLiteral("nothing"));
        QVERIFY(done.waitForAny(15000));
        QCOMPARE(done.size(), 1);
        QVERIFY2(!done.at(0).at(0).toBool(), qPrintable(done.at(0).at(1).toString()));
        QVERIFY2(done.at(0).at(1).toString().contains(reason),
                 qPrintable(done.at(0).at(1).toString()));
        QCOMPARE(output(git.run({"rev-parse", "HEAD"})), head);
        QCOMPARE(output(git.run({"rev-list", "--count", "HEAD"})), QStringLiteral("1"));
    }

    // `git reset`, which Unstage All runs, fails without unstaging
    // anything when another git holds index.lock. Only a git that
    // couldn't start was reported: the click silently did nothing.
    void unstageAllReportsALockedIndex() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        repo->writeFile(QStringLiteral("f0.txt"), QByteArrayLiteral("staged\n"));
        QVERIFY(repo->stageFile(QStringLiteral("f0.txt")).ok());

        GitService svc;
        QVERIFY(svc.openRepository(repo->path()));
        QFile lock(repo->path() + QStringLiteral("/.git/index.lock"));
        QVERIFY(lock.open(QIODevice::WriteOnly));
        lock.close();
        const auto unlock = qScopeGuard([&] { lock.remove(); });
        QSignalSpy failed(&svc, &GitService::operationFailed);
        svc.unstageAll();
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("unstageAll"));
        QVERIFY2(failed.at(0).at(1).toString().contains(QStringLiteral("index.lock")),
                 qPrintable(failed.at(0).at(1).toString()));
    }

    // Unstage All is `git reset`, which with no paths also ends a merge
    // or a cherry-pick in progress: MERGE_HEAD or CHERRY_PICK_HEAD went.
    // The stopped pick of `git cherry-pick A B C` was then dropped by
    // Continue ("Cherry-pick complete.", its resolution uncommitted).
    // Everything is unstaged, and the operation stays in progress.
    void unstageAllKeepsTheOperationInProgress_data() {
        QTest::addColumn<bool>("cherryPick");
        QTest::newRow("merge") << false;
        QTest::newRow("cherry-pick of several commits") << true;
    }

    void unstageAllKeepsTheOperationInProgress() {
        QFETCH(bool, cherryPick);
        const IsolatedGitConfig config;
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        QVERIFY(succeeded(work.run({"config", "core.autocrlf", "false"})));
        const QString main = QString::fromStdString(branchOf(work));
        QVERIFY(succeeded(work.run({"checkout", "-q", "-b", "side"})));
        repo->writeFile(QStringLiteral("a.txt"), QByteArrayLiteral("a\n"));
        QVERIFY(succeeded(work.run({"add", "a.txt"})));
        QVERIFY(succeeded(work.run({"commit", "-q", "-m", "A"})));
        repo->writeFile(QStringLiteral("f0.txt"), QByteArrayLiteral("side\n"));
        QVERIFY(succeeded(work.run({"commit", "-q", "-am", "B"})));
        repo->writeFile(QStringLiteral("c.txt"), QByteArrayLiteral("c\n"));
        QVERIFY(succeeded(work.run({"add", "c.txt"})));
        QVERIFY(succeeded(work.run({"commit", "-q", "-m", "C"})));
        QVERIFY(succeeded(work.run({"checkout", "-q", main.toStdString()})));
        repo->writeFile(QStringLiteral("f0.txt"), QByteArrayLiteral("main\n"));
        QVERIFY(succeeded(work.run({"commit", "-q", "-am", "M"})));
        if (cherryPick)
            QVERIFY(!succeeded(work.run({"cherry-pick", "side~2", "side~1", "side"})));
        else
            QVERIFY(!succeeded(work.run({"merge", "-q", "side"})));
        const QString marker = cherryPick ? QStringLiteral("/.git/CHERRY_PICK_HEAD")
                                          : QStringLiteral("/.git/MERGE_HEAD");
        QVERIFY(QFileInfo::exists(repo->path() + marker));
        // Resolved and staged.
        repo->writeFile(QStringLiteral("f0.txt"), QByteArrayLiteral("resolved\n"));
        QVERIFY(succeeded(work.run({"add", "f0.txt"})));

        GitService svc;
        QVERIFY(svc.openRepository(repo->path()));
        QSignalSpy failed(&svc, &GitService::operationFailed);
        svc.unstageAll();
        QCOMPARE(failed.size(), 0);
        QCOMPARE(output(work.run({"diff", "--cached", "--name-only"})), QString());
        QVERIFY2(QFileInfo::exists(repo->path() + marker), qPrintable(marker));
        QCOMPARE(repo->repo().state(), cherryPick ? gitbolt::git::RepoState::CherryPick
                                                  : gitbolt::git::RepoState::Merge);
        QCOMPARE(output(work.run({"show", ":f0.txt"})), QStringLiteral("main"));
        QFile resolved(repo->path() + QStringLiteral("/f0.txt"));
        QVERIFY(resolved.open(QIODevice::ReadOnly));
        QCOMPARE(resolved.readAll(), QByteArrayLiteral("resolved\n"));
    }

    // And on a branch with no commit yet, where there's no HEAD to
    // reset to: everything comes out of the index.
    void unstageAllOnAnUnbornBranch() {
        const IsolatedGitConfig config;
        gitbolt::test::TestRepo repo;
        repo.writeFile(QStringLiteral("new.txt"), QByteArrayLiteral("new\n"));
        QVERIFY(repo.stageFile(QStringLiteral("new.txt")).ok());
        gitbolt::git::GitProcess work(repo.path().toStdString());
        QCOMPARE(output(work.run({"ls-files"})), QStringLiteral("new.txt"));
        GitService svc;
        QVERIFY(svc.openRepository(repo.path()));
        QSignalSpy failed(&svc, &GitService::operationFailed);
        svc.unstageAll();
        QVERIFY2(failed.isEmpty(), qPrintable(failed.isEmpty() ? QString()
                                                : failed.at(0).at(1).toString()));
        QCOMPARE(output(work.run({"ls-files"})), QString());
        QVERIFY(QFileInfo::exists(repo.path() + QStringLiteral("/new.txt")));
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

        svc.cherryPick(head.value());
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

    // The remote ops, and checkoutBranch, say how they went: MainWindow
    // narrates the op it ran by that. It used to read a window-wide flag
    // that any failed checkout or push set, so a checkout failing while
    // a Fetch ran turned the Fetch, which went through, into a red ✗.
    void remoteOpsReturnTheirOwnOutcome() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        QTemporaryDir originDir;
        QVERIFY(originDir.isValid());
        auto origin = addBareOrigin(*repo, originDir);
        QVERIFY2(origin.ok(), origin ? "" : origin.error().message().c_str());
        auto& work = repo->repo();
        auto head = work.head();
        QVERIFY(head.ok());
        QVERIFY(work.createBranch("other", head.value()).ok());

        GitService svc;
        QSignalSpy failedSpy(&svc, &GitService::operationFailed);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(svc.push(QStringLiteral("origin"), QString()));
        QVERIFY(svc.fetch());
        QVERIFY(svc.pull(QStringLiteral("origin"), QString()));
        QVERIFY(svc.push(QStringLiteral("origin"), QStringLiteral("other")));
        QVERIFY(svc.deleteRemoteBranch(QStringLiteral("origin"), QStringLiteral("other")));
        QVERIFY(svc.checkoutBranch(QStringLiteral("other")));
        QCOMPARE(failedSpy.count(), 0);

        QVERIFY(!svc.checkoutBranch(QStringLiteral("no-such-branch")));
        QVERIFY(work.removeRemote("origin").ok());
        QVERIFY(work.addRemote("origin", (originDir.path() + QStringLiteral("/missing"))
                                             .toStdString()).ok());
        QVERIFY(!svc.fetch());
        QVERIFY(!svc.pull(QStringLiteral("origin"), QStringLiteral("other")));
        QVERIFY(!svc.push(QStringLiteral("origin"), QString()));
        QVERIFY(!svc.deleteRemoteBranch(QStringLiteral("origin"), QStringLiteral("other")));
        QCOMPARE(failedSpy.count(), 5);

        svc.closeRepository();
        QVERIFY(!svc.fetch());
        QVERIFY(!svc.checkoutBranch(QStringLiteral("other")));
    }

    // Fetch prunes: a branch deleted on the server (merged and deleted
    // on GitHub, say) kept its remote-tracking ref, as plain `git
    // fetch` does, in the sidebar for good. Unless git config says not
    // to: fetch.prune set either way is the user's call, and a remote's
    // own remote.<name>.prune outranks it, as in git.
    void fetchPrunesUnlessConfigSaysNot() {
        // git's own config only: a fetch.prune of the developer's would
        // decide the first case. (TestRepoHelper isolates libgit2's.)
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QString globalConfig = home.filePath(QStringLiteral("gitconfig"));
        QFile empty(globalConfig);
        QVERIFY(empty.open(QIODevice::WriteOnly));
        empty.close();
        const gitbolt::test::ScopedEnv global("GIT_CONFIG_GLOBAL",
                                              QFile::encodeName(globalConfig));
        const gitbolt::test::ScopedEnv noSystem("GIT_CONFIG_NOSYSTEM", "1");

        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        QTemporaryDir originDir;
        QVERIFY(originDir.isValid());
        auto origin = addBareOrigin(*repo, originDir);
        QVERIFY2(origin.ok(), origin ? "" : origin.error().message().c_str());
        auto& work = repo->repo();
        gitbolt::git::GitProcess workGit(repo->path().toStdString());
        auto config = work.config();

        const auto tracked = [&work] {
            return work.resolveRef("refs/remotes/origin/gone").ok();
        };
        // "gone" pushed, which makes origin/gone here too, then deleted
        // on the server.
        const auto deletedOnServer = [&] {
            return succeeded(workGit.run({"push", "-q", "origin", "HEAD:refs/heads/gone"}))
                && tracked()
                && origin->deleteBranch("gone").ok();
        };

        GitService svc;
        QSignalSpy failedSpy(&svc, &GitService::operationFailed);
        QVERIFY(svc.openRepository(repo->path()));

        // Nothing set: pruned, by the toolbar's Fetch and by the
        // auto-fetch's alike.
        QVERIFY(deletedOnServer());
        svc.fetch();
        QCOMPARE(failedSpy.count(), 0);
        QVERIFY(!tracked());
        QVERIFY(deletedOnServer());
        svc.fetch(QString(), std::make_shared<std::atomic<bool>>(false),
                  /*allowPrompts=*/false);
        QCOMPARE(failedSpy.count(), 0);
        QVERIFY(!tracked());

        // fetch.prune=false: kept.
        QVERIFY(config.setBool("fetch.prune", false).ok());
        QVERIFY(deletedOnServer());
        svc.fetch();
        QVERIFY(tracked());

        // remote.origin.prune=false, fetch.prune unset: kept.
        QVERIFY(config.remove("fetch.prune").ok());
        QVERIFY(config.setBool("remote.origin.prune", false).ok());
        QVERIFY(deletedOnServer());
        svc.fetch();
        QVERIFY(tracked());

        // remote.origin.prune=true outranks fetch.prune=false: pruned.
        QVERIFY(config.setBool("fetch.prune", false).ok());
        QVERIFY(config.setBool("remote.origin.prune", true).ok());
        QVERIFY(deletedOnServer());
        svc.fetch();
        QVERIFY(!tracked());
        QCOMPARE(failedSpy.count(), 0);
    }

    // Fetch's default prune reached past refs/remotes/: with a tags
    // refspec, a mirror's +refs/*:refs/*, or pruneTags set, it deleted
    // local tags and branches the server doesn't have (unpushed ones),
    // which a plain `git fetch` keeps, the silent auto-fetch included.
    // There, unless fetch.prune or remote.<name>.prune asks for it,
    // GitBolt's Fetch prunes nothing.
    void fetchDefaultPruneKeepsLocalRefs_data() {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QString>("value");
        QTest::newRow("tags refspec")
            << "remote.origin.fetch" << "+refs/tags/*:refs/tags/*";
        QTest::newRow("mirror refspec") << "remote.origin.fetch" << "+refs/*:refs/*";
        QTest::newRow("branches refspec")
            << "remote.origin.fetch" << "+refs/heads/*:refs/heads/*";
        QTest::newRow("fetch.pruneTags") << "fetch.pruneTags" << "true";
        QTest::newRow("remote pruneTags") << "remote.origin.pruneTags" << "true";
    }

    void fetchDefaultPruneKeepsLocalRefs() {
        QFETCH(QString, key);
        QFETCH(QString, value);
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QString globalConfig = home.filePath(QStringLiteral("gitconfig"));
        QFile empty(globalConfig);
        QVERIFY(empty.open(QIODevice::WriteOnly));
        empty.close();
        const gitbolt::test::ScopedEnv global("GIT_CONFIG_GLOBAL",
                                              QFile::encodeName(globalConfig));
        const gitbolt::test::ScopedEnv noSystem("GIT_CONFIG_NOSYSTEM", "1");

        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        QTemporaryDir originDir;
        QVERIFY(originDir.isValid());
        auto origin = addBareOrigin(*repo, originDir);
        QVERIFY2(origin.ok(), origin ? "" : origin.error().message().c_str());
        gitbolt::git::GitProcess workGit(repo->path().toStdString());
        QVERIFY(succeeded(workGit.run({"push", "-q", "origin", "HEAD:refs/heads/main",
                                       "HEAD:refs/heads/gone"})));
        QVERIFY(succeeded(workGit.run({"fetch", "-q", "origin"})));
        QVERIFY(origin->deleteBranch("gone").ok());
        // Never pushed: on this side only.
        QVERIFY(succeeded(workGit.run({"tag", "local-tag"})));
        QVERIFY(succeeded(workGit.run({"branch", "local-branch"})));
        // git won't fetch into a branch that is checked out.
        QVERIFY(succeeded(workGit.run({"checkout", "-q", "--detach"})));
        QVERIFY(succeeded(workGit.run({"config", "--add", key.toStdString(),
                                       value.toStdString()})));
        const auto has = [&](const char* ref) {
            return succeeded(workGit.run({"rev-parse", "--verify", "-q", ref}));
        };

        GitService svc;
        QSignalSpy failedSpy(&svc, &GitService::operationFailed);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(svc.fetch());
        QVERIFY(svc.fetch(QString(), std::make_shared<std::atomic<bool>>(false),
                          /*allowPrompts=*/false));
        QVERIFY2(failedSpy.isEmpty(), qPrintable(failedSpy.isEmpty()
                     ? QString() : failedSpy.first().at(1).toString()));
        QVERIFY(has("refs/tags/local-tag"));
        QVERIFY(has("refs/heads/local-branch"));
        // As plain `git fetch` leaves it: not pruned at all.
        QVERIFY(has("refs/remotes/origin/gone"));

        // Asked for in config, git prunes as it would in a terminal.
        QVERIFY(succeeded(workGit.run({"config", "remote.origin.prune", "true"})));
        QVERIFY(svc.fetch());
        QVERIFY(!has("refs/remotes/origin/gone"));
    }

    // The default prune went on into the submodules: git hands the
    // fetch's `-c fetch.prune=true` to the fetch it runs in each, where
    // it outranks the submodule's own config. A submodule with a tags
    // refspec lost its unpushed tags, and one set not to prune was
    // pruned, though the superproject's config allowed neither. Its
    // config counts as the superproject's does now.
    void fetchDefaultPruneSparesSubmodules_data() {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QString>("value");
        QTest::newRow("submodule tags refspec")
            << "remote.origin.fetch" << "+refs/tags/*:refs/tags/*";
        QTest::newRow("submodule fetch.prune=false") << "fetch.prune" << "false";
    }

    void fetchDefaultPruneSparesSubmodules() {
        QFETCH(QString, key);
        QFETCH(QString, value);
        // git's own config only, which lets these local submodules be
        // cloned and fetched (protocol.file.allow) and commits be made.
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QString globalConfig = home.filePath(QStringLiteral("gitconfig"));
        QFile configFile(globalConfig);
        QVERIFY(configFile.open(QIODevice::WriteOnly));
        configFile.write("[protocol \"file\"]\n\tallow = always\n"
                         "[user]\n\tname = T\n\temail = t@example.com\n"
                         "[commit]\n\tgpgSign = false\n"
                         "[init]\n\tdefaultBranch = main\n");
        configFile.close();
        const gitbolt::test::ScopedEnv global("GIT_CONFIG_GLOBAL",
                                              QFile::encodeName(globalConfig));
        const gitbolt::test::ScopedEnv noSystem("GIT_CONFIG_NOSYSTEM", "1");

        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const auto git = [](const QString& dir, std::vector<std::string> args) {
            const auto out = gitbolt::git::GitProcess(dir.toStdString()).run(args);
            if (!succeeded(out))
                qWarning("git %s in %s: %s", args.front().c_str(), qPrintable(dir),
                         out ? out->stderrData.c_str() : out.error().message().c_str());
            return succeeded(out);
        };
        const QString subUp = scratch.filePath(QStringLiteral("sub-upstream"));
        const QString superUp = scratch.filePath(QStringLiteral("super-upstream"));
        const QString work = scratch.filePath(QStringLiteral("work"));
        const QString workSub = work + QStringLiteral("/sub");
        QVERIFY(QDir().mkpath(subUp) && QDir().mkpath(superUp));
        QVERIFY(git(subUp, {"init", "-q"}));
        QVERIFY(git(subUp, {"commit", "-q", "--allow-empty", "-m", "s1"}));
        QVERIFY(git(subUp, {"branch", "old"}));
        QVERIFY(git(superUp, {"init", "-q"}));
        QVERIFY(git(superUp, {"submodule", "add", "-q", subUp.toStdString(), "sub"}));
        QVERIFY(git(superUp, {"commit", "-q", "-m", "with sub"}));
        QVERIFY(git(scratch.path(), {"clone", "-q", "--recurse-submodules",
                                     superUp.toStdString(), "work"}));

        // In the clone's submodule: origin/old, which the server then
        // deletes; a tag never pushed; and the config under test.
        QVERIFY(git(workSub, {"rev-parse", "--verify", "-q", "refs/remotes/origin/old"}));
        QVERIFY(git(subUp, {"branch", "-D", "old"}));
        QVERIFY(git(workSub, {"tag", "local-unpushed"}));
        QVERIFY(git(workSub, {"config", "--add", key.toStdString(), value.toStdString()}));
        // A new submodule commit upstream, so the superproject's fetch
        // goes on into the submodule (fetch.recurseSubmodules=on-demand).
        QVERIFY(git(subUp, {"commit", "-q", "--allow-empty", "-m", "s2"}));
        QVERIFY(git(superUp + QStringLiteral("/sub"), {"pull", "-q", "origin", "HEAD"}));
        QVERIFY(git(superUp, {"commit", "-q", "-am", "bump sub"}));
        // The superproject itself allows the default prune.
        QVERIFY(!succeeded(gitbolt::git::GitProcess(work.toStdString())
                               .run({"config", "--get", "fetch.prune"})));

        GitService svc;
        QSignalSpy failedSpy(&svc, &GitService::operationFailed);
        QVERIFY(svc.openRepository(work));
        QVERIFY(svc.fetch());
        QVERIFY2(failedSpy.isEmpty(), qPrintable(failedSpy.isEmpty()
                     ? QString() : failedSpy.first().at(1).toString()));
        // The fetch did go on into the submodule: s2 is there now.
        const QString s2 =
            output(gitbolt::git::GitProcess(subUp.toStdString()).run({"rev-parse", "HEAD"}));
        QVERIFY(!s2.isEmpty());
        QVERIFY(git(workSub, {"cat-file", "-e", s2.toStdString() + "^{commit}"}));
        const auto subHas = [&](const char* ref) {
            return git(workSub, {"rev-parse", "--verify", "-q", ref});
        };
        QVERIFY(subHas("refs/tags/local-unpushed"));
        QVERIFY(subHas("refs/remotes/origin/old"));
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

    // At an `edit` stop the commit is made, and the paused bar asks for
    // changes there. Continue turns unstaged ones down, which left Skip,
    // and git's --skip resets the work tree first: a tracked file's
    // edits and a new file staged were gone for good, with "Rebase
    // complete.". Skip is refused while there are any; untracked files
    // don't count (git's reset leaves them).
    void skipAtAStopWhoseCommitIsMadeKeepsUncommittedWork() {
        const IsolatedGitConfig config;
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
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        const auto step = [&](const std::function<void()>& run) {
            log.clear();
            steps.clear();
            run();
            return log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"})
                && steps.size() == 1;
        };
        QVERIFY2(step([&] { svc.interactiveRebase(plan); }),
                 qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(steps.at(0), (QVariantList{QStringLiteral("rebase"), true, true}));
        QCOMPARE(svc.committedRebaseStop(), QStringLiteral("edit"));
        QVERIFY(!svc.hasTrackedChanges());

        // The user's work at the stop: an edit, and a new file staged.
        repo->writeFile(QStringLiteral("f0.txt"), QByteArrayLiteral("my edit\n"));
        repo->writeFile(QStringLiteral("f9.txt"), QByteArrayLiteral("new\n"));
        QVERIFY(succeeded(work.run({"add", "f9.txt"})));
        QVERIFY(svc.hasTrackedChanges());
        QVERIFY2(step([&] { svc.rebaseSkip(); }), qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(steps.at(0), (QVariantList{QStringLiteral("rebase --skip"), false, true}));
        QVERIFY2(log.indexOfMatch(QRegularExpression(
                     QStringLiteral("^operationFailed\\(rebase --skip: Skipping would discard"))) >= 0,
                 qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::Rebase);
        QCOMPARE(output(work.run({"status", "--porcelain", "--untracked-files=no"})),
                 QStringLiteral("M f0.txt\nA  f9.txt"));   // " M f0.txt", trimmed
        QCOMPARE(output(work.run({"show", ":f9.txt"})), QStringLiteral("new"));

        // With only an untracked file left, Skip goes on, and keeps it.
        QVERIFY(succeeded(work.run({"checkout", "--", "f0.txt"})));
        QVERIFY(succeeded(work.run({"rm", "-q", "--cached", "f9.txt"})));
        QVERIFY(!svc.hasTrackedChanges());
        QVERIFY2(step([&] { svc.rebaseSkip(); }), qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(steps.at(0), (QVariantList{QStringLiteral("rebase --skip"), true, false}));
        QCOMPARE(output(work.run({"log", "--format=%s"})),
                 QStringLiteral("commit 2\ncommit 1\ncommit 0"));
        QVERIFY(QFileInfo::exists(repo->path() + QStringLiteral("/f9.txt")));
    }

    // The same `edit` stop, where HEAD is no longer the stopped commit
    // as git first picked it, or never matched it:
    //   - a rebase onto a branch that changed lines near the commit's:
    //     the pick has other context lines, and the patch-id taken with
    //     them differed from the commit's;
    //   - the commit amended at the stop, or split (reset to its parent
    //     to commit it in parts).
    // Skip ran git's reset --hard over the work there, after a question
    // that said the commit's changes were left out. git's own
    // rebase-merge/amend says the stop is an `edit`'s, whatever HEAD is.
    void skipAtAnEditStopKeepsWorkWhateverHeadIs_data() {
        QTest::addColumn<QString>("how");
        QTest::newRow("onto a base that changed nearby lines") << QStringLiteral("moved");
        QTest::newRow("commit amended at the stop") << QStringLiteral("amended");
        QTest::newRow("commit split at the stop") << QStringLiteral("split");
    }

    void skipAtAnEditStopKeepsWorkWhateverHeadIs() {
        QFETCH(QString, how);
        const IsolatedGitConfig config;
        gitbolt::test::TestRepo repo;
        gitbolt::git::GitProcess work(repo.path().toStdString());
        QVERIFY(succeeded(work.run({"config", "core.autocrlf", "false"})));
        const auto commit = [&](const QByteArray& content, const char* message) {
            repo.writeFile(QStringLiteral("f.txt"), content);
            return succeeded(work.run({"add", "f.txt"}))
                && succeeded(work.run({"commit", "-q", "-m", message}));
        };
        QByteArray lines;
        for (int i = 1; i <= 10; ++i)
            lines += "line " + QByteArray::number(i) + '\n';
        QVERIFY(commit(lines, "base"));
        const std::string main = branchOf(work);
        QVERIFY(succeeded(work.run({"checkout", "-q", "-b", "side"})));
        QVERIFY(commit(QByteArray(lines).replace("line 6\n", "line 6 by C\n"), "C"));
        const std::string c = output(work.run({"rev-parse", "HEAD"})).toStdString();
        std::string onto = output(work.run({"rev-parse", "HEAD~1"})).toStdString();
        if (how == QLatin1String("moved")) {
            QVERIFY(succeeded(work.run({"checkout", "-q", main})));
            // Three lines above C's: in its context.
            QVERIFY(commit(QByteArray(lines).replace("line 3\n", "line 3 by main\n"), "M"));
            onto = output(work.run({"rev-parse", "HEAD"})).toStdString();
            QVERIFY(succeeded(work.run({"checkout", "-q", "side"})));
        }

        const auto stopped = work.interactiveRebase(onto, "edit " + c + " C\n");
        QVERIFY2(succeeded(stopped), stopped.ok() ? stopped->stderrData.c_str() : "");
        QCOMPARE(repo.repo().state(), gitbolt::git::RepoState::Rebase);
        if (how == QLatin1String("moved")) {
            // Picked anew, the same change.
            QVERIFY(output(work.run({"rev-parse", "HEAD"})).toStdString() != c);
            QCOMPARE(output(work.run({"show", "HEAD:f.txt"})).count(QStringLiteral("by")), 2);
        } else if (how == QLatin1String("amended")) {
            repo.writeFile(QStringLiteral("f.txt"),
                           QByteArray(lines).replace("line 6\n", "line 6 by C\n")
                               .replace("line 8\n", "line 8 amended\n"));
            QVERIFY(succeeded(work.run({"commit", "-q", "-a", "--amend", "--no-edit"})));
        } else {
            QVERIFY(succeeded(work.run({"reset", "-q", "HEAD~1"})));
        }
        // The user's work at the stop: a new file, staged.
        repo.writeFile(QStringLiteral("new.txt"), QByteArrayLiteral("mine\n"));
        QVERIFY(succeeded(work.run({"add", "new.txt"})));

        QCOMPARE(work.committedRebaseStop(), std::string("edit"));
        const auto skipped = work.rebaseSkip();
        QVERIFY2(!skipped.ok(), skipped.ok() ? "skipped" : "");
        QCOMPARE(skipped.error().code(), gitbolt::git::GitErrorCode::Uncommitted);
        QCOMPARE(repo.repo().state(), gitbolt::git::RepoState::Rebase);
        QCOMPARE(output(work.run({"show", ":new.txt"})), QStringLiteral("mine"));
        QVERIFY(QFileInfo::exists(repo.path() + QStringLiteral("/new.txt")));
    }

    // A reword a commit-msg hook turned down, picked onto a branch that
    // changed lines near the commit's (not a fast-forward): Continue's
    // retry didn't know the pick for the reword's commit (its patch-id
    // was taken with the context lines), and "Rebase complete." came
    // with the old message. Nor where HEAD keeps no reflog
    // (core.logAllRefUpdates=false), which the retry asked whether HEAD
    // was git's pick or the user's amend: git said nothing, and it
    // never ran.
    void rewordRetryAfterAPickOntoAMovedBase_data() {
        QTest::addColumn<bool>("reflog");
        QTest::newRow("with a reflog") << true;
        QTest::newRow("with no reflog") << false;
    }

    void rewordRetryAfterAPickOntoAMovedBase() {
        QFETCH(bool, reflog);
        const IsolatedGitConfig config;
        gitbolt::test::TestRepo repo;
        gitbolt::git::GitProcess work(repo.path().toStdString());
        QVERIFY(succeeded(work.run({"config", "core.autocrlf", "false"})));
        if (!reflog) {
            QVERIFY(succeeded(work.run({"config", "core.logAllRefUpdates", "false"})));
            QVERIFY(QDir(repo.path() + QStringLiteral("/.git/logs")).removeRecursively());
        }
        const auto commit = [&](const QByteArray& content, const char* message) {
            repo.writeFile(QStringLiteral("f.txt"), content);
            return succeeded(work.run({"add", "f.txt"}))
                && succeeded(work.run({"commit", "-q", "-m", message}));
        };
        QByteArray lines;
        for (int i = 1; i <= 10; ++i)
            lines += "line " + QByteArray::number(i) + '\n';
        QVERIFY(commit(lines, "base"));
        const std::string main = branchOf(work);
        QVERIFY(succeeded(work.run({"checkout", "-q", "-b", "side"})));
        QVERIFY(commit(QByteArray(lines).replace("line 6\n", "line 6 by C\n"), "C"));
        const std::string c = output(work.run({"rev-parse", "HEAD"})).toStdString();
        QVERIFY(succeeded(work.run({"checkout", "-q", main})));
        QVERIFY(commit(QByteArray(lines).replace("line 3\n", "line 3 by main\n"), "M"));
        const std::string onto = output(work.run({"rev-parse", "HEAD"})).toStdString();
        QVERIFY(succeeded(work.run({"checkout", "-q", "side"})));
        QVERIFY(writeHook(repo.path() + QStringLiteral("/.git/hooks"), "commit-msg",
                          "if [ -f .git/BLOCK ]; then\n"
                          "    echo 'commit-msg: BAD is not allowed' >&2; exit 1\nfi\n"));
        repo.writeFile(QStringLiteral(".git/BLOCK"), QByteArray());

        const auto stopped = work.interactiveRebase(onto, "reword " + c + " C\n",
                                                    {{c, "C, reworded\n"}});
        QVERIFY(stopped.ok() && !stopped->success());
        QCOMPARE(repo.repo().state(), gitbolt::git::RepoState::Rebase);
        QVERIFY(output(work.run({"rev-parse", "HEAD"})).toStdString() != c);
        QCOMPARE(output(work.run({"log", "-1", "--format=%s"})), QStringLiteral("C"));
        QCOMPARE(work.committedRebaseStop(), std::string("reword"));
        QCOMPARE(QFileInfo::exists(repo.path() + QStringLiteral("/.git/logs/HEAD")), reflog);

        QVERIFY(QFile::remove(repo.path() + QStringLiteral("/.git/BLOCK")));
        const auto continued = work.rebaseContinue();
        QVERIFY2(succeeded(continued), continued.ok() ? continued->stderrData.c_str() : "");
        QCOMPARE(repo.repo().state(), gitbolt::git::RepoState::None);
        QCOMPARE(output(work.run({"log", "--format=%s"})),
                 QStringLiteral("C, reworded\nM\nbase"));
    }

    // The same reword stop, its message then amended by the user (the
    // Commit dialog's Amend is there at a rebase stop) to one the hook
    // takes. Continue's retry put the plan's message back over theirs,
    // which the hook turned down again, on every Continue: the rebase
    // could never be finished from the bar. The user's amend stands,
    // where HEAD keeps no reflog to tell it by too, and where it keeps
    // the old message and adds to it (a body, a trailer): with no
    // reflog, that passed for git's own pick, which has the old one.
    // At a fast-forward (the commit kept where it was) and at a pick
    // onto another base, from a terminal and from GitBolt's Commit
    // dialog; and with a prepare-commit-msg hook, which may add to git's
    // own pick (the hook here adds nothing).
    void rewordRetryLeavesTheUsersOwnAmendAlone_data() {
        QTest::addColumn<bool>("reflog");
        QTest::addColumn<bool>("fastForward");
        QTest::addColumn<QString>("amended");
        QTest::addColumn<bool>("inGitBolt");
        QTest::addColumn<bool>("prepareHook");
        const QString renamed = QStringLiteral("OK renamed by me");
        const QString addedTo = QStringLiteral("OK C\n\nRefs: T-1");
        QTest::newRow("with a reflog") << true << true << renamed << false << false;
        QTest::newRow("with no reflog") << false << true << renamed << false << false;
        QTest::newRow("with no reflog, the old message added to")
            << false << true << addedTo << false << false;
        QTest::newRow("with no reflog, picked anew, the old message added to")
            << false << false << addedTo << false << false;
        QTest::newRow("with no reflog, picked anew, added to in GitBolt")
            << false << false << addedTo << true << false;
        QTest::newRow("with no reflog, added to in GitBolt")
            << false << true << addedTo << true << false;
        QTest::newRow("with a reflog, picked anew, the old message added to")
            << true << false << addedTo << false << false;
        QTest::newRow("with no reflog and a prepare-commit-msg hook, the old message added to")
            << false << true << addedTo << false << true;
        QTest::newRow("with no reflog and a prepare-commit-msg hook, picked anew, "
                      "added to in GitBolt")
            << false << false << addedTo << true << true;
    }

    void rewordRetryLeavesTheUsersOwnAmendAlone() {
        QFETCH(bool, reflog);
        QFETCH(bool, fastForward);
        QFETCH(QString, amended);
        QFETCH(bool, inGitBolt);
        QFETCH(bool, prepareHook);
        const IsolatedGitConfig config;
        gitbolt::test::TestRepo repo;
        gitbolt::git::GitProcess work(repo.path().toStdString());
        QVERIFY(succeeded(work.run({"config", "core.autocrlf", "false"})));
        if (!reflog)
            QVERIFY(succeeded(work.run({"config", "core.logAllRefUpdates", "false"})));
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("OK base")).ok());
        QVERIFY(repo.writeAndCommit(QStringLiteral("b.txt"), "b\n", QStringLiteral("OK B")).ok());
        const auto c = repo.writeAndCommit(QStringLiteral("c.txt"), "c\n", QStringLiteral("OK C"));
        QVERIFY(c.ok());
        // Onto C's parent, C stays as it is; onto the base, with B left
        // out, git picks it anew.
        const std::string onto =
            output(work.run({"rev-parse", fastForward ? "HEAD~1" : "HEAD~2"})).toStdString();
        QVERIFY(writeHook(repo.path() + QStringLiteral("/.git/hooks"), "commit-msg",
                          "if ! head -1 \"$1\" | grep -q '^OK'; then\n"
                          "    echo 'commit-msg: must start with OK' >&2; exit 1\nfi\n"));
        if (prepareHook)
            QVERIFY(writeHook(repo.path() + QStringLiteral("/.git/hooks"), "prepare-commit-msg",
                              ":\n"));

        const auto stopped = work.interactiveRebase(onto, "reword " + c->toHex() + " OK C\n",
                                                    {{c->toHex(), "renamed\n"}});
        QVERIFY(stopped.ok() && !stopped->success());
        QCOMPARE(repo.repo().state(), gitbolt::git::RepoState::Rebase);
        QCOMPARE(output(work.run({"rev-parse", "HEAD"})) == QString::fromStdString(c->toHex()),
                 fastForward);
        if (inGitBolt) {
            const auto made = work.commit(amended.toStdString(), /*amend=*/true);
            QVERIFY2(succeeded(made), made.ok() ? made->stderrData.c_str()
                                                : made.error().message().c_str());
        } else {
            QVERIFY(succeeded(work.run({"commit", "-q", "--amend", "-m", amended.toStdString()})));
        }
        QCOMPARE(QFileInfo::exists(repo.path() + QStringLiteral("/.git/logs/HEAD")), reflog);

        const auto continued = work.rebaseContinue();
        QVERIFY2(succeeded(continued), continued.ok() ? continued->stderrData.c_str()
                                                      : continued.error().message().c_str());
        QCOMPARE(repo.repo().state(), gitbolt::git::RepoState::None);
        QCOMPARE(output(work.run({"log", "-1", "--format=%B"})), amended);
        QCOMPARE(output(work.run({"log", "--format=%s"})),
                 amended.section(QLatin1Char('\n'), 0, 0)
                     + (fastForward ? QStringLiteral("\nOK B\nOK base")
                                    : QStringLiteral("\nOK base")));
    }

    // A reword of a commit the branch it is rebased onto already has (a
    // cherry-pick of it, the same author, date and change): the pick
    // comes out empty, and git stops there with it pending, HEAD on the
    // branch's copy. That copy passed for the reword's pick, and
    // Continue's retry ran a commit --amend, which git refuses there
    // ("cannot amend"), on every Continue. git's --continue drops it.
    void rewordOfACommitThatComesOutEmptyContinues() {
        const IsolatedGitConfig config;
        gitbolt::test::TestRepo repo;
        gitbolt::git::GitProcess work(repo.path().toStdString());
        QVERIFY(succeeded(work.run({"config", "core.autocrlf", "false"})));
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("base")).ok());
        const std::string main = branchOf(work);
        QVERIFY(succeeded(work.run({"checkout", "-q", "-b", "side"})));
        repo.writeFile(QStringLiteral("c.txt"), "c\n");
        QVERIFY(succeeded(work.run({"add", "c.txt"})));
        QVERIFY(succeeded(work.run({"commit", "-q", "-m", "C"})));
        const std::string c = output(work.run({"rev-parse", "HEAD"})).toStdString();
        QVERIFY(succeeded(work.run({"checkout", "-q", main})));
        // A commit first, so that the copy isn't C itself (the same
        // parent, tree, message and second made the same commit).
        repo.writeFile(QStringLiteral("m.txt"), "m\n");
        QVERIFY(succeeded(work.run({"add", "m.txt"})));
        QVERIFY(succeeded(work.run({"commit", "-q", "-m", "M"})));
        QVERIFY(succeeded(work.run({"cherry-pick", c})));
        const std::string onto = output(work.run({"rev-parse", "HEAD"})).toStdString();
        QVERIFY(succeeded(work.run({"checkout", "-q", "side"})));

        const auto stopped = work.interactiveRebase(onto, "reword " + c + " C\n",
                                                    {{c, "C, reworded\n"}});
        QVERIFY(stopped.ok());
        if (repo.repo().state() != gitbolt::git::RepoState::Rebase)
            QSKIP("this git doesn't stop at a pick that comes out empty");
        QCOMPARE(output(work.run({"rev-parse", "HEAD"})).toStdString(), onto);
        QVERIFY(QFileInfo::exists(repo.path() + QStringLiteral("/.git/CHERRY_PICK_HEAD")));
        QCOMPARE(work.committedRebaseStop(), std::string());

        const auto continued = work.rebaseContinue();
        QVERIFY2(succeeded(continued), continued.ok() ? continued->stderrData.c_str()
                                                      : continued.error().message().c_str());
        QCOMPARE(repo.repo().state(), gitbolt::git::RepoState::None);
        QCOMPARE(output(work.run({"rev-parse", "HEAD"})).toStdString(), onto);
    }

    // `git cherry-pick X Y` turned down over staged changes leaves the
    // sequence (its todo) with no pick pending. git's --skip there runs
    // `reset --merge HEAD`, and the staged changes, the user's, went
    // with "Cherry-pick complete.". Skip is refused while there are any.
    void sequenceSkipKeepsStagedWorkWhenNothingWasPicked() {
        const IsolatedGitConfig config;
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        QVERIFY(succeeded(work.run({"config", "core.autocrlf", "false"})));
        const std::string main = branchOf(work);
        QVERIFY(succeeded(work.run({"checkout", "-q", "-b", "side"})));
        for (const char* name : {"X", "Y"}) {
            repo->writeFile(QString::fromLatin1(name) + ".txt", QByteArray(name) + '\n');
            QVERIFY(succeeded(work.run({"add", std::string(name) + ".txt"})));
            QVERIFY(succeeded(work.run({"commit", "-q", "-m", name})));
        }
        QVERIFY(succeeded(work.run({"checkout", "-q", main})));
        repo->writeFile(QStringLiteral("f0.txt"), QByteArrayLiteral("staged work\n"));
        QVERIFY(succeeded(work.run({"add", "f0.txt"})));
        QVERIFY(!succeeded(work.run({"cherry-pick", "side~1", "side"})));
        QVERIFY(QFileInfo::exists(repo->path() + QStringLiteral("/.git/sequencer")));
        QVERIFY(!QFileInfo::exists(repo->path() + QStringLiteral("/.git/CHERRY_PICK_HEAD")));
        QVERIFY(repo->repo().sequencePaused());
        QVERIFY(repo->repo().sequenceSkippable());
        QVERIFY(work.stagedWorkWithoutPick());

        const auto skipped = work.sequenceSkip(/*revert=*/false);
        QVERIFY2(!skipped.ok(), skipped.ok() ? skipped->stderrData.c_str() : "");
        QCOMPARE(skipped.error().code(), gitbolt::git::GitErrorCode::Uncommitted);
        QCOMPARE(output(work.run({"show", ":f0.txt"})), QStringLiteral("staged work"));
        QVERIFY(QFileInfo::exists(repo->path() + QStringLiteral("/.git/sequencer")));

        // Nothing staged: Skip leaves X out, and picks Y.
        QVERIFY(succeeded(work.run({"stash", "-q"})));
        QVERIFY(!work.stagedWorkWithoutPick());
        const auto next = work.sequenceSkip(/*revert=*/false);
        QVERIFY2(succeeded(next), next.ok() ? next->stderrData.c_str() : "");
        QCOMPARE(output(work.run({"log", "--format=%s"})), QStringLiteral("Y\ncommit 0"));
    }

    // The same sequence, its staged changes then committed (git's hint
    // says to, and so did Skip's refusal): HEAD has moved, and git's
    // --continue takes X for done though it never picked it, so the bar's
    // Continue left X out with "Cherry-pick complete.". unpickedSequenceStop()
    // names X there, for Continue to ask first; at a conflict stop that
    // was resolved and committed (CHERRY_PICK_HEAD's author and date
    // kept), and for a revert that was, it names nothing.
    void unpickedSequenceStopTellsAStopGitNeverPicked() {
        const IsolatedGitConfig config;
        for (const bool revert : {false, true}) {
            auto repo = repoWithCommits(1);
            QVERIFY(repo);
            gitbolt::git::GitProcess work(repo->path().toStdString());
            QVERIFY(succeeded(work.run({"config", "core.autocrlf", "false"})));
            const std::string main = branchOf(work);
            const std::string verb = revert ? "revert" : "cherry-pick";
            // X and Y, authored a while back: a commit of the user's in
            // the same second would pass for a pick.
            const auto commit = [&](const std::string& file, const char* message) {
                repo->writeFile(QString::fromStdString(file), QByteArray(message) + '\n');
                return succeeded(work.run({"add", file}))
                    && succeeded(work.run({"commit", "-q", "--date=2020-01-02T03:04:05",
                                           "-m", message}));
            };
            if (!revert)
                QVERIFY(succeeded(work.run({"checkout", "-q", "-b", "side"})));
            QVERIFY(commit("X.txt", "X"));
            QVERIFY(commit("Y.txt", "Y"));
            const std::string y = output(work.run({"rev-parse", "HEAD"})).toStdString();
            const std::string x = output(work.run({"rev-parse", "HEAD~1"})).toStdString();
            if (!revert)
                QVERIFY(succeeded(work.run({"checkout", "-q", main})));
            const std::vector<std::string> sequence = revert
                ? std::vector<std::string>{"revert", "--no-edit", y, x}
                : std::vector<std::string>{"cherry-pick", x, y};
            // The todo's "<short id> <subject>" of the first.
            const auto namesTheFirst = [&](const std::string& unpicked) {
                const QString said = QString::fromStdString(unpicked);
                const QString id = said.section(QLatin1Char(' '), 0, 0);
                return id.size() >= 7
                    && QString::fromStdString(revert ? y : x).startsWith(id)
                    && said.section(QLatin1Char(' '), 1) == QLatin1String(revert ? "Y" : "X");
            };

            // Refused over staged changes, which are then committed.
            repo->writeFile(QStringLiteral("f0.txt"), QByteArrayLiteral("staged work\n"));
            QVERIFY(succeeded(work.run({"add", "f0.txt"})));
            QVERIFY(!succeeded(work.run(sequence)));
            QVERIFY(QFileInfo::exists(repo->path() + QStringLiteral("/.git/sequencer")));
            QVERIFY2(namesTheFirst(work.unpickedSequenceStop()), work.unpickedSequenceStop().c_str());
            QVERIFY(succeeded(work.run({"commit", "-q", "-m", "my staged work"})));
            QVERIFY(!repo->repo().sequenceSkippable());
            QVERIFY2(namesTheFirst(work.unpickedSequenceStop()), work.unpickedSequenceStop().c_str());
            QVERIFY(succeeded(work.run({verb, "--abort"})));

            // A conflict at the first: resolved and committed, it is the
            // pick (or the revert), with git's message (a terminal's
            // `git commit --no-edit`) or with words of the user's own
            // (GitBolt's Commit dialog, `git commit -F -`). A revert
            // was told by git's "This reverts commit <id>." alone, which
            // the user's words don't have: Continue then said git never
            // reverted the commit, and advised Abort. So it did once
            // that commit was amended (a typo fixed): GitBolt's note
            // named the commit the amend replaced.
            for (const QString& how : {QStringLiteral("git's message"), QStringLiteral("own words"),
                                       QStringLiteral("own words, amended")}) {
                const bool ownWords = how != QLatin1String("git's message");
                QVERIFY(succeeded(work.run({"reset", "-q", "--hard", main})));
                const QString first = QString::fromStdString(revert ? "Y.txt" : "X.txt");
                repo->writeFile(first, QByteArrayLiteral("in the way\n"));
                QVERIFY(succeeded(work.run({"add", first.toStdString()})));
                QVERIFY(succeeded(work.run({"commit", "-q", "-m", "in the way"})));
                QVERIFY(!succeeded(work.run(sequence)));
                QVERIFY(QFileInfo::exists(repo->path()
                                          + (revert ? QStringLiteral("/.git/REVERT_HEAD")
                                                    : QStringLiteral("/.git/CHERRY_PICK_HEAD"))));
                QCOMPARE(work.unpickedSequenceStop(), std::string());
                repo->writeFile(first, QByteArrayLiteral("resolved\n"));
                QVERIFY(succeeded(work.run({"add", first.toStdString()})));
                if (ownWords) {
                    const auto made = work.commit("Back out the first, my way\n");
                    QVERIFY2(succeeded(made), made.ok() ? made->stderrData.c_str()
                                                        : made.error().message().c_str());
                    QCOMPARE(output(work.run({"log", "-1", "--format=%s"})),
                             QStringLiteral("Back out the first, my way"));
                } else {
                    QVERIFY(succeeded(work.run({"commit", "-q", "--no-edit"})));
                }
                if (how == QLatin1String("own words, amended")) {
                    const auto amended = work.commit("Back out the first, typo fixed\n",
                                                     /*amend=*/true);
                    QVERIFY2(succeeded(amended), amended.ok() ? amended->stderrData.c_str()
                                                              : amended.error().message().c_str());
                    QCOMPARE(output(work.run({"log", "-1", "--format=%s"})),
                             QStringLiteral("Back out the first, typo fixed"));
                }
                QVERIFY(QFileInfo::exists(repo->path() + QStringLiteral("/.git/sequencer")));
                QVERIFY2(work.unpickedSequenceStop().empty(),
                         qPrintable(how + QStringLiteral(": ")
                                    + QString::fromStdString(work.unpickedSequenceStop())));
                QVERIFY(succeeded(work.run({verb, "--abort"})));
            }
        }
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

    // A reword whose new message a hook turns down (commit-msg here)
    // stops the rebase with the commit picked under its old message.
    // git's own --continue goes on from there, and Continue said
    // "Rebase complete." with the reword gone. It tries the plan's
    // message again first, hooks and all: refused while they still say
    // no, the rebase still stopped; taken once they don't.
    void rewordRejectedByAHookIsRetriedOnContinue() {
        const IsolatedGitConfig config;
        auto repo = repoWithCommits(1);     // commit 0
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        const auto one = repo->writeAndCommit(QStringLiteral("one.txt"), QByteArrayLiteral("one\n"),
                                              QStringLiteral("one"));
        const auto two = repo->writeAndCommit(QStringLiteral("two.txt"), QByteArrayLiteral("two\n"),
                                              QStringLiteral("two"));
        const auto onto = repo->repo().resolveRef("HEAD~2");
        QVERIFY(one.ok() && two.ok() && onto.ok());
        QVERIFY(writeHook(repo->path() + QStringLiteral("/.git/hooks"), "commit-msg",
                          "if [ -f .git/BLOCK ]; then\n"
                          "    echo 'commit-msg: BAD is not allowed' >&2; exit 1\nfi\n"));
        repo->writeFile(QStringLiteral(".git/BLOCK"), QByteArray());

        using Type = gitbolt::git::RebaseOperationType;
        gitbolt::git::RebasePlan plan;
        plan.onto = *onto;
        plan.head = *two;
        plan.branch = branchOf(work);
        plan.operations = {{Type::Pick, *two, "two"},
                           {Type::Reword, *one, "one", "one, reworded\n"}};

        GitService svc;
        SignalLog log(svc);
        MainThreadSpy steps(&svc, &GitService::rebaseStepFinished);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        const auto step = [&](const std::function<void()>& run) {
            log.clear();
            steps.clear();
            run();
            return log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"})
                && steps.size() == 1;
        };
        const auto saidNo = [&](const QString& name) {
            return log.indexOfMatch(QRegularExpression(
                       QStringLiteral("^operationFailed\\(%1: .*commit-msg: BAD is not allowed")
                           .arg(QRegularExpression::escape(name)),
                       QRegularExpression::DotMatchesEverythingOption)) >= 0;
        };

        QVERIFY2(step([&] { svc.interactiveRebase(plan); }),
                 qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(steps.at(0), (QVariantList{QStringLiteral("rebase"), false, true}));
        QVERIFY2(saidNo(QStringLiteral("rebase")), qPrintable(log.join(QStringLiteral(", "))));

        QVERIFY2(step([&] { svc.rebaseContinue(); }),
                 qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(steps.at(0), (QVariantList{QStringLiteral("rebase --continue"), false, true}));
        QVERIFY2(saidNo(QStringLiteral("rebase --continue")),
                 qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(output(work.run({"log", "-1", "--format=%s"})), QStringLiteral("one"));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::Rebase);

        QVERIFY(QFile::remove(repo->path() + QStringLiteral("/.git/BLOCK")));
        QVERIFY2(step([&] { svc.rebaseContinue(); }),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(steps.at(0), (QVariantList{QStringLiteral("rebase --continue"), true, false}));
        QCOMPARE(output(work.run({"log", "--format=%s"})),
                 QStringLiteral("two\none, reworded\ncommit 0"));
    }

    // Continue's retry of a reword knew the commit it was for by its
    // message alone. A stop before the reword's commit was made (here
    // an untracked file in its way, which git reschedules) left the
    // commit before it at HEAD, and with the same message ("wip") that
    // one was renamed: two commits came out "feature X". The retry is
    // for the reword's own commit only now; Continue then rewords it.
    void rewordRetryAmendsOnlyTheRewordsCommit() {
        const IsolatedGitConfig config;
        auto repo = repoWithCommits(1);     // commit 0
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        QVERIFY(succeeded(work.run({"config", "core.autocrlf", "false"})));
        const auto base = repo->repo().resolveRef("HEAD");
        const auto w1 = repo->writeAndCommit(QStringLiteral("a.txt"), QByteArrayLiteral("a\n"),
                                             QStringLiteral("wip"));
        const auto w2 = repo->writeAndCommit(QStringLiteral("x.txt"), QByteArrayLiteral("x\n"),
                                             QStringLiteral("wip"));
        QVERIFY(succeeded(work.run({"rm", "-q", "x.txt"})));
        QVERIFY(succeeded(work.run({"commit", "-q", "-m", "rm x"})));
        const auto w3 = repo->repo().resolveRef("HEAD");
        QVERIFY(base.ok() && w1.ok() && w2.ok() && w3.ok());
        // In the way of W2's x.txt.
        repo->writeFile(QStringLiteral("x.txt"), QByteArrayLiteral("untracked\n"));

        using Type = gitbolt::git::RebaseOperationType;
        gitbolt::git::RebasePlan plan;
        plan.onto = *base;
        plan.head = *w3;
        plan.branch = branchOf(work);
        plan.operations = {{Type::Pick, *w3, "rm x"},
                           {Type::Reword, *w2, "wip", "feature X\n"},
                           {Type::Pick, *w1, "wip"}};

        GitService svc;
        SignalLog log(svc);
        MainThreadSpy steps(&svc, &GitService::rebaseStepFinished);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        const auto step = [&](const std::function<void()>& run) {
            log.clear();
            steps.clear();
            run();
            return log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"})
                && steps.size() == 1;
        };
        QVERIFY2(step([&] { svc.interactiveRebase(plan); }),
                 qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(steps.at(0), (QVariantList{QStringLiteral("rebase"), false, true}));
        // Stopped with W1 at HEAD, W2 still to come.
        QCOMPARE(output(work.run({"rev-parse", "HEAD"})), QString::fromStdString(w1->toHex()));

        QVERIFY(QFile::remove(repo->path() + QStringLiteral("/x.txt")));
        QVERIFY2(step([&] { svc.rebaseContinue(); }),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(steps.at(0), (QVariantList{QStringLiteral("rebase --continue"), true, false}));
        QCOMPARE(output(work.run({"log", "--format=%s"})),
                 QStringLiteral("rm x\nfeature X\nwip\ncommit 0"));
        QCOMPARE(output(work.run({"log", "-1", "--format=%H", "HEAD~2"})),
                 QString::fromStdString(w1->toHex()));
    }

    // A reword a commit-msg hook turned down, in a repository whose
    // prepare-commit-msg hook adds to every message git is given (a
    // ticket number): the pick git made had the old message plus that,
    // so Continue's retry didn't know it for the reword's commit, and
    // "Rebase complete." came with the reword gone. Two rewords, so the
    // second is picked anew rather than fast-forwarded. Where HEAD keeps
    // no reflog too: the pick is told by its message there, the old one
    // with whatever the hook added.
    void rewordRetryOutlivesAPrepareCommitMsgHook_data() {
        QTest::addColumn<bool>("reflog");
        QTest::newRow("with a reflog") << true;
        QTest::newRow("with no reflog") << false;
    }

    void rewordRetryOutlivesAPrepareCommitMsgHook() {
        QFETCH(bool, reflog);
        const IsolatedGitConfig config;
        auto repo = repoWithCommits(1);     // commit 0
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        const auto one = repo->writeAndCommit(QStringLiteral("one.txt"), QByteArrayLiteral("one\n"),
                                              QStringLiteral("one"));
        const auto two = repo->writeAndCommit(QStringLiteral("two.txt"), QByteArrayLiteral("two\n"),
                                              QStringLiteral("two"));
        if (!reflog) {
            QVERIFY(succeeded(work.run({"config", "core.logAllRefUpdates", "false"})));
            QVERIFY(QDir(repo->path() + QStringLiteral("/.git/logs")).removeRecursively());
        }
        const auto onto = repo->repo().resolveRef("HEAD~2");
        QVERIFY(one.ok() && two.ok() && onto.ok());
        const QString hooks = repo->path() + QStringLiteral("/.git/hooks");
        QVERIFY(writeHook(hooks, "prepare-commit-msg",
                          "case \"$2\" in commit|'') ;; *) printf '\\n[T-1]\\n' >> \"$1\";; esac\n"));
        QVERIFY(writeHook(hooks, "commit-msg",
                          "if [ -f .git/BLOCK ] && head -1 \"$1\" | grep -q '^bad'; then\n"
                          "    echo 'commit-msg: bad is not allowed' >&2; exit 1\nfi\n"));
        repo->writeFile(QStringLiteral(".git/BLOCK"), QByteArray());

        using Type = gitbolt::git::RebaseOperationType;
        gitbolt::git::RebasePlan plan;
        plan.onto = *onto;
        plan.head = *two;
        plan.branch = branchOf(work);
        plan.operations = {{Type::Reword, *two, "two", "bad two\n"},
                           {Type::Reword, *one, "one", "one, reworded\n"}};

        GitService svc;
        SignalLog log(svc);
        MainThreadSpy steps(&svc, &GitService::rebaseStepFinished);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        const auto step = [&](const std::function<void()>& run) {
            log.clear();
            steps.clear();
            run();
            return log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"})
                && steps.size() == 1;
        };
        QVERIFY2(step([&] { svc.interactiveRebase(plan); }),
                 qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(steps.at(0), (QVariantList{QStringLiteral("rebase"), false, true}));
        // Stopped with "two" picked, under its old message and the hook's.
        const QString picked = output(work.run({"log", "-1", "--format=%B"}));
        QVERIFY2(picked.startsWith(QStringLiteral("two\n")) && picked.endsWith(QStringLiteral("[T-1]")),
                 qPrintable(picked));
        QCOMPARE(svc.committedRebaseStop(), QStringLiteral("reword"));
        QCOMPARE(QFileInfo::exists(repo->path() + QStringLiteral("/.git/logs/HEAD")), reflog);

        QVERIFY(QFile::remove(repo->path() + QStringLiteral("/.git/BLOCK")));
        QVERIFY2(step([&] { svc.rebaseContinue(); }),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(steps.at(0), (QVariantList{QStringLiteral("rebase --continue"), true, false}));
        QCOMPARE(output(work.run({"log", "--format=%s"})),
                 QStringLiteral("bad two\none, reworded\ncommit 0"));
    }

    // A reword that stopped on a conflict, carried on from a terminal:
    // `git rebase --continue` there commits with the message git kept
    // for the stop (rebase-merge/message; MERGE_MSG for a `git
    // commit`), which was the commit's old one, as only GitBolt's
    // editor knew the new. They hold the plan's message now.
    void rewordStoppedOnAConflictContinuesInATerminal() {
        const IsolatedGitConfig config;
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        QVERIFY(succeeded(work.run({"config", "core.autocrlf", "false"})));
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
        plan.operations = {{Type::Pick, clean, "side clean"},
                           {Type::Reword, clash, "side clash", "clash, reworded\n"}};

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
        {
            // Closed before git goes on: Windows lets nobody replace a
            // file another process has open (QFile shares no delete), and
            // the terminal's git then failed to write MERGE_MSG for the
            // next pick ("failed to finalize '.git/MERGE_MSG'").
            QFile mergeMsg(repo->path() + QStringLiteral("/.git/MERGE_MSG"));
            QVERIFY(mergeMsg.open(QIODevice::ReadOnly));
            QVERIFY2(mergeMsg.readAll().startsWith("clash, reworded"), "MERGE_MSG");
        }

        repo->writeFile(QStringLiteral("f0.txt"), QByteArrayLiteral("resolved\n"));
        QVERIFY(succeeded(work.run({"add", "f0.txt"})));
        // A terminal's git, with an editor that keeps what it's given.
        QProcess terminal;
        terminal.setWorkingDirectory(repo->path());
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GIT_EDITOR"), QStringLiteral("true"));
        terminal.setProcessEnvironment(env);
        terminal.start(QString::fromStdString(gitbolt::git::GitProcess::findGitExecutable()),
                       {QStringLiteral("rebase"), QStringLiteral("--continue")});
        QVERIFY(terminal.waitForFinished(60000));
        QVERIFY2(terminal.exitCode() == 0, terminal.readAllStandardError().constData());
        QCOMPARE(output(work.run({"log", "--format=%s"})),
                 QStringLiteral("side clean\nclash, reworded\nmain change\ncommit 0"));
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

    // A step whose outcome was on its way (git done, the report posted)
    // when another repository was opened: the check is made where the
    // swap happens, so nothing of it arrives after the open.
    void rebaseStepPostedBeforeASwapReportsNothing() {
        auto repo = repoWithCommits(1);
        auto other = repoWithCommits(1);
        QVERIFY(repo && other);
        GitService svc;
        SignalLog log(svc);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        // Nothing to continue: git fails at once.
        svc.rebaseContinue();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        log.clear();
        QVERIFY(svc.openRepository(other->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QTest::qWait(100);
        QVERIFY2(!log.contains(QStringLiteral("rebaseStep")) && !log.hasFailure(),
                 qPrintable(log.join(QStringLiteral(", "))));
    }

    // The same for a cherry-pick, whose outcome went out from the worker:
    // a pick that stopped on conflicts just before another repository
    // was opened said so over that one ("Cherry-pick produced
    // conflicts…"), and offered to resolve them there.
    void cherryPickPostedBeforeASwapReportsNothing() {
        auto repo = repoWithCommits(1);
        auto other = repoWithCommits(1);
        QVERIFY(repo && other);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        QVERIFY(succeeded(work.run({"checkout", "-q", "-b", "side"})));
        repo->writeFile(QStringLiteral("f0.txt"), QByteArrayLiteral("side\n"));
        QVERIFY(succeeded(work.run({"commit", "-q", "-am", "side"})));
        const auto picked = repo->repo().head();
        QVERIFY(picked.ok());
        QVERIFY(succeeded(work.run({"checkout", "-q", "-"})));
        repo->writeFile(QStringLiteral("f0.txt"), QByteArrayLiteral("main\n"));
        QVERIFY(succeeded(work.run({"commit", "-q", "-am", "main"})));

        GitService svc;
        SignalLog log(svc);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        svc.cherryPick(*picked);
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        // The pick stopped on its conflict, its outcome on its way.
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::CherryPick);
        log.clear();
        QVERIFY(svc.openRepository(other->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QTest::qWait(100);
        QVERIFY2(!log.contains(QStringLiteral("cherryPickComplete")) && !log.hasFailure(),
                 qPrintable(log.join(QStringLiteral(", "))));
    }

    // The same for a commit, which runs on a worker too (git commit and
    // its hooks): its outcome went out from the worker, so one posted
    // just before another repository was opened said "Commit created: …"
    // over that one.
    void commitPostedBeforeASwapReportsNothing() {
        const IsolatedGitConfig config;
        auto repo = repoWithCommits(1);
        auto other = repoWithCommits(1);
        QVERIFY(repo && other);
        repo->writeFile(QStringLiteral("x.txt"), QByteArrayLiteral("x\n"));
        QVERIFY(repo->stageFile(QStringLiteral("x.txt")).ok());
        gitbolt::git::GitProcess git(repo->path().toStdString());
        const QString head = output(git.run({"rev-parse", "HEAD"}));

        GitService svc;
        SignalLog log(svc);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        svc.commitChanges(QStringLiteral("posted"));
        QVERIFY(QThreadPool::globalInstance()->waitForDone(15000));
        // git made the commit, its outcome on its way.
        QCOMPARE(output(git.run({"rev-parse", "HEAD~1"})), head);
        log.clear();
        QVERIFY(svc.openRepository(other->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QTest::qWait(100);
        QVERIFY2(!log.contains(QStringLiteral("commitComplete")) && !log.hasFailure(),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY(!svc.rebaseStepRunning());
    }

    // Git maintenance (gc, prune, fsck, repack) and Git Flow ran on a
    // worker and emitted from it: an outcome posted just before another
    // repository was opened came after it, said of that one. They go to
    // the repository they ran on now, the same one opened again included.
    void maintenanceOutcomeStaysWithItsRepository() {
        auto repo = repoWithCommits(1);
        auto other = repoWithCommits(1);
        QVERIFY(repo && other);
        GitService svc;
        SignalLog log(svc);
        MainThreadSpy done(&svc, &GitService::maintenanceComplete);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        svc.runFsck();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(15000));
        QVERIFY(svc.openRepository(other->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QTest::qWait(100);
        QVERIFY2(done.isEmpty(), qPrintable(done.isEmpty() ? QString()
                                            : done.first().first().toString()));

        // One of the open repository's, which is then opened again: still
        // its own.
        svc.runFsck();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(15000));
        QVERIFY(svc.openRepository(other->path()));
        QVERIFY(done.waitForAny(10000));
        QCOMPARE(done.size(), 1);
    }

    // A commit whose outcome arrives while an open is on its way waits
    // for it, and goes to the repository open then: its own, opened
    // again (a Commit & Push then still pushes, the window's Push back
    // on by then), and nowhere if another one landed.
    void commitOutcomeArrivingDuringAnOpenWaitsForIt_data() {
        QTest::addColumn<bool>("sameRepository");
        QTest::newRow("its own repository opens again") << true;
        QTest::newRow("another repository opens") << false;
    }

    void commitOutcomeArrivingDuringAnOpenWaitsForIt() {
        QFETCH(bool, sameRepository);
        const IsolatedGitConfig config;
        auto repo = repoWithCommits(1);
        auto other = repoWithCommits(1);
        QVERIFY(repo && other);
        repo->writeFile(QStringLiteral("x.txt"), QByteArrayLiteral("x\n"));
        QVERIFY(repo->stageFile(QStringLiteral("x.txt")).ok());

        GitService svc;
        SignalLog log(svc);
        QStringList order;
        QObject context;
        connect(&svc, &GitService::repositoryOpened, &context,
                [&](const QString&) { order << QStringLiteral("opened"); });
        connect(&svc, &GitService::commitComplete, &context,
                [&](bool ok, const QString&) {
            order << (ok ? QStringLiteral("commitComplete") : QStringLiteral("commitFailed"));
        });
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        order.clear();
        // The commit is made and its outcome posted before the open
        // starts.
        svc.commitChanges(QStringLiteral("posted"));
        QVERIFY(QThreadPool::globalInstance()->waitForDone(15000));
        svc.openRepositoryAsync(sameRepository ? repo->path() : other->path());
        QVERIFY(svc.openPending());
        QVERIFY(QTest::qWaitFor([&] { return !svc.openPending(); }, 10000));
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QTest::qWait(100);
        const QStringList expected = sameRepository
            ? QStringList{QStringLiteral("opened"), QStringLiteral("commitComplete")}
            : QStringList{QStringLiteral("opened")};
        QVERIFY2(order == expected, qPrintable(order.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
    }

    // A commit whose hook ends while another repository is opening: its
    // "Commit created: …" (or the hook's refusal) came over "Opening…".
    void commitEndingDuringAnOpenWaitsForIt() {
#ifdef Q_OS_WIN
        QSKIP("holds the open with a FIFO");
#else
        const IsolatedGitConfig config;
        auto repo = repoWithCommits(1);
        auto other = repoWithCommits(1);
        QVERIFY(repo && other);
        QVERIFY(writeHook(repo->path() + QStringLiteral("/.git/hooks"), "pre-commit",
                          "while [ ! -f .git/GO ]; do sleep 0.05; done\n"));
        repo->writeFile(QStringLiteral("x.txt"), QByteArrayLiteral("x\n"));
        QVERIFY(repo->stageFile(QStringLiteral("x.txt")).ok());

        // The other repository's open is held: its config includes a
        // FIFO, which libgit2's open reads, until it is let through.
        QTemporaryDir gateDir;
        QVERIFY(gateDir.isValid());
        const QByteArray fifo = QFile::encodeName(gateDir.filePath(QStringLiteral("gate")));
        QCOMPARE(::mkfifo(fifo.constData(), 0600), 0);
        gitbolt::git::GitProcess otherGit(other->path().toStdString());
        QVERIFY(succeeded(otherGit.run({"config", "include.path", fifo.toStdString()})));
        std::atomic<bool> gateOpen{false};
        std::atomic<bool> gateDone{false};
        std::thread gate([&] {
            while (!gateDone) {
                if (gateOpen) {
                    const int fd = ::open(fifo.constData(), O_WRONLY | O_NONBLOCK);
                    if (fd >= 0)
                        ::close(fd);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });

        GitService svc;
        SignalLog log(svc);
        const auto release = qScopeGuard([&] {
            repo->writeFile(QStringLiteral(".git/GO"), QByteArray());
            gateOpen = true;
            QThreadPool::globalInstance()->waitForDone(10000);
            gateDone = true;
            gate.join();
        });
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        svc.commitChanges(QStringLiteral("held"));
        QVERIFY(svc.rebaseStepRunning());
        log.clear();
        svc.openRepositoryAsync(other->path());
        QVERIFY(svc.openPending());

        // The commit ends, the open still on its way.
        repo->writeFile(QStringLiteral(".git/GO"), QByteArray());
        gitbolt::git::GitProcess git(repo->path().toStdString());
        QVERIFY(QTest::qWaitFor([&] {
            return output(git.run({"log", "-1", "--format=%s"})) == QStringLiteral("held");
        }, 15000));
        QTest::qWait(300);
        QVERIFY(svc.openPending());
        QVERIFY2(!log.contains(QStringLiteral("commitComplete")) && !log.hasFailure(),
                 qPrintable(log.join(QStringLiteral(", "))));

        // Nor once the other repository is open.
        gateOpen = true;
        QVERIFY(QTest::qWaitFor([&] { return !svc.openPending(); }, 10000));
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QTest::qWait(100);
        QCOMPARE(svc.openKey(), other->repo().path());
        QVERIFY2(!log.contains(QStringLiteral("commitComplete")) && !log.hasFailure(),
                 qPrintable(log.join(QStringLiteral(", "))));
#endif
    }

    // A step that ends while another repository is opening: its outcome
    // came at once, the open still on its way, so the window said
    // "rebase --continue failed: … could not apply …" and asked "Open the
    // conflict resolver now?" over "Opening work-b…", and a Yes once
    // work-b had landed opened the resolver on it. The outcome now waits
    // for the open, and goes nowhere once another repository is open.
    void rebaseStepEndingDuringAnOpenWaitsForIt() {
#ifdef Q_OS_WIN
        QSKIP("holds the open with a FIFO");
#else
        auto repo = repoWithCommits(2);
        auto other = repoWithCommits(1);
        QVERIFY(repo && other);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        // --continue waits at the second exec until there's a "go".
        QVERIFY(!succeeded(work.run({"rebase", "--exec", "false", "--exec",
                                     "while [ ! -f go ]; do sleep 0.05; done", "HEAD~1"})));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::Rebase);

        // The other repository's open is held: its config includes a
        // FIFO, which libgit2's open reads, until it is let through.
        QTemporaryDir gateDir;
        QVERIFY(gateDir.isValid());
        const QByteArray fifo = QFile::encodeName(gateDir.filePath(QStringLiteral("gate")));
        QCOMPARE(::mkfifo(fifo.constData(), 0600), 0);
        gitbolt::git::GitProcess otherGit(other->path().toStdString());
        QVERIFY(succeeded(otherGit.run({"config", "include.path", fifo.toStdString()})));
        std::atomic<bool> gateOpen{false};
        std::atomic<bool> gateDone{false};
        std::thread gate([&] {
            while (!gateDone) {
                if (gateOpen) {
                    const int fd = ::open(fifo.constData(), O_WRONLY | O_NONBLOCK);
                    if (fd >= 0)
                        ::close(fd);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });

        GitService svc;
        SignalLog log(svc);
        // Let everything through on the way out, should a check fail
        // first: ~GitService waits for the open and the step.
        const auto release = qScopeGuard([&] {
            repo->writeFile(QStringLiteral("go"), QByteArrayLiteral(""));
            gateOpen = true;
            QThreadPool::globalInstance()->waitForDone(10000);
            gateDone = true;
            gate.join();
        });
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        svc.rebaseContinue();
        log.clear();
        svc.openRepositoryAsync(other->path());
        QVERIFY(svc.openPending());

        // The step ends, the open still on its way.
        repo->writeFile(QStringLiteral("go"), QByteArrayLiteral(""));
        QVERIFY(QTest::qWaitFor([&] { return !svc.rebaseStepRunning(); }, 10000));
        QTest::qWait(300);
        QVERIFY(svc.openPending());
        QVERIFY2(!log.contains(QStringLiteral("rebaseStep")) && !log.hasFailure(),
                 qPrintable(log.join(QStringLiteral(", "))));

        // Nor once the other repository is open.
        gateOpen = true;
        QVERIFY(QTest::qWaitFor([&] { return !svc.openPending(); }, 10000));
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QTest::qWait(100);
        QCOMPARE(svc.openKey(), other->repo().path());
        QVERIFY2(!log.contains(QStringLiteral("rebaseStep")) && !log.hasFailure(),
                 qPrintable(log.join(QStringLiteral(", "))));
#endif
    }

    // The same, in the order the event loop takes: the step's outcome is
    // posted, and an open starts before it arrives. Once the open is in,
    // the outcome goes to the repository that is open then: nowhere if
    // another one landed, and to its own if the open failed and left
    // that one open (it waited no more than that).
    void rebaseStepOutcomeArrivingDuringAnOpenWaitsForIt_data() {
        QTest::addColumn<bool>("openFails");
        QTest::newRow("another repository opens") << false;
        QTest::newRow("the open fails") << true;
    }

    void rebaseStepOutcomeArrivingDuringAnOpenWaitsForIt() {
        QFETCH(bool, openFails);
        auto repo = repoWithCommits(1);
        auto other = repoWithCommits(1);
        QTemporaryDir notARepo;
        QVERIFY(repo && other && notARepo.isValid());
        GitService svc;
        SignalLog log(svc);
        QStringList order;
        QObject context;
        connect(&svc, &GitService::repositoryOpened, &context,
                [&](const QString&) { order << QStringLiteral("opened"); });
        connect(&svc, &GitService::repositoryOpenFailed, &context,
                [&](const QString&, const QString&, int) { order << QStringLiteral("openFailed"); });
        connect(&svc, &GitService::rebaseStepFinished, &context,
                [&](const QString&, bool, bool) { order << QStringLiteral("rebaseStep"); });
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        order.clear();
        // Nothing to continue: git fails at once, and the outcome is
        // posted before the open starts.
        svc.rebaseContinue();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        svc.openRepositoryAsync(openFails ? notARepo.path() : other->path());
        QVERIFY(QTest::qWaitFor([&] { return !svc.openPending(); }, 10000));
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QTest::qWait(100);
        if (openFails) {
            QVERIFY2(order == (QStringList{QStringLiteral("openFailed"),
                                           QStringLiteral("rebaseStep")}),
                     qPrintable(order.join(QStringLiteral(", "))));
        } else {
            QVERIFY2(order == QStringList{QStringLiteral("opened")},
                     qPrintable(order.join(QStringLiteral(", "))));
            QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        }
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
        // Nor a commit: it is a step of its own.
        MainThreadSpy commits(&svc, &GitService::commitComplete);
        svc.commitChanges(QStringLiteral("during the step"));
        QVERIFY(commits.waitForAny());
        QCOMPARE(commits.at(0), (QVariantList{false, QStringLiteral(
            "Another git operation is still running on this repository; "
            "try again when it's done.")}));
        for (const char* op : {"rebase --skip", "rebase --abort", "abort", "resolve conflicts"})
            QVERIFY2(log.contains(QStringLiteral("operationFailed(%1: Another git operation is "
                                                 "still running on this repository; try "
                                                 "again when it's done.)")
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

    // Quitting while a rebase step runs: MainWindow calls cancelGitOps()
    // (~GitService does too), and the service's destructor then waits
    // for its jobs. A step held up by an exec or a hook that never ended
    // held the quit for as long as it ran; a second launch meanwhile
    // found nobody to hand its path to and gave up. git and what it runs
    // now get SIGINT, as on Ctrl-C: the step ends at once, "Cancelled",
    // with the rebase still in progress and no index.lock left behind.
    void quitStopsARunningRebaseStep() {
        auto repo = repoWithCommits(2);
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        // --continue waits at the second exec until there's a "go".
        QVERIFY(!succeeded(work.run({"rebase", "--exec", "false", "--exec",
                                     "while [ ! -f go ]; do sleep 0.05; done", "HEAD~1"})));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::Rebase);

        GitService svc;
        SignalLog log(svc);
        // A "go" on the way out, should a check fail first: ~GitService
        // waits for the step.
        const auto release = qScopeGuard(
            [&] { repo->writeFile(QStringLiteral("go"), QByteArrayLiteral("")); });
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        log.clear();

        svc.rebaseContinue();
        QVERIFY(waitingAtTheExec(work, repo->path()));
        svc.cancelGitOps();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
        QVERIFY2(log.waitFor({QStringLiteral("operationFailed(rebase --continue: Cancelled)"),
                              QStringLiteral("rebaseStep")}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::Rebase);
        QVERIFY(!QFileInfo::exists(
            repo->path() + QLatin1Char('/')
            + output(work.run({"rev-parse", "--git-path", "index.lock"}))));
    }

    // The same for the Rebase dialog's `git rebase -i`, here held up by
    // a post-commit hook. It ran a QProcess of its own, which nothing
    // could stop.
    void quitStopsARunningInteractiveRebase() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        QVERIFY(succeeded(work.run({"checkout", "-q", "-b", "side"})));
        auto sideTip = repo->writeAndCommit(QStringLiteral("side.txt"), QByteArrayLiteral("side\n"),
                                            QStringLiteral("side change"));
        QVERIFY(sideTip.ok());
        QVERIFY(succeeded(work.run({"checkout", "-q", "-"})));
        auto mainTip = repo->writeAndCommit(QStringLiteral("main.txt"), QByteArrayLiteral("main\n"),
                                            QStringLiteral("main change"));
        QVERIFY(mainTip.ok());
        QVERIFY(succeeded(work.run({"checkout", "-q", "side"})));

        // Picked onto main, "side change" is committed anew, and the
        // hook notes its pid (Git for Windows' sh: the Windows pid,
        // which processAlive() looks up; its own $$ is an MSYS2 one)
        // and waits for a "go".
        QTemporaryDir hooks;
        QVERIFY(hooks.isValid());
        const QString pidFile = hooks.filePath(QStringLiteral("hook.pid"));
        const QString go = hooks.filePath(QStringLiteral("go"));
        QFile hook(hooks.filePath(QStringLiteral("post-commit")));
        QVERIFY(hook.open(QIODevice::WriteOnly));
        hook.write("#!/bin/sh\n{ cat /proc/$$/winpid 2>/dev/null || echo $$; } > '"
                   + QFile::encodeName(pidFile) + "'\n"
                   "while [ ! -f '" + QFile::encodeName(go) + "' ]; do sleep 0.05; done\n");
        hook.close();
        QVERIFY(hook.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(repo->repo().config().setString("core.hooksPath",
                                                hooks.path().toStdString()).ok());

        gitbolt::git::RebasePlan plan;
        plan.onto = *mainTip;
        plan.head = *sideTip;
        plan.branch = branchOf(work);
        plan.operations = {{gitbolt::git::RebaseOperationType::Pick, *sideTip, "side change"}};

        GitService svc;
        SignalLog log(svc);
        const auto release = qScopeGuard([&] {
            QFile f(go);
            if (f.open(QIODevice::WriteOnly))
                f.close();
        });
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        log.clear();

        svc.interactiveRebase(plan);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(pidFile).size() > 0, 10000);
        QFile pids(pidFile);
        QVERIFY(pids.open(QIODevice::ReadOnly));
        const qint64 hookPid = pids.readAll().trimmed().toLongLong();
        QVERIFY(hookPid > 0);
        svc.cancelGitOps();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(5000));
        QTRY_VERIFY_WITH_TIMEOUT(!gitbolt::test::processAlive(hookPid), 3000);
        QVERIFY2(log.waitFor({QStringLiteral("operationFailed(rebase: Cancelled)"),
                              QStringLiteral("rebaseStep")}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::Rebase);
    }

    // A commit and a rebase step are one git at a time. A Continue
    // clicked while an amend's pre-commit hook ran finished the rebase
    // under it, and the amend then had nothing to amend: the new
    // message was lost. Continue is refused while the commit runs, and
    // goes on with the amended commit after.
    void commitAndStepsTakeTurns() {
        const IsolatedGitConfig config;
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        QVERIFY(succeeded(work.run({"config", "core.autocrlf", "false"})));
        const auto one = repo->writeAndCommit(QStringLiteral("one.txt"), QByteArrayLiteral("one\n"),
                                              QStringLiteral("one"));
        const auto two = repo->writeAndCommit(QStringLiteral("two.txt"), QByteArrayLiteral("two\n"),
                                              QStringLiteral("two"));
        const auto onto = repo->repo().resolveRef("HEAD~2");
        QVERIFY(one.ok() && two.ok() && onto.ok());

        using Type = gitbolt::git::RebaseOperationType;
        gitbolt::git::RebasePlan plan;
        plan.onto = *onto;
        plan.head = *two;
        plan.branch = branchOf(work);
        plan.operations = {{Type::Pick, *two, "two"}, {Type::Edit, *one, "one"}};

        GitService svc;
        SignalLog log(svc);
        MainThreadSpy steps(&svc, &GitService::rebaseStepFinished);
        MainThreadSpy started(&svc, &GitService::stepStarted);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        log.clear();
        svc.interactiveRebase(plan);
        QVERIFY2(log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(steps.at(0), (QVariantList{QStringLiteral("rebase"), true, true}));

        // Stopped at "one": a change staged, and an amend whose hook
        // waits for a "go".
        repo->writeFile(QStringLiteral("one.txt"), QByteArrayLiteral("one, amended\n"));
        QVERIFY(succeeded(work.run({"add", "one.txt"})));
        QVERIFY(writeHook(repo->path() + QStringLiteral("/.git/hooks"), "pre-commit",
                          "touch .git/hook-started\n"
                          "while [ ! -f .git/go ]; do sleep 0.05; done\n"));
        const auto release = qScopeGuard(
            [&] { repo->writeFile(QStringLiteral(".git/go"), QByteArray()); });
        log.clear();
        steps.clear();
        started.clear();
        MainThreadSpy commits(&svc, &GitService::commitComplete);
        svc.commitChanges(QStringLiteral("one, reworded"), /*amend=*/true);
        QVERIFY(started.waitForAny());
        QCOMPARE(started.at(0), (QVariantList{QStringLiteral("commit")}));
        QTRY_VERIFY_WITH_TIMEOUT(
            QFileInfo::exists(repo->path() + QStringLiteral("/.git/hook-started")), 10000);
        QVERIFY(svc.rebaseStepRunning());

        svc.rebaseContinue();
        QVERIFY2(log.waitFor({QStringLiteral("operationFailed(rebase --continue: Another git "
                                             "operation is still running on this repository; "
                                             "try again when it's done.)")}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::Rebase);

        repo->writeFile(QStringLiteral(".git/go"), QByteArray());
        QVERIFY(commits.waitForAny(15000));
        QVERIFY2(commits.at(0).at(0).toBool(), qPrintable(commits.at(0).at(1).toString()));
        QVERIFY(!svc.rebaseStepRunning());
        QVERIFY(steps.isEmpty());

        log.clear();
        svc.rebaseContinue();
        QVERIFY2(log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(steps.last(), (QVariantList{QStringLiteral("rebase --continue"), true, false}));
        QCOMPARE(output(work.run({"log", "--format=%s"})),
                 QStringLiteral("two\none, reworded\ncommit 0"));
        QCOMPARE(output(work.run({"show", "HEAD~1:one.txt"})), QStringLiteral("one, amended"));
    }

    // A merge's, cherry-pick's or revert's Abort is a step too: while it
    // puts the work tree back, a second Abort, a Continue or a commit is
    // refused rather than run as a second git (which failed on
    // index.lock, telling the user to delete it). It reports as
    // "merge --abort".
    void abortIsAStep() {
        const IsolatedGitConfig config;
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        QVERIFY(succeeded(work.run({"config", "core.autocrlf", "false"})));
        QVERIFY(succeeded(work.run({"checkout", "-q", "-b", "side"})));
        QVERIFY(repo->writeAndCommit(QStringLiteral("f0.txt"), QByteArrayLiteral("side\n"),
                                     QStringLiteral("side change")).ok());
        QVERIFY(succeeded(work.run({"checkout", "-q", "-"})));
        QVERIFY(repo->writeAndCommit(QStringLiteral("f0.txt"), QByteArrayLiteral("main\n"),
                                     QStringLiteral("main change")).ok());
        QVERIFY(!succeeded(work.run({"merge", "-q", "side"})));
        QVERIFY(succeeded(work.run({"config", "filter.slow.smudge", "sleep 2; cat"})));
        QFile attributes(repo->path() + QStringLiteral("/.git/info/attributes"));
        QVERIFY(attributes.open(QIODevice::WriteOnly));
        attributes.write("f0.txt filter=slow\n");
        attributes.close();

        GitService svc;
        SignalLog log(svc);
        MainThreadSpy steps(&svc, &GitService::rebaseStepFinished);
        MainThreadSpy started(&svc, &GitService::stepStarted);
        MainThreadSpy commits(&svc, &GitService::commitComplete);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        log.clear();

        QVERIFY(svc.abortConflictState());
        QVERIFY(started.waitForAny());
        QCOMPARE(started.at(0), (QVariantList{QStringLiteral("merge --abort")}));
        QVERIFY(svc.rebaseStepRunning());
        QVERIFY(!svc.abortConflictState());
        svc.commitChanges(QStringLiteral("during the abort"));
        QVERIFY2(log.waitFor({QStringLiteral("operationFailed(abort: Another git operation is "
                                             "still running on this repository; try again "
                                             "when it's done.)"),
                              QStringLiteral("commitComplete")}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY(!commits.at(0).at(0).toBool());

        QVERIFY2(log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"}, 15000),
                 qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(steps.size(), 1);
        QCOMPARE(steps.at(0), (QVariantList{QStringLiteral("merge --abort"), true, false}));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::None);
        QVERIFY(!QFileInfo::exists(repo->path() + QStringLiteral("/.git/index.lock")));
    }

    // The resolver's and the in-progress bar's Abort of a merge: git
    // puts the work tree back, as long as a checkout can take, on a
    // worker now; it ran on the GUI thread. The refreshes follow. Here
    // a smudge filter makes putting f0.txt back take 2 s.
    void abortConflictStateAbortsAMergeOnAWorker() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        QVERIFY(succeeded(work.run({"checkout", "-q", "-b", "side"})));
        QVERIFY(repo->writeAndCommit(QStringLiteral("f0.txt"), QByteArrayLiteral("side\n"),
                                     QStringLiteral("side change")).ok());
        QVERIFY(succeeded(work.run({"checkout", "-q", "-"})));
        QVERIFY(repo->writeAndCommit(QStringLiteral("f0.txt"), QByteArrayLiteral("main\n"),
                                     QStringLiteral("main change")).ok());
        QVERIFY(!succeeded(work.run({"merge", "-q", "side"})));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::Merge);
        QVERIFY(succeeded(work.run({"config", "filter.slow.smudge", "sleep 2; cat"})));
        QFile attributes(repo->path() + QStringLiteral("/.git/info/attributes"));
        QVERIFY(attributes.open(QIODevice::WriteOnly));
        attributes.write("f0.txt filter=slow\n");
        attributes.close();

        GitService svc;
        SignalLog log(svc);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        log.clear();
        QElapsedTimer timer;
        timer.start();
        QVERIFY(svc.abortConflictState());
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QString::number(timer.elapsed())));
        QVERIFY2(log.waitFor({"status", "log", "branches"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::None);
    }

    // A conflict on a symbolic link, which git leaves in the work tree as
    // one side's link: here HEAD's, pointing at a file outside the
    // repository. The resolver wrote its result to the work tree's path,
    // and so through the link into that file, and staging the path then
    // kept the link: the file was overwritten and the resolution lost.
    // The resolver is told the conflict is a link's (its sides' modes).
    // Text that is neither side is refused now, changing nothing, and
    // the side picked is staged and written as the link it is.
    void resolvingALinkConflictNeverWritesThroughTheLink() {
#ifdef Q_OS_WIN
        QSKIP("QFile::link makes a shortcut on Windows, not a link");
#else
        const IsolatedGitConfig config;
        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        const QString marker = outside.filePath(QStringLiteral("MARKER"));
        {
            QFile file(marker);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("original marker\n");
        }
        gitbolt::test::TestRepo repo;
        gitbolt::git::GitProcess work(repo.path().toStdString());
        QVERIFY(stageLink(repo, QStringLiteral("link"), QStringLiteral("a")));
        QVERIFY(repo.commit(QStringLiteral("link to a")).ok());
        QVERIFY(succeeded(work.run({"checkout", "-q", "-b", "side"})));
        QVERIFY(stageLink(repo, QStringLiteral("link"), QStringLiteral("b")));
        QVERIFY(repo.commit(QStringLiteral("link to b")).ok());
        QVERIFY(succeeded(work.run({"checkout", "-q", "-"})));
        QVERIFY(stageLink(repo, QStringLiteral("link"), marker));
        QVERIFY(repo.commit(QStringLiteral("link out")).ok());
        QVERIFY(!succeeded(work.run({"merge", "-q", "side"})));
        const QString link = QDir(repo.path()).filePath(QStringLiteral("link"));
        QCOMPARE(linkTarget(link), marker);

        GitService svc;
        SignalLog log(svc);
        QObject receiver;
        std::vector<gitbolt::git::MergeConflictEntry> conflicts;
        QObject::connect(&svc, &GitService::conflictsReady, &receiver,
                         [&](std::vector<gitbolt::git::MergeConflictEntry> entries) {
            conflicts = std::move(entries);
        });
        QVERIFY(svc.openRepository(repo.path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        svc.refreshConflicts();
        QVERIFY(QTest::qWaitFor([&] { return !conflicts.empty(); }, 10000));
        QCOMPARE(conflicts.size(), size_t(1));
        QVERIFY(conflicts[0].isSymlink());
        QCOMPARE(conflicts[0].oursMode, 0120000u);
        QCOMPARE(conflicts[0].theirsMode, 0120000u);
        QCOMPARE(QString::fromStdString(conflicts[0].oursContent), marker);
        QCOMPARE(QString::fromStdString(conflicts[0].theirsContent), QStringLiteral("b"));

        log.clear();
        QVERIFY(svc.resolveConflicts({{QStringLiteral("link"), QStringLiteral("edited\n")}}));
        QVERIFY2(log.contains(QStringLiteral(
                     "operationFailed(resolve conflicts: link is a symbolic link: "
                     "accept Ours or Theirs for it, as they are.)")),
                 qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(fileBytes(marker), QByteArrayLiteral("original marker\n"));
        QCOMPARE(linkTarget(link), marker);
        QCOMPARE(output(work.run({"ls-files", "-u", "--", "link"})).count(QLatin1Char('\n')), 2);

        // Accept Theirs.
        log.clear();
        QVERIFY(svc.resolveConflicts({{QStringLiteral("link"), QStringLiteral("b")}}));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(fileBytes(marker), QByteArrayLiteral("original marker\n"));
        QCOMPARE(output(work.run({"ls-files", "-s", "--", "link"})),
                 QStringLiteral("120000 %1 0\tlink")
                     .arg(output(work.run({"rev-parse", "side:link"}))));
        QCOMPARE(linkTarget(link), QStringLiteral("b"));
        QCOMPARE(output(work.run({"status", "--porcelain", "--", "link"})),
                 QStringLiteral("M  link"));
#endif
    }

    // The same in a rebase, which leaves the upstream's link (ours: the
    // commit the rebase builds on) in the work tree. Mark All Resolved
    // takes ours for a conflict nobody looked at: it is staged as the
    // link it is, and the file it points to stays as it was.
    void resolvingALinkConflictInARebaseKeepsOursAsALink() {
#ifdef Q_OS_WIN
        QSKIP("QFile::link makes a shortcut on Windows, not a link");
#else
        const IsolatedGitConfig config;
        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        const QString marker = outside.filePath(QStringLiteral("MARKER"));
        {
            QFile file(marker);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("original marker\n");
        }
        gitbolt::test::TestRepo repo;
        gitbolt::git::GitProcess work(repo.path().toStdString());
        QVERIFY(stageLink(repo, QStringLiteral("link"), QStringLiteral("a")));
        QVERIFY(repo.commit(QStringLiteral("link to a")).ok());
        const QString upstream = output(work.run({"branch", "--show-current"}));
        QVERIFY(succeeded(work.run({"checkout", "-q", "-b", "topic"})));
        QVERIFY(stageLink(repo, QStringLiteral("link"), QStringLiteral("b")));
        QVERIFY(repo.commit(QStringLiteral("link to b")).ok());
        QVERIFY(succeeded(work.run({"checkout", "-q", upstream.toStdString()})));
        QVERIFY(stageLink(repo, QStringLiteral("link"), marker));
        QVERIFY(repo.commit(QStringLiteral("link out")).ok());
        QVERIFY(succeeded(work.run({"checkout", "-q", "topic"})));
        QVERIFY(!succeeded(work.run({"rebase", upstream.toStdString()})));
        QCOMPARE(repo.repo().state(), gitbolt::git::RepoState::Rebase);
        const QString link = QDir(repo.path()).filePath(QStringLiteral("link"));
        QCOMPARE(linkTarget(link), marker);

        GitService svc;
        SignalLog log(svc);
        QVERIFY(svc.openRepository(repo.path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        log.clear();
        QVERIFY(svc.resolveConflicts({{QStringLiteral("link"), marker}}));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(fileBytes(marker), QByteArrayLiteral("original marker\n"));
        QCOMPARE(output(work.run({"ls-files", "-s", "--", "link"})),
                 QStringLiteral("120000 %1 0\tlink")
                     .arg(output(work.run({"rev-parse", "HEAD:link"}))));
        QCOMPARE(linkTarget(link), marker);
        QCOMPARE(output(work.run({"status", "--porcelain", "--", "link"})), QString());
#endif
    }

    // A text conflict resolves as it always has: the result is the work
    // tree's file and the index's, the conflict is gone (kept for
    // resolve-undo, as `git add` keeps it), and an executable is still
    // one. Each is now staged first and then written from the index.
    void resolvingATextConflictWritesAndStagesTheResult() {
        const IsolatedGitConfig config;
        gitbolt::test::TestRepo repo;
        gitbolt::git::GitProcess work(repo.path().toStdString());
        // Git for Windows' system config has core.autocrlf on, and the
        // checks compare bytes.
        QVERIFY(succeeded(work.run({"config", "core.autocrlf", "false"})));
        const auto commitBoth = [&](const QByteArray& text, const QString& message) {
            repo.writeFile(QStringLiteral("a.txt"), text + '\n');
            repo.writeFile(QStringLiteral("run.sh"), "#!/bin/sh\necho " + text + '\n');
            // Executable in the work tree, and in the index where git
            // ignores the work tree's modes (core.fileMode off, Windows).
            return QFile::setPermissions(repo.path() + QStringLiteral("/run.sh"),
                                         QFile::permissions(repo.path() + QStringLiteral("/run.sh"))
                                             | QFile::ExeOwner)
                && succeeded(work.run({"add", "a.txt", "run.sh"}))
                && succeeded(work.run({"update-index", "--chmod=+x", "run.sh"}))
                && repo.commit(message).ok();
        };
        QVERIFY(commitBoth("base", QStringLiteral("base")));
        QVERIFY(succeeded(work.run({"checkout", "-q", "-b", "side"})));
        QVERIFY(commitBoth("side", QStringLiteral("side")));
        QVERIFY(succeeded(work.run({"checkout", "-q", "-"})));
        QVERIFY(commitBoth("main", QStringLiteral("main")));
        QVERIFY(!succeeded(work.run({"merge", "-q", "side"})));
        QCOMPARE(output(work.run({"diff", "--name-only", "--diff-filter=U"})),
                 QStringLiteral("a.txt\nrun.sh"));

        GitService svc;
        SignalLog log(svc);
        QVERIFY(svc.openRepository(repo.path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        log.clear();
        QVERIFY(svc.resolveConflicts(
            {{QStringLiteral("a.txt"), QStringLiteral("resolved\n")},
             {QStringLiteral("run.sh"), QStringLiteral("#!/bin/sh\necho resolved\n")}}));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));

        QCOMPARE(fileBytes(repo.path() + QStringLiteral("/a.txt")), QByteArrayLiteral("resolved\n"));
        QCOMPARE(fileBytes(repo.path() + QStringLiteral("/run.sh")),
                 QByteArrayLiteral("#!/bin/sh\necho resolved\n"));
        QCOMPARE(output(work.run({"show", ":a.txt"})), QStringLiteral("resolved"));
        QCOMPARE(output(work.run({"show", ":run.sh"})), QStringLiteral("#!/bin/sh\necho resolved"));
        QCOMPARE(output(work.run({"ls-files", "-u"})), QString());
        const QStringList staged = output(work.run({"ls-files", "-s", "--", "a.txt", "run.sh"}))
                                       .split(QLatin1Char('\n'));
        QCOMPARE(staged.size(), 2);
        QVERIFY2(staged[0].startsWith(QLatin1String("100644 ")), qPrintable(staged[0]));
        QVERIFY2(staged[1].startsWith(QLatin1String("100755 ")), qPrintable(staged[1]));
        // Staged, and the work tree the same.
        QCOMPARE(output(work.run({"status", "--porcelain"})), QStringLiteral("M  a.txt\nM  run.sh"));
        QVERIFY(!output(work.run({"ls-files", "--resolve-undo"})).isEmpty());
#ifndef Q_OS_WIN
        QVERIFY(QFileInfo(repo.path() + QStringLiteral("/run.sh")).isExecutable());
#endif
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
        svc.cherryPick(picked.value());
        QVERIFY2(log.waitForAfter(QStringLiteral("cherryPickComplete"), {"status", "log"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(sideStaged, "the status refresh ran before the cherry-pick applied");
    }

    // A pick of a commit whose changes are on the branch already
    // changes nothing. It said "Cherry-pick completed successfully." and
    // left the pick in progress, the bar offering Commit, which git then
    // refused ("The previous cherry-pick is now empty…", advising
    // --allow-empty or `git cherry-pick --skip`, neither of which
    // GitBolt offers). It says so now, and nothing is left in progress.
    void cherryPickOfAChangeAlreadyOnTheBranchLeavesNothingInProgress() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        QVERIFY(succeeded(work.run({"checkout", "-q", "-b", "side"})));
        QVERIFY(repo->writeAndCommit(QStringLiteral("b.txt"), QByteArrayLiteral("b\n"),
                                     QStringLiteral("add b")).ok());
        auto picked = repo->repo().head();
        QVERIFY(picked.ok());
        QVERIFY(succeeded(work.run({"checkout", "-q", "-"})));
        QVERIFY(repo->writeAndCommit(QStringLiteral("b.txt"), QByteArrayLiteral("b\n"),
                                     QStringLiteral("b, the same")).ok());
        const QString head = output(work.run({"rev-parse", "HEAD"}));

        GitService svc;
        SignalLog log(svc);
        MainThreadSpy picks(&svc, &GitService::cherryPickComplete);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        svc.cherryPick(picked.value());
        QVERIFY(picks.waitForAny(10000));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(picks.size(), 1);
        QCOMPARE(picks.at(0).at(0).toBool(), true);
        QVERIFY2(picks.at(0).at(1).toString().contains(QStringLiteral("already on this branch")),
                 qPrintable(picks.at(0).at(1).toString()));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::None);
        QVERIFY(!QFileInfo::exists(repo->path() + QStringLiteral("/.git/CHERRY_PICK_HEAD")));
        QVERIFY(!QFileInfo::exists(repo->path() + QStringLiteral("/.git/MERGE_MSG")));
        QCOMPARE(output(work.run({"rev-parse", "HEAD"})), head);
        QCOMPARE(output(work.run({"status", "--porcelain"})), QString());
    }

    // A pick while an earlier one waits for its commit is refused
    // (Repository::cherryPick has the why), with a failure alone: its
    // cherryPickComplete(false) offered to resolve "the cherry-pick's"
    // conflicts, which would be the earlier pick's, or a merge's.
    void cherryPickDuringAPickIsRefused() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        // Through git itself: TestRepo's commits reuse an index that the
        // checkouts change behind its back.
        QVERIFY(succeeded(work.run({"checkout", "-q", "-b", "side"})));
        repo->writeFile(QStringLiteral("a.txt"), QByteArrayLiteral("a\n"));
        QVERIFY(succeeded(work.run({"add", "a.txt"})));
        QVERIFY(succeeded(work.run({"commit", "-q", "-m", "A"})));
        auto a = repo->repo().head();
        repo->writeFile(QStringLiteral("b.txt"), QByteArrayLiteral("b\n"));
        QVERIFY(succeeded(work.run({"add", "b.txt"})));
        QVERIFY(succeeded(work.run({"commit", "-q", "-m", "B"})));
        auto b = repo->repo().head();
        QVERIFY(a.ok() && b.ok());
        QVERIFY(succeeded(work.run({"checkout", "-q", "-"})));
        const auto picked = repo->repo().cherryPick(*a);
        QVERIFY2(picked.ok(), picked.ok() ? "" : picked.error().message().c_str());
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::CherryPick);

        GitService svc;
        SignalLog log(svc);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        log.clear();
        svc.cherryPick(*b);
        const QString refused = QStringLiteral(
            "operationFailed(cherryPick: A cherry-pick is in progress. Finish or abort it first.)");
        QVERIFY2(log.waitFor({refused}), qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));
        QTest::qWait(100);
        QVERIFY2(!log.contains(QStringLiteral("cherryPickComplete")),
                 qPrintable(log.join(QStringLiteral(", "))));
        QCOMPARE(output(work.run({"rev-parse", "CHERRY_PICK_HEAD"})),
                 QString::fromStdString(a->toHex()));
    }

    // -----------------------------------------------------------------
    // `git cherry-pick A B C` (or a revert of several) from a terminal,
    // stopped at B. Once B is committed, here through the Commit dialog's
    // git commit, the sequence is paused between commits: the state
    // goes out as such, and Continue runs git's --continue, which picks
    // C. Nothing ran it, and with nothing to commit the bar's Commit…
    // led nowhere. Skip leaves B out, after a `git reset --hard` there,
    // or at the stop itself. Once B is committed, git's --skip has
    // "nothing to skip": the state says so, and the bar offers none.
    // -----------------------------------------------------------------
    void sequenceStepsPickTheRest_data() {
        QTest::addColumn<bool>("skip");
        QTest::addColumn<bool>("atTheStop");
        QTest::newRow("continue after committing") << false << false;
        QTest::newRow("skip after a reset") << true << false;
        QTest::newRow("skip at the stop") << true << true;
    }

    void sequenceStepsPickTheRest() {
        QFETCH(bool, skip);
        QFETCH(bool, atTheStop);
        const IsolatedGitConfig config;
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        gitbolt::git::GitProcess work(repo->path().toStdString());
        QVERIFY(succeeded(work.run({"config", "core.autocrlf", "false"})));
        QVERIFY(succeeded(work.run({"checkout", "-q", "-b", "side"})));
        for (const char* name : {"A", "B", "C"}) {
            // B conflicts with main.
            const QString file = qstrcmp(name, "B") == 0 ? QStringLiteral("f0.txt")
                                                          : QString::fromLatin1(name) + ".txt";
            repo->writeFile(file, QByteArray(name) + '\n');
            QVERIFY(succeeded(work.run({"add", file.toStdString()})));
            QVERIFY(succeeded(work.run({"commit", "-q", "-m", name})));
        }
        QVERIFY(succeeded(work.run({"checkout", "-q", "-"})));
        repo->writeFile(QStringLiteral("f0.txt"), QByteArrayLiteral("main\n"));
        QVERIFY(succeeded(work.run({"commit", "-q", "-am", "main"})));
        QVERIFY(!succeeded(work.run({"cherry-pick", "side~2", "side~1", "side"})));

        GitService svc;
        SignalLog log(svc);
        MainThreadSpy states(&svc, &GitService::repoStateReady);
        MainThreadSpy steps(&svc, &GitService::rebaseStepFinished);
        QVERIFY(svc.openRepository(repo->path()));
        QVERIFY(log.waitFor({"status", "log", "branches"}));
        QVERIFY(states.waitForAny());
        // repoStateReady's state, conflicts, sequencePaused and
        // sequenceSkippable.
        const auto lastState = [&] {
            const QVariantList& s = states.last();
            return QStringLiteral("%1 %2 %3 %4")
                .arg(static_cast<int>(s.at(0).value<gitbolt::git::RepoState>()))
                .arg(s.at(1).toInt())
                .arg(s.at(2).toBool() ? QStringLiteral("paused") : QStringLiteral("-"))
                .arg(s.at(3).toBool() ? QStringLiteral("skippable") : QStringLiteral("-"));
        };
        const int cherryPick = static_cast<int>(gitbolt::git::RepoState::CherryPick);
        // Stopped at B, which waits for its commit, or can be left out.
        QCOMPARE(lastState(), QStringLiteral("%1 1 - skippable").arg(cherryPick));

        if (atTheStop) {
            // Skipped from there.
        } else if (skip) {
            QVERIFY(succeeded(work.run({"reset", "-q", "--hard"})));
            svc.refreshStatus();
        } else {
            repo->writeFile(QStringLiteral("f0.txt"), QByteArrayLiteral("resolved\n"));
            QVERIFY(repo->stageFile(QStringLiteral("f0.txt")).ok());
            MainThreadSpy done(&svc, &GitService::commitComplete);
            svc.commitChanges(QStringLiteral("B"));
            QVERIFY(done.waitForAny(15000));
            QVERIFY2(done.at(0).at(0).toBool(), qPrintable(done.at(0).at(1).toString()));
        }
        if (!atTheStop) {
            // Committed, B is past skipping: HEAD moved on from where
            // git's sequencer left it (git: "there is nothing to skip").
            const QString paused = QStringLiteral("%1 0 paused %2")
                                       .arg(cherryPick)
                                       .arg(skip ? QStringLiteral("skippable")
                                                 : QStringLiteral("-"));
            QVERIFY2(QTest::qWaitFor([&] { return lastState() == paused; }, 10000),
                     qPrintable(lastState()));
            if (!skip) {
                const auto refused = work.run({"cherry-pick", "--skip"});
                QVERIFY(refused.ok() && !refused->success());
                QVERIFY2(QString::fromStdString(refused->stderrData)
                             .contains(QLatin1String("nothing to skip")),
                         refused->stderrData.c_str());
            }
        }

        log.clear();
        skip ? svc.sequenceSkip() : svc.sequenceContinue();
        QVERIFY2(log.waitForAfter(QStringLiteral("rebaseStep"), {"status", "log"}),
                 qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY2(!log.hasFailure(), qPrintable(log.join(QStringLiteral(", "))));
        const QString step = skip ? QStringLiteral("cherry-pick --skip")
                                  : QStringLiteral("cherry-pick --continue");
        QCOMPARE(steps.last(), (QVariantList{step, true, false}));
        QCOMPARE(repo->repo().state(), gitbolt::git::RepoState::None);
        QVERIFY(!QFileInfo::exists(repo->path() + QStringLiteral("/.git/sequencer")));
        QCOMPARE(output(work.run({"log", "-4", "--format=%s"})),
                 skip ? QStringLiteral("C\nA\nmain\ncommit 0")
                      : QStringLiteral("C\nB\nA\nmain"));
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
    void cancelGitOpsStopsAStalledFetch() {
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
            svc.cancelGitOps();
            fetch.waitForFinished();
        });
        QTRY_VERIFY_WITH_TIMEOUT(readStallPids(pidFile).stub > 0, 15000);
        const qint64 stub = readStallPids(pidFile).stub;
        QVERIFY(processAlive(stub));
        QVERIFY(!fetch.isFinished());

        QElapsedTimer timer;
        timer.start();
        svc.cancelGitOps();
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
    // flag ends that fetch promptly, as cancelGitOps() would, without
    // cancelling later ops; and cancelGitOps() still stops a fetch
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
            svc.cancelGitOps();
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

        // Only that fetch: the next one runs, and cancelGitOps()
        // stops it though it has an unset flag of its own.
        log.clear();
        fetch = stalledFetch(std::make_shared<std::atomic<bool>>(false));
        stub = readStallPids(pidFile).stub;
        QVERIFY2(stub > 0, qPrintable(log.join(QStringLiteral(", "))));
        QVERIFY(!fetch.isFinished());

        timer.restart();
        svc.cancelGitOps();
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

    // commitChanges runs git on a worker, which asks for the refreshes
    // once it has reported; the UI (CommitDialog, MainWindow) relies on
    // commitComplete reaching it before the refreshed status.
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

    // -----------------------------------------------------------------
    // The sidebar's "New Branch from '<branch>'" names where the new
    // branch starts: it started at HEAD all the same, and work on it
    // then sat on the wrong base. The branch it names, as a branch (a
    // tag of the same name is another commit); HEAD when none is named,
    // as for the toolbar's New Branch; a start point that's gone is
    // reported, and nothing made.
    // -----------------------------------------------------------------
    void createBranchStartsWhereAsked() {
        auto repo = repoWithCommits(1);
        QVERIFY(repo);
        auto atTag = repo->repo().head();
        QVERIFY(atTag.ok());
        QVERIFY(repo->repo().createTag("feature", atTag.value(), "release").ok());
        auto onBranch = repo->writeAndCommit(QStringLiteral("f0.txt"),
                                             QByteArrayLiteral("on branch\n"),
                                             QStringLiteral("branch tip"));
        QVERIFY(onBranch.ok());
        QVERIFY(repo->repo().createBranch("feature", onBranch.value()).ok());
        auto mainTip = repo->writeAndCommit(QStringLiteral("f0.txt"),
                                            QByteArrayLiteral("on main\n"),
                                            QStringLiteral("main tip"));
        QVERIFY(mainTip.ok());

        GitService svc;
        QSignalSpy failedSpy(&svc, &GitService::operationFailed);
        QVERIFY(svc.openRepository(repo->path()));

        svc.createBranch(QStringLiteral("from-feature"), QStringLiteral("feature"));
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(tip(repo->repo(), "refs/heads/from-feature"), onBranch->toHex());
        svc.createBranch(QStringLiteral("from-head"));
        QCOMPARE(failedSpy.count(), 0);
        QCOMPARE(tip(repo->repo(), "refs/heads/from-head"), mainTip->toHex());
        svc.createBranch(QStringLiteral("from-nothing"), QStringLiteral("gone"));
        QCOMPARE(failedSpy.count(), 1);
        QCOMPARE(failedSpy.at(0).at(0).toString(), QStringLiteral("create branch"));
        QVERIFY(!repo->repo().resolveRef("refs/heads/from-nothing").ok());
        QCOMPARE(tip(repo->repo(), "HEAD"), mainTip->toHex());
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

    // Whether a rebase step's git got to the exec that waits for a
    // "go" within 10 s (git lists a command as done as it starts it).
    static bool waitingAtTheExec(const gitbolt::git::GitProcess& work, const QString& path) {
        QFile done(path + QLatin1Char('/')
                   + output(work.run({"rev-parse", "--git-path", "rebase-merge/done"})));
        return QTest::qWaitFor([&] {
            if (!done.open(QIODevice::ReadOnly))
                return false;
            const bool waiting = done.readAll().contains("while [ ! -f go ]");
            done.close();
            return waiting;
        }, 10000);
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
