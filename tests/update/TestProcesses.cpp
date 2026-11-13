// Tests for update/Processes.h: which GitBolts keep a Windows install
// folder in use, and when a process started (so a reused pid doesn't pass).

#include <QDateTime>
#include <QProcess>
#include <QScopeGuard>
#include <QTest>

#include "update/Processes.h"

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

using namespace gitbolt::update;

class TestProcesses : public QObject {
    Q_OBJECT

private slots:
    void insideAWindowsFolder_data() {
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("folder");
        QTest::addColumn<bool>("inside");
        const QString exe = QStringLiteral("C:\\Program Files\\GitBolt\\bin\\GitBolt.exe");
        QTest::newRow("in it") << exe << QStringLiteral("C:\\Program Files\\GitBolt") << true;
        QTest::newRow("as Qt writes the folder") << exe << QStringLiteral("C:/Program Files/GitBolt") << true;
        QTest::newRow("with a trailing separator") << exe << QStringLiteral("C:/Program Files/GitBolt/") << true;
        QTest::newRow("any case") << exe << QStringLiteral("c:/PROGRAM FILES/gitbolt") << true;
        QTest::newRow("not a folder of a longer name")
            << QStringLiteral("C:\\Program Files\\GitBolt2\\bin\\GitBolt.exe") << QStringLiteral("C:/Program Files/GitBolt")
            << false;
        QTest::newRow("the folder itself") << QStringLiteral("C:\\Program Files\\GitBolt")
                                           << QStringLiteral("C:/Program Files/GitBolt") << false;
        QTest::newRow("another drive") << QStringLiteral("D:\\Program Files\\GitBolt\\bin\\GitBolt.exe")
                                       << QStringLiteral("C:/Program Files/GitBolt") << false;
        QTest::newRow("a drive's root") << QStringLiteral("D:\\bin\\GitBolt.exe") << QStringLiteral("D:/") << true;
        QTest::newRow("no path") << QString() << QStringLiteral("C:/Program Files/GitBolt") << false;
        QTest::newRow("no folder") << exe << QString() << false;
    }
    void insideAWindowsFolder() {
        QFETCH(QString, path);
        QFETCH(QString, folder);
        QFETCH(bool, inside);
        QCOMPARE(isInsideWindowsFolder(path, folder), inside);
    }

    // Only GitBolt.exe (any case) counts, this process never; one from
    // another folder is no trouble; one whose path is unknown is.
    void gitBoltsUsingTheFolder() {
        const QString folder = QStringLiteral("C:/Program Files/GitBolt");
        const QString inside = QStringLiteral("C:\\Program Files\\GitBolt\\bin\\GitBolt.exe");
        const QList<RunningProgram> programs{
            {100, QStringLiteral("GitBolt.exe"), inside},   // this process
            {101, QStringLiteral("gitbolt.EXE"), inside},
            {102, QStringLiteral("GitBolt.exe"), QStringLiteral("C:\\Users\\Bob\\GitBolt\\bin\\GitBolt.exe")},
            {103, QStringLiteral("GitBolt.exe"), QString()},
            {104, QStringLiteral("Uninstall.exe"), QStringLiteral("C:\\Program Files\\GitBolt\\Uninstall.exe")},
            {105, QStringLiteral("GitBolt.exe.bak"), inside},
        };
        const FolderUsers users = gitBoltsUsing(programs, folder, 100);
        QCOMPARE(users.running, 1);
        QCOMPARE(users.unknown, 1);
        QVERIFY(users.any());
        QVERIFY(!gitBoltsUsing(programs.mid(0, 1), folder, 100).any());
        QVERIFY(!gitBoltsUsing({}, folder, 100).any());
        QCOMPARE(gitBoltsUsing(programs, folder, 0).running, 2);
    }

    // Windows only lists processes; elsewhere none, and on Windows only
    // ones named GitBolt.exe, never this test.
    void runningGitBoltsAreGitBolts() {
        const QList<RunningProgram> programs = runningGitBolts();
#if !defined(Q_OS_WIN)
        QVERIFY(programs.isEmpty());
#endif
        for (const RunningProgram& program : programs) {
            QCOMPARE(program.name.compare(QStringLiteral("GitBolt.exe"), Qt::CaseInsensitive), 0);
            QVERIFY(program.pid != QCoreApplication::applicationPid());
        }
    }

    // /proc/<pid>/stat: the name in parentheses may hold anything; the
    // start is field 22, in clock ticks after boot.
    void linuxStatLines_data() {
        QTest::addColumn<QByteArray>("line");
        QTest::addColumn<qint64>("started");
        // ppid, pgrp, session, tty, tpgid, flags, faults (4), times (4),
        // priority, nice, threads, itrealvalue, then the start.
        const QByteArray rest = " 1 4321 4321 0 -1 4194560 100 0 0 0 1 2 0 0 20 0 1 0 12345 2000000 200\n";
        QTest::newRow("sleep") << QByteArray("4321 (sleep) S") + rest << qint64(1700000123);
        QTest::newRow("a name with spaces and parentheses")
            << QByteArray("4321 (my (odd) name) ) R") + rest << qint64(1700000123);
        QTest::newRow("a zombie") << QByteArray("4321 (sh) Z") + rest << qint64(0);
        QTest::newRow("dead") << QByteArray("4321 (sh) X") + rest << qint64(0);
        QTest::newRow("cut short") << QByteArray("4321 (sh) S 1 1 0") << qint64(0);
        QTest::newRow("not a number") << QByteArray("4321 (sh) S") + QByteArray(rest).replace("12345", "12a45")
                                      << qint64(0);
        QTest::newRow("no name") << QByteArray("4321 sh S") + rest << qint64(0);
    }
    void linuxStatLines() {
        QFETCH(QByteArray, line);
        QFETCH(qint64, started);
        QCOMPARE(linuxStartTime(line, 1700000000, 100), started);
        QCOMPARE(linuxStartTime(line, 0, 100), qint64(0));
        QCOMPARE(linuxStartTime(line, 1700000000, 0), qint64(0));
    }

    // When a process of this user started; 0 once it is gone, and for
    // no pid at all.
    void startTimes() {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const qint64 self = processStartTime(QCoreApplication::applicationPid());
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
        QVERIFY2(self > 0 && self <= now + 1 && self > now - 3600, qPrintable(QString::number(self)));
#endif
        QCOMPARE(processStartTime(0), qint64(0));
        QCOMPARE(processStartTime(-1), qint64(0));
#if defined(Q_OS_UNIX)
        QProcess child;
        child.start(QStringLiteral("/bin/sleep"), {QStringLiteral("30")});
        QVERIFY(child.waitForStarted());
        auto stop = qScopeGuard([&] {
            child.kill();
            child.waitForFinished();
        });
        // Kept: processId() is 0 once the child has finished.
        const qint64 pid = child.processId();
        const qint64 started = processStartTime(pid);
        QVERIFY2(started >= now - 1 && started <= QDateTime::currentSecsSinceEpoch() + 1,
                 qPrintable(QString::number(started)));
        child.kill();
        QVERIFY(child.waitForFinished());
        QCOMPARE(child.processId(), qint64(0));
        QCOMPARE(processStartTime(pid), qint64(0));
        // Another user's (init, launchd): not this user's helper.
        if (::getuid() != 0)
            QCOMPARE(processStartTime(1), qint64(0));
#endif
    }
};

QTEST_GUILESS_MAIN(TestProcesses)
#include "TestProcesses.moc"
