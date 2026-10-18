#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>

class QEvent;

namespace gitbolt::app {

/// Folders the system asks GitBolt to open while it runs, as opposed to
/// a path on the command line: on macOS, open-documents events (a Dock
/// drop, `open -a GitBolt <folder>`, a launch by Finder) and Finder's
/// "Open in GitBolt" service (platform/macos/MacServices.h). Qt turns
/// the events into QFileOpenEvents sent to the application object; this
/// is the event filter that takes them, installed on qApp before the
/// event loop first turns, so none is missed.
///
/// Requests are held until setReady(), when the window can open them,
/// and then emitted queued, never from inside the event or service
/// call. Several before one is emitted (a burst of Dock drops, or the
/// ones that came during launch) end in the last: each would replace
/// the one before anyway.
class OpenRequests : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    /// A folder to open: made absolute and clean, with no trailing
    /// "/" (Qt hands folders over as URLs that have one, and Recent
    /// would get a second entry). A file means the folder it is in.
    void submit(const QString& path);

    /// The request held so far, taken: no openRequested for it. For a
    /// launch whose folder came as an event rather than in argv, before
    /// the single-instance guard decides who opens it.
    QString takePending();

    /// The path this launch opens anyway (from argv, or takePending).
    /// The same folder requested within 2 s is dropped: AppKit may send
    /// a launch argument back as an open-documents event. Doesn't look
    /// at the disk.
    void setLaunchPath(const QString& path);

    /// From now on, requests are emitted (queued); one already held
    /// goes now.
    void setReady();

    /// The clean form submit() gives a path. Exposed for tests.
    static QString normalized(const QString& path);

signals:
    void openRequested(const QString& path);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void schedule();
    void flush();

    QString pending_;
    QString launchPath_;
    QElapsedTimer launchClock_;
    bool ready_ = false;
    bool scheduled_ = false;
};

} // namespace gitbolt::app
