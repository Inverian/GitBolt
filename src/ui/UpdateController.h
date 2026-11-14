#pragma once

#include "telemetry/EventSchema.h"
#include "ui/UpdateSetup.h"
#include "update/Failure.h"
#include "update/Installer.h"
#include "update/Processes.h"
#include "update/Release.h"
#include "update/Version.h"

#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <optional>

class QTimer;
class QWidget;

namespace gitbolt::conf { class SettingsService; }
namespace gitbolt::dialogs { class UpdateDialog; }
namespace gitbolt::update { class Checker; class Downloader; class Installer; }
namespace gitbolt::widgets { class UpdateNoticeBar; }

namespace gitbolt::ui {

/// In-app updates, the window's side of them: when to ask, what to
/// tell the user, and the install they ask for. MainWindow creates one
/// when the setup is on() (UpdateSetup.h) and hands Help › Check for
/// Updates to checkNow().
///
/// When it asks:
///   - checkNow(): always, and the dialog shows what it finds.
///   - The daily check: a QTimer named "updateCheckTimer", started by
///     start() only when the setup schedules checks (an official build,
///     not a test session, not GITBOLT_UPDATE_CHECK=0). It first fires
///     15 s after the window is up, then hourly; each tick asks only if
///     Settings' switch is on and the last answer is a day old
///     (update::dueForAutoCheck). A test session's timer exists but
///     never runs: e2e tests fire it through the bridge.
///   Every answer GitHub gives (newer or not) is saved as
///   updates/lastCheck; a failure isn't.
///
/// What it tells:
///   - A manual check: the dialog, whatever the answer, failures too.
///   - An automatic find: the notice bar, unless the user skipped that
///     version (updates/skippedVersion, until a newer one comes out) or
///     dismissed the bar for it this session. Failures go to the log
///     only.
///
/// What it acts on: the release the dialog shows. A find that comes
/// while the dialog shows something else (the user reading an earlier
/// one) leaves Install and Skip alone, and goes to the bar once the
/// dialog closes.
///
/// Installing, after Install and Restart: the Downloader fetches the
/// package's asset (assetNameFor) and checks it, into <cache>/updates/
/// <version>/ (a test session: its private gitbolt-updates-XXXXXX in
/// the temporary folder, UpdateSetup.h); the Installer checks and stages
/// it; its helper is started with this process's pid, and GitBolt
/// quits, which the helper waits for, and starts GitBolt again with the
/// repository the window showed. (Windows asks UAC first, over the
/// dialog: a No leaves GitBolt running, the dialog Declined, and the
/// update ready for Try Again. While the prompt waits, Cancel is greyed
/// out, and closing the dialog or quitting waits for the answer: a Yes
/// installs, a No then stops. Before the download and again before UAC,
/// another GitBolt running from the install folder, whose files the
/// installer couldn't replace, stops it: the dialog says to close it and
/// Try Again.) Cancel, closing the dialog, a failure,
/// or quitting first stops wherever it is and deletes what was
/// downloaded: nothing installs later. Once the helper runs there is nothing left
/// to cancel: the dialog says GitBolt restarts, and if GitBolt is still
/// running kQuitGraceMs later (the quit was refused), that it has to
/// quit for the update to go in; still running kHelperDeadlineMs after
/// the helper started, when the helper has given up, that the update
/// was abandoned, and checks and Install work again.
///
/// A release this system is too old for (Installer::needsNewerSystem:
/// the downloaded installer's minimum, macOS's or Windows') is
/// remembered with why (updates/incompatibleVersion): shown again, it
/// is one this copy can't install, without being downloaded again,
/// until a newer one comes out or the system's version changes.
///
/// After an install: every helper leaves <downloads>/install-result,
/// one line "<status> <version> <reason>" (status ok, declined, failed
/// or gave-up), before it relaunches GitBolt or gives up. start() reads
/// it once and deletes it, as checkInstallResult() does later: ok says
/// so in the notice bar, with What's New, until dismissed; anything
/// else, for a version newer than this one (or this one, in the copy the
/// helper started, once the installer ran), opens the dialog on why, the
/// end of install.log, and how to install by hand or repair GitBolt.
///
/// Usage statistics, fixed codes only: updateResult once per attempt (at
/// once, or at the next start() for one the helper finishes), checkFailed
/// for a failed check (never for being offline), commandUsed for the
/// dialog's and the bar's buttons.
class UpdateController : public QObject {
    Q_OBJECT
public:
    static constexpr int kFirstCheckDelayMs = 15 * 1000;
    static constexpr int kCheckIntervalMs = 60 * 60 * 1000;
    /// How long after asking to quit, with the helper running, GitBolt
    /// counts the quit as refused.
    static constexpr int kQuitGraceMs = 5 * 1000;
    /// How long after starting the helper GitBolt, still running, takes
    /// it to have given up: its wait for GitBolt to quit
    /// (Installer::kHelperWaitSeconds), and a minute for it to start.
    static constexpr int kHelperDeadlineMs = (update::Installer::kHelperWaitSeconds + 60) * 1000;
    /// The helper's outcome and log, in the downloads folder (the
    /// Installer's workDir, never version-named, so removeOldDownloads
    /// keeps them).
    static constexpr char kInstallResultFile[] = "install-result";
    static constexpr char kInstallLogFile[] = "install.log";
    /// Also there, left by the launch: "<from> <to> <unix time> <helper
    /// pid>" (the pid 0 if unknown, and missing before 1.0). With no
    /// result once kResultPatienceSeconds are over, the attempt is no_result.
    static constexpr char kInstallMarkerFile[] = "pending-install";
    /// How long a marker without a result may be a helper still at work
    /// (its wait for the quit, then Windows' install), with a margin.
    static constexpr int kResultPatienceSeconds = update::Installer::kHelperWaitSeconds + 30 * 60;

    /// How this process steps aside for the helper. Tests replace them,
    /// to get as far as the helper without quitting themselves or having
    /// it wait for them.
    struct RestartHooks {
        /// The pid the helper waits for; null: this process's.
        std::function<qint64()> pid;
        /// Asks GitBolt to quit; null: a queued QCoreApplication::quit().
        std::function<void()> quit;
        int quitGraceMs = kQuitGraceMs;
        int helperDeadlineMs = kHelperDeadlineMs;
        /// The GitBolts running now; null: update::runningGitBolts().
        std::function<QList<update::RunningProgram>()> programs = {};
    };

    /// `setup` is on(). `settings` and `bar` are the window's, and live
    /// as long as it; so does this (its parent is `window`).
    UpdateController(UpdateSetup setup, conf::SettingsService* settings, widgets::UpdateNoticeBar* bar,
                     QWidget* window);
    ~UpdateController() override;

    const UpdateSetup& setup() const { return setup_; }

    void setRestartHooks(RestartHooks hooks);
    /// Starts the helper for `pid` in installer.launchHelper's place
    /// (null, the default: that, with `owner`'s window for Windows' UAC
    /// prompt). Tests stand in for that prompt with it: they emit the
    /// Installer's declined() or failed() and return false, as a No does.
    using LaunchHook = std::function<bool(update::Installer& installer, qint64 pid, QWidget* owner)>;
    void setLaunchHook(LaunchHook hook);
    /// The repository the window shows, which the GitBolt the update
    /// starts opens; empty: none.
    void setReopenPath(std::function<QString()> path);

    /// main(), before this launch runs as the GitBolt: an update is going
    /// in now. The marker in `downloadDir` is younger than the patience,
    /// names a helper that runs and isn't this process (a helper that
    /// execs GitBolt becomes it), and no result is there yet.
    /// `startTime` (null: update::processStartTime) must put the helper's
    /// start before the marker's: not a process that took its pid since.
    static bool updateInstalling(const QString& downloadDir,
                                 const std::function<qint64(qint64)>& startTime = {});

    /// Once the window is up: says how the last install went (the
    /// helper's install-result), deletes the installers of this version
    /// and older ones (update::removeOldDownloads), and schedules the
    /// daily check if the setup does.
    void start();
    /// Help › Check for Updates: shows the dialog and asks GitHub. While
    /// an install runs, shows the dialog as it is.
    void checkNow();
    /// A launch was handed over to this GitBolt: says how an update that
    /// went in while it ran ended, as start() would have. Not while this
    /// copy's own install is under way.
    void checkInstallResult();

    /// The dialog, once it has been shown; null before.
    dialogs::UpdateDialog* dialog() const;

signals:
    /// How an install attempt ended (fixed codes only).
    void updateResult(const gitbolt::telemetry::UpdateResult& result);
    /// A check GitHub answered with an error, a rate limit or something
    /// that isn't a release: its failure code.
    void checkFailed(const QString& reason);
    /// One of the dialog's or the notice bar's buttons was clicked.
    void commandUsed(const QString& objectName);

private:
    void tick();
    void onUpToDate(const update::Version& latest, bool manual);
    void onAvailable(const update::Release& release, bool manual);
    void onCheckFailed(const QString& message, bool manual);
    /// The notice bar for `release`, unless it went in meanwhile, or
    /// (automatic) it was skipped or its bar dismissed.
    void offerInBar(const update::Release& release, bool manual);
    /// The dialog's Available or NotInstallable state for `release`,
    /// which Install and Skip then act on. Not while installing.
    void showRelease(const update::Release& release);
    /// Why this copy can't install `release`, or empty.
    QString whyNotInstallable(const update::Release& release) const;
    void install();
    void onDownloaded(const QString& path);
    void onPrepared();
    /// Windows' UAC said no: GitBolt stays, the update stays prepared
    /// for Try Again, and the dialog says how to install it by hand.
    void onDeclined(const QString& howToInstall);
    /// Asks GitBolt to quit, and says so if it is still here later.
    void quitForHelper();
    /// The helper was started: the download is its, and GitBolt watches
    /// for the time it gives up.
    void helperTookOver();
    /// GitBolt is still running when the helper has given up (the quit
    /// was refused, and the user went on working): the install is over,
    /// and failed; checks and installs work again.
    void helperGaveUp();
    /// Why `version` was refused on this system before (needs a newer
    /// system, Installer::needsNewerSystem), or empty.
    QString refusedForThisSystem(const update::Version& version) const;
    void failInstall(const QString& message);
    /// Windows: why another GitBolt running from the install folder keeps
    /// the installer from replacing its files, or empty.
    QString othersInTheWay() const;
    /// An install othersInTheWay() stopped at `stage`: reported, and the
    /// dialog says `why`.
    void blockInstall(const char* stage, const QString& why);
    /// The window's repository, if it is an absolute path to a folder.
    QString reopenPath() const;
    /// start(), checkInstallResult(): reads and deletes the helper's
    /// install-result, and the pending-install marker with it. `running`:
    /// this process ran while the update went in.
    void reportEarlierInstall(bool running = false);
    /// Logs and emits updateResult; `from` and `to` as x.y.z or empty.
    void reportResult(const char* status, const char* stage, update::Failure failure, const QString& from,
                      const QString& to);
    /// The marker for the next start, once the helper runs.
    void writeInstallMarker();
    /// Every button under `widget` emits commandUsed with its name.
    void countClicks(QWidget* widget);
    /// Stops the download or the preparation, and deletes what was
    /// downloaded. Never after the helper was launched.
    void stopInstall();
    bool installing() const;
    dialogs::UpdateDialog* openDialog();

    UpdateSetup setup_;
    conf::SettingsService* settings_ = nullptr;
    QPointer<widgets::UpdateNoticeBar> bar_;
    QWidget* window_ = nullptr;
    update::Checker* checker_ = nullptr;
    update::Downloader* downloader_ = nullptr;
    update::Installer* installer_ = nullptr;   ///< while preparing; one per install
    QTimer* timer_ = nullptr;
    QTimer* quitGrace_ = nullptr;   ///< after the quit request: was it refused?
    QTimer* helperDeadline_ = nullptr;   ///< after the helper started: has it given up? (made then)
    QPointer<dialogs::UpdateDialog> dialog_;
    RestartHooks hooks_;
    LaunchHook launchHook_;
    std::function<QString()> reopenPath_;

    /// The newest release the feed offered this session (the bar's).
    std::optional<update::Release> release_;
    /// The release the dialog last showed: what Install, Skip and Try
    /// Again act on.
    std::optional<update::Release> shownRelease_;
    /// A newer find that came while the dialog was up with something
    /// else, for the bar once it closes; and whether a manual check
    /// found it.
    std::optional<update::Release> pendingRelease_;
    bool pendingManual_ = false;
    /// The checked installer, from the Downloader's finished() until
    /// the helper takes it over (or it is deleted).
    QString downloaded_;
    /// The dialog shows Checking and waits for the running check.
    bool dialogWaits_ = false;
    /// What Try Again does: check again, or install again.
    bool retryChecks_ = false;
    /// Try Again launches the prepared update again: Windows said no, or
    /// another GitBolt was in the way.
    bool retryLaunch_ = false;
    /// The launch waits for Windows' prompt, the event loop running; and
    /// a stopInstall() meanwhile, held until it is answered.
    bool launching_ = false;
    bool stopAfterLaunch_ = false;
    /// The bar was dismissed for this version (and older) this session.
    std::optional<update::Version> dismissed_;
    /// Installed while this older process ran (the bar says to start
    /// GitBolt again): the bar offers neither it nor an older one.
    std::optional<update::Version> installedMeanwhile_;
    /// The helper runs: GitBolt is quitting, and it owns the download.
    bool launched_ = false;
    /// The quit went through (aboutToQuit): GitBolt is on its way out.
    bool quitting_ = false;
    /// Where the running install is: download, prepare or launch.
    const char* installStage_ = "download";
};

} // namespace gitbolt::ui
