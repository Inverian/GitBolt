#include "util/LoginPath.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QList>
#include <QStringList>

#include <chrono>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#ifndef Q_OS_WIN
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef Q_OS_MACOS
#include <crt_externs.h>
#else
extern "C" char** environ;
#endif
#endif

namespace gitbolt::util {

namespace {

constexpr QChar kListSeparator =
#ifdef Q_OS_WIN
    QLatin1Char(';');
#else
    QLatin1Char(':');
#endif

struct Lookup {
    std::mutex mutex;
    bool started = false;
    std::shared_future<QString> result;
};

Lookup& lookup() {
    static Lookup instance;
    return instance;
}

#ifndef Q_OS_WIN
// What the shell prints its PATH after. An rc file may print anything
// before it (a greeting, nvm's notes); the line after this one is ours.
constexpr char kMarker[] = "__GITBOLT_LOGIN_PATH__";

char** hostEnviron() {
#ifdef Q_OS_MACOS
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

QString defaultShell() {
    if (const QByteArray shell = qgetenv("SHELL"); !shell.isEmpty())
        return QFile::decodeName(shell);
    // getpwuid_r: this runs on a thread of its own.
    struct passwd pw {};
    struct passwd* found = nullptr;
    std::vector<char> buffer(16384);
    if (getpwuid_r(getuid(), &pw, buffer.data(), buffer.size(), &found) == 0 && found
        && found->pw_shell && *found->pw_shell)
        return QFile::decodeName(found->pw_shell);
    return QStringLiteral("/bin/sh");
}

// The line after the marker, if it's a PATH: absolute entries only.
QString parseShellOutput(const QByteArray& output) {
    const QList<QByteArray> lines = output.split('\n');
    for (qsizetype i = 0; i + 1 < lines.size(); ++i) {
        if (lines[i].trimmed() != kMarker)
            continue;
        const QString path = QFile::decodeName(lines[i + 1].trimmed());
        const QStringList entries = path.split(kListSeparator, Qt::SkipEmptyParts);
        if (entries.isEmpty())
            return {};
        for (const QString& entry : entries) {
            if (!QDir::isAbsolutePath(entry))
                return {};
        }
        return path;
    }
    return {};
}
#endif

} // namespace

QString mergePathLists(const QString& first, const QString& second) {
    QStringList merged;
    for (const QString& list : {first, second}) {
        for (const QString& entry : list.split(kListSeparator, Qt::SkipEmptyParts)) {
            if (!merged.contains(entry))
                merged.append(entry);
        }
    }
    return merged.join(kListSeparator);
}

bool shouldLookUpLoginPath() {
#if defined(Q_OS_WIN)
    return false;
#elif defined(Q_OS_MACOS)
    return true;
#else
    return !isatty(STDIN_FILENO);
#endif
}

QString queryLoginShellPath(const QString& shell, int timeoutMs) {
#ifdef Q_OS_WIN
    Q_UNUSED(shell);
    Q_UNUSED(timeoutMs);
    return {};
#else
    const QByteArray program = QFile::encodeName(shell);
    if (program.isEmpty())
        return {};
    // `printenv`, not `echo "$PATH"`: fish quotes its list its own way.
    QByteArray command = QByteArrayLiteral("echo ");
    command += kMarker;
    command += "; printenv PATH";
    QByteArray flags = QByteArrayLiteral("-ilc");
    std::vector<char*> argv{const_cast<char*>(program.constData()), flags.data(),
                            command.data(), nullptr};

    int fds[2];
    if (::pipe(fds) != 0)
        return {};
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    // A session of its own: no controlling terminal, so an interactive
    // shell started from one (zsh opens /dev/tty) can't take it over,
    // and one group to kill, whatever the shell started, on a timeout.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    int spawnFlags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_SETSID
    spawnFlags |= POSIX_SPAWN_SETSID;
#else
    spawnFlags |= POSIX_SPAWN_SETPGROUP;
    posix_spawnattr_setpgroup(&attributes, 0);
#endif
    posix_spawnattr_setflags(&attributes, static_cast<short>(spawnFlags));
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&attributes, &none);
    sigset_t defaults;
    sigemptyset(&defaults);
    for (const int sig : {SIGINT, SIGQUIT, SIGTERM, SIGPIPE, SIGCHLD, SIGHUP})
        sigaddset(&defaults, sig);
    posix_spawnattr_setsigdefault(&attributes, &defaults);

    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, program.constData(), &actions, &attributes,
                                    argv.data(), hostEnviron());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    ::close(fds[1]);
    if (spawned != 0) {
        ::close(fds[0]);
        return {};
    }

    QByteArray output;
    bool timedOut = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            timedOut = true;
            break;
        }
        pollfd readable{fds[0], POLLIN, 0};
        const int ready = ::poll(&readable, 1, static_cast<int>(left));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0) {
            timedOut = ready == 0;
            break;
        }
        char chunk[4096];
        const ssize_t got = ::read(fds[0], chunk, sizeof chunk);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;      // EOF: the shell (and whatever kept its stdout) is done
        output.append(chunk, got);
        if (output.size() > 1024 * 1024)
            break;      // not a PATH
    }
    ::close(fds[0]);
    if (timedOut)
        ::kill(-pid, SIGKILL);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (timedOut)
        return {};
    // Whatever it exited with (an rc file's last command may fail): the
    // marker says whether the command ran.
    return parseShellOutput(output);
#endif
}

void startLoginPathLookup(const QString& shell) {
#ifdef Q_OS_WIN
    Q_UNUSED(shell);
#else
    Lookup& state = lookup();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.started)
        return;
    state.started = true;
    std::promise<QString> promise;
    state.result = promise.get_future().share();
    // Detached: a quit doesn't wait for a slow shell (it's killed at
    // its timeout all the same, by this thread or with the process).
    std::thread([shell, promise = std::move(promise)]() mutable {
        const QString program = shell.isEmpty() ? defaultShell() : shell;
        promise.set_value(queryLoginShellPath(program, kLoginPathTimeoutMs));
    }).detach();
#endif
}

QString pathForChildren(const QString& own) {
    std::shared_future<QString> result;
    {
        Lookup& state = lookup();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.started)
            return own;
        result = state.result;
    }
    // The lookup has its own timeout; a little more for the thread.
    if (result.wait_for(std::chrono::milliseconds(kLoginPathTimeoutMs + 1000))
        != std::future_status::ready)
        return own;
    const QString login = result.get();
    return login.isEmpty() ? own : mergePathLists(login, own);
}

} // namespace gitbolt::util
