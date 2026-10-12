#pragma once

#include <QDir>
#include <QLocalServer>
#include <QObject>
#include <QString>

#include <functional>

class QLockFile;

namespace gitbolt::app {

// The single-instance guard: one GitBolt per user, which a later
// launch (the shell extensions, a plain CLI launch) hands its
// repository path to instead of opening a second window. A library of
// its own so tests/app/TestSingleInstance.cpp runs it on every
// platform: the e2e tests, which launch the app itself, are Unix-only,
// and Windows' named pipes behave unlike Unix sockets here. main()
// keeps what the user sees.

/// Per-user directory for the single-instance lock and forwarding
/// socket. Linux/BSD: $XDG_RUNTIME_DIR, private to the user and never
/// aged out by tmp cleaners — in a shared /tmp, one user's lock (and
/// socket) would turn every other user's launch away. macOS's $TMPDIR
/// and Windows' %TEMP% are per-user already.
QString instanceRuntimeDir();

/// Name of the forwarding socket that the lock holder listens on.
/// Unix: a socket file beside the lock, in the same per-user directory.
/// Windows: a pipe name. Pipes share one machine-wide namespace, so a
/// fixed name would let one user's instance hold the pipe (its
/// UserAccessOption DACL shutting everyone else out) while a second
/// user's own instance, holding that user's lock, could never listen
/// — every later launch by that user would find no listener. A hash of
/// the lock's directory gives the pipe exactly the lock's scope: per
/// user, or per session where %TEMP% is (Remote Desktop hosts). Pipes
/// die with their owner, so there is never a stale one to clear.
QString instanceServerName(const QDir& runtimeDir, const QString& instanceName);

enum class Handoff { Delivered, HungUp, NoListener, Cancelled };

/// Called about every 100 ms while a hand-off waits on its owner, with
/// how long it has waited; returning false stops the wait. Whatever it
/// shows meanwhile needs it to turn the event loop too: the wait blocks
/// the calling thread.
using HandoffWait = std::function<bool(qint64 waitedMs)>;

/// Sends this launch's repo path (empty: just raise the window) to the
/// instance listening on `serverName`, as one line, and waits for the
/// line it answers with:
/// - Delivered: it answered.
/// - HungUp: the owner took the connection but closed it unanswered:
///   an instance on its way out (or killed while this waited), whose
///   lock is about to come free. The caller retries, and so takes its
///   place.
/// - NoListener: nothing took the connection. The caller retries.
/// - Cancelled: `wait` said to stop. The path is taken back: a
///   "cancel" line follows it, and the owner, once free, drops it.
///
/// It waits for as long as the connection lasts, which proves the
/// owner alive: its end dies with it. An owner can be busy for a long
/// time (a libgit2 checkout of a large tree still runs on its GUI
/// thread, and a modal dialog can hold up the hand-off as long as it is
/// open) and acts on the line once it is free. On Windows even the
/// write waits for that: Qt's pipes have no buffer, so a write
/// completes only once the owner's event loop has accepted the
/// connection, and is cancelled if this process exits first; a Unix
/// socket buffers the line at once. A deadline on the write used to
/// turn an owner that was merely busy for longer into "not
/// responding", and one on the answer (Unix) left the path with an
/// owner that might never read it, if it died busy.
Handoff handOffToRunningInstance(const QString& serverName, const QString& path,
                                 const HandoffWait& wait = {});

enum class Claim {
    Owner,         ///< this launch holds the lock: it runs, and listens
    Delivered,     ///< the owner took the path: this launch is done
    Unresponsive,  ///< the lock is held, and nobody took the path
    Unguarded,     ///< the lock can't be created at all: run anyway
    Cancelled,     ///< `wait` stopped a hand-off to a busy owner
};

/// Takes `lock`, or hands `path` to the instance that holds it. An
/// instance with the lock but no listener is usually still starting up
/// (it locks well before it listens) or shutting down, so this retries
/// for `patienceMs`; an owner that hangs up was alive a moment ago, so
/// its lock gets the full patience again to come free — a few times
/// only: an owner that keeps taking the line and hanging up isn't on its
/// way out, and waiting on it would never end. A lock file that is stale
/// in a way its contents can't prove (truncated by a crash) is removed.
/// `wait` is passed to each hand-off.
Claim claimInstance(QLockFile& lock, const QString& serverName, const QString& path,
                    int patienceMs = 10000, const HandoffWait& wait = {});

/// The lock holder's end of the hand-off: one line in from each launch,
/// empty for a bare relaunch, answered with one line out. A path that a
/// "cancel" line follows, its launch having given up the wait, is
/// answered and dropped. Only the lock
/// holder listens, so clearing a socket left by a killed instance can
/// never take a live one's. UserAccessOption keeps other local users
/// from driving our window.
class InstanceServer : public QObject {
    Q_OBJECT
public:
    explicit InstanceServer(QObject* parent = nullptr);

    /// Clears a stale socket and listens on `serverName`; false, with
    /// errorString() saying why, if it can't.
    bool listen(const QString& serverName);
    QString errorString() const;

signals:
    /// A launch's path, its receipt sent already.
    void pathReceived(const QString& path);

private:
    // A child, so it moves to the thread this one is moved to.
    QLocalServer server_{this};
};

/// The repository path argument as this launch opens or hands it over:
/// absolute, a relative one resolved against the working directory. On
/// Windows a trailing quote is put back as the backslash it was:
/// Explorer's "Open in GitBolt" runs `GitBolt.exe "%V"`, and %V ends in
/// a backslash for a drive root, so the command line ends in "E:\" and
/// Windows' argument rules read that \" as a literal quote, giving
/// E:". No Windows path contains a quote.
QString repoPathArgument(const QString& argument);

} // namespace gitbolt::app
