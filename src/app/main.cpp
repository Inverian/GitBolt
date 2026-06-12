#include "TestBridge.h"
#include "ui/MainWindow.h"
#include "conf/SettingsService.h"
#include "conf/ThemeService.h"
#include "util/CrashHandler.h"

#include <memory>

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QIcon>
#include <QInputDialog>
#include <QMessageBox>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSharedMemory>
#include <QStyleHints>
#include <QTextStream>

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
    app.setApplicationVersion(QStringLiteral("0.1.0"));
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

    // Single-instance guard via shared memory.
    //
    // QSharedMemory does NOT auto-clean on Unix when a process is killed
    // with SIGKILL or crashes — the segment leaks and would falsely block
    // future launches forever. The standard idiom is: try to attach first;
    // if we can attach to an existing segment, detach immediately to release
    // the orphan, then proceed to create our own.
    //
    // This still races against a *real* second instance starting at the
    // same time, but for a desktop app that's acceptable. The behavior
    // matches what most QSharedMemory tutorials recommend.
    // GITBOLT_INSTANCE_NAME keys both the guard segment and the
    // forwarding socket into a private namespace. Without it, an e2e
    // harness launching GitBolt while the user has a real session
    // open would forward its test repo into the user's window.
    const QString instanceOverride =
        qEnvironmentVariable("GITBOLT_INSTANCE_NAME");
    QSharedMemory singleInstanceGuard(
        instanceOverride.isEmpty()
            ? QStringLiteral("GitBolt-SingleInstance")
            : instanceOverride + QStringLiteral("-guard"));
    if (singleInstanceGuard.attach()) {
        // Either there's a real running instance, or we attached to an
        // orphaned segment from a previous crashed run. Either way,
        // detach to release our handle.
        singleInstanceGuard.detach();
    }
    const QString instanceServerName =
        instanceOverride.isEmpty() ? QStringLiteral("gitbolt-instance")
                                   : instanceOverride;
    if (!singleInstanceGuard.create(1)) {
        // A real instance is running (or a foreign-user orphan holds
        // the segment). Forward our repo argument to it — this is
        // how "Open in GitBolt" from Finder/Nautilus reaches an
        // already-open window; both shell extensions spawn a fresh
        // process that used to die here with a modal warning,
        // dropping the path on the floor.
        QLocalSocket forwarder;
        forwarder.connectToServer(instanceServerName);
        if (forwarder.waitForConnected(1000)) {
            // argv[1] is the repo path when present; an empty
            // payload still raises the running window.
            QString fwd;
            if (app.arguments().size() > 1) {
                const QFileInfo fi(app.arguments().at(1));
                fwd = fi.absoluteFilePath();
            }
            forwarder.write(fwd.toUtf8());
            forwarder.flush();
            forwarder.waitForBytesWritten(1000);
            return 0;
        }
        // No listener (e.g. the other instance is still starting up,
        // or the segment is a foreign-user orphan): fall back to the
        // old clear, dismissable warning.
        QMessageBox::warning(nullptr, QStringLiteral("GitBolt"),
                             QObject::tr("Another instance of GitBolt is already running."));
        return 1;
    }

    // Initialize libgit2
    git_libgit2_init();

    // Parse command-line arguments: gitbolt [path]
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
    // extensions, a plain CLI launch) forwards its path here instead
    // of dying on the single-instance guard. UserAccessOption keeps
    // other local users from driving our window.
    QLocalServer instanceServer;
    QLocalServer::removeServer(instanceServerName);  // stale socket
    instanceServer.setSocketOptions(QLocalServer::UserAccessOption);
    if (instanceServer.listen(instanceServerName)) {
        QObject::connect(
            &instanceServer, &QLocalServer::newConnection,
            &window, [&instanceServer, &window]() {
                while (QLocalSocket* sock =
                           instanceServer.nextPendingConnection()) {
                    QObject::connect(
                        sock, &QLocalSocket::readyRead, &window,
                        [sock, &window]() {
                            const QString path = QString::fromUtf8(
                                sock->readAll()).trimmed();
                            window.show();
                            window.raise();
                            window.activateWindow();
                            if (!path.isEmpty())
                                window.openRepositoryAtPath(path);
                            sock->disconnectFromServer();
                        });
                    QObject::connect(sock, &QLocalSocket::disconnected,
                                     sock, &QObject::deleteLater);
                }
            });
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

    // Clean up libgit2
    git_libgit2_shutdown();

    return exitCode;
}
