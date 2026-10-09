#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <deque>
#include <mutex>

namespace gitbolt::git {

/// One captured `git` invocation. Used as both the signal payload
/// and the ring-buffer entry for replay when a UI surface opens
/// late and wants to see the history.
struct GitProcessLogEntry {
    QString    workdir;
    QStringList args;
    int        exitCode = 0;
    qint64     durationMs = 0;
};

/// Single application-wide signal hub for "git X Y Z just ran" events.
/// GitProcess::run posts to this hub on every external git invocation;
/// any UI surface (Tools → Git command log, status bar, debug overlay)
/// can connect to `commandLogged` and display the events.
///
/// Implemented as a QObject singleton accessed via `instance()`. Lives
/// for the duration of the application — never freed.
///
/// Beyond live signaling, the hub keeps a fixed-size ring buffer of
/// the last N events so a dialog opened mid-session can pre-populate
/// itself with everything that has run already (call `recent()`).
/// Buffer access is mutex-guarded so emitCommand can be safely
/// called from any thread that hosts GitProcess::run, and pool
/// threads routinely do (remote ops, rebase, Git Flow, maintenance);
/// GUI-thread receivers get commandLogged queued.
class GitProcessLog : public QObject {
    Q_OBJECT
public:
    static GitProcessLog& instance();

    /// Called by GitProcess::run after the process has completed.
    /// `args` is the full argv excluding the leading `git`.
    /// `exitCode` is the process exit code; `durationMs` is wall-
    /// clock execution time in milliseconds (timing the call gives
    /// users a feel for slow ops without needing a profiler).
    void emitCommand(const QString& workdir,
                     const QStringList& args,
                     int exitCode,
                     qint64 durationMs);

    /// Snapshot of the ring buffer in chronological order (oldest
    /// first). Returned by value because callers want a stable
    /// view; the buffer keeps mutating live.
    std::vector<GitProcessLogEntry> recent() const;

signals:
    /// Fired after every GitProcess::run completes. Includes the
    /// working directory, the argv (already shell-quoted-ish for
    /// display), the exit code, and the wall-clock duration.
    void commandLogged(const QString& workdir,
                       const QStringList& args,
                       int exitCode,
                       qint64 durationMs);

private:
    GitProcessLog() = default;

    static constexpr size_t kRingCap = 200;
    mutable std::mutex          mutex_;
    std::deque<GitProcessLogEntry> ring_;
};

} // namespace gitbolt::git
