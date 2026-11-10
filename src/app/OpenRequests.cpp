#include "app/OpenRequests.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QTimer>
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

std::optional<QString> OpenRequests::waitForLaunchFolder(const std::function<bool()>& launched, int capMs)
{
    // A real exec, not one processEvents(): on macOS that runs AppKit's
    // own [NSApp run] launch, whose folder event can come after the
    // first turn of the loop but always before didFinishLaunching.
    const auto done = [this, &launched] { return !pending_.isEmpty() || launched(); };
    bool timedOut = false;
    if (!done()) {
        QEventLoop loop;
        QElapsedTimer clock;
        clock.start();
        QTimer poll;
        poll.setInterval(10);
        // Ends with 1: a 0 is QCoreApplication::exit(0), a quit from macOS
        // the next exec() would forget (kept if it came in the same pass).
        QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
            if (!loop.isRunning())
                return;
            if (done()) {
                loop.exit(1);
            } else if (clock.elapsed() >= capMs) {
                timedOut = true;
                loop.exit(1);
            }
        });
        poll.start();
        if (loop.exec() != 1)
            return std::nullopt;
    }
    // A folder event posted just as the launch finished.
    QCoreApplication::processEvents();
    if (timedOut)
        qWarning("macOS had not finished launching GitBolt after %d ms", capMs);
    return takePending();
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
