#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace gitbolt::update {

/// A running program, as the system's process list names it.
struct RunningProgram {
    qint64 pid = 0;
    QString name;   ///< its file name, e.g. GitBolt.exe
    /// Its full path; empty when this process may not ask (another user's).
    QString path;
};

/// Windows: the running programs named GitBolt.exe, each with its path
/// where Windows gives it. Empty elsewhere, where an update replaces
/// files a running copy has open without trouble.
QList<RunningProgram> runningGitBolts();

/// `path` is inside `folder`, by Windows' rules: either separator, any
/// case. As the administrator part's in-use check decides it.
bool isInsideWindowsFolder(const QString& path, const QString& folder);

/// The GitBolt.exe processes of `programs`, `self` aside, that keep the
/// install folder `folder` in use.
struct FolderUsers {
    int running = 0;   ///< running from the folder
    /// Path unknown: another user's, whom an install for all users
    /// serves too, so it counts as running from the folder.
    int unknown = 0;
    bool any() const { return running + unknown > 0; }
};
FolderUsers gitBoltsUsing(const QList<RunningProgram>& programs, const QString& folder, qint64 self);

/// When process `pid` started, in seconds since the epoch, if it runs
/// as this user; 0 if it doesn't run (a zombie counts as gone) or isn't
/// this user's.
qint64 processStartTime(qint64 pid);

/// Linux: the start time /proc/<pid>/stat's `line` gives, with /proc/stat's
/// btime and the clock's ticks per second; 0 for a zombie or a line it
/// can't read. Here for the tests.
qint64 linuxStartTime(const QByteArray& line, qint64 bootTime, qint64 ticksPerSecond);

} // namespace gitbolt::update
