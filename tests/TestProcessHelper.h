#pragma once
//
// Helpers for tests that start git and check what it left running:
// a scoped environment variable, a process-liveness probe, and the
// URL of StallTransport (tests/git/StallTransport.cpp), a git "server"
// that never answers. Tests using the transport define
// GITBOLT_TEST_STALL_TRANSPORT (see tests/CMakeLists.txt).
//

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QList>
#include <QString>
#include <QtGlobal>

#include <string>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#endif

namespace gitbolt::test {

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

inline bool processAlive(qint64 pid)
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
// git connects to StallTransport while these are in scope: ext:: is
// off by default (protocol.ext.allow=never), and the transport writes
// its pids to `pidFile` once git has started it.
class StallTransportEnv {
public:
    explicit StallTransportEnv(const QString& pidFile)
        : allow_("GIT_ALLOW_PROTOCOL", "ext"),
          pidEnv_("GITBOLT_TEST_STALL_PIDFILE", QFile::encodeName(pidFile)) {}

    /// The URL to clone or fetch from.
    static std::string url()
    {
        // In an ext:: command '%' escapes and a space separates arguments.
        QString stub = QDir::toNativeSeparators(QStringLiteral(GITBOLT_TEST_STALL_TRANSPORT));
        stub.replace(QLatin1Char('%'), QStringLiteral("%%"));
        stub.replace(QLatin1Char(' '), QStringLiteral("% "));
        return "ext::" + stub.toStdString();
    }

private:
    ScopedEnv allow_;
    ScopedEnv pidEnv_;
};

/// What the transport recorded in `pidFile`: its own pid and, on Unix,
/// its process group (whose leader is git when git runs in a group of
/// its own). Zeros until the transport has started.
struct StallPids {
    qint64 stub = 0;
    qint64 group = 0;
};

inline StallPids readStallPids(const QString& pidFile)
{
    QFile f(pidFile);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QList<QByteArray> lines = f.readAll().split('\n');
    return {lines.value(0).trimmed().toLongLong(), lines.value(1).trimmed().toLongLong()};
}
#endif

} // namespace gitbolt::test
