//
// TestGitProcess — GitProcess::run() against a real temp repository.
//
// Every git command runs in a process group of its own (a job object on
// Windows), and a timeout stops it the way a cancel does: git is
// interrupted first, SIGINT on Unix and a Ctrl-C on its console on
// Windows, so it rolls back the lockfiles it holds, then whatever is
// left is killed. A timeout used to kill git alone: its index.lock
// stayed behind, which every later stage, commit and checkout tripped
// over, and the filter or hook it was running ran on, orphaned. On
// Windows, ending the job alone left the lock behind just the same.
//
// The rest of the group gets its time too: a pre-commit hook that set
// the unstaged changes aside puts them back when it is interrupted, and
// a cancelled commit (quitting during it) must not end that halfway.
//

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTest>
#include <QThread>

#include <atomic>
#include <memory>
#include <thread>

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

    // pre-commit's way with unstaged changes (staged_files_only): saved
    // to a patch and checked out, so the hooks see only what is staged,
    // and put back when the hook ends, however it ends. Interrupted, the
    // hook puts them back on its way out: a shell in a trap, Python in a
    // `finally` (which only SIGINT reaches: SIGTERM's default action
    // skips it). Cancelled 2 ms after the signal, once git had exited,
    // the hook was killed halfway, and the edits were gone from the work
    // tree, kept only in the hook's patch file.
    void cancelledCommitLetsAHookPutChangesBack_data() {
        QTest::addColumn<QString>("language");
        QTest::newRow("sh trap") << QStringLiteral("sh");
        QTest::newRow("python finally") << QStringLiteral("python");
    }

    void cancelledCommitLetsAHookPutChangesBack() {
        QFETCH(QString, language);
#ifdef Q_OS_WIN
        // The Ctrl-C reaches Git for Windows' sh as SIGINT, and the job
        // waits for it the same way; not run here, as the msys2
        // runtime's handling of a trap under a hidden console is not
        // what this test is about.
        QSKIP("Unix process groups");
#endif
        QString interpreter = QStringLiteral("/bin/sh");
        if (language == QLatin1String("python")) {
            interpreter = QStandardPaths::findExecutable(QStringLiteral("python3"));
            if (interpreter.isEmpty())
                QSKIP("no python3");
        }
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("f.txt"), QByteArrayLiteral("one\n"),
                                    QStringLiteral("base")).ok());
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"config", "user.name", "T"})));
        QVERIFY(succeeded(git.run({"config", "user.email", "t@example.com"})));
        QVERIFY(succeeded(git.run({"config", "commit.gpgSign", "false"})));
        QVERIFY(succeeded(git.run({"config", "core.hooksPath", ".git/hooks"})));

        // Staged: "two". Unstaged on top of it: "three".
        writeFile(repo.path() + QStringLiteral("/f.txt"), "two\n");
        QVERIFY(succeeded(git.run({"add", "f.txt"})));
        writeFile(repo.path() + QStringLiteral("/f.txt"), "three\n");

        // The restore takes a moment (a big patch does), so a kill that
        // comes right behind git always lands in the middle of it.
        const QByteArray sh =
            "#!/bin/sh\n"
            "git diff --binary > .git/unstaged.patch\n"
            "restore() { sleep 0.3; git apply .git/unstaged.patch && echo done > .git/restored; exit 1; }\n"
            "trap restore INT TERM\n"
            "git checkout -- .\n"
            "echo started > .git/hook-started\n"
            "sleep 30\n";
        const QByteArray python =
            "#!" + QFile::encodeName(interpreter) + "\n"
            "import subprocess, time\n"
            "patch = subprocess.run(['git', 'diff', '--binary'], capture_output=True).stdout\n"
            "open('.git/unstaged.patch', 'wb').write(patch)\n"
            "subprocess.run(['git', 'checkout', '--', '.'])\n"
            "try:\n"
            "    open('.git/hook-started', 'w').write('started')\n"
            "    time.sleep(30)\n"
            "finally:\n"
            "    time.sleep(0.3)\n"
            "    subprocess.run(['git', 'apply', '.git/unstaged.patch'])\n"
            "    open('.git/restored', 'w').write('done')\n";
        const QString hook = repo.path() + QStringLiteral("/.git/hooks/pre-commit");
        writeFile(hook, language == QLatin1String("sh") ? sh : python);
        QVERIFY(QFile::setPermissions(hook, QFile::ReadOwner | QFile::WriteOwner
                                                | QFile::ExeOwner));

        // As quitting cancels it: the service's flag, from another thread.
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        gitbolt::git::GitProcess committer(repo.path().toStdString());
        committer.addCancelFlag(cancel);
        std::optional<gitbolt::git::Result<gitbolt::git::ProcessOutput>> result;
        std::thread worker([&] { result.emplace(committer.commit("msg\n", false)); });
        const QString started = repo.path() + QStringLiteral("/.git/hook-started");
        const bool hookStarted = QTest::qWaitFor([&] { return QFileInfo::exists(started); }, 10000);
        cancel->store(true);
        worker.join();
        QVERIFY(hookStarted);
        QVERIFY(result && !result->ok());
        QCOMPARE(result->error().message(), std::string("Cancelled"));

        // Nothing committed, the index as staged, the edit back.
        QVERIFY(QFileInfo::exists(repo.path() + QStringLiteral("/.git/restored")));
        QCOMPARE(readFile(repo.path() + QStringLiteral("/f.txt")), QByteArray("three\n"));
        const auto log = git.run({"log", "--format=%s"});
        QVERIFY(succeeded(log));
        QCOMPARE(QByteArray::fromStdString(log->stdoutData), QByteArray("base\n"));
        const auto staged = git.run({"show", ":f.txt"});
        QVERIFY(succeeded(staged));
        QCOMPARE(QByteArray::fromStdString(staged->stdoutData), QByteArray("two\n"));
        QVERIFY(!QFileInfo::exists(repo.path() + QStringLiteral("/.git/index.lock")));
    }

    // The prepare-commit-msg hook git would run, found where git looks:
    // in .git/hooks or core.hooksPath, executable on Unix. Git for
    // Windows runs any file of that name, and one named with ".exe" too
    // (find_hook()'s STRIP_EXTENSION retry), as a compiled hook is.
    // That one was missed, and with no HEAD reflog a reword a hook
    // turned down was dropped on Continue, with "Rebase complete.".
    void prepareCommitMsgHookAsGitFindsIt_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("executable");
        QTest::addColumn<bool>("found");
        QTest::newRow("none") << QString() << true << false;
        QTest::newRow("a script") << QStringLiteral("prepare-commit-msg") << true << true;
        QTest::newRow("in core.hooksPath")
            << QStringLiteral("hooksPath/prepare-commit-msg") << true << true;
        QTest::newRow("a sample") << QStringLiteral("prepare-commit-msg.sample") << true << false;
#ifdef Q_OS_WIN
        QTest::newRow("a compiled one, .exe")
            << QStringLiteral("prepare-commit-msg.exe") << true << true;
        QTest::newRow("a compiled one, .exe, in core.hooksPath")
            << QStringLiteral("hooksPath/prepare-commit-msg.exe") << true << true;
#else
        // git on Unix tries the name alone, and runs it only when it
        // can be executed.
        QTest::newRow("a .exe") << QStringLiteral("prepare-commit-msg.exe") << true << false;
        QTest::newRow("not executable") << QStringLiteral("prepare-commit-msg") << false << false;
#endif
    }

    void prepareCommitMsgHookAsGitFindsIt() {
        QFETCH(QString, name);
        QFETCH(bool, executable);
        QFETCH(bool, found);
        gitbolt::test::TestRepo repo;
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QString dir = repo.path() + QStringLiteral("/.git/hooks");
        QString file = name;
        if (name.startsWith(QLatin1String("hooksPath/"))) {
            dir = repo.path() + QStringLiteral("/elsewhere");
            QVERIFY(succeeded(git.run({"config", "core.hooksPath", dir.toStdString()})));
            file = name.section(QLatin1Char('/'), 1);
        }
        QVERIFY(QDir().mkpath(dir));
        if (!file.isEmpty()) {
            const QString path = dir + QLatin1Char('/') + file;
            writeFile(path, "#!/bin/sh\nexit 0\n");
            QFile::Permissions permissions = QFile::ReadOwner | QFile::WriteOwner;
            if (executable)
                permissions |= QFile::ExeOwner;
            QVERIFY(QFile::setPermissions(path, permissions));
        }
        QCOMPARE(git.hasPrepareCommitMsgHook(), found);
    }

private:
    static void writeFile(const QString& path, const QByteArray& data) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(data);
    }

    static QByteArray readFile(const QString& path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("(unreadable)");
    }

    static bool succeeded(const gitbolt::git::Result<gitbolt::git::ProcessOutput>& r) {
        return r.ok() && r.value().success();
    }
};

QTEST_GUILESS_MAIN(TestGitProcess)
#include "TestGitProcess.moc"
