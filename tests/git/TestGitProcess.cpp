//
// TestGitProcess — GitProcess::run() against a real temp repository.
//
// Every git command runs in a process group of its own (a job object on
// Windows), and a timeout stops it the way a cancel does: git is
// interrupted first, SIGTERM on Unix and a Ctrl-C on its console on
// Windows, so it rolls back the lockfiles it holds, then whatever is
// left is killed. A timeout used to kill git alone: its index.lock
// stayed behind, which every later stage, commit and checkout tripped
// over, and the filter or hook it was running ran on, orphaned. On
// Windows, ending the job alone left the lock behind just the same.
//

#include <QFile>
#include <QFileInfo>
#include <QTest>

#include "../TestProcessHelper.h"
#include "../TestRepoHelper.h"
#include "git/GitProcess.h"

class TestGitProcess : public QObject {
    Q_OBJECT

private slots:
    // What runWithInput() feeds git arrives once, whole, and git sees
    // its end (`hash-object --stdin` reads to EOF).
    void inputReachesGitOnce() {
        gitbolt::test::TestRepo repo;
        gitbolt::git::GitProcess git(repo.path().toStdString());
        const auto direct = git.runWithInput({"hash-object", "--stdin"}, "payload\n");
        QVERIFY(succeeded(direct));
        QFile file(repo.path() + QStringLiteral("/payload.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("payload\n");
        file.close();
        const auto fromFile = git.run({"hash-object", "payload.txt"});
        QVERIFY(succeeded(fromFile));
        QCOMPARE(direct->stdoutData, fromFile->stdoutData);
    }

    // A checkout that times out in a slow smudge filter: git, the filter
    // and its index.lock are all gone soon after run() returns.
    void timeoutStopsGitAndWhatItStarted() {
#ifdef Q_OS_WIN
        // As GitBolt runs: with no console of its own, so that git gets
        // a hidden one, which the Ctrl-C goes to. (A console test's git
        // shares the test's, and gets no Ctrl-C.) Last of the tests
        // here that need one.
        FreeConsole();
#endif
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), QByteArrayLiteral("main\n"),
                                    QStringLiteral("main")).ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"config", "core.autocrlf", "false"})));
        QVERIFY(succeeded(git.run({"checkout", "-q", "-b", "other"})));
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), QByteArrayLiteral("other\n"),
                                    QStringLiteral("other")).ok());
        QVERIFY(succeeded(git.run({"checkout", "-q", "-"})));

        // Checking out `other` smudges its a.txt through a filter that
        // notes its pid (Git for Windows' sh: the Windows pid) and takes
        // 30 s. The attributes go in .git/info, so the work tree has
        // nothing to commit.
        const QString pidFile = repo.path() + QStringLiteral("/.git/filter.pid");
        QVERIFY(succeeded(git.run(
            {"config", "filter.slow.smudge",
             "{ cat /proc/$$/winpid 2>/dev/null || echo $$; } >> '" + pidFile.toStdString()
                 + "'; sleep 30; cat"})));
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
        const qint64 filter = pids.readLine().trimmed().toLongLong();
        QVERIFY(filter > 0);
        QTRY_VERIFY_WITH_TIMEOUT(!gitbolt::test::processAlive(filter), 3000);
    }

private:
    static bool succeeded(const gitbolt::git::Result<gitbolt::git::ProcessOutput>& r) {
        return r.ok() && r.value().success();
    }
};

QTEST_GUILESS_MAIN(TestGitProcess)
#include "TestGitProcess.moc"
