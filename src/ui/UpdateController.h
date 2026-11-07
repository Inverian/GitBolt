#pragma once

#include "ui/UpdateSetup.h"
#include "update/Release.h"
#include "update/Version.h"

#include <QObject>
#include <QPointer>
#include <QString>

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
/// Installing, after Install and Restart: the Downloader fetches the
/// package's asset (assetNameFor) and checks it, into <cache>/updates/
/// <version>/ (a test session: its temporary folder's gitbolt-updates;
/// an AppImage: next to the running one, where the helper renames it
/// over it); the Installer checks and stages it; its helper
/// is started with this process's pid, and GitBolt quits, which the
/// helper waits for. Cancel, closing the dialog, a failure, or quitting
/// first stops wherever it is and deletes what was downloaded: nothing
/// is left beside the AppImage, nothing installs later.
class UpdateController : public QObject {
    Q_OBJECT
public:
    static constexpr int kFirstCheckDelayMs = 15 * 1000;
    static constexpr int kCheckIntervalMs = 60 * 60 * 1000;

    /// `setup` is on(). `settings` and `bar` are the window's, and live
    /// as long as it; so does this (its parent is `window`).
    UpdateController(UpdateSetup setup, conf::SettingsService* settings, widgets::UpdateNoticeBar* bar,
                     QWidget* window);
    ~UpdateController() override;

    const UpdateSetup& setup() const { return setup_; }

    /// Once the window is up: deletes the installers of this version
    /// and older ones (update::removeOldDownloads), and schedules the
    /// daily check if the setup does.
    void start();
    /// Help › Check for Updates: shows the dialog and asks GitHub. While
    /// an install runs, shows the dialog as it is.
    void checkNow();

    /// The dialog, once it has been shown; null before.
    dialogs::UpdateDialog* dialog() const;

private:
    void tick();
    void onUpToDate(const update::Version& latest, bool manual);
    void onAvailable(const update::Release& release, bool manual);
    void onCheckFailed(const QString& message, bool manual);
    /// The dialog's Available or NotInstallable state for release_.
    void showRelease();
    /// Why this copy can't install release_, or empty.
    QString whyNotInstallable() const;
    void install();
    void onDownloaded(const QString& path);
    void onPrepared();
    void failInstall(const QString& message);
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
    QPointer<dialogs::UpdateDialog> dialog_;

    /// The newest release the feed offered this session.
    std::optional<update::Release> release_;
    /// The checked installer, from the Downloader's finished() until
    /// the helper takes it over (or it is deleted).
    QString downloaded_;
    /// The dialog shows Checking and waits for the running check.
    bool dialogWaits_ = false;
    /// What Try Again does: check again, or install again.
    bool retryChecks_ = false;
    /// The bar was dismissed for this version (and older) this session.
    std::optional<update::Version> dismissed_;
    /// The helper runs: GitBolt is quitting, and it owns the download.
    bool launched_ = false;
};

} // namespace gitbolt::ui
