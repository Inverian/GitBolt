//
// TestGitProcess — GitProcess::run() against a real temp repository.
//
// Every git command runs in a process group of its own, and a timeout
// stops the group the way a cancel does: SIGTERM first, so git rolls
// back the lockfiles it holds, as on Ctrl-C, then SIGKILL for whatever
// is left. A timeout used to SIGKILL git alone: its index.lock stayed
// behind, which every later stage, commit and checkout tripped over,
// and the filter or hook it was running ran on, orphaned.
//

#include <QFile>
#include <QFileInfo>
#include <QTest>

#include "../TestRepoHelper.h"
#include "git/GitProcess.h"

#ifndef Q_OS_WIN
#include <cerrno>
#include <csignal>
#endif

class TestGitProcess : public QObject {
    Q_OBJECT

private slots:
    // A checkout that times out in a slow smudge filter: git, the filter
    // and its index.lock are all gone soon after run() returns.
    void timeoutStopsGitAndWhatItStarted() {
#ifdef Q_OS_WIN
        QSKIP("Unix process groups; on Windows git runs in a job object");
#else
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), QByteArrayLiteral("main\n"),
                                    QStringLiteral("main")).ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "other"})));
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), QByteArrayLiteral("other\n"),
                                    QStringLiteral("other")).ok());
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));

        // Checking out `other` smudges its a.txt through a filter that
        // notes its pid and takes 30 s. The attributes go in
        // .git/info, so the work tree has nothing to commit.
        const QString pidFile = repo.path() + QStringLiteral("/.git/filter.pid");
        QVERIFY(succeeded(git.run(
            {"config", "filter.slow.smudge",
             "echo $$ >> '" + pidFile.toStdString() + "'; sleep 30; cat"})));
        QFile attributes(repo.path() + QStringLiteral("/.git/info/attributes"));
        QVERIFY(attributes.open(QIODevice::WriteOnly));
        attributes.write("* filter=slow\n");
        attributes.close();

        const auto result = git.run({"checkout", "-q", "other"}, /*timeoutMs=*/1000);
        QVERIFY(!result.ok());
        QCOMPARE(result.error().message(), std::string("Git process timed out"));

        const QString indexLock = repo.path() + QStringLiteral("/.git/index.lock");
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(indexLock), 3000);
        QFile pids(pidFile);
        QVERIFY(pids.open(QIODevice::ReadOnly));
        const pid_t filter = static_cast<pid_t>(pids.readLine().trimmed().toLongLong());
        QVERIFY(filter > 0);
        QTRY_VERIFY_WITH_TIMEOUT(::kill(filter, 0) != 0 && errno == ESRCH, 3000);
#endif
    }

private:
    static bool succeeded(const gitbolt::git::Result<gitbolt::git::ProcessOutput>& r) {
        return r.ok() && r.value().success();
    }
};

QTEST_GUILESS_MAIN(TestGitProcess)
#include "TestGitProcess.moc"
