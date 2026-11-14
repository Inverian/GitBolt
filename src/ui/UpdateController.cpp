#include "ui/UpdateController.h"

#include "conf/SettingsService.h"
#include "dialogs/UpdateDialog.h"
#include "update/Checker.h"
#include "update/Downloader.h"
#include "update/Installer.h"
#include "update/Package.h"
#include "widgets/UpdateNoticeBar.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include <cstdlib>
#include <utility>

namespace gitbolt::ui {

namespace {

// install-result is one short line; anything longer isn't one.
constexpr qint64 kMaxResultBytes = 4096;
// The reason is the helper's own plain English; shown as plain text,
// and cut short should it ever be long.
constexpr qsizetype kMaxReasonChars = 300;
// How much of install.log the dialog shows, from its end.
constexpr int kLogTailLines = 10;
constexpr qint64 kLogTailBytes = 16 * 1024;
// How much later than the marker's time its helper may seem to have
// started: the clocks' rounding, and a small step of the system's.
constexpr qint64 kHelperStartSlackSeconds = 10;

struct InstallResult {
    QString status;   ///< ok, declined, failed or gave-up
    update::Version version;
    QString reason;
};

// "<status> <version> <reason>\n", UTF-8. Lenient where a helper's
// shell may differ (a UTF-8 BOM, a CRLF, no newline, no reason), strict
// about the rest: one line, a known status, an x.y.z version.
std::optional<InstallResult> parseInstallResult(QByteArray bytes)
{
    if (bytes.size() > kMaxResultBytes)
        return std::nullopt;
    if (bytes.startsWith("\xEF\xBB\xBF"))
        bytes.remove(0, 3);
    const QString text = QString::fromUtf8(bytes).trimmed();
    if (text.isEmpty() || text.contains(QLatin1Char('\n')) || text.contains(QLatin1Char('\r')))
        return std::nullopt;
    const qsizetype afterStatus = text.indexOf(QLatin1Char(' '));
    if (afterStatus <= 0)
        return std::nullopt;
    InstallResult result;
    result.status = text.left(afterStatus);
    static const QStringList kStatuses{QStringLiteral("ok"), QStringLiteral("declined"),
                                       QStringLiteral("failed"), QStringLiteral("gave-up")};
    if (!kStatuses.contains(result.status))
        return std::nullopt;
    const QString rest = text.mid(afterStatus + 1);
    const qsizetype afterVersion = rest.indexOf(QLatin1Char(' '));
    const QString versionText = afterVersion < 0 ? rest : rest.left(afterVersion);
    // x.y.z exactly, as the helpers write it (parse() would take a tag).
    const auto version = update::Version::parse(versionText);
    if (!version || version->toString() != versionText)
        return std::nullopt;
    result.version = *version;
    QString reason = afterVersion < 0 ? QString() : rest.mid(afterVersion + 1);
    for (QChar& c : reason) {
        if (c.category() == QChar::Other_Control)
            c = QLatin1Char(' ');
    }
    reason = reason.simplified();
    if (reason.size() > kMaxReasonChars)
        reason = reason.left(kMaxReasonChars) + QStringLiteral("…");
    result.reason = reason;
    return result;
}

// x.y.z exactly, as the helpers and the marker write it.
bool isPlainVersion(const QString& text)
{
    const auto version = update::Version::parse(text);
    return version && version->toString() == text;
}

struct InstallMarker {
    QString from;   ///< the version that launched the helper
    QString to;     ///< the version it installs
    qint64 at = 0;  ///< when, in seconds since the epoch
    qint64 helperPid = 0;   ///< 0: unknown
};

// "<from> <to> <unix-seconds> <helper-pid>\n", strictly; without the pid
// as before 1.0.
std::optional<InstallMarker> parseInstallMarker(const QByteArray& bytes)
{
    if (bytes.size() > kMaxResultBytes)
        return std::nullopt;
    const QStringList parts = QString::fromUtf8(bytes).trimmed().split(QLatin1Char(' '));
    if (parts.size() != 3 && parts.size() != 4)
        return std::nullopt;
    bool ok = false;
    const qint64 at = parts.at(2).toLongLong(&ok);
    bool pidOk = true;
    const qint64 pid = parts.size() == 4 ? parts.at(3).toLongLong(&pidOk) : 0;
    if (!ok || !pidOk || pid < 0 || !isPlainVersion(parts.at(0)) || !isPlainVersion(parts.at(1)))
        return std::nullopt;
    return InstallMarker{parts.at(0), parts.at(1), at, pid};
}

// The marker in `downloads`, unless it is missing, a link or not one.
std::optional<InstallMarker> readInstallMarker(const QDir& downloads)
{
    const QString path = downloads.filePath(QLatin1String(UpdateController::kInstallMarkerFile));
    QFile file(path);
    if (QFileInfo(path).isSymLink() || !file.open(QIODevice::ReadOnly))
        return std::nullopt;
    return parseInstallMarker(file.read(kMaxResultBytes + 1));
}

// x.y.z's release page, built from a version Version::parse accepted:
// no text from GitHub goes into it.
QUrl releasePage(const QString& version)
{
    return QUrl(dialogs::UpdateDialog::releasesPageUrl().toString() + QStringLiteral("/tag/v") + version);
}

// update_result's package for this kind of install.
QString packageName(update::PackageKind kind)
{
    switch (kind) {
    case update::PackageKind::MacBundle: return QStringLiteral("dmg");
    case update::PackageKind::WindowsInstall: return QStringLiteral("nsis");
    case update::PackageKind::Deb: return QStringLiteral("deb");
    case update::PackageKind::Unknown: break;
    }
    return QStringLiteral("other");
}

// The last lines of the helper's log, or empty.
QString logTail(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    if (file.size() > kLogTailBytes)
        file.seek(file.size() - kLogTailBytes);
    QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    for (QString& line : lines) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
    }
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty())
        lines.removeLast();
    if (lines.size() > kLogTailLines)
        lines = lines.mid(lines.size() - kLogTailLines);
    return lines.join(QLatin1Char('\n'));
}

// `text` as a sentence: ends in a full stop unless it already ends one.
QString sentence(const QString& text)
{
    if (text.endsWith(QLatin1Char('.')) || text.endsWith(QLatin1Char('!')) || text.endsWith(QLatin1Char('?'))
        || text.endsWith(QStringLiteral("…")))
        return text;
    return text + QLatin1Char('.');
}

} // namespace

UpdateController::UpdateController(UpdateSetup setup, conf::SettingsService* settings,
                                   widgets::UpdateNoticeBar* bar, QWidget* window)
    : QObject(window)
    , setup_(std::move(setup))
    , settings_(settings)
    , bar_(bar)
    , window_(window)
    , checker_(new update::Checker(setup_.config, this))
    , downloader_(new update::Downloader(setup_.config, this))
    , timer_(new QTimer(this))
    , quitGrace_(new QTimer(this))
{
    // Found by the test bridge's fire-timer (window->findChild).
    timer_->setObjectName(QStringLiteral("updateCheckTimer"));
    // A test session's never runs (start() leaves it be), yet e2e tests
    // fire it to see what an automatic find does: the bridge fires a
    // stopped timer only when it says so.
    if (setup_.gate == UpdateGate::TestFeed)
        timer_->setProperty("fireWhileStopped", true);
    connect(timer_, &QTimer::timeout, this, &UpdateController::tick);

    // After the helper started: GitBolt is still here, so the quit was
    // refused (a dialog that wouldn't close, CloneDialog mid-clone). Not
    // on the way out, where ~MainWindow may wait for git work: the quit
    // went through then (aboutToQuit).
    quitGrace_->setSingleShot(true);
    connect(quitGrace_, &QTimer::timeout, this, [this] {
        if (!quitting_ && launched_)
            openDialog()->showQuitNeeded();
    });
    if (auto* app = QCoreApplication::instance())
        connect(app, &QCoreApplication::aboutToQuit, this, [this] { quitting_ = true; });

    connect(checker_, &update::Checker::upToDate, this, &UpdateController::onUpToDate);
    connect(checker_, &update::Checker::available, this, &UpdateController::onAvailable);
    connect(checker_, &update::Checker::failed, this, &UpdateController::onCheckFailed);

    connect(downloader_, &update::Downloader::progress, this, [this](qint64 received, qint64 total) {
        if (dialog_)
            dialog_->showDownloading(received, total);
    });
    connect(downloader_, &update::Downloader::finished, this, &UpdateController::onDownloaded);
    connect(downloader_, &update::Downloader::failed, this, &UpdateController::failInstall);

    if (bar_) {
        countClicks(bar_);
        connect(bar_, &widgets::UpdateNoticeBar::viewRequested, this, [this] {
            if (!release_) {
                checkNow();
                return;
            }
            openDialog();
            // showRelease also stops the dialog waiting for a check that
            // still runs: its answer mustn't replace what View shows, or
            // an install started from it.
            if (!installing() && !launched_)
                showRelease(*release_);
        });
        connect(bar_, &widgets::UpdateNoticeBar::dismissed, this, [this] {
            if (const auto version = update::Version::parse(bar_->version()))
                dismissed_ = version;
        });
        connect(bar_, &widgets::UpdateNoticeBar::whatsNewRequested, this, [](const QString& version) {
            if (isPlainVersion(version))
                QDesktopServices::openUrl(releasePage(version));
        });
    }
}

UpdateController::~UpdateController()
{
    // Quitting mid-install (Cmd+Q while it downloads): nothing of it
    // stays, and nothing installs later. The Downloader and Installer
    // clean up after themselves too, as they go with this object; the
    // download they handed over is this object's to delete.
    stopInstall();
}

void UpdateController::setRestartHooks(RestartHooks hooks)
{
    hooks_ = std::move(hooks);
}

void UpdateController::setLaunchHook(LaunchHook hook)
{
    launchHook_ = std::move(hook);
}

void UpdateController::setReopenPath(std::function<QString()> path)
{
    reopenPath_ = std::move(path);
}

bool UpdateController::updateInstalling(const QString& downloadDir, const std::function<qint64(qint64)>& startTime)
{
    if (downloadDir.isEmpty())
        return false;
    const QDir downloads(downloadDir);
    const auto marker = readInstallMarker(downloads);
    if (!marker || marker->helperPid <= 0 || marker->helperPid == QCoreApplication::applicationPid()
        || std::llabs(QDateTime::currentSecsSinceEpoch() - marker->at) > kResultPatienceSeconds)
        return false;
    // The helper starts before the launch writes the marker.
    const qint64 started = startTime ? startTime(marker->helperPid) : update::processStartTime(marker->helperPid);
    if (started <= 0 || started > marker->at + kHelperStartSlackSeconds)
        return false;
    const QFileInfo result(downloads.filePath(QLatin1String(kInstallResultFile)));
    return !result.exists() && !result.isSymLink();
}

dialogs::UpdateDialog* UpdateController::dialog() const
{
    return dialog_;
}

void UpdateController::start()
{
    // The result first: the cleanup below may clear the folder it is in.
    reportEarlierInstall();
    // The installers of this version and older ones are done with (a
    // finished update's, an abandoned one's), now that this one runs, and
    // so is whatever an update that crashed or was killed left beside
    // this copy (a staged bundle, a mount).
    update::removeOldDownloads(setup_.config.downloadDir, setup_.config.current);
    update::removeStaleInstallFiles(setup_.package, setup_.config.downloadDir);
    if (setup_.schedulesChecks())
        timer_->start(kFirstCheckDelayMs);
}

void UpdateController::checkInstallResult()
{
    if (!installing() && !launched_)
        reportEarlierInstall(true);
}

void UpdateController::reportEarlierInstall(bool running)
{
    const QDir downloads(setup_.config.downloadDir);
    // The marker the launch left: who launched it, for what, and when.
    const QString markerPath = downloads.filePath(QLatin1String(kInstallMarkerFile));
    const QFileInfo markerInfo(markerPath);
    const bool markerThere = markerInfo.exists() || markerInfo.isSymLink();
    std::optional<InstallMarker> marker;
    if (QFile file(markerPath); markerThere && !markerInfo.isSymLink() && file.open(QIODevice::ReadOnly))
        marker = parseInstallMarker(file.read(kMaxResultBytes + 1));
    const auto removeMarker = [&] {
        if (markerThere && !QFile::remove(markerPath))
            qWarning("Update: couldn't remove %s", qPrintable(markerPath));
    };

    const QString path = downloads.filePath(QLatin1String(kInstallResultFile));
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) {
        // No result yet: a helper may still be at work (a start by hand
        // while pkexec or the installer waits), until the patience is
        // over; then it never reported back. Either clock way.
        if (!markerThere)
            return;
        if (marker && std::llabs(QDateTime::currentSecsSinceEpoch() - marker->at) <= kResultPatienceSeconds)
            return;
        removeMarker();
        if (marker)
            reportResult("no_result", "install", update::Failure::NoResult, marker->from, marker->to);
        return;
    }
    std::optional<InstallResult> result;
    if (QFile file(path); file.open(QIODevice::ReadOnly))
        result = parseInstallResult(file.read(kMaxResultBytes + 1));
    // Said once: whatever it says, the next start doesn't say it again.
    if (!QFile::remove(path))
        qWarning("Update: couldn't remove %s", qPrintable(path));
    removeMarker();
    if (!result) {
        qWarning("Update: %s isn't an install result; ignored", qPrintable(path));
        if (marker)
            reportResult("no_result", "install", update::Failure::NoResult, marker->from, marker->to);
        return;
    }
    // setup_.config.current, not applicationVersion(): a test session's
    // relaunched copy inherits GITBOLT_TEST_APP_VERSION, and is that
    // version as far as the updater goes.
    const update::Version& current = setup_.config.current;
    const QString version = result->version.toString();
    // The marker's from, if it is this attempt's (0.9.x writes none).
    const QString from = marker && marker->to == version ? marker->from : QString();
    if (result->status == QLatin1String("ok")) {
        // An older version's ok is from before an update made outside
        // GitBolt: stale.
        if (result->version >= current) {
            reportResult("ok", "install", update::Failure::None, from, version);
            // Running all along, this process isn't the new version.
            const bool restart = running && result->version > current;
            if (restart)
                installedMeanwhile_ = result->version;
            if (bar_)
                bar_->showUpdated(version, restart);
        }
        return;
    }
    const update::Failure failure = update::installResultFailure(result->status, result->reason);
    // Once the installer ran, some of its files may be in place: GitBolt
    // was left half updated, not unchanged. So was a Mac whose old copy
    // couldn't be put back.
    const bool maybeChanged = failure == update::Failure::InstallerExit || failure == update::Failure::WrongVersion
                              || failure == update::Failure::UninstallerMissing
                              || failure == update::Failure::ElevatedError
                              || failure == update::Failure::ElevatedTimeout
                              || failure == update::Failure::ElevatedNoResult
                              || failure == update::Failure::RollbackFailed;
    // A newer version runs now, or this one after a failure that changed
    // nothing: whatever went wrong was put right since. Not so once the
    // installer ran: the new GitBolt.exe may run without the rest.
    if (!(result->version > current) && !(result->version == current && maybeChanged))
        return;
    if (result->status == QLatin1String("gave-up"))
        reportResult("gave_up", "quit", failure, from, version);
    else
        reportResult(result->status == QLatin1String("declined") ? "declined" : "failed", "install", failure, from,
                     version);

    const auto withReason = [&](const QString& what) {
        return result->reason.isEmpty() ? what + QLatin1Char('.')
                                        : what + QStringLiteral(": ") + sentence(result->reason);
    };
    QString why;
    if (result->status == QLatin1String("declined"))
        why = withReason(tr("Installing GitBolt %1 needs an administrator's permission, and it wasn't given")
                             .arg(version));
    else if (result->status == QLatin1String("gave-up"))
        // Not the helper's reason: it says the same, in seconds.
        why = tr("GitBolt %1 wasn't installed: GitBolt didn't quit within %2 minutes of Install and Restart.")
                  .arg(version)
                  .arg(update::Installer::kHelperWaitSeconds / 60);
    else
        why = withReason(tr("Installing GitBolt %1 failed after GitBolt quit").arg(version));

    const QString asset = update::assetNameFor(setup_.package.kind, result->version);
    QString byHand;
    switch (setup_.package.kind) {
    case update::PackageKind::MacBundle:
        byHand = tr("To install it yourself, download %1 from the release page, open it, and drag GitBolt "
                    "into your Applications folder.")
                     .arg(asset);
        break;
    case update::PackageKind::WindowsInstall:
        // The installer asks for permission itself.
        byHand = maybeChanged ? tr("To repair it, download %1 from the release page and open it; Windows asks "
                                   "for administrator permission.")
                                    .arg(asset)
                              : tr("To install it yourself, download %1 from the release page and open it; "
                                   "Windows asks for administrator permission.")
                                    .arg(asset);
        // The installer can't replace files another GitBolt has open, nor
        // those another program has (the reason names one).
        if (failure == update::Failure::InUse)
            byHand = tr("Close every other GitBolt window, in other users' sessions too, then click Try Again.");
        else if (failure == update::Failure::FileInUse)
            byHand = tr("Close the program that is using files in %1, then click Try Again. If you can't tell "
                        "which program it is, restart Windows, then choose Help › Check for Updates.")
                         .arg(QDir::toNativeSeparators(setup_.package.target));
        break;
    case update::PackageKind::Deb:
        byHand = tr("To install it yourself, download %1 from the release page, and in its folder run: "
                    "sudo apt install ./%1")
                     .arg(asset);
        break;
    case update::PackageKind::Unknown:
        byHand = tr("To install it yourself, download it from the release page.");
        break;
    }

    QString message = why + QLatin1Char(' ')
                      + (maybeChanged ? tr("GitBolt may be incomplete now.") : tr("GitBolt wasn't changed."))
                      + QStringLiteral("\n\n") + byHand;
    const QString logPath = downloads.filePath(QLatin1String(kInstallLogFile));
    const QString log = logTail(logPath);
    if (QFileInfo::exists(logPath))
        message += QStringLiteral("\n\n") + tr("The installer's log is %1.").arg(QDir::toNativeSeparators(logPath));
    // Try Again checks again, which offers the release (and Install)
    // once more.
    retryChecks_ = true;
    openDialog()->showEarlierInstallFailed(version, message, log);
}

void UpdateController::tick()
{
    // The first tick comes kFirstCheckDelayMs after start(); hourly
    // from then on.
    if (timer_->isActive() && timer_->interval() != kCheckIntervalMs)
        timer_->setInterval(kCheckIntervalMs);
    if (setup_.envOptOut || !settings_->updateAutoCheck() || installing() || launched_)
        return;
    if (!update::dueForAutoCheck(settings_->updateLastCheck(), QDateTime::currentDateTimeUtc()))
        return;
    checker_->check(false);
}

void UpdateController::checkNow()
{
    dialogs::UpdateDialog* dialog = openDialog();
    if (installing() || launched_)
        return;   // it shows the install; Cancel first
    dialog->showChecking();
    dialogWaits_ = true;
    // While an automatic check runs, this only makes it a manual one.
    checker_->check(true);
}

void UpdateController::onUpToDate(const update::Version& latest, bool manual)
{
    settings_->setUpdateLastCheck(QDateTime::currentDateTimeUtc());
    if (manual && dialogWaits_ && dialog_) {
        dialogWaits_ = false;
        // An install that started meanwhile keeps the dialog.
        if (!installing() && !launched_)
            dialog_->showUpToDate(latest, setup_.config.current);
    }
    // A release the bar offered, or would, is gone (GitHub took it down).
    if (pendingRelease_ && pendingRelease_->version > latest)
        pendingRelease_.reset();
    // (Not the bar's word that an update went in: it offers nothing.)
    if (bar_ && !bar_->isHidden() && !bar_->version().isEmpty()) {
        const auto offered = update::Version::parse(bar_->version());
        if (!offered || *offered > latest)
            bar_->hide();
    }
}

void UpdateController::onAvailable(const update::Release& release, bool manual)
{
    settings_->setUpdateLastCheck(QDateTime::currentDateTimeUtc());
    // Every answer says what the newest release is: one the bar was to
    // offer that is newer than this is gone (GitHub took it down), as
    // in onUpToDate, whatever the dialog shows now.
    if (pendingRelease_ && pendingRelease_->version > release.version)
        pendingRelease_.reset();
    // A release this system was too old for stays refused until a newer
    // one comes out, or the system changes (it may be new enough now).
    if (const auto refused = update::Version::parse(settings_->incompatibleUpdateVersion());
        refused
        && (release.version > *refused || settings_->incompatibleUpdateSystem() != update::runningSystem().toString()))
        settings_->setIncompatibleUpdate({}, {}, {});
    // An automatic check that ran on while an install started: the
    // install keeps the release it started with.
    if (installing() || launched_) {
        dialogWaits_ = false;
        return;
    }
    release_ = release;
    if (manual && dialogWaits_ && dialog_) {
        showRelease(release);
        if (bar_)
            bar_->hide();   // the dialog says it
        return;
    }
    // The dialog is up with something else (the user may be reading
    // what an earlier check found): what it shows stays what Install
    // and Skip act on, and the bar says this one once it closes.
    if (dialog_ && dialog_->isVisible()) {
        if (!shownRelease_ || release.version != shownRelease_->version) {
            pendingRelease_ = release;
            pendingManual_ = manual;
        }
        return;
    }
    // An automatic find, or a manual one whose dialog was closed while
    // it asked.
    offerInBar(release, manual);
}

void UpdateController::offerInBar(const update::Release& release, bool manual)
{
    // Already installed: the bar goes on saying to start GitBolt again.
    if (installedMeanwhile_ && release.version <= *installedMeanwhile_)
        return;
    // Skip This Version and the bar's × silence the automatic check
    // only.
    if (!manual) {
        const auto skipped = update::Version::parse(settings_->skippedUpdateVersion());
        if ((skipped && release.version <= *skipped) || (dismissed_ && release.version <= *dismissed_))
            return;
    }
    if (bar_)
        bar_->showVersion(release.version.toString());
}

void UpdateController::onCheckFailed(const QString& message, bool manual)
{
    // Only what GitHub answered: being offline, or a refusal of GitBolt's
    // own, isn't news.
    const update::Failure failure = checker_->failure();
    if (failure == update::Failure::HttpError || failure == update::Failure::RateLimited
        || failure == update::Failure::BadResponse)
        emit checkFailed(QString::fromLatin1(update::failureName(failure)));
    if (manual && dialogWaits_ && dialog_) {
        dialogWaits_ = false;
        // An install that started meanwhile keeps the dialog: Close on
        // a check's failure would cancel it.
        if (!installing() && !launched_) {
            retryChecks_ = true;
            dialog_->showCheckFailed(message);
            return;
        }
    }
    // Automatic: nobody asked, so nobody is interrupted. The next tick
    // tries again (no lastCheck was saved).
    qInfo("Update check: %s", qPrintable(message));
}

QString UpdateController::whyNotInstallable(const update::Release& release) const
{
    const update::PackageInfo& package = setup_.package;
    if (!package.canInstall) {
        // The deb's reason gives the command with its file name.
        QString why = package.whyNot;
        why.replace(QStringLiteral("<version>"), release.version.toString());
        return why.isEmpty() ? tr("This copy of GitBolt can't install updates. Download the new version "
                                  "from the release page.")
                             : why;
    }
    // Said again without downloading it again.
    if (const QString why = refusedForThisSystem(release.version); !why.isEmpty())
        return why;
    const QString assetName = update::assetNameFor(package.kind, release.version);
    if (!release.asset(assetName))
        return tr("This release has no installer for this system (%1). Download GitBolt from the "
                  "release page.")
            .arg(assetName);
    if (!release.asset(QString::fromLatin1(update::kChecksumsAsset)))
        return tr("This release lists no checksums (%1), so GitBolt can't check its installer. "
                  "Download it from the release page.")
            .arg(QLatin1String(update::kChecksumsAsset));
    return {};
}

void UpdateController::showRelease(const update::Release& release)
{
    if (!dialog_ || installing() || launched_)
        return;
    // Whatever a check that still runs finds, the dialog shows this
    // now: its answer goes to the bar, not over it.
    dialogWaits_ = false;
    shownRelease_ = release;
    if (pendingRelease_ && pendingRelease_->version <= release.version)
        pendingRelease_.reset();
    dialog_->showAvailable(release, setup_.config.current, whyNotInstallable(release), setup_.package.kind);
}

void UpdateController::install()
{
    if (!shownRelease_ || installing() || launched_ || !whyNotInstallable(*shownRelease_).isEmpty())
        return;
    const update::Release& release = *shownRelease_;
    const update::PackageInfo& package = setup_.package;
    const QString assetName = update::assetNameFor(package.kind, release.version);
    retryChecks_ = false;
    installStage_ = "download";
    // Nothing is downloaded that couldn't go in.
    if (const QString why = othersInTheWay(); !why.isEmpty()) {
        blockInstall("download", why);
        return;
    }
    openDialog()->showDownloading(0, release.asset(assetName)->size);
    downloader_->start(release, assetName);
}

void UpdateController::onDownloaded(const QString& path)
{
    downloaded_ = path;
    installStage_ = "prepare";
    // workDir is the downloads folder: its mount point, helper script,
    // install.log and install-result are never version-named, so
    // removeOldDownloads leaves them alone.
    installer_ = new update::Installer(setup_.package, setup_.config.current, setup_.config.downloadDir, this);
    connect(installer_, &update::Installer::status, this, [this](const QString& text) {
        if (dialog_)
            dialog_->showPreparing(text);
    });
    connect(installer_, &update::Installer::prepared, this, &UpdateController::onPrepared);
    connect(installer_, &update::Installer::failed, this, &UpdateController::failInstall);
    connect(installer_, &update::Installer::declined, this, &UpdateController::onDeclined);
    if (dialog_)
        dialog_->showPreparing(tr("Checking the download…"));
    // shownRelease_ is what install() started with: nothing replaces it
    // while installing (showRelease).
    installer_->prepare(path, shownRelease_->version);
}

void UpdateController::onPrepared()
{
    if (launching_)
        return;   // re-entered while Windows' prompt waits
    // The helper waits for this pid to exit before it touches anything:
    // the instance lock goes with this process, so the new copy can't
    // be folded into this one (SingleInstance.h). A false says why
    // through failed() (failInstall) or declined() (onDeclined) already.
    // UAC's prompt is owned by the window in front: a modal over the
    // dialog, else the dialog (a disabled owner may leave it on the taskbar).
    const qint64 pid = hooks_.pid ? hooks_.pid() : QCoreApplication::applicationPid();
    retryLaunch_ = false;
    installStage_ = "launch";
    // Opened since the download began: the update stays prepared, as
    // after Windows' No.
    if (const QString why = othersInTheWay(); !why.isEmpty()) {
        retryLaunch_ = true;
        blockInstall("launch", why);
        return;
    }
    installer_->setReopenPath(reopenPath());
    QWidget* owner = dialog_.data();
    if (QWidget* modal = QApplication::activeModalWidget(); modal && modal->isVisible())
        owner = modal;
    if (dialog_ && setup_.package.kind == update::PackageKind::WindowsInstall)
        dialog_->showAskingPermission();
    // Windows' prompt runs the event loop until it is answered: a Cancel,
    // close or quit meanwhile waits for the answer (stopInstall), and a
    // Yes wins over it, as the installer then already runs.
    launching_ = true;
    const bool started = launchHook_ ? launchHook_(*installer_, pid, owner)
                                     : installer_->launchHelper(pid, owner ? quintptr(owner->winId()) : 0);
    launching_ = false;
    if (std::exchange(stopAfterLaunch_, false) && !started)
        stopInstall();
    if (!started)
        return;
    launched_ = true;
    writeInstallMarker();
    helperTookOver();
    // Nothing is left to cancel: the helper installs once GitBolt has
    // quit, whatever this process does now.
    if (dialog_)
        dialog_->showRestarting();
    quitForHelper();
}

void UpdateController::onDeclined(const QString& howToInstall)
{
    // Nothing was started: GitBolt doesn't quit, and the checked
    // download stays for Try Again (closing the dialog deletes it).
    retryLaunch_ = true;
    reportResult("declined", "launch", update::Failure::PermissionDeclined, setup_.config.current.toString(),
                 shownRelease_ ? shownRelease_->version.toString() : QString());
    if (dialog_)
        dialog_->showDeclined(howToInstall);
}

void UpdateController::quitForHelper()
{
    // The usual quit: the window waits for running git work
    // (~MainWindow), and the helper waits for that.
    if (hooks_.quit)
        hooks_.quit();
    else
        QMetaObject::invokeMethod(qApp, &QCoreApplication::quit, Qt::QueuedConnection);
    quitGrace_->start(hooks_.quitGraceMs);
}

void UpdateController::helperTookOver()
{
    if (!helperDeadline_) {
        helperDeadline_ = new QTimer(this);
        helperDeadline_->setObjectName(QStringLiteral("updateHelperDeadline"));
        helperDeadline_->setSingleShot(true);
        connect(helperDeadline_, &QTimer::timeout, this, &UpdateController::helperGaveUp);
    }
    helperDeadline_->start(hooks_.helperDeadlineMs);
}

void UpdateController::helperGaveUp()
{
    if (quitting_ || !launched_)
        return;
    // Quitting now would install nothing and start nothing again, so
    // the dialog mustn't go on asking for it, and nothing is left to
    // wait for: checks and Install work again.
    quitGrace_->stop();
    launched_ = false;
    // The helper deleted the download as it gave up (and the staged
    // bundle beside the installed one, which the next start removes
    // too, should it still be there); what is still there goes.
    stopInstall();
    // Its result says what the dialog says now: the next start needn't
    // say it again.
    const QString resultPath = QDir(setup_.config.downloadDir).filePath(QLatin1String(kInstallResultFile));
    std::optional<InstallResult> result;
    if (QFile file(resultPath); file.open(QIODevice::ReadOnly))
        result = parseInstallResult(file.read(kMaxResultBytes + 1));
    if (result && result->status == QLatin1String("gave-up") && shownRelease_
        && result->version == shownRelease_->version)
        QFile::remove(resultPath);
    // Said now: the next start mustn't take it for an attempt that never
    // reported back.
    QFile::remove(QDir(setup_.config.downloadDir).filePath(QLatin1String(kInstallMarkerFile)));
    reportResult("gave_up", "quit", update::Failure::QuitTimeout, setup_.config.current.toString(),
                 shownRelease_ ? shownRelease_->version.toString() : QString());
    retryChecks_ = false;
    openDialog()->showInstallFailed(
        tr("GitBolt didn't quit within %1 minutes of Install and Restart, so the update was abandoned.")
            .arg(update::Installer::kHelperWaitSeconds / 60));
}

QString UpdateController::refusedForThisSystem(const update::Version& version) const
{
    const auto refused = update::Version::parse(settings_->incompatibleUpdateVersion());
    if (!refused || *refused != version || settings_->incompatibleUpdateSystem() != update::runningSystem().toString())
        return {};
    return settings_->incompatibleUpdateReason();
}

void UpdateController::failInstall(const QString& message)
{
    // The code first: stopInstall() deletes the Installer.
    update::Failure failure = update::Failure::Other;
    if (qstrcmp(installStage_, "download") == 0)
        failure = downloader_->failure();
    else if (installer_)
        failure = installer_->failure();
    if (failure == update::Failure::None)   // a launch hook's failed()
        failure = update::Failure::Other;
    const QString to = shownRelease_ ? shownRelease_->version.toString() : QString();
    // A release that needs a newer system than this one (the minimum
    // only the downloaded installer says, macOS's or Windows'):
    // remembered, and shown as one this copy can't install, with why
    // (whyNotInstallable), which it says again later without
    // downloading it again.
    if (installer_ && installer_->needsNewerSystem() && shownRelease_) {
        reportResult("incompatible", "prepare", update::Failure::NeedsNewerSystem, setup_.config.current.toString(),
                     to);
        settings_->setIncompatibleUpdate(shownRelease_->version.toString(), message,
                                         update::runningSystem().toString());
        stopInstall();
        retryChecks_ = false;
        showRelease(*shownRelease_);
        return;
    }
    reportResult("failed", installStage_, failure, setup_.config.current.toString(), to);
    stopInstall();
    retryChecks_ = false;
    if (dialog_)
        dialog_->showInstallFailed(message);
    else
        qWarning("Update: %s", qPrintable(message));
}

QString UpdateController::othersInTheWay() const
{
    if (setup_.package.kind != update::PackageKind::WindowsInstall)
        return {};
    const update::FolderUsers users =
        update::gitBoltsUsing(hooks_.programs ? hooks_.programs() : update::runningGitBolts(), setup_.package.target,
                              QCoreApplication::applicationPid());
    if (users.running > 0)
        return tr("Another GitBolt window is open, and the installer can't replace GitBolt's files while it runs. "
                  "Close the other GitBolt windows, then click Try Again.");
    if (users.unknown > 0)
        return tr("GitBolt is open in another user's session on this computer, and the installer can't replace "
                  "its files while it runs. Ask them to close it, or sign them out, then click Try Again.");
    return {};
}

void UpdateController::blockInstall(const char* stage, const QString& why)
{
    reportResult("failed", stage, update::Failure::InUse, setup_.config.current.toString(),
                 shownRelease_ ? shownRelease_->version.toString() : QString());
    openDialog()->showInstallFailed(why);
}

QString UpdateController::reopenPath() const
{
    const QString path = reopenPath_ ? reopenPath_() : QString();
    return !path.isEmpty() && QDir::isAbsolutePath(path) && QFileInfo(path).isDir() ? QDir::cleanPath(path)
                                                                                     : QString();
}

void UpdateController::stopInstall()
{
    if (launched_)
        return;
    // Not while Windows' prompt waits: the launch is still on the stack,
    // and a Yes runs the download. onPrepared stops it after a No.
    if (launching_) {
        stopAfterLaunch_ = true;
        return;
    }
    downloader_->cancel();
    if (installer_) {
        installer_->cancel();
        // Not delete: this may run inside one of its signals
        // (launchHelper's failed()).
        installer_->deleteLater();
        installer_ = nullptr;
    }
    if (!downloaded_.isEmpty()) {
        QFile::remove(downloaded_);
        downloaded_.clear();
    }
}

void UpdateController::reportResult(const char* status, const char* stage, update::Failure failure,
                                    const QString& from, const QString& to)
{
    telemetry::UpdateResult result;
    result.status = QString::fromLatin1(status);
    result.stage = QString::fromLatin1(stage);
    result.reason = QString::fromLatin1(update::failureName(failure));
    result.fromVersion = from.isEmpty() ? QStringLiteral("unknown") : from;
    result.toVersion = to.isEmpty() ? QStringLiteral("unknown") : to;
    result.package = packageName(setup_.package.kind);
    // Fixed codes and versions only: the e2e tests read this line.
    qInfo("Update: %s (%s, %s), %s to %s, %s", status, stage, update::failureName(failure),
          qPrintable(result.fromVersion), qPrintable(result.toVersion), qPrintable(result.package));
    emit updateResult(result);
}

void UpdateController::writeInstallMarker()
{
    if (!shownRelease_)
        return;
    const QString path = QDir(setup_.config.downloadDir).filePath(QLatin1String(kInstallMarkerFile));
    // The helper's pid tells a later launch that the update is going in.
    const QByteArray line = QStringLiteral("%1 %2 %3 %4\n")
                                .arg(setup_.config.current.toString(), shownRelease_->version.toString())
                                .arg(QDateTime::currentSecsSinceEpoch())
                                .arg(installer_ ? installer_->helperPid() : 0)
                                .toUtf8();
    // Without it the outcome still comes, from the result alone.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(line) != line.size() || !file.commit())
        qWarning("Update: couldn't write %s", qPrintable(path));
}

void UpdateController::countClicks(QWidget* widget)
{
    for (QAbstractButton* button : widget->findChildren<QAbstractButton*>()) {
        connect(button, &QAbstractButton::clicked, this, [this, button] { emit commandUsed(button->objectName()); });
    }
}

bool UpdateController::installing() const
{
    return !launched_ && (downloader_->busy() || installer_ != nullptr);
}

dialogs::UpdateDialog* UpdateController::openDialog()
{
    if (!dialog_) {
        dialog_ = new dialogs::UpdateDialog(window_);
        countClicks(dialog_);
        connect(dialog_, &dialogs::UpdateDialog::installClicked, this, &UpdateController::install);
        connect(dialog_, &dialogs::UpdateDialog::skipClicked, this, [this] {
            // The version on screen, not a newer one found since.
            if (!shownRelease_)
                return;
            settings_->setSkippedUpdateVersion(shownRelease_->version.toString());
            if (bar_)
                bar_->hide();
        });
        connect(dialog_, &dialogs::UpdateDialog::cancelClicked, this, [this] {
            stopInstall();
            if (shownRelease_)
                showRelease(*shownRelease_);
        });
        connect(dialog_, &dialogs::UpdateDialog::retryClicked, this, [this] {
            // After Windows' No, or another GitBolt in the way: launch
            // the update that is still prepared again.
            if (retryLaunch_ && installer_) {
                onPrepared();
                return;
            }
            if (retryChecks_ || !shownRelease_)
                checkNow();
            else
                install();
        });
        // GitBolt didn't quit after the helper started: ask again.
        connect(dialog_, &dialogs::UpdateDialog::quitClicked, this, [this] {
            if (launched_) {
                dialog_->showRestarting();
                quitForHelper();
            }
        });
        // Closed (Later, Close, ×, Escape, Cmd+W): an install stops; a
        // check runs on, and what it finds goes to the bar. So does a
        // find that came while the dialog was up.
        connect(dialog_, &QDialog::finished, this, [this] {
            dialogWaits_ = false;
            if (installing())
                stopInstall();
            if (pendingRelease_) {
                const update::Release pending = *pendingRelease_;
                pendingRelease_.reset();
                if (!launched_ && (!shownRelease_ || pending.version > shownRelease_->version))
                    offerInBar(pending, pendingManual_);
            }
        });
    }
    dialog_->show();
    dialog_->raise();
    dialog_->activateWindow();
    return dialog_;
}

} // namespace gitbolt::ui
