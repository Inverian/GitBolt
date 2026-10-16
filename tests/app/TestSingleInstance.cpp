//
// TestSingleInstance — the single-instance guard (src/app/
// SingleInstance.h), on every platform. The e2e tests launch the app
// itself, and only on Unix, so on Windows nothing ran this code before:
// not the hashed pipe name, not a hand-off's write waiting for a busy
// owner on Qt's unbuffered pipes, not a hang-up, not the wait for a
// lock to come free, not Explorer's command line. The hand-offs here
// run between real processes (InstanceHelper.cpp, either end with no
// window or settings) on all three CI platforms:
//
//   - a launch hands its path (or nothing, for a bare relaunch) to the
//     instance holding the lock, which answers and passes it on;
//   - a busy owner (its event loop stuck, as when git runs on its GUI
//     thread) gets the path once it is free: the hand-off waits for as
//     long as the owner lives. On Unix it used to give up after 10 s
//     with the path left for an owner that might die before reading it;
//   - an owner killed while a launch waits on it leaves its place to
//     that launch, which then opens its path itself;
//   - Explorer's "Open in GitBolt" on a drive root ("E:\", which
//     Windows' argument rules read as E:") opens the drive root.
//
// And in this process:
//
//   - nobody listening, or an owner that hangs up unanswered, is told
//     apart from a delivery;
//   - the caller of a hand-off has its say every 100 ms and can stop
//     the wait (the busy window's Cancel), which takes the path back;
//   - a free lock is taken; one nobody listens for is given up on after
//     the patience;
//   - the pipe name is the lock directory's on Windows, case and all;
//   - the path argument is made absolute.
//
// Server names carry a UUID: Windows' pipe names are machine-wide.
//

#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QUuid>

#include "app/SingleInstance.h"

#include <functional>

using gitbolt::app::Claim;
using gitbolt::app::Handoff;

namespace {

QString uniqueServerName()
{
    return QStringLiteral("gb-test-instance-")
           + QUuid::createUuid().toString(QUuid::Id128).left(16);
}

// A lock file in a directory of its own.
struct LockDir {
    QTemporaryDir dir;
    QString path() const { return dir.filePath(QStringLiteral("gb-test-instance.lock")); }
};

// InstanceHelper in a process of its own, told to quit (an owner then
// removes its socket file) or else killed if still running when this
// goes, so no exit path of a test leaves one behind.
class Helper {
public:
    explicit Helper(const QStringList& arguments)
    {
        process_.setProgram(QStringLiteral(GITBOLT_INSTANCE_HELPER));
        process_.setArguments(arguments);
        process_.start();
    }

    ~Helper()
    {
        if (process_.state() == QProcess::NotRunning)
            return;
        send("quit");
        // Its stdin ends there too: nothing it reads can hold it up.
        process_.closeWriteChannel();
        if (!process_.waitForFinished(10000)) {
            process_.kill();
            process_.waitForFinished(10000);
        }
    }

    QProcess& process() { return process_; }

    void send(const QByteArray& command) { process_.write(command + '\n'); }

    // The next line it prints that starts with `prefix`, waiting up to
    // `timeoutMs` for it; empty if none comes.
    QString waitForLine(const QString& prefix, int timeoutMs = 30000)
    {
        const QDeadlineTimer deadline(timeoutMs);
        for (;;) {
            output_ += QString::fromLocal8Bit(process_.readAllStandardOutput());
            const QStringList lines = output_.split(QLatin1Char('\n'));
            // The last one may be incomplete.
            for (qsizetype i = consumed_; i + 1 < lines.size(); ++i) {
                const QString line = lines.at(i).trimmed();
                if (line.startsWith(prefix)) {
                    consumed_ = i + 1;
                    return line;
                }
            }
            if (deadline.hasExpired())
                return {};
            if (!process_.waitForReadyRead(100) && process_.state() == QProcess::NotRunning
                && process_.bytesAvailable() == 0)
                return {};
        }
    }

    // Everything it printed so far, for failure messages.
    QString output()
    {
        output_ += QString::fromLocal8Bit(process_.readAllStandardOutput());
        return output_;
    }

private:
    QProcess  process_;
    QString   output_;
    qsizetype consumed_ = 0;
};

// An instance holding the lock at `lockPath` and listening on
// `serverName`.
class OwnerProcess : public Helper {
public:
    OwnerProcess(const QString& lockPath, const QString& serverName)
        : Helper({QStringLiteral("owner"), lockPath, serverName})
    {
    }

    bool isReady() { return waitForLine(QStringLiteral("ready")) == QStringLiteral("ready"); }

    // Its event loop stops turning for `ms`, as when git runs on
    // GitBolt's GUI thread. Returns once it has stopped.
    bool beBusyFor(int ms)
    {
        send("busy " + QByteArray::number(ms));
        return waitForLine(QStringLiteral("busy")) == QStringLiteral("busy");
    }
};

// A later launch: claims the lock or hands `path` over, and says how
// that went.
class LaunchProcess : public Helper {
public:
    LaunchProcess(const QString& lockPath, const QString& serverName, const QString& path)
        : Helper({QStringLiteral("launch"), lockPath, serverName, path})
    {
    }

    QString outcome(int timeoutMs = 30000) { return waitForLine(QString(), timeoutMs); }
};

// A thread with an event loop, and a way to run code on it.
class Worker {
public:
    Worker()
    {
        context_ = new QObject;
        context_->moveToThread(&thread_);
        QObject::connect(&thread_, &QThread::finished, context_, &QObject::deleteLater);
        thread_.start();
    }

    ~Worker()
    {
        thread_.quit();
        thread_.wait();
    }

    void run(const std::function<void()>& work)
    {
        QMetaObject::invokeMethod(context_, work, Qt::BlockingQueuedConnection);
    }

private:
    QThread  thread_;
    QObject* context_ = nullptr;
};

} // namespace

class TestSingleInstance : public QObject {
    Q_OBJECT

private slots:
    void handsOverToTheOwner()
    {
        LockDir dir;
        QVERIFY(dir.dir.isValid());
        const QString name = uniqueServerName();
        OwnerProcess owner(dir.path(), name);
        QVERIFY2(owner.isReady(), qPrintable(owner.output()));

        LaunchProcess launch(dir.path(), name, QStringLiteral("/repo/b"));
        QCOMPARE(launch.outcome(), QStringLiteral("delivered"));
        QCOMPARE(owner.waitForLine(QStringLiteral("path:")), QStringLiteral("path:/repo/b"));

        // A bare relaunch: nothing to open, still answered.
        LaunchProcess bare(dir.path(), name, QString());
        QCOMPARE(bare.outcome(), QStringLiteral("delivered"));
        QCOMPARE(owner.waitForLine(QStringLiteral("path:")), QStringLiteral("path:"));
    }

    // An owner told to quit exits at once, its stdin still open (as
    // GitBolt's own is). On Linux it waited at its exit for its stdin
    // reader, until the 10 s above were up and it was killed: each test
    // that ended with one took 10 s more there.
    void anOwnerToldToQuitExitsAtOnce()
    {
        LockDir dir;
        QVERIFY(dir.dir.isValid());
        OwnerProcess owner(dir.path(), uniqueServerName());
        QVERIFY2(owner.isReady(), qPrintable(owner.output()));
        QElapsedTimer clock;
        clock.start();
        owner.send("quit");
        QVERIFY2(owner.process().waitForFinished(5000),
                 qPrintable(QStringLiteral("still running after %1 ms").arg(clock.elapsed())));
        QCOMPARE(owner.process().exitStatus(), QProcess::NormalExit);
        QCOMPARE(owner.process().exitCode(), 0);
    }

    // Busy for 3 s: longer than that would make a slow test. The
    // hand-off waits it out on every platform; on Windows even its
    // write can't leave before.
    void busyOwnerGetsThePathOnceFree()
    {
        LockDir dir;
        QVERIFY(dir.dir.isValid());
        const QString name = uniqueServerName();
        OwnerProcess owner(dir.path(), name);
        QVERIFY2(owner.isReady(), qPrintable(owner.output()));
        QVERIFY2(owner.beBusyFor(3000), qPrintable(owner.output()));

        QElapsedTimer clock;
        clock.start();
        LaunchProcess launch(dir.path(), name, QStringLiteral("/repo/a"));
        QCOMPARE(launch.outcome(), QStringLiteral("delivered"));
        qInfo("hand-off to a busy owner took %lld ms", clock.elapsed());
        QVERIFY2(clock.elapsed() >= 2000, qPrintable(QString::number(clock.elapsed())));
        QCOMPARE(owner.waitForLine(QStringLiteral("path:")), QStringLiteral("path:/repo/a"));
    }

    // The owner is killed while a launch waits on it: the launch takes
    // its place (and opens its path itself), rather than leave the path
    // with an owner that never reads it.
    void aKilledOwnersPlaceIsTaken()
    {
        LockDir dir;
        QVERIFY(dir.dir.isValid());
        const QString name = uniqueServerName();
        // The socket file a killed owner leaves (Unix).
        const auto staleSocket = qScopeGuard([&name] { QLocalServer::removeServer(name); });
        OwnerProcess owner(dir.path(), name);
        QVERIFY2(owner.isReady(), qPrintable(owner.output()));
        QVERIFY2(owner.beBusyFor(60000), qPrintable(owner.output()));

        LaunchProcess launch(dir.path(), name, QStringLiteral("/repo/b"));
        QTest::qWait(1500);
        QVERIFY2(launch.process().state() == QProcess::Running, qPrintable(launch.output()));
        owner.process().kill();
        QVERIFY(owner.process().waitForFinished(10000));

        QCOMPARE(launch.outcome(), QStringLiteral("owner"));
    }

    // Explorer runs `GitBolt.exe "%V"`, and %V ends in a backslash for a
    // drive root: "E:\" reaches the app as E: and a quote.
    void explorerDriveRoot()
    {
#ifndef Q_OS_WIN
        QSKIP("Explorer's command line is Windows'");
#else
        QProcess path;
        path.setProgram(QStringLiteral(GITBOLT_INSTANCE_HELPER));
        path.setNativeArguments(QStringLiteral("path \"E:\\\""));
        path.start();
        QVERIFY(path.waitForFinished(30000));
        QCOMPARE(QString::fromLocal8Bit(path.readAllStandardOutput()).trimmed(),
                 QStringLiteral("E:/"));
#endif
    }

    void noListener()
    {
        QElapsedTimer clock;
        clock.start();
        QCOMPARE(gitbolt::app::handOffToRunningInstance(uniqueServerName(),
                                                        QStringLiteral("/repo")),
                 Handoff::NoListener);
        QVERIFY2(clock.elapsed() < 3000, qPrintable(QString::number(clock.elapsed())));
    }

    // An owner on its way out takes the connection and closes it.
    void hungUp()
    {
        const QString name = uniqueServerName();
        Worker worker;
        QLocalServer* raw = nullptr;
        const auto cleanup = qScopeGuard([&worker, &raw] { worker.run([&raw] { delete raw; }); });
        bool listening = false;
        worker.run([&] {
            raw = new QLocalServer;
            listening = raw->listen(name);
            QObject::connect(raw, &QLocalServer::newConnection, raw, [raw] {
                while (QLocalSocket* sock = raw->nextPendingConnection()) {
                    sock->abort();
                    sock->deleteLater();
                }
            });
        });
        QVERIFY(listening);
        QCOMPARE(gitbolt::app::handOffToRunningInstance(name, QStringLiteral("/repo")),
                 Handoff::HungUp);
    }

    // The wait on a busy owner asks its caller every 100 ms, and the
    // caller (the busy window's Cancel) can stop it.
    void aWaitCanBeCancelled()
    {
        LockDir dir;
        QVERIFY(dir.dir.isValid());
        const QString name = uniqueServerName();
        OwnerProcess owner(dir.path(), name);
        QVERIFY2(owner.isReady(), qPrintable(owner.output()));
        QVERIFY2(owner.beBusyFor(3000), qPrintable(owner.output()));

        int asked = 0;
        QElapsedTimer clock;
        clock.start();
        const auto cancelSoon = [&asked](qint64 waitedMs) {
            ++asked;
            return waitedMs < 1000;
        };
        QCOMPARE(gitbolt::app::handOffToRunningInstance(name, QStringLiteral("/repo/a"),
                                                        cancelSoon),
                 Handoff::Cancelled);
        QVERIFY2(clock.elapsed() >= 900 && clock.elapsed() < 2500,
                 qPrintable(QString::number(clock.elapsed())));
        QVERIFY2(asked >= 5, qPrintable(QString::number(asked)));

        // And through the claim, the lock held by that owner.
        QVERIFY2(owner.beBusyFor(3000), qPrintable(owner.output()));
        QLockFile lock(dir.path());
        lock.setStaleLockTime(0);
        QCOMPARE(gitbolt::app::claimInstance(lock, name, QStringLiteral("/repo/a"), 10000,
                                             cancelSoon),
                 Claim::Cancelled);
        QVERIFY(!lock.isLocked());

        // Once free, the owner opens neither: the cancelled launches
        // took their path back. (On Unix the socket had buffered it, and
        // the owner switched to the repository regardless.) A path
        // handed over after them comes first.
        LaunchProcess after(dir.path(), name, QStringLiteral("/repo/after"));
        QCOMPARE(after.outcome(), QStringLiteral("delivered"));
        QCOMPARE(owner.waitForLine(QStringLiteral("path:")), QStringLiteral("path:/repo/after"));
    }

    void claimsAFreeLock()
    {
        LockDir dir;
        QVERIFY(dir.dir.isValid());
        QLockFile lock(dir.path());
        lock.setStaleLockTime(0);
        QCOMPARE(gitbolt::app::claimInstance(lock, uniqueServerName(), QStringLiteral("/repo")),
                 Claim::Owner);
        QVERIFY(lock.isLocked());
    }

    // Held, and nobody listening for the whole patience: an owner that
    // is alive (it holds the lock) but never answers.
    void claimGivesUpWhenNobodyListens()
    {
        LockDir dir;
        QVERIFY(dir.dir.isValid());
        QLockFile ownerLock(dir.path());
        QVERIFY(ownerLock.tryLock(0));

        QLockFile lock(dir.path());
        lock.setStaleLockTime(0);
        QElapsedTimer clock;
        clock.start();
        QCOMPARE(gitbolt::app::claimInstance(lock, uniqueServerName(), QStringLiteral("/repo"),
                                             1000),
                 Claim::Unresponsive);
        QVERIFY2(clock.elapsed() >= 1000, qPrintable(QString::number(clock.elapsed())));
        QVERIFY(!lock.isLocked());
        QVERIFY(ownerLock.isLocked());
    }

    void serverNameIsTheLocksScope()
    {
        const QString runtime = QDir::tempPath();
#ifdef Q_OS_WIN
        // A pipe name, the same for the lock's directory however it is
        // spelled, and another for another directory.
        const QString name = gitbolt::app::instanceServerName(QDir(runtime),
                                                              QStringLiteral("gb"));
        QVERIFY(name.startsWith(QStringLiteral("gb-")));
        QVERIFY(!name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\')));
        QCOMPARE(gitbolt::app::instanceServerName(QDir(runtime.toUpper()), QStringLiteral("gb")),
                 name);
        QCOMPARE(gitbolt::app::instanceServerName(QDir(runtime.toLower()), QStringLiteral("gb")),
                 name);
        QVERIFY(gitbolt::app::instanceServerName(QDir(runtime + QStringLiteral("/other")),
                                                 QStringLiteral("gb"))
                != name);
#else
        // A socket file beside the lock.
        QCOMPARE(gitbolt::app::instanceServerName(QDir(runtime), QStringLiteral("gb")),
                 QDir(runtime).filePath(QStringLiteral("gb")));
#endif
    }

    void repoPathArgument_data()
    {
        QTest::addColumn<QString>("argument");
        QTest::addColumn<QString>("path");

        QTest::newRow("relative") << QStringLiteral(".") << QDir::currentPath();
#ifdef Q_OS_WIN
        // What Explorer's command line for a drive root comes to (see
        // explorerDriveRoot), and for any other folder.
        QTest::newRow("a drive root from Explorer") << QStringLiteral("E:\"")
                                                    << QStringLiteral("E:/");
        QTest::newRow("a folder from Explorer") << QStringLiteral("C:\\repo")
                                                << QStringLiteral("C:/repo");
#else
        // Unix paths can contain a quote.
        QTest::newRow("a quote is kept") << QStringLiteral("/tmp/a\"")
                                         << QStringLiteral("/tmp/a\"");
#endif
    }

    void repoPathArgument()
    {
        QFETCH(QString, argument);
        QFETCH(QString, path);
        QCOMPARE(gitbolt::app::repoPathArgument(argument), path);
    }
};

QTEST_GUILESS_MAIN(TestSingleInstance)
#include "TestSingleInstance.moc"
