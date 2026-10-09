#include "TestBridge.h"
#include "ui/MainWindow.h"
#include "conf/SettingsService.h"
#include "conf/ThemeService.h"
#include "util/CrashHandler.h"

#include <memory>

#include <QApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QInputDialog>
#include <QMessageBox>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTextStream>
#include <QThread>
#include <QThreadPool>

#include <git2.h>

namespace {

// Askpass mode: git and ssh re-invoke this same binary as
// `$GIT_ASKPASS "<prompt>"` when they need a credential (the env is
// wired up in GitProcess::applyEnvironment). Show one modal field,
// print the answer to stdout, exit. Cancel exits nonzero, which makes
// git abort the operation with a normal auth error instead of hanging.
int runAskpass(const QString& prompt)
{
    // "Username for 'https://…'" wants visible text; everything else
    // (Password for…, Enter passphrase for key…) is a secret.
    const bool secret =
        !prompt.contains(QStringLiteral("username"), Qt::CaseInsensitive);

    QInputDialog dialog;
    dialog.setWindowTitle(QStringLiteral("GitBolt"));
    dialog.setLabelText(prompt.trimmed().isEmpty()
                            ? QStringLiteral("Credential:")
                            : prompt.trimmed());
    dialog.setInputMode(QInputDialog::TextInput);
    if (secret)
        dialog.setTextEchoMode(QLineEdit::Password);
    // The parent process is a faceless git child — nothing focuses
    // this window for us, and it must not get lost behind the app.
    dialog.setWindowFlag(Qt::WindowStaysOnTopHint);
    dialog.show();
    dialog.raise();
    dialog.activateWindow();
    if (dialog.exec() != QDialog::Accepted)
        return 1;

    QTextStream(stdout) << dialog.textValue() << "\n";
    return 0;
}

// Per-user directory for the single-instance lock and forwarding
// socket. Linux/BSD: $XDG_RUNTIME_DIR, private to the user and never
// aged out by tmp cleaners — in a shared /tmp, one user's lock (and
// socket) would turn every other user's launch away. macOS's $TMPDIR
// and Windows' %TEMP% are per-user already.
QString instanceRuntimeDir()
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
    const QString runtime =
        QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (!runtime.isEmpty())
        return runtime;
#endif
    return QDir::tempPath();
}

// Name of the forwarding socket that the lock holder listens on.
// Unix: a socket file beside the lock, in the same per-user directory.
// Windows: a pipe name. Pipes share one machine-wide namespace, so a
// fixed name would let one user's instance hold the pipe (its
// UserAccessOption DACL shutting everyone else out) while a second
// user's own instance, holding that user's lock, could never listen
// — every later launch by that user would find no listener. A hash of
// the lock's directory gives the pipe exactly the lock's scope: per
// user, or per session where %TEMP% is (Remote Desktop hosts). Pipes
// die with their owner, so there is never a stale one to clear.
QString instanceServerName(const QDir& runtimeDir,
                           const QString& instanceName)
{
#ifdef Q_OS_WIN
    const QByteArray scope = QCryptographicHash::hash(
        runtimeDir.absolutePath().toLower().toUtf8(),
        QCryptographicHash::Sha256);
    return instanceName + QLatin1Char('-')
           + QString::fromLatin1(scope.toHex().left(16));
#else
    return runtimeDir.filePath(instanceName);
#endif
}

// Clears a leftover lock file that QLockFile's own PID check can never
// prove stale: one truncated by a crash mid-write, or naming another
// machine ID. Safe against a live owner: its flock (Unix) or open
// handle (Windows) dies with it, and removeStaleLockFile() fails while
// one is held. The age check stays clear of the instant between a new
// owner creating the file and locking it. Serialised through
// "<lock>.rmlock" exactly as QLockFile serialises its own stale
// removal: two launchers removing at once could otherwise each unlink
// a lock the other had just re-created, and both would run as the
// single instance.
bool removeUnprovableStaleLock(QLockFile& lock)
{
    const auto settled = [&lock]() {
        const QDateTime written = QFileInfo(lock.fileName()).lastModified();
        return written.isValid()
               && qAbs(written.secsTo(QDateTime::currentDateTime())) >= 2;
    };
    if (!settled())
        return false;
    QLockFile removal(lock.fileName() + QStringLiteral(".rmlock"));
    // Re-checked under the removal lock: another launcher may have
    // just removed the file and re-created it as a live lock.
    return removal.tryLock(0) && settled() && lock.removeStaleLockFile();
}

enum class Handoff { Delivered, Queued, HungUp, NoListener };

// Sends this launch's repo path (empty: just raise the window) to the
// instance listening on `serverName`, as one line, and waits for the
// line it answers with:
// - Delivered: it answered.
// - Queued: the line left this process but no answer came in time.
//   The owner is alive but busy — submodule commands, sparse checkout
//   and Find Large Files run git synchronously on its GUI thread, for
//   minutes at worst — and acts on the line once it is free.
// - HungUp: the owner took the connection but closed it unanswered:
//   an instance on its way out (or killed while this waited), whose
//   lock is about to come free. The caller retries.
// - NoListener: nothing took the connection. The caller retries.
//
// Until the line has left, this waits for as long as the connection
// lasts: it proves the owner alive, as its end dies with it. Only
// Windows waits here — Qt's pipes have no buffer, so a write
// completes only once the owner's event loop has accepted the
// connection, and is cancelled if this process exits first; a Unix
// socket buffers the line at once. A deadline here used to turn an
// owner that was merely busy for longer into "not responding".
Handoff handOffToRunningInstance(const QString& serverName,
                                 const QString& path)
{
    QLocalSocket sock;
    sock.connectToServer(serverName);
    if (!sock.waitForConnected(1000))
        return Handoff::NoListener;

    sock.write(path.toUtf8() + '\n');
    while (sock.bytesToWrite() > 0) {
        if (sock.state() != QLocalSocket::ConnectedState)
            return Handoff::HungUp;
        sock.waitForBytesWritten(1000);
    }

    const QDeadlineTimer receipt(10000);
    while (!sock.canReadLine()) {
        if (sock.state() != QLocalSocket::ConnectedState)
            return Handoff::HungUp;
        if (receipt.hasExpired())
            return Handoff::Queued;
        sock.waitForReadyRead(int(receipt.remainingTime()));
    }
    return Handoff::Delivered;
}

// The hand-off is with a busy owner, which acts on it once it is free.
// Nothing for the user to do, so no dialog — just a note for whoever
// started this from a terminal.
void reportBusyInstance(const QString& path)
{
    if (path.isEmpty())
        qWarning("GitBolt is already running but busy; its window "
                 "comes to the front once it is free.");
    else
        qWarning("GitBolt is already running but busy; it opens %s "
                 "once it is free.", qPrintable(path));
}

// The lock's owner is alive (it still holds the lock) but never took
// the hand-off. Tell the user, who can close or kill it — except on a
// headless platform (offscreen/minimal: CI, the e2e harness), where a
// modal has nobody to dismiss it and would hang this process for good.
void reportUnresponsiveInstance()
{
    const QString text = QObject::tr(
        "Another instance of GitBolt is already running but is not responding.");
    qWarning("%s", qPrintable(text));
    const QString platform = QGuiApplication::platformName();
    if (platform != QStringLiteral("offscreen")
        && platform != QStringLiteral("minimal"))
        QMessageBox::warning(nullptr, QStringLiteral("GitBolt"), text);
}

} // namespace

int main(int argc, char* argv[]) {
    // Install crash handler before anything else
    gitbolt::util::CrashHandler::install();

    // macOS hides menu icons by default in Qt. We want GitExtensions-style
    // colored glyphs in the Commands menu, so force them on. Must be set
    // BEFORE the QApplication is constructed.
    QApplication::setAttribute(Qt::AA_DontShowIconsInMenus, false);

    QApplication app(argc, argv);

    // Use Fusion style on macOS so QMenu honors our per-action icons
    // (the native QMacStyle ignores iconVisibleInMenu/AA_DontShowIconsInMenus).
    // Fusion matches cross-platform GitExtensions look-and-feel.
    app.setStyle(QStringLiteral("fusion"));

    app.setApplicationName(QStringLiteral("GitBolt"));
    // GITBOLT_VERSION comes from the project() version in the
    // top-level CMakeLists — the single source of truth that CPack
    // also stamps into package names. A literal here once drifted.
    app.setApplicationVersion(QStringLiteral(GITBOLT_VERSION));
    app.setOrganizationName(QStringLiteral("GitBolt"));
    app.setOrganizationDomain(QStringLiteral("gitbolt.dev"));

    // App-level window icon. Resolves via the Qt resource system
    // (":/icons/*") which the .qrc at src/app/resources/gitbolt.qrc
    // bundles from the canonical resources/icons/ folder — so any
    // time that folder is regenerated (e.g. tools/generate-icon.py),
    // the next build automatically picks up the new icon with no
    // further wiring. Covers Linux taskbar / Windows title bar;
    // macOS layers the .icns bundle file on top via CFBundleIconFile.
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/gitbolt-256.png")));

    // Askpass re-invocation MUST short-circuit before the single-
    // instance guard: with GitBolt already running, the guard would
    // forward this process's argument (the prompt text!) to the open
    // window and exit 0 — git would read empty stdout as an empty
    // password. ssh passes no identifying flag, so the env marker set
    // by GitProcess::applyEnvironment is the switch.
    if (qEnvironmentVariableIsSet("GITBOLT_ASKPASS_MODE")) {
        return runAskpass(argc > 1 ? QString::fromLocal8Bit(argv[1])
                                   : QString());
    }

    // Initialize libgit2, and shut it down only once everything that
    // uses it is gone: locals are destroyed in reverse order, so this
    // runs after the window below, whose GitService waits for its
    // libgit2 workers and frees the repository; then it waits for the
    // pool jobs left (a dashboard probe opens repositories). Shutting
    // down at the end of main(), as before, did all that after libgit2
    // was gone.
    git_libgit2_init();
    const auto libgit2Shutdown = qScopeGuard([] {
        QThreadPool::globalInstance()->waitForDone();
        git_libgit2_shutdown();
    });

    // Parse command-line arguments: gitbolt [path]. This MUST come
    // before the single-instance guard: --help, --version and a bad
    // option exit() from inside process(), skipping every destructor,
    // so a guard taken first was left behind by each such run.
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("GitBolt - A modern Git GUI"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("path"),
                                 QObject::tr("Repository path to open on launch."));
    parser.process(app);

    const QStringList positionalArgs = parser.positionalArguments();
    QString initialRepoPath;
    if (!positionalArgs.isEmpty()) {
        // Resolve the argument to an absolute, canonical path relative to
        // the user's current working directory. A bare "." should become
        // the full path, not stay as the literal ".".
        const QFileInfo fi(positionalArgs.first());
        initialRepoPath = fi.absoluteFilePath();
    }

    // Single-instance guard: a lock file in a per-user directory.
    //
    // It used to be QSharedMemory, i.e. a System V segment on macOS,
    // which nothing reclaims when its owner is SIGKILLed or crashes.
    // Leaked segments piled up until macOS's limit of 32 was reached,
    // after which every launch failed to create one and was turned
    // away as "already running". QLockFile records our PID (plus host
    // and boot IDs), and tryLock() itself clears a lock whose owner is
    // gone. Stale time 0: a live owner's lock never expires by age.
    //
    // GITBOLT_INSTANCE_NAME keys both the lock and the forwarding
    // socket into a private namespace. Without it, an e2e harness
    // launching GitBolt while the user has a real session open would
    // forward its test repo into the user's window.
    const QString instanceOverride =
        qEnvironmentVariable("GITBOLT_INSTANCE_NAME");
    const QString instanceName =
        instanceOverride.isEmpty() ? QStringLiteral("gitbolt-instance")
                                   : instanceOverride;
    const QDir runtimeDir(instanceRuntimeDir());
    const QString serverName = instanceServerName(runtimeDir, instanceName);
    QLockFile instanceLock(
        runtimeDir.filePath(instanceName + QStringLiteral(".lock")));
    instanceLock.setStaleLockTime(0);
    QDeadlineTimer patience(10000);
    int hangUps = 0;
    while (!instanceLock.tryLock(0)) {
        if (instanceLock.error() != QLockFile::LockFailedError) {
            // Can't create the lock at all (unwritable directory, full
            // disk). Nothing the user could fix from a dialog, and no
            // reason to refuse to start: run unguarded.
            qWarning("Cannot create %s; starting without the "
                     "single-instance guard",
                     qPrintable(instanceLock.fileName()));
            break;
        }
        // Another instance holds the lock. Hand it our repo argument —
        // this is how "Open in GitBolt" from Finder/Nautilus reaches
        // an already-open window: both shell extensions spawn a fresh
        // process.
        const Handoff handoff =
            handOffToRunningInstance(serverName, initialRepoPath);
        if (handoff == Handoff::Delivered)
            return 0;
        if (handoff == Handoff::Queued) {
            reportBusyInstance(initialRepoPath);
            return 0;
        }
        // The owner hung up: alive a moment ago, however long this
        // waited on it, and on its way out now. Its lock gets the full
        // patience to come free — a few times only: an owner that keeps
        // taking the line and hanging up isn't on its way out, and
        // waiting on it would never end.
        if (handoff == Handoff::HungUp && ++hangUps <= 3) {
            patience = QDeadlineTimer(10000);
        } else if (patience.hasExpired()) {
            reportUnresponsiveInstance();
            return 1;
        }
        // Nobody listening, or the owner hung up unanswered. Usually it
        // is still starting up (it locks well before its server below
        // exists) or shutting down, so retry — unless the lock is stale
        // in a way its contents can't prove, which removal settles.
        if (removeUnprovableStaleLock(instanceLock))
            continue;
        QThread::msleep(100);
    }

    // Settings service — constructed before the theme so the saved
    // theme choice is restored before any window paints. Also used
    // further down for window-geometry persistence.
    gitbolt::conf::SettingsService settings;

    // Let QStyleHints report the real OS scheme — applyTheme's
    // "System" branch samples it. This MUST run before the theme is
    // applied: resetting the scheme afterwards regenerates the
    // application palette from the system and silently clobbers an
    // explicit Light/Dark restore (the bug that made a persisted
    // Dark theme come back light).
    app.styleHints()->setColorScheme(Qt::ColorScheme::Unknown);

    // Initialize theme service and apply before showing any window.
    // setTheme() no-ops when the saved name equals the default, so
    // the explicit applyTheme covers the fresh-install/System case.
    auto* themeService = new gitbolt::conf::ThemeService(&app);
    themeService->setTheme(settings.theme());
    themeService->applyTheme(&app);

    // Persist every later change (the Settings dialog calls
    // ThemeService::setTheme; nothing used to write the choice
    // anywhere, so it reset to System on each launch).
    QObject::connect(themeService,
                     &gitbolt::conf::ThemeService::themeChanged,
                     &settings,
                     [&settings](const QString& name) {
                         settings.setTheme(name);
                     });

    // Check for crash report from a previous session
    if (gitbolt::util::CrashHandler::hasPendingCrashReport()) {
        QString report = gitbolt::util::CrashHandler::readCrashReport();
        QMessageBox crashBox;
        crashBox.setWindowTitle(QObject::tr("Crash Detected"));
        crashBox.setText(QObject::tr(
            "GitBolt crashed during the last session. "
            "A crash report has been saved."));
        crashBox.setDetailedText(report);
        crashBox.setStandardButtons(QMessageBox::Ok);
        crashBox.exec();
        gitbolt::util::CrashHandler::clearCrashReport();
    }

    // Create and show the main window. Inject the app-wide theme
    // service so the Settings dialog can list and switch themes
    // from one shared instance (rather than MainWindow creating a
    // second ThemeService that would fight over the palette).
    gitbolt::ui::MainWindow window;
    window.setThemeService(themeService);

    // Restore window geometry from previous session. (Dock/toolbar
    // state via QMainWindow::saveState is no longer persisted — the
    // window has no dock widgets and the toolbar is non-movable, so
    // there's nothing left for saveState to encode.)
    QByteArray geometry = settings.restoreWindowGeometry();
    if (!geometry.isEmpty())
        window.restoreGeometry(geometry);

    window.show();

    // Test bridge — agent/test control channel, never active unless
    // explicitly requested via the environment. Declared after
    // `window` so it is destroyed first. See docs/AGENT_TESTING.md.
    std::unique_ptr<gitbolt::app::TestBridge> testBridge;
    if (qEnvironmentVariableIsSet("GITBOLT_TEST_BRIDGE"))
        testBridge = std::make_unique<gitbolt::app::TestBridge>(&window);

    // Second-instance handshake: a later `gitbolt <path>` (the shell
    // extensions, a plain CLI launch) hands its path over here — one
    // line, empty for a bare relaunch, answered with one — instead of
    // opening a second window. Only the lock holder listens, so
    // clearing a socket left by a killed instance can never take a
    // live one's.
    // UserAccessOption keeps other local users from driving our window.
    QLocalServer instanceServer;
    if (instanceLock.isLocked()) {
        QLocalServer::removeServer(serverName);  // stale socket
        instanceServer.setSocketOptions(QLocalServer::UserAccessOption);
        if (instanceServer.listen(serverName)) {
            QObject::connect(
                &instanceServer, &QLocalServer::newConnection,
                &window, [&instanceServer, &window]() {
                    while (QLocalSocket* sock =
                               instanceServer.nextPendingConnection()) {
                        QObject::connect(
                            sock, &QLocalSocket::readyRead, &window,
                            [sock, &window]() {
                                if (!sock->canReadLine())
                                    return;  // rest of the path in flight
                                const QString path = QString::fromUtf8(
                                    sock->readLine()).trimmed();
                                // The sender's receipt, flushed now:
                                // opening the repo below may sit in a
                                // dialog for a while.
                                sock->write("ok\n");
                                sock->flush();
                                sock->disconnectFromServer();
                                window.show();
                                window.raise();
                                window.activateWindow();
                                if (!path.isEmpty())
                                    window.openRepositoryAtPath(path);
                            });
                        QObject::connect(sock, &QLocalSocket::disconnected,
                                         sock, &QObject::deleteLater);
                    }
                });
        } else {
            qWarning("Single-instance server: listen(%s) failed: %s",
                     qPrintable(serverName),
                     qPrintable(instanceServer.errorString()));
        }
    }

    // Open repository from command line if provided. We queue the call so
    // it runs after the event loop starts and the window is fully shown.
    if (!initialRepoPath.isEmpty()) {
        QMetaObject::invokeMethod(&window, [&window, initialRepoPath]() {
            window.openRepositoryAtPath(initialRepoPath);
        }, Qt::QueuedConnection);
    }

    int exitCode = app.exec();

    // Save window geometry on exit. Splitter state is persisted
    // separately inside MainWindow::closeEvent.
    settings.saveWindowGeometry(window.saveGeometry());

    return exitCode;
}
