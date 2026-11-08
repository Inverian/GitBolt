#include "ui/UpdateController.h"

#include "conf/SettingsService.h"
#include "dialogs/UpdateDialog.h"
#include "update/Checker.h"
#include "update/Downloader.h"
#include "update/Installer.h"
#include "update/Package.h"
#include "widgets/UpdateNoticeBar.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QWidget>

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
    // this copy (a staged bundle, a part-downloaded AppImage, a mount).
    update::removeOldDownloads(setup_.config.downloadDir, setup_.config.current);
    update::removeStaleInstallFiles(setup_.package, setup_.config.downloadDir, setup_.config.current);
    if (setup_.schedulesChecks())
        timer_->start(kFirstCheckDelayMs);
}

void UpdateController::reportEarlierInstall()
{
    const QDir downloads(setup_.config.downloadDir);
    const QString path = downloads.filePath(QLatin1String(kInstallResultFile));
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink())
        return;
    std::optional<InstallResult> result;
    if (QFile file(path); file.open(QIODevice::ReadOnly))
        result = parseInstallResult(file.read(kMaxResultBytes + 1));
    // Said once: whatever it says, the next start doesn't say it again.
    if (!QFile::remove(path))
        qWarning("Update: couldn't remove %s", qPrintable(path));
    if (!result) {
        qWarning("Update: %s isn't an install result; ignored", qPrintable(path));
        return;
    }
    // setup_.config.current, not applicationVersion(): a test session's
    // relaunched copy inherits GITBOLT_TEST_APP_VERSION, and is that
    // version as far as the updater goes.
    const update::Version& current = setup_.config.current;
    const QString version = result->version.toString();
    if (result->status == QLatin1String("ok")) {
        // An older version's ok is from before an update made outside
        // GitBolt: stale.
        if (result->version >= current)
            emit statusMessage(tr("GitBolt was updated to %1.").arg(version));
        return;
    }
    // This version, or a newer one, runs now: whatever went wrong then
    // was put right since.
    if (!(result->version > current))
        return;

    const auto withReason = [&](const QString& what) {
        return result->reason.isEmpty() ? what + QLatin1Char('.')
                                        : what + QStringLiteral(": ") + sentence(result->reason);
    };
    QString why;
    if (result->status == QLatin1String("declined"))
        why = withReason(tr("Installing GitBolt %1 needs an administrator's permission, and it wasn't given")
                             .arg(version));
    else if (result->status == QLatin1String("gave-up"))
        why = withReason(tr("GitBolt %1 wasn't installed: GitBolt didn't quit within 10 minutes of Install "
                            "and Restart")
                             .arg(version));
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
        byHand = tr("To install it yourself, download %1 from the release page and run it as an "
                    "administrator.")
                     .arg(asset);
        break;
    case update::PackageKind::Deb:
        byHand = tr("To install it yourself, download %1 from the release page, and in its folder run: "
                    "sudo apt install ./%1")
                     .arg(asset);
        break;
    case update::PackageKind::AppImage:
        byHand = tr("To install it yourself, download %1 from the release page and use it in place of "
                    "this AppImage.")
                     .arg(asset);
        break;
    case update::PackageKind::Unknown:
        byHand = tr("To install it yourself, download it from the release page.");
        break;
    }

    QString message = why + QLatin1Char(' ') + tr("GitBolt wasn't changed.") + QStringLiteral("\n\n") + byHand;
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
    if (bar_ && !bar_->isHidden()) {
        const auto offered = update::Version::parse(bar_->version());
        if (!offered || *offered > latest)
            bar_->hide();
    }
}

void UpdateController::onAvailable(const update::Release& release, bool manual)
{
    settings_->setUpdateLastCheck(QDateTime::currentDateTimeUtc());
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
    QString dir;   // the Downloader's own: <cache>/updates/<version>/
    if (package.kind == update::PackageKind::AppImage) {
        // Next to the running AppImage, which the helper renames it
        // over: same folder, same file system.
        const QFileInfo target(package.target);
        dir = target.absolutePath();
        if (QDir(dir).filePath(assetName) == target.absoluteFilePath()) {
            retryChecks_ = false;
            openDialog()->showInstallFailed(
                tr("The new AppImage has this one's name, %1, so it can't be downloaded next to it. "
                   "Rename this AppImage, or download the new one from the release page.")
                    .arg(assetName));
            return;
        }
    }
    retryChecks_ = false;
    openDialog()->showDownloading(0, release.asset(assetName)->size);
    downloader_->start(release, assetName, dir);
}

void UpdateController::onDownloaded(const QString& path)
{
    downloaded_ = path;
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
    if (dialog_)
        dialog_->showPreparing(tr("Checking the download…"));
    // shownRelease_ is what install() started with: nothing replaces it
    // while installing (showRelease).
    installer_->prepare(path, shownRelease_->version);
}

void UpdateController::onPrepared()
{
    // The helper waits for this pid to exit before it touches anything:
    // the instance lock goes with this process, so the new copy can't
    // be folded into this one (SingleInstance.h). A false says why
    // through failed() (failInstall) already.
    const qint64 pid = hooks_.pid ? hooks_.pid() : QCoreApplication::applicationPid();
    if (!installer_->launchHelper(pid))
        return;
    launched_ = true;
    // Nothing is left to cancel: the helper installs once GitBolt has
    // quit, whatever this process does now.
    if (dialog_)
        dialog_->showRestarting();
    quitForHelper();
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

void UpdateController::failInstall(const QString& message)
{
    stopInstall();
    retryChecks_ = false;
    if (dialog_)
        dialog_->showInstallFailed(message);
    else
        qWarning("Update: %s", qPrintable(message));
}

void UpdateController::stopInstall()
{
    if (launched_)
        return;
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

bool UpdateController::installing() const
{
    return !launched_ && (downloader_->busy() || installer_ != nullptr);
}

dialogs::UpdateDialog* UpdateController::openDialog()
{
    if (!dialog_) {
        dialog_ = new dialogs::UpdateDialog(window_);
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
