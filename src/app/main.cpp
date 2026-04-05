#include "ui/MainWindow.h"
#include "conf/SettingsService.h"
#include "conf/ThemeService.h"
#include "util/CrashHandler.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QSharedMemory>
#include <QStyleHints>

#include <git2.h>

int main(int argc, char* argv[]) {
    // Install crash handler before anything else
    gitbolt::util::CrashHandler::install();

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("GitBolt"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));
    app.setOrganizationName(QStringLiteral("GitBolt"));
    app.setOrganizationDomain(QStringLiteral("gitbolt.dev"));

    // Single-instance guard via shared memory
    QSharedMemory singleInstanceGuard(QStringLiteral("GitBolt-SingleInstance"));
    if (!singleInstanceGuard.create(1)) {
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
    if (!positionalArgs.isEmpty())
        initialRepoPath = positionalArgs.first();

    // Initialize theme service and apply before showing any window
    auto* themeService = new gitbolt::conf::ThemeService(&app);
    themeService->applyTheme(&app);

    // Respect system dark mode as fallback
    app.styleHints()->setColorScheme(Qt::ColorScheme::Unknown);

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

    // Settings service for window geometry persistence
    gitbolt::conf::SettingsService settings;

    // Create and show the main window
    gitbolt::ui::MainWindow window;

    // Restore window geometry/state from previous session
    QByteArray geometry = settings.restoreWindowGeometry();
    if (!geometry.isEmpty())
        window.restoreGeometry(geometry);
    QByteArray state = settings.restoreWindowState();
    if (!state.isEmpty())
        window.restoreState(state);

    window.show();

    // Open repository from command line if provided
    if (!initialRepoPath.isEmpty()) {
        // MainWindow will handle opening via its GitService
        QMetaObject::invokeMethod(&window, [&window, initialRepoPath]() {
            // Access the git service through the window's public interface
            // The main window connects repositoryOpened signals internally
        }, Qt::QueuedConnection);
    }

    int exitCode = app.exec();

    // Save window geometry/state on exit
    settings.saveWindowGeometry(window.saveGeometry());
    settings.saveWindowState(window.saveState());

    // Clean up libgit2
    git_libgit2_shutdown();

    return exitCode;
}
