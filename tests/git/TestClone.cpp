//
// TestClone — GitProcess::clone, the Clone dialog's engine: `git clone
// --progress` through QProcess, its stderr parsed into CloneProgress.
//
//   - CloneProgressParser against canned git output: a real capture
//     fed whole, byte by byte and in odd chunks (git separates updates
//     with '\r', so a pipe read can end anywhere inside one), sideband
//     padding / ANSI suffixes, CRLF, size units, the error-line tail.
//   - Real clones of a local bare repo over file://: progress phases,
//     the resulting repository and HEAD.
//   - Failure: git's own "fatal:" text comes back and the destination
//     is put back the way it was (gone, or empty if it pre-existed);
//     a non-empty destination is refused before git runs. A checkout
//     that fails after a complete fetch keeps the repository, as git
//     does.
//   - Cancel: against a transport that never answers (StallTransport
//     via ext::), returns promptly, cleans up, and leaves no process
//     of git's tree behind — also when git's own cleanup can't
//     delete everything (Unix: a directory without write permission).
//   - redactUrl, which hides URL credentials from the command log.
//
// These tests need a git binary on PATH — unlike the libgit2 suites,
// the code under test is the CLI path itself.
//

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include "../TestRepoHelper.h"
#include "git/CloneProgress.h"
#include "git/GitProcess.h"
#include "git/Repository.h"

#include <atomic>
#include <memory>
#include <thread>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#endif

using gitbolt::git::CloneProgress;
using gitbolt::git::CloneProgressParser;
using gitbolt::git::GitProcess;
using Phase = gitbolt::git::CloneProgress::Phase;

namespace {

// Trimmed from a real `git clone --progress file://…` (git 2.39)
// stderr, byte for byte — including the sideband's 8-space padding
// and the "remote: Total" line that lands in the middle of
// "Receiving objects" — plus the "Updating files" phase git adds for
// a checkout slower than ~2 s.
const char kRealClone[] =
    "Cloning into 'out1'...\n"
    "remote: Enumerating objects: 812, done.        \n"
    "remote: Counting objects:   0% (1/812)        \r"
    "remote: Counting objects:  50% (406/812)        \r"
    "remote: Counting objects: 100% (812/812)        \r"
    "remote: Counting objects: 100% (812/812), done.        \n"
    "remote: Compressing objects:   0% (1/812)        \r"
    "remote: Compressing objects: 100% (812/812), done.        \n"
    "Receiving objects:   0% (1/812)\r"
    "Receiving objects:  50% (406/812)\r"
    "Receiving objects:  87% (707/812)\r"
    "remote: Total 812 (delta 505), reused 0 (delta 0), pack-reused 0        \n"
    "Receiving objects:  88% (715/812)\r"
    "Receiving objects: 100% (812/812)\r"
    "Receiving objects: 100% (812/812), 44.25 KiB | 4.92 MiB/s, done.\n"
    "Resolving deltas:   0% (0/505)\r"
    "Resolving deltas:  50% (253/505)\r"
    "Resolving deltas: 100% (505/505)\r"
    "Resolving deltas: 100% (505/505), done.\n"
    "Updating files:  50% (150/300)\r"
    "Updating files: 100% (300/300)\r"
    "Updating files: 100% (300/300), done.\n";

// Any entry makes a directory non-empty — hidden ones included.
constexpr QDir::Filters kAnyEntry =
    QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;

// rwx for the owner only: not what mkpath() creates, so a folder that
// was deleted and re-created instead of emptied shows.
constexpr QFile::Permissions kOwnerOnly = QFile::ReadOwner | QFile::WriteOwner
    | QFile::ExeOwner | QFile::ReadUser | QFile::WriteUser | QFile::ExeUser;

bool sameProgress(const CloneProgress& a, const CloneProgress& b)
{
    return a.phase == b.phase && a.receivedObjects == b.receivedObjects
        && a.indexedObjects == b.indexedObjects && a.totalObjects == b.totalObjects
        && a.indexedDeltas == b.indexedDeltas && a.totalDeltas == b.totalDeltas
        && a.receivedBytes == b.receivedBytes && a.completedSteps == b.completedSteps
        && a.totalSteps == b.totalSteps;
}

std::vector<CloneProgress> parseInChunks(std::string_view text, std::size_t chunk)
{
    CloneProgressParser parser;
    std::vector<CloneProgress> all;
    for (std::size_t i = 0; i < text.size(); i += chunk) {
        auto got = parser.feed(text.substr(i, chunk));
        all.insert(all.end(), got.begin(), got.end());
    }
    auto tail = parser.finish();
    all.insert(all.end(), tail.begin(), tail.end());
    return all;
}

QString fileUrl(const QString& path)
{
    return QUrl::fromLocalFile(path).toString();
}

// Set an environment variable for one scope; git children inherit it
// (GitProcess builds their environment from the live process env).
class ScopedEnv {
public:
    ScopedEnv(const char* name, const QByteArray& value)
        : name_(name), had_(qEnvironmentVariableIsSet(name)), old_(qgetenv(name))
    {
        qputenv(name, value);
    }
    ~ScopedEnv()
    {
        if (had_)
            qputenv(name_, old_);
        else
            qunsetenv(name_);
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* name_;
    bool had_;
    QByteArray old_;
};

bool processAlive(qint64 pid)
{
#if defined(Q_OS_WIN)
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h)
        return false;
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

#ifdef GITBOLT_TEST_STALL_TRANSPORT
// A clone of a "server" that never answers (StallTransport, run by git
// via ext::), on a worker thread: it sits connected until cancelled.
// The destructor cancels and joins, so every exit path of a test —
// QVERIFY failures included — leaves no thread or process behind.
class StalledClone {
public:
    StalledClone(const QString& dest, const QString& pidFile)
        // ext:: is off by default (protocol.ext.allow=never).
        : allow_("GIT_ALLOW_PROTOCOL", "ext"),
          pidEnv_("GITBOLT_TEST_STALL_PIDFILE", QFile::encodeName(pidFile)),
          pidFile_(pidFile)
    {
        // In an ext:: command '%' escapes and a space separates arguments.
        QString stub = QDir::toNativeSeparators(QStringLiteral(GITBOLT_TEST_STALL_TRANSPORT));
        stub.replace(QLatin1Char('%'), QStringLiteral("%%"));
        stub.replace(QLatin1Char(' '), QStringLiteral("% "));
        worker_ = std::thread([this, url = "ext::" + stub.toStdString(),
                               path = dest.toStdString()] {
            result_ = GitProcess::clone(url, path, nullptr, cancel_);
        });
    }
    ~StalledClone()
    {
        cancel();
        if (worker_.joinable())
            worker_.join();
    }
    StalledClone(const StalledClone&) = delete;
    StalledClone& operator=(const StalledClone&) = delete;

    /// The stub has started: git has created the destination and is
    /// connected (to nothing). Reads the pids the stub recorded.
    bool stubRunning()
    {
        QFile f(pidFile_);
        if (!f.open(QIODevice::ReadOnly))
            return false;
        const QList<QByteArray> lines = f.readAll().split('\n');
        stubPid_ = lines.value(0).trimmed().toLongLong();
        gitPid_ = lines.value(1).trimmed().toLongLong();
        return stubPid_ > 0;
    }
    /// git's grandchild, where git-remote-https and ssh live.
    qint64 stubPid() const { return stubPid_; }
    /// Unix: the stub's process group, whose leader is git itself.
    qint64 gitPid() const { return gitPid_; }

    void cancel() { cancel_->store(true); }
    /// Joins the worker and returns what GitProcess::clone returned.
    const gitbolt::git::Result<void>& wait()
    {
        worker_.join();
        return result_;
    }

private:
    ScopedEnv allow_;
    ScopedEnv pidEnv_;
    QString pidFile_;
    qint64 stubPid_ = 0;
    qint64 gitPid_ = 0;
    std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);
    gitbolt::git::Result<void> result_;
    std::thread worker_;
};
#endif

#if !defined(Q_OS_WIN)
// A file in a directory without write permission: nothing can delete
// it — git's cleanup and QDir::removeRecursively() included — until
// unlock(), which the destructor also does so QTemporaryDir can clean
// up after a failed test.
class UndeletableEntry {
public:
    explicit UndeletableEntry(const QString& parent)
        : dir_(QDir(parent).filePath(QStringLiteral("locked"))),
          file_(QDir(dir_).filePath(QStringLiteral("f"))) {}
    ~UndeletableEntry() { unlock(); }
    UndeletableEntry(const UndeletableEntry&) = delete;
    UndeletableEntry& operator=(const UndeletableEntry&) = delete;

    bool create()
    {
        QFile f(file_);
        if (!QDir().mkpath(dir_) || !f.open(QIODevice::WriteOnly))
            return false;
        f.close();
        return QFile::setPermissions(dir_, QFile::ReadOwner | QFile::ExeOwner
                                               | QFile::ReadUser | QFile::ExeUser);
    }
    /// False for root, whom the missing write permission doesn't stop.
    bool binds() const
    {
        return !QFile(QDir(dir_).filePath(QStringLiteral("probe"))).open(QIODevice::WriteOnly);
    }
    bool exists() const { return QFileInfo::exists(file_); }
    void unlock() { QFile::setPermissions(dir_, kOwnerOnly); }

private:
    QString dir_;
    QString file_;
};
#endif

} // namespace

class TestClone : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // Touch libgit2 once so its static init/shutdown lifecycle runs
        // in a known order (see TestCommitLogModel).
        gitbolt::test::TestRepo touch;
        QVERIFY(!touch.path().isEmpty());

        if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty())
            QSKIP("no git on PATH");

        // Keep the developer's / runner's git config (url rewrites,
        // autocrlf, credential helpers…) out of the git children.
        QVERIFY(configDir_.isValid());
        const QString emptyConfig = configDir_.filePath(QStringLiteral("gitconfig"));
        QFile f(emptyConfig);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
        qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(emptyConfig));
    }

    // -----------------------------------------------------------------
    // Parser
    // -----------------------------------------------------------------

    void parsesRealCloneOutput()
    {
        const auto updates = parseInChunks(kRealClone, sizeof(kRealClone));

        // Under way at "Cloning into"; the server's enumerate / count /
        // compress lines keep it at Receiving with nothing received.
        QVERIFY(!updates.empty());
        QCOMPARE(updates.front().phase, Phase::Receiving);
        QCOMPARE(updates.front().totalObjects, 0u);
        QCOMPARE(updates[7].phase, Phase::Receiving);
        QCOMPARE(updates[7].totalObjects, 0u);

        // Phases only move forward; each ends complete; counters of an
        // earlier phase survive into the later ones.
        int lastPhase = 0;
        const CloneProgress* lastReceiving = nullptr;
        const CloneProgress* lastResolving = nullptr;
        for (const auto& u : updates) {
            QVERIFY(static_cast<int>(u.phase) >= lastPhase);
            lastPhase = static_cast<int>(u.phase);
            if (u.phase == Phase::Receiving)
                lastReceiving = &u;
            if (u.phase == Phase::Resolving)
                lastResolving = &u;
        }
        QVERIFY(lastReceiving && lastResolving);
        QCOMPARE(lastReceiving->receivedObjects, 812u);
        QCOMPARE(lastReceiving->totalObjects, 812u);
        QCOMPARE(lastReceiving->receivedBytes, uint64_t(44 * 1024 + 25 * 1024 / 100));
        QCOMPARE(lastResolving->indexedDeltas, 505u);
        QCOMPARE(lastResolving->totalDeltas, 505u);
        QCOMPARE(lastResolving->receivedObjects, 812u);
        const CloneProgress& last = updates.back();
        QCOMPARE(last.phase, Phase::CheckingOut);
        QCOMPARE(last.completedSteps, 300u);
        QCOMPARE(last.totalSteps, 300u);

        // 1 + 7 remote + 6 receiving + 4 resolving + 3 checkout.
        QCOMPARE(updates.size(), std::size_t(21));
        // Nothing here is an error: no "Cloning into", no "remote: Total".
        CloneProgressParser parser;
        parser.feed(kRealClone);
        QCOMPARE(QString::fromStdString(parser.messages()), QString());
    }

    void chunkingDoesNotChangeResult()
    {
        // A pipe read can end anywhere — mid-number, between '\r' and
        // the next title. Every split must yield the same updates.
        const std::string_view text(kRealClone, sizeof(kRealClone) - 1);
        const auto whole = parseInChunks(text, text.size());
        for (std::size_t chunk : {std::size_t(1), std::size_t(2), std::size_t(7),
                                  std::size_t(31), std::size_t(64)}) {
            const auto split = parseInChunks(text, chunk);
            QCOMPARE(split.size(), whole.size());
            for (std::size_t i = 0; i < whole.size(); ++i)
                QVERIFY2(sameProgress(split[i], whole[i]),
                         qPrintable(QStringLiteral("chunk %1, update %2").arg(chunk).arg(i)));
        }
    }

    void updateSplitAcrossReads()
    {
        CloneProgressParser parser;
        QVERIFY(parser.feed("Receiving objects:  45% (450/").empty());
        auto got = parser.feed("1000), 1.20 MiB | 2.30 MiB/s\rReceiving obj");
        QCOMPARE(got.size(), std::size_t(1));
        QCOMPARE(got[0].phase, Phase::Receiving);
        QCOMPARE(got[0].receivedObjects, 450u);
        QCOMPARE(got[0].totalObjects, 1000u);
        QCOMPARE(got[0].receivedBytes, uint64_t(1048576 + 20 * 1048576 / 100));

        got = parser.feed("ects:  46% (460/1000)\r");
        QCOMPARE(got.size(), std::size_t(1));
        QCOMPARE(got[0].receivedObjects, 460u);
        // No size on this line: the last known byte count carries over.
        QCOMPARE(got[0].receivedBytes, uint64_t(1048576 + 20 * 1048576 / 100));
    }

    void sizeUnits()
    {
        CloneProgressParser parser;
        auto got = parser.feed("Receiving objects: 100% (3/3), 512 bytes | 512.00 KiB/s, done.\n");
        QCOMPARE(got.size(), std::size_t(1));
        QCOMPARE(got[0].receivedBytes, uint64_t(512));
        got = parser.feed("Receiving objects:  10% (1/10), 3.50 GiB | 1 byte/s\r");
        QCOMPARE(got.size(), std::size_t(1));
        QCOMPARE(got[0].receivedBytes, uint64_t(3) * (1u << 30) + (uint64_t(1) << 30) / 2);
    }

    void sidebandDecorationAndLineEndings()
    {
        CloneProgressParser parser;
        // Terminal-style clear-to-EOL suffix instead of space padding.
        auto got = parser.feed("remote: Counting objects:  10% (1/10)\x1b[K\r");
        QCOMPARE(got.size(), std::size_t(1));
        QCOMPARE(got[0].totalObjects, 0u);
        // Non-progress server text and git's fatal line are kept,
        // decoration stripped; CRLF (Windows ssh) is one line ending.
        QVERIFY(parser.feed("remote: Repository not found.\x1b[K\n").empty());
        QVERIFY(parser.feed("fatal: repository 'https://example.invalid/x.git/' not found\r\n").empty());
        QCOMPARE(QString::fromStdString(parser.messages()),
                 QStringLiteral("remote: Repository not found.\n"
                                "fatal: repository 'https://example.invalid/x.git/' not found"));
    }

    void keepsLastFewMessages()
    {
        // ssh's error, then git's explanation: the last five lines are
        // kept (the oldest falls off), blank ones skipped, a final line
        // without a terminator only counts once finish() flushes it —
        // and an error that happens to look numeric ("error: 3, …") is
        // never taken for progress.
        CloneProgressParser parser;
        parser.feed("Cloning into 'x'...\n"
                    "ERROR: Permission denied (publickey).\n"
                    "fatal: Could not read from remote repository.\n"
                    "\n"
                    "Please make sure you have the correct access rights\n"
                    "and the repository exists.\n"
                    "error: 3, maybe\n"
                    "fatal: early EOF");     // no terminator yet
        QVERIFY(!QString::fromStdString(parser.messages()).contains(QStringLiteral("early EOF")));
        parser.finish();
        QCOMPARE(QString::fromStdString(parser.messages()),
                 QStringLiteral("fatal: Could not read from remote repository.\n"
                                "Please make sure you have the correct access rights\n"
                                "and the repository exists.\n"
                                "error: 3, maybe\n"
                                "fatal: early EOF"));
    }

    void flagsCheckoutFailure()
    {
        // git 2.39 after a complete fetch whose checkout then failed (a
        // required smudge filter that errors): git keeps the repository
        // and says so at exit, with advice on finishing the checkout.
        CloneProgressParser parser;
        parser.feed("Receiving objects: 100% (5/5), done.\n"
                    "error: external filter 'false' failed 1\n"
                    "error: external filter 'false' failed\n"
                    "fatal: a.txt: smudge filter broken failed\n"
                    "warning: Clone succeeded, but checkout failed.\n"
                    "You can inspect what was checked out with 'git status'\n"
                    "and retry with 'git restore --source=HEAD :/'\n"
                    "\n");
        QVERIFY(parser.checkoutFailed());
        QCOMPARE(QString::fromStdString(parser.messages()),
                 QStringLiteral("error: external filter 'false' failed\n"
                                "fatal: a.txt: smudge filter broken failed\n"
                                "warning: Clone succeeded, but checkout failed.\n"
                                "You can inspect what was checked out with 'git status'\n"
                                "and retry with 'git restore --source=HEAD :/'"));

        // Any other failure is not one — and the server can't claim it.
        CloneProgressParser fatal;
        fatal.feed("fatal: repository 'https://example.invalid/x.git/' not found\n");
        QVERIFY(!fatal.checkoutFailed());
        CloneProgressParser server;
        server.feed("remote: warning: Clone succeeded, but checkout failed.\n");
        QVERIFY(!server.checkoutFailed());
    }

    // -----------------------------------------------------------------
    // redactUrl
    // -----------------------------------------------------------------

    void redactsUrlCredentials_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<QString>("shown");
        const auto same = [](const char* name, const char* url) {
            QTest::newRow(name) << QString::fromLatin1(url) << QString::fromLatin1(url);
        };

        QTest::newRow("user:password")
            << QStringLiteral("https://alice:s3cret@example.com/o/r.git")
            << QStringLiteral("https://alice:***@example.com/o/r.git");
        QTest::newRow("token as user name")
            << QStringLiteral("https://0123456789abcdef@github.com/o/r.git")
            << QStringLiteral("https://***@github.com/o/r.git");
        QTest::newRow("token and port")
            << QStringLiteral("https://0123456789abcdef@example.com:8443/o/r.git")
            << QStringLiteral("https://***@example.com:8443/o/r.git");
        QTest::newRow("token, no path")
            << QStringLiteral("https://0123456789abcdef@example.com")
            << QStringLiteral("https://***@example.com");
        QTest::newRow("raw @ in the password")
            << QStringLiteral("https://alice:p@ss@example.com/r.git")
            << QStringLiteral("https://alice:***@example.com/r.git");
        QTest::newRow("ssh password")
            << QStringLiteral("ssh://git:s3cret@example.com/r.git")
            << QStringLiteral("ssh://git:***@example.com/r.git");
        same("port only", "https://example.com:8443/o/r.git");
        same("no userinfo", "https://github.com/o/r.git");
        same("@ in the path", "https://example.com/o/@scope/r.git");
        // An ssh login name is no secret, in either spelling.
        same("scp-style", "git@github.com:o/r.git");
        same("ssh user", "ssh://git@github.com/o/r.git");
        same("ssh user and port", "ssh://git@example.com:2222/o/r.git");
        same("file", "file:///tmp/r.git");
    }

    void redactsUrlCredentials()
    {
        QFETCH(QString, url);
        QFETCH(QString, shown);
        QCOMPARE(QString::fromStdString(GitProcess::redactUrl(url.toStdString())), shown);
    }

    // -----------------------------------------------------------------
    // Real clones through the git CLI
    // -----------------------------------------------------------------

    void clonesLocalBareRepo()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString head;
        QString branch;
        const QString bare = makeBareSource(dir, head, branch);
        QVERIFY(!bare.isEmpty());
        const QString url = fileUrl(bare);
        const QString dest = dir.filePath(QStringLiteral("clone"));

        std::vector<CloneProgress> updates;
        const auto result = GitProcess::clone(
            url.toStdString(), dest.toStdString(),
            [&updates](const CloneProgress& p) { updates.push_back(p); });
        QVERIFY2(result.ok(), result.error().message().c_str());

        // Progress: both fetch phases seen, in order, each complete.
        int lastPhase = 0;
        const CloneProgress* lastReceiving = nullptr;
        const CloneProgress* lastResolving = nullptr;
        for (const auto& u : updates) {
            QVERIFY(static_cast<int>(u.phase) >= lastPhase);
            lastPhase = static_cast<int>(u.phase);
            if (u.phase == Phase::Receiving)
                lastReceiving = &u;
            if (u.phase == Phase::Resolving)
                lastResolving = &u;
        }
        QVERIFY(lastReceiving);
        QVERIFY(lastReceiving->totalObjects > 0);
        QCOMPARE(lastReceiving->receivedObjects, lastReceiving->totalObjects);
        QVERIFY(lastResolving);    // the fixture's edits pack as deltas
        QVERIFY(lastResolving->totalDeltas > 0);
        QCOMPARE(lastResolving->indexedDeltas, lastResolving->totalDeltas);

        // The clone: same HEAD and branch, origin pointing at the URL,
        // a checked-out work tree.
        auto repo = gitbolt::git::Repository::open(dest.toStdString());
        QVERIFY(repo.ok());
        QVERIFY(!repo->isBare());
        auto cloneHead = repo->head();
        QVERIFY(cloneHead.ok());
        QCOMPARE(QString::fromStdString(cloneHead->toHex()), head);
        QCOMPARE(QString::fromStdString(*repo->headBranchName()), branch);
        auto remotes = repo->remotes();
        QVERIFY(remotes.ok());
        QCOMPARE(remotes->size(), std::size_t(1));
        QCOMPARE(QString::fromStdString(remotes->front().name), QStringLiteral("origin"));
        QCOMPARE(QString::fromStdString(remotes->front().url), url);
        QFile f(QDir(dest).filePath(QStringLiteral("f0.txt")));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll().endsWith("edit 3\n"));
    }

    void clonesIntoExistingEmptyDirectory()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString head;
        QString branch;
        const QString bare = makeBareSource(dir, head, branch);
        QVERIFY(!bare.isEmpty());
        const QString dest = dir.filePath(QStringLiteral("empty"));
        QVERIFY(QDir().mkpath(dest));

        const auto result = GitProcess::clone(fileUrl(bare).toStdString(), dest.toStdString());
        QVERIFY2(result.ok(), result.error().message().c_str());
        QVERIFY(QFileInfo::exists(QDir(dest).filePath(QStringLiteral(".git"))));
    }

    void failureReportsGitErrorAndCleansUp()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString url = fileUrl(dir.filePath(QStringLiteral("missing.git")));
        const QString dest = dir.filePath(QStringLiteral("out"));

        const auto result = GitProcess::clone(url.toStdString(), dest.toStdString());
        QVERIFY(!result.ok());
        const QString message = QString::fromStdString(result.error().message());
        QVERIFY2(message.contains(QStringLiteral("fatal:")), qPrintable(message));
        QVERIFY2(!message.contains(QStringLiteral("Cloning into")), qPrintable(message));
        QVERIFY(!QFileInfo::exists(dest));
    }

    void failureKeepsPreexistingEmptyDirectory()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dest = dir.filePath(QStringLiteral("out"));
        QVERIFY(QDir().mkpath(dest));

        const auto result = GitProcess::clone(
            fileUrl(dir.filePath(QStringLiteral("missing.git"))).toStdString(),
            dest.toStdString());
        QVERIFY(!result.ok());
        // Emptied, not deleted: the user picked (or made) this folder.
        QVERIFY(QFileInfo(dest).isDir());
        QVERIFY(QDir(dest).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden));
    }

    void refusesNonEmptyDestination()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString head;
        QString branch;
        const QString bare = makeBareSource(dir, head, branch);
        QVERIFY(!bare.isEmpty());
        // Only a hidden file: QDir's default filter calls this empty,
        // git does not — and a failed clone must never delete it.
        const QString dest = dir.filePath(QStringLiteral("occupied"));
        QVERIFY(QDir().mkpath(dest));
        const QString keep = QDir(dest).filePath(QStringLiteral(".keep"));
        QFile f(keep);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("mine\n");
        f.close();

        const auto result = GitProcess::clone(fileUrl(bare).toStdString(), dest.toStdString());
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code(), gitbolt::git::GitErrorCode::Exists);
        QVERIFY(QFileInfo::exists(keep));
        QVERIFY(!QFileInfo::exists(QDir(dest).filePath(QStringLiteral(".git"))));
    }

    void checkoutFailureKeepsTheRepository()
    {
        // The fetch completes, then the checkout fails — here on a
        // required smudge filter that errors, as a broken git-lfs
        // does. git keeps that repository on purpose; so must clone,
        // and say where it is, with git's advice.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString head;
        QString branch;
        const QString bare = makeBareSource(dir, head, branch, "*.txt filter=broken\n");
        QVERIFY(!bare.isEmpty());
        const QString dest = dir.filePath(QStringLiteral("out"));

        ScopedEnv count("GIT_CONFIG_COUNT", "2");
        ScopedEnv key0("GIT_CONFIG_KEY_0", "filter.broken.smudge");
        ScopedEnv value0("GIT_CONFIG_VALUE_0", "false");
        ScopedEnv key1("GIT_CONFIG_KEY_1", "filter.broken.required");
        ScopedEnv value1("GIT_CONFIG_VALUE_1", "true");
        const auto result = GitProcess::clone(fileUrl(bare).toStdString(), dest.toStdString());
        QVERIFY(!result.ok());
        const QString message = QString::fromStdString(result.error().message());
        QCOMPARE(result.error().code(), gitbolt::git::GitErrorCode::CheckoutFailed);
        QVERIFY2(message.contains(QDir::toNativeSeparators(dest)), qPrintable(message));
        QVERIFY2(message.contains(QStringLiteral("Clone succeeded, but checkout failed")),
                 qPrintable(message));
        QVERIFY2(message.contains(QStringLiteral("'git status'")), qPrintable(message));

        // The whole history is there, only the work tree isn't.
        auto repo = gitbolt::git::Repository::open(dest.toStdString());
        QVERIFY(repo.ok());
        auto cloneHead = repo->head();
        QVERIFY(cloneHead.ok());
        QCOMPARE(QString::fromStdString(cloneHead->toHex()), head);
    }

    void cancelStopsTheWholeProcessTree()
    {
#ifndef GITBOLT_TEST_STALL_TRANSPORT
        QSKIP("StallTransport not built");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dest = dir.filePath(QStringLiteral("out"));
        StalledClone clone(dest, dir.filePath(QStringLiteral("stall.pid")));
        QTRY_VERIFY_WITH_TIMEOUT(clone.stubRunning(), 15000);
        QVERIFY(QFileInfo::exists(dest));
        QVERIFY(processAlive(clone.stubPid()));

        QElapsedTimer timer;
        timer.start();
        clone.cancel();
        const auto& result = clone.wait();
        QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));

        QVERIFY(!result.ok());
        QCOMPARE(result.error().code(), gitbolt::git::GitErrorCode::User);
        QVERIFY(!QFileInfo::exists(dest));
        // The grandchild went down with git — not orphaned to sleep on.
        QTRY_VERIFY_WITH_TIMEOUT(!processAlive(clone.stubPid()), 5000);
#endif
    }

    // On Unix git's own SIGTERM handler usually cleans up a cancelled
    // clone, leaving GitProcess::clone nothing to do. Here it can't:
    // an entry it isn't allowed to delete defeats it, as an open file
    // does on Windows (where TerminateJobObject gives git no chance at
    // all). The clone's own cleanup has to finish the job, retrying
    // until the entry can go, and must leave a folder that existed
    // before the clone where it was: emptied, its permissions intact.
    void cancelCleansUpWhatGitCannot_data()
    {
        QTest::addColumn<bool>("existedBefore");
        QTest::newRow("new destination") << false;
        QTest::newRow("existing empty destination") << true;
    }

    void cancelCleansUpWhatGitCannot()
    {
#if defined(Q_OS_WIN)
        QSKIP("needs Unix directory permissions");
#elif !defined(GITBOLT_TEST_STALL_TRANSPORT)
        QSKIP("StallTransport not built");
#else
        QFETCH(bool, existedBefore);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dest = dir.filePath(QStringLiteral("out"));
        if (existedBefore) {
            QVERIFY(QDir().mkpath(dest));
            QVERIFY(QFile::setPermissions(dest, kOwnerOnly));
        }
        StalledClone clone(dest, dir.filePath(QStringLiteral("stall.pid")));
        QTRY_VERIFY_WITH_TIMEOUT(clone.stubRunning(), 15000);
        QVERIFY(clone.gitPid() > 0 && clone.gitPid() != clone.stubPid());
        QVERIFY(processAlive(clone.gitPid()));
        UndeletableEntry entry(dest);
        QVERIFY(entry.create());
        if (!entry.binds())
            QSKIP("directory permissions don't stop this user (root?)");

        clone.cancel();
        // git is gone, its cleanup over: it gave up on the entry, which
        // nothing has deleted since. Let the clone's first attempts
        // fail too, then make the entry deletable for a retry.
        QTRY_VERIFY_WITH_TIMEOUT(!processAlive(clone.gitPid()), 5000);
        QVERIFY(entry.exists());
        QTest::qWait(150);
        entry.unlock();

        const auto& result = clone.wait();
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code(), gitbolt::git::GitErrorCode::User);
        if (existedBefore) {
            QVERIFY(QFileInfo(dest).isDir());
            QVERIFY(QDir(dest).isEmpty(kAnyEntry));
            QVERIFY(QFile::permissions(dest) == kOwnerOnly);
        } else {
            QVERIFY(!QFileInfo::exists(dest));
        }
#endif
    }

    void cancelCleanupGivesUp()
    {
#if defined(Q_OS_WIN)
        QSKIP("needs Unix directory permissions");
#elif !defined(GITBOLT_TEST_STALL_TRANSPORT)
        QSKIP("StallTransport not built");
#else
        // An entry that never becomes deletable: the cleanup stops
        // retrying after its bounded backoff (~3 s) and leaves it,
        // rather than holding the worker, and app exit, hostage.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString dest = dir.filePath(QStringLiteral("out"));
        StalledClone clone(dest, dir.filePath(QStringLiteral("stall.pid")));
        QTRY_VERIFY_WITH_TIMEOUT(clone.stubRunning(), 15000);
        UndeletableEntry entry(dest);
        QVERIFY(entry.create());
        if (!entry.binds())
            QSKIP("directory permissions don't stop this user (root?)");

        QElapsedTimer timer;
        timer.start();
        clone.cancel();
        const auto& result = clone.wait();
        QVERIFY2(timer.elapsed() < 10000, qPrintable(QString::number(timer.elapsed())));
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code(), gitbolt::git::GitErrorCode::User);
        QVERIFY(entry.exists());
#endif
    }

private:
    // A bare repository with three commits on its default branch,
    // whose edits pack as deltas, and `attributes` as its
    // .gitattributes if given. Returns its path ("" on failure) and
    // reports HEAD and the branch name.
    static QString makeBareSource(const QTemporaryDir& dir, QString& head, QString& branch,
                                  const QByteArray& attributes = {})
    {
        gitbolt::test::TestRepo src;
        if (!attributes.isEmpty())
            src.writeFile(QStringLiteral(".gitattributes"), attributes);
        QByteArray body;
        for (int line = 0; line < 200; ++line)
            body += "line " + QByteArray::number(line) + " of a file that packs as deltas\n";
        for (int i = 0; i < 20; ++i)
            src.writeFile(QStringLiteral("f%1.txt").arg(i), body);
        if (!src.stageAll().ok() || !src.commit(QStringLiteral("base")).ok())
            return {};
        for (int edit = 1; edit <= 3; ++edit) {
            for (int i = 0; i < 20; ++i)
                src.appendFile(QStringLiteral("f%1.txt").arg(i),
                               "edit " + QByteArray::number(edit) + "\n");
            if (!src.stageAll().ok() || !src.commit(QStringLiteral("edit %1").arg(edit)).ok())
                return {};
        }
        auto srcHead = src.repo().head();
        auto srcBranch = src.repo().headBranchName();
        if (!srcHead.ok() || !srcBranch.ok())
            return {};
        head = QString::fromStdString(srcHead->toHex());
        branch = QString::fromStdString(*srcBranch);

        const QString bare = dir.filePath(QStringLiteral("source.git"));
        auto out = GitProcess(dir.path().toStdString())
                       .run({"clone", "--bare", "-q", src.path().toStdString(),
                             bare.toStdString()});
        if (!out.ok() || !out->success())
            return {};
        return bare;
    }

    QTemporaryDir configDir_;
};

QTEST_GUILESS_MAIN(TestClone)
#include "TestClone.moc"
