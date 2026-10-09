//
// StallTransport — a git "server" that never answers, for TestClone's
// cancel test. The test clones `ext::<this binary>`: git starts it as
// the transport and waits for a ref advertisement that never comes,
// so the clone sits in its connect phase — deterministically, with no
// network and no large fixture — until it is cancelled.
//
// It is a grandchild of git (git → git remote-ext → this), exactly
// where git-remote-https and ssh live in a real clone, and it records
// its pid before stalling so the test can check that cancel stopped
// git's whole process tree, not just git. On Unix it records its
// process group too: GitProcess::clone makes git the group's leader,
// so that is git's pid, and the test can tell when git is gone.
//

#include <QCoreApplication>
#include <QFile>

#include <chrono>
#include <thread>

#if !defined(Q_OS_WIN)
#include <unistd.h>
#endif

int main()
{
    const QString pidFile = qEnvironmentVariable("GITBOLT_TEST_STALL_PIDFILE");
    if (!pidFile.isEmpty()) {
        // Write-then-rename, so the test never reads a partial pid.
        QFile tmp(pidFile + QStringLiteral(".tmp"));
        if (tmp.open(QIODevice::WriteOnly)) {
            tmp.write(QByteArray::number(QCoreApplication::applicationPid()) + '\n');
#if !defined(Q_OS_WIN)
            tmp.write(QByteArray::number(static_cast<qint64>(::getpgrp())) + '\n');
#endif
            tmp.close();
            tmp.rename(pidFile);
        }
    }
    // Far longer than the test runs: if cancel failed to kill this
    // process, the test sees it still alive.
    std::this_thread::sleep_for(std::chrono::seconds(60));
    return 0;
}
