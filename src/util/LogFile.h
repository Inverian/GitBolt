#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QFile>
#include <QMutex>
#include <QString>
#include <QStringView>

namespace gitbolt::util {

/// The folder of GitBolt's log: ~/Library/Logs/GitBolt on macOS,
/// %LOCALAPPDATA%/GitBolt/logs on Windows, and $XDG_STATE_HOME/GitBolt
/// on Linux (an absolute XDG_STATE_HOME only; else ~/.local/state).
QString logDirectory();

/// A size-capped log: <dir>/<base>.log is the newest file, then
/// <base>.1.log up to <base>.<files-1>.log; the oldest goes when they
/// rotate. Created owner-only, as it may hold paths. Each line goes to
/// disk at once, so the last one before a crash is kept. Local only:
/// nothing uploads or reads it.
class RotatingLog {
public:
    RotatingLog(QString dir, QString base, qint64 maxBytes = 1024 * 1024, int files = 3);

    RotatingLog(const RotatingLog&) = delete;
    RotatingLog& operator=(const RotatingLog&) = delete;

    /// Appends `line` and a newline; first rotates the files when the
    /// line would take the newest past the cap. Thread-safe.
    void append(QByteArrayView line);

    QString filePath() const;

private:
    QString pathOf(int index) const;
    void open();
    void rotate();

    QMutex mutex_;
    const QString dir_;
    const QString base_;
    const qint64 maxBytes_;
    const int files_;
    QFile file_;
    qint64 size_ = 0;
    // A rename failed (Windows: another process has the file open):
    // this session keeps appending, and the next open tries again.
    bool rotationFailed_ = false;
};

/// "<local time, UTC offset> <level> <text>": the message handler's
/// line, for writing one directly.
QByteArray logLine(char level, QStringView text);

/// Also sends every Qt message (qDebug … qFatal) to `log`, as
/// "<time> <D|I|W|C|F> [<category> ]<message>", then on to the handler
/// installed before (Qt's own: stderr). A message raised while one is
/// written goes on without the log.
void installMessageLog(RotatingLog* log);

/// Puts back the handler installMessageLog() replaced. Call before
/// the log is destroyed.
void uninstallMessageLog();

} // namespace gitbolt::util
