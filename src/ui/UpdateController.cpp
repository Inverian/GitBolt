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
{
    // Found by the test bridge's fire-timer (window->findChild).
    timer_->setObjectName(QStringLiteral("updateCheckTimer"));
    // A test session's never runs (start() leaves it be), yet e2e tests
    // fire it to see what an automatic find does: the bridge fires a
    // stopped timer only when it says so.
    if (setup_.gate == UpdateGate::TestFeed)
        timer_->setProperty("fireWhileStopped", true);
    connect(timer_, &QTimer::timeout, this, &UpdateController::tick);

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
            if (!installing() && !launched_)
                showRelease();
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

dialogs::UpdateDialog* UpdateController::dialog() const
{
    return dialog_;
}

void UpdateController::start()
{
    // The installers of this version and older ones are done with (a
    // finished update's, an abandoned one's), now that this one runs.
    update::removeOldDownloads(setup_.config.downloadDir, setup_.config.current);
    if (setup_.schedulesChecks())
        timer_->start(kFirstCheckDelayMs);
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
        dialog_->showUpToDate(latest, setup_.config.current);
    }
    // A release the bar offered is gone (GitHub took it down).
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
    if (installing() || launched_)
        return;
    release_ = release;
    if (manual && dialogWaits_ && dialog_) {
        dialogWaits_ = false;
        showRelease();
        if (bar_)
            bar_->hide();   // the dialog says it
        return;
    }
    // The dialog is up with something else (the user may be reading
    // what an earlier check found): the bar waits for the next check.
    if (dialog_ && dialog_->isVisible())
        return;
    // An automatic find, or a manual one whose dialog was closed while
    // it asked. Skip This Version and the bar's × silence the automatic
    // check only.
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
        retryChecks_ = true;
        dialog_->showCheckFailed(message);
        return;
    }
    // Automatic: nobody asked, so nobody is interrupted. The next tick
    // tries again (no lastCheck was saved).
    qInfo("Update check: %s", qPrintable(message));
}

QString UpdateController::whyNotInstallable() const
{
    const update::PackageInfo& package = setup_.package;
    if (!package.canInstall) {
        // The deb's reason gives the command with its file name.
        QString why = package.whyNot;
        why.replace(QStringLiteral("<version>"), release_->version.toString());
        return why.isEmpty() ? tr("This copy of GitBolt can't install updates. Download the new version "
                                  "from the release page.")
                             : why;
    }
    const QString assetName = update::assetNameFor(package.kind, release_->version);
    if (!release_->asset(assetName))
        return tr("This release has no installer for this system (%1). Download GitBolt from the "
                  "release page.")
            .arg(assetName);
    if (!release_->asset(QString::fromLatin1(update::kChecksumsAsset)))
        return tr("This release lists no checksums (%1), so GitBolt can't check its installer. "
                  "Download it from the release page.")
            .arg(QLatin1String(update::kChecksumsAsset));
    return {};
}

void UpdateController::showRelease()
{
    if (!release_ || !dialog_)
        return;
    dialog_->showAvailable(*release_, setup_.config.current, whyNotInstallable());
}

void UpdateController::install()
{
    if (!release_ || installing() || launched_ || !whyNotInstallable().isEmpty())
        return;
    const update::PackageInfo& package = setup_.package;
    const QString assetName = update::assetNameFor(package.kind, release_->version);
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
    openDialog()->showDownloading(0, release_->asset(assetName)->size);
    downloader_->start(*release_, assetName, dir);
}

void UpdateController::onDownloaded(const QString& path)
{
    downloaded_ = path;
    // workDir is the downloads folder: its mount point, helper script
    // and install.log are never version-named, so removeOldDownloads
    // leaves them alone.
    installer_ = new update::Installer(setup_.package, setup_.config.current, setup_.config.downloadDir, this);
    connect(installer_, &update::Installer::status, this, [this](const QString& text) {
        if (dialog_)
            dialog_->showPreparing(text);
    });
    connect(installer_, &update::Installer::prepared, this, &UpdateController::onPrepared);
    connect(installer_, &update::Installer::failed, this, &UpdateController::failInstall);
    if (dialog_)
        dialog_->showPreparing(tr("Checking the download…"));
    installer_->prepare(path, release_->version);
}

void UpdateController::onPrepared()
{
    if (dialog_)
        dialog_->showPreparing(tr("Restarting GitBolt…"));
    // The helper waits for this pid to exit before it touches anything:
    // the instance lock goes with this process, so the new copy can't
    // be folded into this one (SingleInstance.h). A false says why
    // through failed() (failInstall) already.
    if (!installer_->launchHelper(QCoreApplication::applicationPid()))
        return;
    launched_ = true;
    // The usual quit: the window waits for running git work
    // (~MainWindow), and the helper waits for that.
    QMetaObject::invokeMethod(qApp, &QCoreApplication::quit, Qt::QueuedConnection);
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
            if (!release_)
                return;
            settings_->setSkippedUpdateVersion(release_->version.toString());
            if (bar_)
                bar_->hide();
        });
        connect(dialog_, &dialogs::UpdateDialog::cancelClicked, this, [this] {
            stopInstall();
            showRelease();
        });
        connect(dialog_, &dialogs::UpdateDialog::retryClicked, this, [this] {
            if (retryChecks_ || !release_)
                checkNow();
            else
                install();
        });
        // Closed (Later, Close, ×, Escape, Cmd+W): an install stops; a
        // check runs on, and what it finds goes to the bar.
        connect(dialog_, &QDialog::finished, this, [this] {
            dialogWaits_ = false;
            if (installing())
                stopInstall();
        });
    }
    dialog_->show();
    dialog_->raise();
    dialog_->activateWindow();
    return dialog_;
}

} // namespace gitbolt::ui
