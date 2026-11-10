//
// TestOpenRequests — the folders macOS asks a running GitBolt to open
// (src/app/OpenRequests.h): open-documents events, as Qt delivers them
// to the application object, and Finder's service. Platform-neutral:
// the events are sent here the way Qt sends them. A launch's folder
// is waited for until the system has finished launching GitBolt.
//

#include "app/OpenRequests.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>

#include <optional>

using gitbolt::app::OpenRequests;

namespace {

// What Qt does with an open-documents event: a QFileOpenEvent sent to
// the application object. True if a filter took it.
bool sendFileOpen(const QUrl& url)
{
    QFileOpenEvent event(url);
    return QCoreApplication::sendEvent(qApp, &event);
}

// The same event, posted: delivered at the loop's next turn.
void postFileOpen(const QUrl& url)
{
    QCoreApplication::postEvent(qApp, new QFileOpenEvent(url));
}

// What a launch wait ended with, "(quit)" for a quit.
QString folderOf(const std::optional<QString>& waited)
{
    return waited.value_or(QStringLiteral("(quit)"));
}

// Records the events that reach it, past the application's filters.
class EventRecorder : public QObject {
public:
    QList<QEvent::Type> seen;

    bool event(QEvent* e) override
    {
        seen << e->type();
        return QObject::event(e);
    }
};

QStringList emitted(const QSignalSpy& spy)
{
    QStringList paths;
    for (const QList<QVariant>& args : spy)
        paths << args.at(0).toString();
    return paths;
}

} // namespace

class TestOpenRequests : public QObject {
    Q_OBJECT

private slots:
    // Held until the window is up, then emitted once: a launch by
    // Finder has its folder before there is a window to open it in.
    void heldUntilReady() {
        QTemporaryDir dir;
        OpenRequests requests;
        qApp->installEventFilter(&requests);
        QSignalSpy spy(&requests, &OpenRequests::openRequested);

        QVERIFY(sendFileOpen(QUrl::fromLocalFile(dir.path())));
        QCoreApplication::processEvents();
        QCOMPARE(spy.count(), 0);

        requests.setReady();
        QCOMPARE(spy.count(), 0);   // queued, never from inside setReady
        QTRY_COMPARE(spy.count(), 1);
        QCOMPARE(emitted(spy), QStringList{QDir::cleanPath(dir.path())});
        QCoreApplication::processEvents();
        QCOMPARE(spy.count(), 1);
    }

    // Emitted queued, after the event or service call has returned:
    // the open runs a modal box when it fails, and Finder's service
    // call would wait for it.
    void emittedAfterTheEventReturns() {
        QTemporaryDir dir;
        OpenRequests requests;
        qApp->installEventFilter(&requests);
        requests.setReady();
        QSignalSpy spy(&requests, &OpenRequests::openRequested);

        QVERIFY(sendFileOpen(QUrl::fromLocalFile(dir.path())));
        QCOMPARE(spy.count(), 0);
        QTRY_COMPARE(spy.count(), 1);
    }

    // Several before one is emitted end in the last: each open
    // replaces the one before it anyway.
    void aBurstEndsInTheLast() {
        QTemporaryDir a, b, c, d;
        OpenRequests requests;
        qApp->installEventFilter(&requests);
        QSignalSpy spy(&requests, &OpenRequests::openRequested);

        requests.submit(a.path());
        requests.submit(b.path());
        requests.setReady();
        QTRY_COMPARE(spy.count(), 1);
        QCOMPARE(emitted(spy), QStringList{QDir::cleanPath(b.path())});

        requests.submit(c.path());
        requests.submit(d.path());
        QTRY_COMPARE(spy.count(), 2);
        QCoreApplication::processEvents();
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.at(1).at(0).toString(), QDir::cleanPath(d.path()));
    }

    // Qt hands a folder over as a URL with a trailing "/"; Recent must
    // not get "repo/" next to "repo".
    void trailingSlashIsDropped() {
        QTemporaryDir dir;
        OpenRequests requests;
        qApp->installEventFilter(&requests);
        requests.setReady();
        QSignalSpy spy(&requests, &OpenRequests::openRequested);

        QVERIFY(sendFileOpen(QUrl::fromLocalFile(dir.path() + QStringLiteral("/"))));
        QTRY_COMPARE(spy.count(), 1);
        const QString path = spy.first().at(0).toString();
        QVERIFY2(!path.endsWith(QLatin1Char('/')), qPrintable(path));
        QCOMPARE(path, QDir::cleanPath(dir.path()));
    }

    // A file dropped on the Dock icon: the folder it is in.
    void aFileMeansItsFolder() {
        QTemporaryDir dir;
        const QString file = QDir(dir.path()).filePath(QStringLiteral("notes.txt"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        QCOMPARE(OpenRequests::normalized(file), QDir::cleanPath(dir.path()));
        // One that is not there is left for the open to report.
        const QString missing = QDir(dir.path()).filePath(QStringLiteral("gone/x"));
        QCOMPARE(OpenRequests::normalized(missing), QDir::cleanPath(missing));
        QCOMPARE(OpenRequests::normalized(QString()), QString());
    }

    // GitBolt declares no URL scheme: a URL that is no file is left
    // alone, and so is every other event. (The filter is the
    // application's, so it sees the events of every object.)
    void otherEventsPassThrough() {
        OpenRequests requests;
        qApp->installEventFilter(&requests);
        requests.setReady();
        QSignalSpy spy(&requests, &OpenRequests::openRequested);

        QVERIFY(!sendFileOpen(QUrl(QStringLiteral("https://www.gitbolt.com/"))));
        EventRecorder recorder;
        QFileOpenEvent web(QUrl(QStringLiteral("https://www.gitbolt.com/")));
        QCoreApplication::sendEvent(&recorder, &web);
        QEvent user(QEvent::User);
        QCoreApplication::sendEvent(&recorder, &user);
        QCOMPARE(recorder.seen, (QList<QEvent::Type>{QEvent::FileOpen, QEvent::User}));
        QCoreApplication::processEvents();
        QCOMPARE(spy.count(), 0);
    }

    // AppKit may send a launch argument back as an open-documents
    // event: the launch opens it already, so one is dropped, and only
    // one; a different folder is a request of its own.
    void theLaunchPathComingBackIsDropped() {
        QTemporaryDir launch, other;
        OpenRequests requests;
        qApp->installEventFilter(&requests);
        requests.setLaunchPath(launch.path() + QStringLiteral("/"));
        requests.setReady();
        QSignalSpy spy(&requests, &OpenRequests::openRequested);

        requests.submit(launch.path());
        QCoreApplication::processEvents();
        QCOMPARE(spy.count(), 0);

        requests.submit(other.path());
        QTRY_COMPARE(spy.count(), 1);
        requests.submit(launch.path());
        QTRY_COMPARE(spy.count(), 2);
        QCOMPARE(emitted(spy), (QStringList{QDir::cleanPath(other.path()),
                                            QDir::cleanPath(launch.path())}));
    }

    void theLaunchPathBeforeReadyIsDropped() {
        QTemporaryDir launch;
        OpenRequests requests;
        requests.setLaunchPath(launch.path());
        QSignalSpy spy(&requests, &OpenRequests::openRequested);
        requests.submit(launch.path());
        requests.setReady();
        QCoreApplication::processEvents();
        QCOMPARE(spy.count(), 0);
    }

    // A launch path the event names as it is, not as its folder: a
    // file given on the command line, sent back by AppKit.
    void aLaunchFileComingBackIsDropped() {
        QTemporaryDir dir;
        const QString file = QDir(dir.path()).filePath(QStringLiteral("notes.txt"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        OpenRequests requests;
        requests.setLaunchPath(file);
        requests.setReady();
        QSignalSpy spy(&requests, &OpenRequests::openRequested);
        requests.submit(file);
        QCoreApplication::processEvents();
        QCOMPARE(spy.count(), 0);
    }

    // Later than that, the same folder is asked for again on purpose.
    void theLaunchPathLaterIsARequest() {
        QTemporaryDir launch;
        OpenRequests requests;
        requests.setLaunchPath(launch.path());
        requests.setReady();
        QSignalSpy spy(&requests, &OpenRequests::openRequested);
        QTest::qWait(2200);
        requests.submit(launch.path());
        QTRY_COMPARE(spy.count(), 1);
    }

    // A launch whose folder came as an event takes it before the
    // single-instance guard: it is opened there, or handed over, and
    // never emitted as well.
    void takenRequestsAreNotEmitted() {
        QTemporaryDir dir;
        OpenRequests requests;
        requests.submit(dir.path());
        QCOMPARE(requests.takePending(), QDir::cleanPath(dir.path()));
        QCOMPARE(requests.takePending(), QString());
        QSignalSpy spy(&requests, &OpenRequests::openRequested);
        requests.setReady();
        QCoreApplication::processEvents();
        QCOMPARE(spy.count(), 0);
    }

    // A launch's folder event can come after the loop's first turn
    // (one turn alone could miss it, and a second launch handed over
    // nothing): it is waited for, and the wait ends as it comes.
    void aLateLaunchFolderIsWaitedFor() {
        QTest::failOnWarning(QRegularExpression(QStringLiteral("had not finished launching")));
        QTemporaryDir dir;
        OpenRequests requests;
        qApp->installEventFilter(&requests);
        bool launched = false;
        QObject context;   // its timers go with it
        QTimer::singleShot(150, &context, [&] { postFileOpen(QUrl::fromLocalFile(dir.path())); });
        QTimer::singleShot(1500, &context, [&] { launched = true; });
        QElapsedTimer clock;
        clock.start();
        QCOMPARE(folderOf(requests.waitForLaunchFolder([&] { return launched; }, 3000)),
                 QDir::cleanPath(dir.path()));
        QVERIFY2(clock.elapsed() < 1500, qPrintable(QString::number(clock.elapsed())));
    }

    // One posted as the launch finishes is still taken.
    void aFolderPostedAsLaunchFinishesIsTaken() {
        QTemporaryDir dir;
        OpenRequests requests;
        qApp->installEventFilter(&requests);
        postFileOpen(QUrl::fromLocalFile(dir.path()));
        QCOMPARE(folderOf(requests.waitForLaunchFolder([] { return true; }, 3000)),
                 QDir::cleanPath(dir.path()));
    }

    // A launch with no folder goes on once the launch has finished.
    void noFolderReturnsOnceLaunched() {
        QTest::failOnWarning(QRegularExpression(QStringLiteral("had not finished launching")));
        OpenRequests requests;
        qApp->installEventFilter(&requests);
        bool launched = false;
        QObject context;
        QTimer::singleShot(100, &context, [&] { launched = true; });
        QElapsedTimer clock;
        clock.start();
        QCOMPARE(folderOf(requests.waitForLaunchFolder([&] { return launched; }, 3000)), QString());
        QVERIFY(launched);
        QVERIFY2(clock.elapsed() < 2000, qPrintable(QString::number(clock.elapsed())));
    }

    // A launch that never says it has finished waits no longer than the
    // cap, and says so.
    void theWaitEndsAtTheCap() {
        OpenRequests requests;
        qApp->installEventFilter(&requests);
        QTest::ignoreMessage(QtWarningMsg, "macOS had not finished launching GitBolt after 300 ms");
        QElapsedTimer clock;
        clock.start();
        QCOMPARE(folderOf(requests.waitForLaunchFolder([] { return false; }, 300)), QString());
        QVERIFY2(clock.elapsed() >= 300, qPrintable(QString::number(clock.elapsed())));
        QVERIFY2(clock.elapsed() < 2000, qPrintable(QString::number(clock.elapsed())));
    }

    // A quit during the wait (macOS's, at a logout) is told, not lost:
    // app.exec() would forget it, and GitBolt start anyway.
    void aQuitDuringTheWaitIsTold() {
        OpenRequests requests;
        qApp->installEventFilter(&requests);
        QObject context;
        // What Qt sends the application when macOS asks it to quit.
        QTimer::singleShot(50, &context, [] {
            QEvent quit(QEvent::Quit);
            QCoreApplication::sendEvent(qApp, &quit);
        });
        QElapsedTimer clock;
        clock.start();
        const std::optional<QString> waited = requests.waitForLaunchFolder([] { return false; }, 3000);
        const qint64 elapsed = clock.elapsed();
        // Qt holds a quit until exec() next starts, and every event loop
        // till then returns at once: one starts and ends, for later tests.
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        QCoreApplication::exec();
        QCOMPARE(folderOf(waited), QStringLiteral("(quit)"));
        QVERIFY2(elapsed < 2000, qPrintable(QString::number(elapsed)));
    }
};

QTEST_GUILESS_MAIN(TestOpenRequests)
#include "TestOpenRequests.moc"
