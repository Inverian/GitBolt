//
// TestLoginPath — the PATH git's children get (util/LoginPath.h).
//
// Started from the Dock, GitBolt has launchd's PATH, and a commit's
// hook that runs node (husky, lint-staged) failed with "node: command
// not found". The login shell's PATH now goes ahead of the session's:
//
//   - the merge keeps both lists' entries, once each, the login's first,
//   - the login shell is run as `-ilc`, its rc files' chatter ignored,
//     and a shell that hangs, fails or says nothing usable gives up
//     nothing (it is killed at the timeout, with what it started),
//   - a real login /bin/sh reads ~/.profile,
//   - once the lookup has started, git's children (an alias here, a
//     hook in the app) get the merged PATH.
//

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include "../TestProcessHelper.h"
#include "../TestRepoHelper.h"
#include "git/GitProcess.h"
#include "util/LoginPath.h"

using namespace gitbolt;

namespace {

#ifndef Q_OS_WIN
// An executable script `name` in `dir`.
QString script(const QTemporaryDir& dir, const char* name, const QByteArray& body) {
    const QString path = dir.filePath(QLatin1String(name));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write("#!/bin/sh\n" + body);
    file.close();
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}
#endif

} // namespace

class TestLoginPath : public QObject {
    Q_OBJECT

private slots:
    void mergeKeepsEveryEntryOnceLoginFirst() {
#ifdef Q_OS_WIN
        QCOMPARE(util::mergePathLists(QStringLiteral("C:\\a;C:\\b"), QStringLiteral("C:\\b;C:\\c")),
                 QStringLiteral("C:\\a;C:\\b;C:\\c"));
#else
        QCOMPARE(util::mergePathLists(QStringLiteral("/opt/homebrew/bin:/usr/bin"),
                                      QStringLiteral("/usr/bin:/bin:/usr/sbin:/sbin")),
                 QStringLiteral("/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin"));
        QCOMPARE(util::mergePathLists(QStringLiteral("/a::/b:"), QStringLiteral(":/c:/a")),
                 QStringLiteral("/a:/b:/c"));
        QCOMPARE(util::mergePathLists(QString(), QStringLiteral("/usr/bin:/bin")),
                 QStringLiteral("/usr/bin:/bin"));
#endif
    }

    void loginShellsPathIsRead() {
#ifdef Q_OS_WIN
        QSKIP("no login shell on Windows");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString args = dir.filePath(QStringLiteral("args"));
        // Chatter from the rc files, before and after; the command is the
        // last argument, as `-c` takes it.
        const QString shell = script(dir, "fakesh",
            "printf '%s\\n' \"$1\" > '" + QFile::encodeName(args) + "'\n"
            "echo 'Welcome back!'\n"
            "PATH=\"/opt/fake/bin:$PATH\"; export PATH\n"
            "eval \"$2\"\n"
            "echo 'bye'\n");
        QVERIFY(!shell.isEmpty());
        const QString path = util::queryLoginShellPath(shell, 5000);
        QVERIFY2(path.startsWith(QStringLiteral("/opt/fake/bin:")), qPrintable(path));
        QFile flags(args);
        QVERIFY(flags.open(QIODevice::ReadOnly));
        QCOMPARE(flags.readAll().trimmed(), QByteArrayLiteral("-ilc"));
#endif
    }

    void aShellThatHangsFailsOrSaysNothingUsableGivesNothing() {
#ifdef Q_OS_WIN
        QSKIP("no login shell on Windows");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString pidFile = dir.filePath(QStringLiteral("pid"));
        // Hangs, with a child that would keep its stdout open.
        const QString hangs = script(dir, "hangs",
            "sleep 30 & echo $! > '" + QFile::encodeName(pidFile) + "'\nwait\n");
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(util::queryLoginShellPath(hangs, 500), QString());
        QVERIFY2(timer.elapsed() < 3000, qPrintable(QString::number(timer.elapsed())));
        QFile pid(pidFile);
        QVERIFY(pid.open(QIODevice::ReadOnly));
        const qint64 child = pid.readAll().trimmed().toLongLong();
        QVERIFY(child > 0);
        QTRY_VERIFY_WITH_TIMEOUT(!test::processAlive(child), 5000);

        QCOMPARE(util::queryLoginShellPath(script(dir, "fails", "exit 1\n"), 5000), QString());
        QCOMPARE(util::queryLoginShellPath(
                     script(dir, "relative", "echo __GITBOLT_LOGIN_PATH__; echo 'bin:/usr/bin'\n"),
                     5000),
                 QString());
        QCOMPARE(util::queryLoginShellPath(dir.filePath(QStringLiteral("missing")), 5000),
                 QString());
#endif
    }

    // A real login shell: /bin/sh reads ~/.profile.
    void aRealLoginShellReadsTheProfile() {
#ifdef Q_OS_WIN
        QSKIP("no login shell on Windows");
#else
        QTemporaryDir home;
        QVERIFY(home.isValid());
        QFile profile(home.filePath(QStringLiteral(".profile")));
        QVERIFY(profile.open(QIODevice::WriteOnly));
        profile.write("PATH=\"$HOME/tools:$PATH\"; export PATH\n");
        profile.close();
        const test::ScopedEnv homeEnv("HOME", QFile::encodeName(home.path()));
        const test::ScopedEnv envEnv("ENV", "");
        const QString path = util::queryLoginShellPath(QStringLiteral("/bin/sh"), 10000);
        const QString tools = QDir(home.path()).filePath(QStringLiteral("tools"));
        QVERIFY2(path.split(QLatin1Char(':')).contains(tools), qPrintable(path));
        QVERIFY2(path.split(QLatin1Char(':')).contains(QStringLiteral("/bin")), qPrintable(path));
#endif
    }

    // Last: the lookup is once per process. git's children, a hook or
    // (here) an alias, get the login shell's entries ahead of their own.
    void gitsChildrenGetTheLoginPath() {
#ifdef Q_OS_WIN
        QSKIP("no login shell on Windows");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString shell = script(dir, "loginsh",
            "PATH=\"/opt/from-login/bin:/usr/bin:$PATH\"; export PATH\neval \"$2\"\n");
        QCOMPARE(util::pathForChildren(QStringLiteral("/usr/bin:/bin")),
                 QStringLiteral("/usr/bin:/bin"));      // not started: as it is
        util::startLoginPathLookup(shell);
        QVERIFY(util::pathForChildren(QStringLiteral("/usr/bin:/bin"))
                    .startsWith(QStringLiteral("/opt/from-login/bin:/usr/bin:")));

        test::TestRepo repo;
        git::GitProcess git(repo.path().toStdString());
        const auto out = git.run({"-c", "alias.showpath=!printenv PATH", "showpath"});
        QVERIFY(out.ok() && out->success());
        const QStringList entries =
            QString::fromStdString(out->stdoutData).trimmed().split(QLatin1Char(':'));
        QVERIFY2(entries.contains(QStringLiteral("/opt/from-login/bin")),
                 qPrintable(entries.join(QLatin1Char(':'))));
#endif
    }
};

QTEST_GUILESS_MAIN(TestLoginPath)
#include "TestLoginPath.moc"
