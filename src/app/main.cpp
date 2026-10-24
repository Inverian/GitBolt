#include "OpenRequests.h"
#include "SingleInstance.h"
#include "TelemetryEndpoints.h"
#include "TestBridge.h"
#include "ui/MainWindow.h"
#include "conf/SettingsService.h"
#include "conf/ThemeService.h"
#include "git/Askpass.h"
#include "git/GitProcess.h"
#include "telemetry/EventQueue.h"
#include "telemetry/TelemetryConfig.h"
#include "telemetry/TelemetryGate.h"
#include "telemetry/TelemetryService.h"
#include "util/CrashHandler.h"
#include "util/LockFiles.h"
#include "util/LoginPath.h"
#include "util/TestSession.h"
#include "util/Translations.h"
#include "widgets/PlainTextDialogs.h"

#ifdef Q_OS_MACOS
#include "platform/macos/MacServices.h"
#endif

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // SetDllDirectoryW
#endif

#include <memory>
#include <string_view>

#include <QApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QInputDialog>
#include <QMessageBox>
#include <QLockFile>
#include <QScopeGuard>
#include <QStyleHints>
#include <QTextStream>
#include <QThreadPool>

#include <git2.h>

namespace {

// Askpass mode: git and ssh re-invoke this same binary as
// `$GIT_ASKPASS "<prompt>"` when they need a credential (the env is
// wired up in GitProcess::applyEnvironment). Show one modal field,
// print the answer to stdout, exit. Cancel exits nonzero, which makes
// git abort the operation with a normal auth error instead of hanging.
// ssh's yes-or-no questions (a new host key) get Yes / No instead.
int runAskpass(const QString& prompt)
{
    const auto kind =
        gitbolt::git::askpassKind(prompt, qgetenv("SSH_ASKPASS_PROMPT"));
    if (kind == gitbolt::git::AskpassKind::YesNo) {
        // Plain text: the host key's fingerprint is to be read, and
        // the prompt is ssh's, not markup. No, unless the user says
        // yes: ssh's confirm() and ask_permission() take the word.
        QMessageBox box(QMessageBox::Question, QStringLiteral("GitBolt"),
                        prompt.trimmed(), QMessageBox::Yes | QMessageBox::No);
        box.setTextFormat(Qt::PlainText);
        box.setDefaultButton(QMessageBox::No);
        box.setWindowFlag(Qt::WindowStaysOnTopHint);
        box.show();
        box.raise();
        box.activateWindow();
        const bool yes = box.exec() == QMessageBox::Yes;
        QTextStream(stdout) << (yes ? "yes" : "no") << "\n";
        return 0;
    }
    const bool secret = kind == gitbolt::git::AskpassKind::Secret;

    QInputDialog dialog;
    dialog.setWindowTitle(QStringLiteral("GitBolt"));
    // Plain text, as the Yes / No box's: the prompt names the URL, and
    // its user name can be the server's (it redirects the request to a
    // URL with one in it), so "<b>…</b>" there was rendered, the very
    // place where the user decides whom to give a password.
    gitbolt::widgets::PlainInputDialog::setLabelText(
        dialog, prompt.trimmed().isEmpty() ? QStringLiteral("Credential:")
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

// How long a hand-off waits on a busy owner before saying so.
constexpr qint64 kBusyNoticeMs = 10000;

// A hand-off waits for as long as its owner lives (see
// handOffToRunningInstance): a busy owner acts on it once it is free,
// and if it dies busy instead, this launch takes its place and opens
// the path itself. A launch from Finder, Nautilus or Explorer would
// wait with nothing to show for it, so after a while this says so: a
// note for whoever started it from a terminal and, where someone can
// see it, a window whose Cancel stops the wait. Not on a headless
// platform (offscreen/minimal: CI, the e2e harness): nobody is there.
class BusyInstanceNotice {
public:
    explicit BusyInstanceNotice(const QString& path) : path_(path) {}

    gitbolt::app::HandoffWait wait()
    {
        return [this](qint64 waitedMs) { return keepWaiting(waitedMs); };
    }

    // Gone once this launch takes the owner's place.
    void dismiss() { box_.reset(); }

private:
    bool keepWaiting(qint64 waitedMs)
    {
        if (waitedMs >= kBusyNoticeMs && !noticed_) {
            noticed_ = true;
            note();
            if (!gitbolt::util::isHeadlessPlatform(QGuiApplication::platformName()))
                showBox();
        }
        // The hand-off blocks this thread: the window's events go
        // through here.
        if (box_)
            QCoreApplication::processEvents();
        return !cancelled_;
    }

    void note() const
    {
        if (path_.isEmpty())
            qWarning("GitBolt is already running but busy; its window "
                     "comes to the front once it is free.");
        else
            qWarning("GitBolt is already running but busy; it opens %s "
                     "once it is free.", qPrintable(path_));
    }

    void showBox()
    {
        box_ = std::make_unique<QMessageBox>(
            QMessageBox::Information, QStringLiteral("GitBolt"),
            path_.isEmpty()
                ? QObject::tr("GitBolt is busy. Its window will come to the "
                              "front when it is free.")
                : QObject::tr("GitBolt is busy. It will open %1 when it is "
                              "free.").arg(QDir::toNativeSeparators(path_)),
            QMessageBox::Cancel);
        box_->setObjectName(QStringLiteral("app.busyInstance"));
        // The path's folder names are anyone's, a cloned repository's
        // included.
        box_->setTextFormat(Qt::PlainText);
        // Cancel, Escape or the close button: this launch stops
        // waiting.
        QObject::connect(box_.get(), &QDialog::finished, box_.get(),
                         [this]() { cancelled_ = true; });
        box_->show();
        box_->raise();
        box_->activateWindow();
    }

    QString                      path_;
    std::unique_ptr<QMessageBox> box_;
    bool                         noticed_   = false;
    bool                         cancelled_ = false;
};

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
        gitbolt::widgets::PlainMessageBox::warning(nullptr, QStringLiteral("GitBolt"), text);
}

namespace endpoints = gitbolt::app::telemetry_endpoints;

// The GitBolt maintainers' usage statistics endpoint, which CI compiles
// into official builds (.github/workflows/ci.yml).
constexpr std::string_view kOfficialHost = "https://telemetry.gitbolt.com";

// Whether this build reports to the GitBolt maintainers: their endpoint
// is compiled in (cmake/Telemetry.cmake). Source builds, forks, PR and
// dry-run builds have no endpoint; a developer's loopback stub build, or
// a fork or staging build with an https endpoint of its own, isn't
// official either. Their crash reports would otherwise be uploaded by
// the user's next launch of a release, which reads the same
// ~/.gitbolt/crash.log, or a release's by them.
constexpr bool isOfficialBuild()
{
    return std::string_view(endpoints::kHost) == kOfficialHost;
}

// The usage statistics queue's directory, next to GitBolt.ini.
// "telemetry" is an official build's. Any other build that sends — a
// developer's loopback stub build, or a fork or staging build — keeps
// one per endpoint: with one shared queue, the next release would
// upload what that build recorded (its events, its errors under its
// version), and that build would drain the release's queue into its
// own endpoint.
QString telemetryQueueDir(const QString& settingsFile)
{
    const QString dir =
        QFileInfo(settingsFile).absolutePath() + QStringLiteral("/telemetry");
    if (isOfficialBuild())
        return dir;
    const QByteArray scope = QCryptographicHash::hash(
        QByteArrayView(endpoints::kHost), QCryptographicHash::Sha256);
    return dir + QStringLiteral("-local-")
           + QString::fromLatin1(scope.toHex().left(8));
}

} // namespace

int main(int argc, char* argv[]) {
#ifdef Q_OS_WIN
    // DLLs from the system's and GitBolt's own folders, never first from
    // the current one: Explorer's "Open in GitBolt" on a folder's
    // background starts GitBolt in that folder, a repository. Before
    // QApplication, which loads the platform plugin and what it needs;
    // not inherited by the programs GitBolt starts.
    SetDllDirectoryW(L"");
#endif
    // An askpass for a git command nobody is waiting on (the periodic
    // auto-fetch, GitProcess::forbidPrompts): no, at once, before
    // anything could show a window or a Dock icon. Fails the question,
    // which fails git or ssh with its own reason.
    if (qgetenv(gitbolt::git::kAskpassModeEnv) == gitbolt::git::kAskpassRefuse)
        return 1;

    // Which repository git works in is the open one's business, never
    // GitBolt's environment's: a shell that exports GIT_DIR or
    // GIT_WORK_TREE (a bare dotfiles setup) sent git's commands to that
    // repository. Gone for the whole process, so nothing it starts
    // inherits them, an editor that Open Externally starts included.
    // (GitProcess, the terminals and GitK leave them out as well.)
    for (const auto& name : gitbolt::git::GitProcess::repositoryEnvVars())
        qunsetenv(name.c_str());

    // Install crash handler before anything else. Its report header
    // says whether a later launch may upload the report: official
    // build, no environment opt-out, not a test session. There is no
    // QApplication yet, so the test-session check reads argv itself.
    gitbolt::util::CrashHandler::install(
        GITBOLT_VERSION, isOfficialBuild(),
        gitbolt::util::isEnvOptOut(gitbolt::util::detectEnvOptOut()),
        gitbolt::util::isTestSession(
            gitbolt::util::detectTestSession(argc, argv)));

    // macOS hides menu icons by default in Qt. We want GitExtensions-style
    // colored glyphs in the Commands menu, so force them on. Must be set
    // BEFORE the QApplication is constructed.
    QApplication::setAttribute(Qt::AA_DontShowIconsInMenus, false);

    QApplication app(argc, argv);

    // English plural forms for every %n text ("1 file", "2 files"),
    // whatever the locale; a build without them shows "(s)".
    gitbolt::util::installEnglishPlurals();

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
    app.setOrganizationDomain(QStringLiteral("gitbolt.com"));
    // The desktop entry is gitbolt.desktop (and the metainfo's
    // launchable). Unset, Qt's Wayland plugin made up the app_id
    // "com.gitbolt.gitbolt" from the domain and the executable, which
    // no desktop file matches: GNOME and KDE gave the window a generic
    // icon and kept it apart from the launcher and a pinned favorite.
    // Inert off Linux; set everywhere so dump-state shows it in any
    // e2e run. Before the askpass branch: its prompt groups too.
    app.setDesktopFileName(QStringLiteral("gitbolt"));

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
    if (qEnvironmentVariableIsSet(gitbolt::git::kAskpassModeEnv)) {
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
        // Resolve the argument to an absolute path relative to the
        // user's current working directory (and on Windows, mend an
        // Explorer drive root's).
        initialRepoPath = gitbolt::app::repoPathArgument(positionalArgs.first());
    }
    // Out of the folder GitBolt was started in, now that the path
    // argument is absolute: on Windows, home (see leaveStartingFolder).
    gitbolt::app::leaveStartingFolder();

    // Folders macOS asks a running GitBolt to open: open-documents
    // events (a Dock drop, `open -a GitBolt <folder>`), taken from here
    // on, and Finder's "Open in GitBolt" service (set up below, once
    // this launch is the one that opens things). Held until the window
    // is up (see OpenRequests.h).
    gitbolt::app::OpenRequests openRequests;
    app.installEventFilter(&openRequests);
#ifdef Q_OS_MACOS
    // A launch by Finder or LaunchServices has its folder in an event,
    // not in argv, and the event comes once the event loop first
    // turns. Turn it now, before the single-instance guard: a launch
    // that finds GitBolt running hands its path over below and exits,
    // and the folder would go with it. (BusyInstanceNotice turns the
    // loop before any window exists, too.)
    if (initialRepoPath.isEmpty()
        && QGuiApplication::platformName() == QLatin1String("cocoa")) {
        QCoreApplication::processEvents();
        initialRepoPath = openRequests.takePending();
    }
    openRequests.setLaunchPath(initialRepoPath);
#endif

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
    const QDir runtimeDir(gitbolt::app::instanceRuntimeDir());
    const QString serverName =
        gitbolt::app::instanceServerName(runtimeDir, instanceName);
    QLockFile instanceLock(
        runtimeDir.filePath(instanceName + QStringLiteral(".lock")));
    instanceLock.setStaleLockTime(0);
    // If another instance holds the lock, hand it our repo argument —
    // this is how "Open in GitBolt" from Explorer or Nautilus, and a
    // `gitbolt <path>`, reach an already-open window: each starts a
    // fresh process. Finder's service and macOS's open events go to a
    // running GitBolt directly (openRequests); a fresh process gets
    // here only when it was launched for one (see above).
    BusyInstanceNotice busyNotice(initialRepoPath);
    const gitbolt::app::Claim claim = gitbolt::app::claimInstance(
        instanceLock, serverName, initialRepoPath, 10000, busyNotice.wait());
    // The wait is over: the owner took the path, or this launch takes
    // the place of one that died busy.
    busyNotice.dismiss();
    switch (claim) {
    case gitbolt::app::Claim::Owner:
        break;
    case gitbolt::app::Claim::Unguarded:
        // Nothing the user could fix from a dialog, and no reason to
        // refuse to start: run unguarded.
        qWarning("Cannot create %s; starting without the "
                 "single-instance guard",
                 qPrintable(instanceLock.fileName()));
        break;
    case gitbolt::app::Claim::Delivered:
        return 0;
    case gitbolt::app::Claim::Cancelled:  // the user stopped the wait
        return 1;
    case gitbolt::app::Claim::Unresponsive:
        reportUnresponsiveInstance();
        return 1;
    }

#ifdef Q_OS_MACOS
    // Finder's "Open in GitBolt", only now that this is the GitBolt
    // that opens things: macOS gives the service to the first process
    // that asks, for as long as it runs. A launch that then handed its
    // folder over, or a test or the dogfood driver (an instance name of
    // their own), would have kept it from the user's GitBolt. Those
    // keep the provider for the test bridge's service-open only.
    gitbolt::app::macos::installServicesProvider(
        [&openRequests](const QStringList& folders) {
            openRequests.submit(folders.first());
        },
        instanceOverride.isEmpty());
    const auto servicesProviderGone = qScopeGuard([] {
        gitbolt::app::macos::installServicesProvider({}, false);
    });
#endif

    // The PATH git's children get, a commit's hooks among them: the
    // login shell's, asked in the background now, well before the
    // first commit (util/LoginPath.h). Started from the Dock, GitBolt
    // has launchd's PATH, and a hook that needs node (husky,
    // lint-staged) turned every commit down.
    if (gitbolt::util::shouldLookUpLoginPath())
        gitbolt::util::startLoginPathLookup();

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

    // Usage statistics (src/telemetry, PRIVACY.md). One gate, evaluated
    // once, here: no endpoints compiled in, a test session, or an
    // opt-out (GITBOLT_TELEMETRY, the Settings switch)
    // keeps this session from sending. The queue lives next to
    // GitBolt.ini, so whatever isolates the INI (XDG_CONFIG_HOME, a
    // test's QSettings::setPath) isolates the queue too.
    namespace telemetry = gitbolt::telemetry;
    const telemetry::Config telemetryConfig = telemetry::Config::fromEndpoints(
        QString::fromLatin1(endpoints::kHost),
        QString::fromLatin1(endpoints::kOpenPanelClientId),
        QString::fromLatin1(endpoints::kOpenPanelSecret),
        QString::fromLatin1(endpoints::kGlitchTipKey),
        QString::fromLatin1(endpoints::kGlitchTipProject),
        app.applicationVersion(), telemetryQueueDir(settings.fileName()));
    const telemetry::Gate telemetryGate = telemetry::evaluateGate({
        .configured = telemetryConfig.isComplete(),
        .platformName = QGuiApplication::platformName(),
        .arguments = app.arguments(),
        .settingOn = settings.usageStatisticsEnabled(),
    });
    // Opted out: whatever an earlier session queued goes, unsent. An
    // inert test session never touches the queue, not even this way.
    if (telemetry::discardsQueue(telemetryGate))
        telemetry::EventQueue::discard(telemetryConfig.queueDir);
    // Declared before `window`, so it outlives the raw pointer the
    // window keeps, and after `settings`, which its ID provider and
    // switch reader use. A disabled one for the Settings switch, so
    // turning it back on works without a restart. The reader re-reads
    // GitBolt.ini at every flush cycle, before each request, and at
    // quit: a switch turned off by another process reaches this one no
    // other way.
    std::unique_ptr<telemetry::TelemetryService> telemetryService;
    if (telemetry::createsService(telemetryGate)) {
        telemetryService = std::make_unique<telemetry::TelemetryService>(
            telemetryConfig,
            [&settings]() { return settings.ensureTelemetryInstallId(); },
            telemetryGate == telemetry::Gate::Active,
            [&settings]() { return settings.reloadUsageStatisticsEnabled(); });
        QObject::connect(&app, &QCoreApplication::aboutToQuit, telemetryService.get(),
                         &telemetry::TelemetryService::persistPending);
    }

    // Check for crash report from a previous session. Not in a test
    // session: ~/.gitbolt/crash.log is the user's (a bridge run outside
    // the e2e harness keeps the real HOME, and writes its own crashes
    // to crash-test.log), so it stays for their next launch. A test
    // that plants one in a HOME of its own opts in with
    // GITBOLT_TEST_CRASH_BOX=1 (tests/e2e/test_crash_box_handoff.py).
    // The report is read here, before the telemetry service starts
    // below; the box that tells the user comes up once the window and
    // the hand-off listener are (see there).
    const bool testSession = gitbolt::util::isTestSession(
        gitbolt::util::detectTestSession(QGuiApplication::platformName(),
                                         app.arguments()));
    const bool crashBoxOptIn =
        qEnvironmentVariable("GITBOLT_TEST_CRASH_BOX") == QLatin1String("1");
    const bool crashPending = (!testSession || crashBoxOptIn)
                              && gitbolt::util::CrashHandler::hasPendingCrashReport();
    QString crashReport;
    bool crashSummaryQueued = false;
    if (crashPending) {
        crashReport = gitbolt::util::CrashHandler::readCrashReport();
        // With usage statistics on, a summary goes into their queue
        // before the file is deleted below: signal, versions and stack
        // frames reduced to module offsets, never the raw report. The
        // parser refuses reports from builds and sessions that must
        // never send, and then the box says nothing about sending.
        crashSummaryQueued =
            telemetryService && telemetryService->queueCrashReport(crashReport);
    }

    // Create and show the main window. Inject the app-wide theme
    // service so the Settings dialog can list and switch themes
    // from one shared instance (rather than MainWindow creating a
    // second ThemeService that would fight over the palette).
    gitbolt::ui::MainWindow window;
    window.setThemeService(themeService);
    window.setTelemetry(telemetryService.get(), telemetryGate);

    // Restore window geometry from previous session. (Dock/toolbar
    // state via QMainWindow::saveState is no longer persisted — the
    // window has no dock widgets and the toolbar is non-movable, so
    // there's nothing left for saveState to encode.)
    QByteArray geometry = settings.restoreWindowGeometry();
    if (!geometry.isEmpty())
        window.restoreGeometry(geometry);

    window.show();

    // Only once the window is up: nothing touches the network on the
    // way there, and the first flush comes seconds after this record.
    if (telemetryService) {
        telemetryService->recordLaunch();
        telemetryService->start();
    }

    // The git GitBolt runs, and what to tell the user about it: a note
    // above the views (MainWindow::checkGitVersion), on a worker once
    // the window is up, since finding git waits for the login shell's
    // PATH.
    window.checkGitVersion();

    // Test bridge — agent/test control channel, never active unless
    // explicitly requested via the environment. Declared after
    // `window` so it is destroyed first. Created before the crash box
    // below, so a test can see and dismiss it. See
    // docs/AGENT_TESTING.md.
    std::unique_ptr<gitbolt::app::TestBridge> testBridge;
    if (qEnvironmentVariableIsSet("GITBOLT_TEST_BRIDGE"))
        testBridge = std::make_unique<gitbolt::app::TestBridge>(&window);

    // Second-instance handshake: a later `gitbolt <path>` (the shell
    // extensions, a plain CLI launch) hands its path over here instead
    // of opening a second window (see SingleInstance.h).
    // The window comes to the front for each, whichever app asked.
    const auto bringForwardAndOpen = [&window](const QString& path) {
#ifdef Q_OS_MACOS
        gitbolt::app::macos::activateApp();
#endif
        // Out of the Dock (or the taskbar) first, keeping it maximized
        // or full screen if it was: show() and raise() leave a
        // minimized window where it is, and the repository opened in
        // it unseen.
        if (window.windowState() & Qt::WindowMinimized)
            window.setWindowState((window.windowState() & ~Qt::WindowMinimized)
                                  | Qt::WindowActive);
        window.show();
        window.raise();
        window.activateWindow();
        if (!path.isEmpty())
            window.openRepositoryAtPath(path);
    };
    gitbolt::app::InstanceServer instanceServer;
    if (instanceLock.isLocked()) {
        if (instanceServer.listen(serverName)) {
            QObject::connect(&instanceServer,
                             &gitbolt::app::InstanceServer::pathReceived,
                             &window, bringForwardAndOpen);
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

    // And the folders macOS asks for, after it: one that came during
    // launch is the newer request.
    QObject::connect(&openRequests, &gitbolt::app::OpenRequests::openRequested,
                     &window, bringForwardAndOpen);
    openRequests.setReady();

    // The crash box, last: its exec() turns the event loop, so a launch
    // that hands over while it is up is answered and opened like any
    // other, after the queued open above, so the newer path wins. Shown
    // before the window and the listener, as it was, it left this
    // instance holding the lock and answering nobody: a second launch
    // waited its 10 s, said "not responding" and dropped its path.
    if (crashPending) {
        QMessageBox crashBox(&window);
        crashBox.setObjectName(QStringLiteral("app.crashDetected"));
        crashBox.setWindowTitle(QObject::tr("Crash Detected"));
        crashBox.setText(QObject::tr(
            "GitBolt crashed during the last session. "
            "A crash report has been saved."));
        // Present tense: the summary goes out first and at once, while
        // the box is still up (the service started above), so the
        // switch can only stop what comes after it.
        if (crashSummaryQueued) {
            crashBox.setInformativeText(QObject::tr(
                "A summary of the report, with no repository data, is being sent "
                "with the usage statistics. You can turn them off for the future in "
                "Tools › Settings › Usage Statistics."));
        }
        crashBox.setDetailedText(crashReport);
        crashBox.setStandardButtons(QMessageBox::Ok);
        crashBox.exec();
        gitbolt::util::CrashHandler::clearCrashReport();
        // The window was closed while the box was up (Cmd+Q, the
        // bridge's quit): a quit then ends at most the box's loop, and
        // the last window closing quits only from inside app.exec(),
        // which would run on with no window.
        if (!window.isVisible())
            QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
    }

    int exitCode = app.exec();

    // Save window geometry on exit. Splitter state is persisted
    // separately inside MainWindow::closeEvent.
    settings.saveWindowGeometry(window.saveGeometry());

    return exitCode;
}
