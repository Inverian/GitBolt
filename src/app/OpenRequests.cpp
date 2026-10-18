#include "app/OpenRequests.h"

#include <QDir>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QUrl>

#include <utility>

namespace gitbolt::app {

namespace {
// How long after setReady() a launch path coming back as an event is
// taken for that echo, and not for a request of its own.
constexpr qint64 kLaunchEchoMs = 2000;
} // namespace

QString OpenRequests::normalized(const QString& path)
{
    if (path.isEmpty())
        return {};
    const QFileInfo info(path);
    // A file dropped on the Dock icon: the folder it is in. One that
    // does not exist stays as it is, for the open to say so.
    const QString absolute = info.exists() && !info.isDir()
                                 ? info.absolutePath()
                                 : info.absoluteFilePath();
    return QDir::cleanPath(absolute);
}

void OpenRequests::submit(const QString& path)
{
    const QString folder = normalized(path);
    if (folder.isEmpty())
        return;
    // Once only: the same folder asked for again later is a request.
    // The launch path is as given (a file, say), not mapped.
    if (!launchPath_.isEmpty()
        && (folder == launchPath_
            || QDir::cleanPath(QFileInfo(path).absoluteFilePath()) == launchPath_)) {
        launchPath_.clear();
        if (!ready_ || launchClock_.elapsed() < kLaunchEchoMs)
            return;
    }
    pending_ = folder;
    if (ready_)
        schedule();
}

QString OpenRequests::takePending()
{
    return std::exchange(pending_, QString());
}

void OpenRequests::setLaunchPath(const QString& path)
{
    // Not normalized(): it would look at the disk, and this runs before
    // the single-instance guard, where a folder on a network share
    // that stopped answering must not hold the launch up.
    launchPath_ = path.isEmpty() ? QString()
                                 : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

void OpenRequests::setReady()
{
    ready_ = true;
    launchClock_.start();
    if (!pending_.isEmpty())
        schedule();
}

bool OpenRequests::eventFilter(QObject* watched, QEvent* event)
{
    // Installed on qApp, so this sees every event of every object:
    // the type check comes first.
    if (event->type() != QEvent::FileOpen)
        return QObject::eventFilter(watched, event);
    // A URL that is no file (GitBolt declares no URL scheme) is left
    // for whoever else wants it.
    const QUrl url = static_cast<QFileOpenEvent*>(event)->url();
    if (!url.isLocalFile())
        return false;
    submit(url.toLocalFile());
    return true;
}

void OpenRequests::schedule()
{
    if (scheduled_)
        return;
    scheduled_ = true;
    QMetaObject::invokeMethod(this, &OpenRequests::flush, Qt::QueuedConnection);
}

void OpenRequests::flush()
{
    scheduled_ = false;
    const QString path = std::exchange(pending_, QString());
    if (!path.isEmpty())
        emit openRequested(path);
}

} // namespace gitbolt::app
