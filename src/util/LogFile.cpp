#include "util/LogFile.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QTimeZone>
#ifdef Q_OS_WIN
#include <QStandardPaths>
#endif

#include <atomic>
#include <cstring>
#include <string>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>  // _O_APPEND
#include <io.h>     // _open_osfhandle, _close
#endif

namespace gitbolt::util {

namespace {

// The installed log, and the handler it replaced. g_logMutex keeps the
// log from going (uninstallMessageLog) while a message is written.
QMutex g_logMutex;
RotatingLog* g_log = nullptr;
std::atomic<QtMessageHandler> g_previous{nullptr};
thread_local bool t_writing = false;

char levelLetter(QtMsgType type) {
    switch (type) {
    case QtDebugMsg:    return 'D';
    case QtInfoMsg:     return 'I';
    case QtWarningMsg:  return 'W';
    case QtCriticalMsg: return 'C';
    case QtFatalMsg:    return 'F';
    }
    return '?';
}

void messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    // Not for a message the write itself raised (a QFile warning).
    if (!t_writing) {
        t_writing = true;
        {
            const QMutexLocker lock(&g_logMutex);
            if (g_log) {
                const bool named = context.category
                                   && std::strcmp(context.category, "default") != 0;
                g_log->append(logLine(levelLetter(type),
                                      named ? QString::fromUtf8(context.category)
                                                  + QLatin1Char(' ') + message
                                            : message));
            }
        }
        t_writing = false;
    }
    if (const QtMessageHandler previous = g_previous.load())
        previous(type, context, message);
}

constexpr QIODevice::OpenMode kAppend =
    QIODevice::WriteOnly | QIODevice::Append | QIODevice::Unbuffered;

#ifdef Q_OS_WIN
// A descriptor for `path` whose every write lands at the end, as with
// O_APPEND: append-only access (no FILE_WRITE_DATA). Qt's Append seeks
// to the end once, at open. A CRT descriptor, which Qt can use as
// GitBolt and Qt share the CRT (/MD). -1 if it can't.
int appendOnlyFd(const QString& path) {
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const HANDLE handle = CreateFileW(native.c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return -1;
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_APPEND);
    if (fd == -1)
        CloseHandle(handle);
    return fd;
}
#endif

} // namespace

QString logDirectory() {
#if defined(Q_OS_MACOS)
    return QDir::homePath() + QStringLiteral("/Library/Logs/GitBolt");
#elif defined(Q_OS_WIN)
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
           + QStringLiteral("/GitBolt/logs");
#else
    // XDG: an absolute XDG_STATE_HOME, else ~/.local/state.
    const QString state = qEnvironmentVariable("XDG_STATE_HOME");
    return (QDir::isAbsolutePath(state) ? state : QDir::homePath() + QStringLiteral("/.local/state"))
           + QStringLiteral("/GitBolt");
#endif
}

RotatingLog::RotatingLog(QString dir, QString base, qint64 maxBytes, int files)
    : dir_(std::move(dir)), base_(std::move(base)), maxBytes_(maxBytes), files_(files) {
    QDir().mkpath(dir_);
    const QMutexLocker lock(&mutex_);
    // An oversized file from an earlier session rotates now.
    if (QFileInfo(pathOf(0)).size() >= maxBytes_)
        rotate();
    else
        open();
}

QString RotatingLog::filePath() const {
    return pathOf(0);
}

QString RotatingLog::pathOf(int index) const {
    return index == 0 ? dir_ + QLatin1Char('/') + base_ + QStringLiteral(".log")
                      : dir_ + QLatin1Char('/') + base_ + QLatin1Char('.')
                            + QString::number(index) + QStringLiteral(".log");
}

// Qt creates the file (owner-only). Windows then swaps in an
// append-only descriptor, so two sessions sharing the file never
// overwrite each other's lines; if that fails, Qt's handle stays.
void RotatingLog::open() {
    file_.setFileName(pathOf(0));
    if (!file_.open(kAppend, QFile::ReadOwner | QFile::WriteOwner))
        return;
    size_ = file_.size();
#ifdef Q_OS_WIN
    if (const int fd = appendOnlyFd(pathOf(0)); fd != -1) {
        file_.close();
        if (!file_.open(fd, kAppend, QFileDevice::AutoCloseHandle)) {
            _close(fd);
            file_.setFileName(pathOf(0));
            (void)file_.open(kAppend);  // closed if not: append() skips
        }
    }
#endif
}

// <base>.log moves aside first: if that fails (Windows: another process
// has it open), no file has changed and this session keeps appending,
// never retrying per line. Then the oldest goes, the others move down
// one, and the newest becomes <base>.1.log. (A leftover aside file is
// from a rotation cut short; it would block every rename after it.)
void RotatingLog::rotate() {
    file_.close();
    const QString aside = dir_ + QLatin1Char('/') + base_ + QStringLiteral(".rotating.log");
    QFile::remove(aside);
    if (!QFile::rename(pathOf(0), aside)) {
        rotationFailed_ = true;
        open();
        return;
    }
    QFile::remove(pathOf(files_ - 1));
    for (int i = files_ - 2; i >= 1; --i) {
        if (QFile::exists(pathOf(i)))
            QFile::rename(pathOf(i), pathOf(i + 1));
    }
    QFile::rename(aside, pathOf(1));
    open();
}

void RotatingLog::append(QByteArrayView line) {
    const QMutexLocker lock(&mutex_);
    const qint64 bytes = line.size() + 1;
    if (!rotationFailed_ && size_ > 0 && size_ + bytes > maxBytes_)
        rotate();
    if (!file_.isOpen())
        return;
    // One write per line, and every write appends (see open()): other
    // processes on the same file (two test sessions) never split it.
    QByteArray data;
    data.reserve(bytes);
    data.append(line).append('\n');
    if (file_.write(data) == bytes)
        size_ += bytes;
}

QByteArray logLine(char level, QStringView text) {
    const QDateTime now = QDateTime::currentDateTime();
    const QString stamp = now.toTimeZone(QTimeZone::fromSecondsAheadOfUtc(now.offsetFromUtc()))
                              .toString(Qt::ISODateWithMs);
    return stamp.toUtf8() + ' ' + level + ' ' + text.toUtf8();
}

void installMessageLog(RotatingLog* log) {
    {
        const QMutexLocker lock(&g_logMutex);
        g_log = log;
    }
    g_previous = qInstallMessageHandler(messageHandler);
}

void uninstallMessageLog() {
    qInstallMessageHandler(g_previous.load());
    const QMutexLocker lock(&g_logMutex);
    g_log = nullptr;
}

} // namespace gitbolt::util
