//
// TestUpdateController — in-app updates on the window's side
// (ui/UpdateController.h), against the loopback stub
// (../update/HttpServer.h), never GitHub:
//
//   - a manual check shows the dialog with whatever it finds: newer
//     (installable or not, and why), the same or older, a failure,
//     which Try Again asks again; every answer is saved as lastCheck,
//     a failure isn't;
//   - the automatic check (its timer fired, as the e2e tests do) shows
//     the notice bar, asks again only once a day, and says nothing about
//     a skipped version, a version whose bar was dismissed this session,
//     or anything with the switch off or GITBOLT_UPDATE_CHECK=0;
//   - only an official build's setup schedules the timer; a test
//     session's exists, stopped, and may be fired; start() clears old
//     downloads;
//   - Install and Restart downloads into the version's folder of the
//     downloads; Cancel, closing the dialog, a failed download and a
//     download abandoned after it finished all leave nothing there, and
//     never start the helper; on macOS, a disk image that won't attach
//     is deleted too, and one for a newer macOS is remembered: shown as
//     not installable with why, never downloaded again, until a newer
//     release or another system version;
//   - what Install and Skip act on is the release the dialog shows: a
//     newer one found meanwhile waits for the bar (unless a later
//     answer says GitHub took it down), and a manual check whose dialog
//     was taken over by the bar's View doesn't replace the install
//     screen with its late answer;
//   - once the helper runs (a real one, for a stand-in deb with
//     stand-in pkexec and launcher, and a child process for it to wait
//     for; the quit is caught, never done), the dialog has nothing to
//     cancel, says so when GitBolt
//     doesn't quit, and the download is the helper's; once the helper
//     has given up (a short deadline here), the install is over and
//     failed, and checks and Install work again;
//   - after an install, start() reads the helper's install-result
//     once: ok goes to the status bar; declined, failed or gave-up for
//     a newer version open the dialog on why, the end of install.log
//     and how to install by hand for this kind of package; a stale or
//     unreadable one is dropped;
//   - Windows' UAC refusing (a No, or an error), which comes back while
//     GitBolt runs (the launch hook stands in for the prompt): GitBolt
//     doesn't quit, the dialog says how to install it by hand, and
//     after a No, Try Again asks again for the same checked download;
//   - every attempt's outcome, for usage statistics (updateResult): at
//     start() from install-result and the pending-install marker the
//     launch leaves (a marker alone, past its patience, is no_result),
//     or at once for a download, preparation or launch that fails, a
//     No, and a helper that gave up; with fixed codes, each of which
//     telemetry::updateReason lets through; a check's failure code
//     (never being offline), and every click on the dialog's and the
//     bar's buttons, by name;
//   - MainWindow: without the in-app check (a source build, or a test
//     session with no test feed) both Check for Updates items are
//     greyed out; with it, they ask the feed; on macOS the application
//     menu's About GitBolt and Check for Updates… have their roles as
//     they join their placeholder menu, which then doesn't show in the
//     menu bar.
//
// The deb is the package most tests install: it needs no platform's
// tools. On Unix, PATH starts with stand-ins for apt-get and pkexec, so
// nothing asks for root or changes a package.
//

#include <QAbstractButton>
#include <QAction>
#include <QActionEvent>
#include <QApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVBoxLayout>
#include <QVersionNumber>

#include "../TestProcessHelper.h"
#include "../update/FakeApps.h"
#include "AppMenuWatcher.h"
#include "../update/HttpServer.h"
#include "conf/SettingsService.h"
#include "dialogs/UpdateDialog.h"
#include "telemetry/EventSchema.h"
#include "ui/MainWindow.h"
#include "ui/UpdateController.h"
#include "ui/UpdateSetup.h"
#include "update/Checker.h"
#include "update/Downloader.h"
#include "update/Installer.h"
#include "update/Package.h"
#include "widgets/UpdateNoticeBar.h"

using gitbolt::conf::SettingsService;
using gitbolt::dialogs::UpdateDialog;
using gitbolt::test::AppMenuWatcher;
using gitbolt::test::HttpServer;
using gitbolt::test::Served;
using gitbolt::ui::UpdateController;
using gitbolt::ui::UpdateGate;
using gitbolt::ui::UpdateSetup;
using gitbolt::update::PackageInfo;
using gitbolt::update::PackageKind;
using gitbolt::widgets::UpdateNoticeBar;
using State = UpdateDialog::State;

namespace {

const QByteArray kFeedPath = "/repos/Inverian/GitBolt/releases/latest";
const QString kDebAsset = QStringLiteral("gitbolt_0.1.1_amd64.deb");

class UrlCatcher : public QObject {
    Q_OBJECT
public:
    UrlCatcher() {
        QDesktopServices::setUrlHandler(QStringLiteral("http"), this, "open");
        QDesktopServices::setUrlHandler(QStringLiteral("https"), this, "open");
    }
    ~UrlCatcher() override {
        QDesktopServices::unsetUrlHandler(QStringLiteral("http"));
        QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
    }
    QList<QUrl> urls;
public slots:
    void open(const QUrl& url) { urls << url; }
};

// A release as GitHub's releases/latest has it, its assets on `server`,
// with SHA256SUMS.txt over them unless `checksums` is false. `sums`
// overrides what that file says.
QByteArray releaseJson(const HttpServer& server, const QString& version,
                       const QList<QPair<QString, QByteArray>>& assets, bool checksums = true,
                       const QByteArray& sums = {}) {
    QJsonArray list;
    QByteArray sumsFile;
    const auto add = [&](const QString& name, const QByteArray& body) {
        list.append(QJsonObject{
            {QStringLiteral("name"), name},
            {QStringLiteral("browser_download_url"),
             server.url(QStringLiteral("/download/v%1/%2").arg(version, name)).toString()},
            {QStringLiteral("size"), body.size()},
        });
    };
    for (const auto& [name, body] : assets) {
        add(name, body);
        sumsFile += QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex() + "  " + name.toUtf8() + '\n';
    }
    if (checksums)
        add(QStringLiteral("SHA256SUMS.txt"), sums.isEmpty() ? sumsFile : sums);
    return QJsonDocument(QJsonObject{
                             {QStringLiteral("tag_name"), QStringLiteral("v") + version},
                             {QStringLiteral("name"), QStringLiteral("GitBolt ") + version},
                             {QStringLiteral("draft"), false},
                             {QStringLiteral("prerelease"), false},
                             {QStringLiteral("html_url"),
                              QStringLiteral("https://github.com/Inverian/GitBolt/releases/tag/v") + version},
                             {QStringLiteral("body"), QStringLiteral("## [%1]\n\n- Fixed.").arg(version)},
                             {QStringLiteral("assets"), list},
                         })
        .toJson();
}

// Serves `version`'s release on the feed, and its files.
void serve(HttpServer& server, const QString& version, const QList<QPair<QString, QByteArray>>& assets,
           bool checksums = true, const QByteArray& sums = {}) {
    server.routes.insert(kFeedPath, Served::json(releaseJson(server, version, assets, checksums, sums)));
    QByteArray sumsFile;
    for (const auto& [name, body] : assets) {
        server.routes.insert(QStringLiteral("/download/v%1/%2").arg(version, name).toUtf8(), Served::ok(body));
        sumsFile += QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex() + "  " + name.toUtf8() + '\n';
    }
    server.routes.insert(QStringLiteral("/download/v%1/SHA256SUMS.txt").arg(version).toUtf8(),
                         Served::ok(sums.isEmpty() ? sumsFile : sums));
}

// main()'s setup for a test session pointed at `server`, running 0.1.0.
UpdateSetup testSetup(const HttpServer& server, const QString& cacheDir, PackageInfo package = {}) {
    gitbolt::ui::UpdateSetupInputs inputs;
    inputs.testSession = gitbolt::util::TestSessionKind::Bridge;
    inputs.env.insert(QStringLiteral("GITBOLT_TEST_UPDATE_FEED"), server.url(QString::fromLatin1(kFeedPath)).toString());
    inputs.env.insert(QStringLiteral("GITBOLT_TEST_APP_VERSION"), QStringLiteral("0.1.0"));
    inputs.appVersion = QStringLiteral("0.1.5");
    inputs.cacheDir = cacheDir;
    // A test session's downloads: the temporary folder's, never the cache.
    inputs.tempDir = cacheDir;
    UpdateSetup setup = gitbolt::ui::setUpUpdates(inputs);
    setup.package = std::move(package);
    return setup;
}

QStringList entries(const QString& dir) {
    return QDir(dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
}

QByteArray contents(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<missing>");
}

bool writeFile(const QString& path, const QByteArray& bytes, bool executable = false) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
        return false;
    file.close();
    return !executable || file.setPermissions(file.permissions() | QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther);
}

// The buttons the dialog shows, by objectName.
QStringList shownButtons(const UpdateDialog* dialog) {
    QStringList names;
    for (const QPushButton* b : dialog->findChildren<QPushButton*>()) {
        if (!b->isHidden())
            names << b->objectName();
    }
    names.sort();
    return names;
}

QString headline(const UpdateDialog* dialog) {
    return dialog->findChild<QLabel*>(QStringLiteral("updateDialog.headline"))->text();
}

// Every package's installer, so that any kind can install 0.1.1.
const QList<QPair<QString, QByteArray>> kEveryInstaller{
    {QStringLiteral("GitBolt-0.1.1-Darwin-arm64.dmg"), "dmg"},
    {QStringLiteral("GitBolt-0.1.1-Windows-AMD64.exe"), "exe"},
    {QStringLiteral("gitbolt_0.1.1_amd64.deb"), "deb"},
};

// "<status> <stage> <reason> <from> <to> <package>", to compare.
QString described(const gitbolt::telemetry::UpdateResult& r) {
    return QStringList{r.status, r.stage, r.reason, r.fromVersion, r.toVersion, r.package}.join(u' ');
}

// The window's side of it, without a MainWindow: the controller with a
// window, the window's settings and its notice bar; and what it says
// for usage statistics.
struct Harness {
    explicit Harness(UpdateSetup setup) {
        auto* column = new QVBoxLayout(&window);
        bar = new UpdateNoticeBar(&window);
        column->addWidget(bar);
        controller = new UpdateController(std::move(setup), &settings, bar, &window);
        QObject::connect(controller, &UpdateController::updateResult, controller,
                         [this](const gitbolt::telemetry::UpdateResult& r) { results << described(r); });
        QObject::connect(controller, &UpdateController::checkFailed, controller,
                         [this](const QString& reason) { checkFailures << reason; });
        QObject::connect(controller, &UpdateController::commandUsed, controller,
                         [this](const QString& name) { commands << name; });
    }

    UpdateDialog* dialog() const { return controller->dialog(); }
    QTimer* timer() const { return window.findChild<QTimer*>(QStringLiteral("updateCheckTimer")); }
    gitbolt::update::Downloader* downloader() const { return window.findChild<gitbolt::update::Downloader*>(); }
    // What is in the downloads' folder of `version` (absent: nothing).
    QStringList downloads(const QString& version = QStringLiteral("0.1.1")) const {
        return entries(QDir(controller->setup().config.downloadDir).filePath(version));
    }
    gitbolt::update::Checker* checker() const { return window.findChild<gitbolt::update::Checker*>(); }
    // As the bridge's fire-timer does.
    void fire() const { QMetaObject::invokeMethod(timer(), "timeout"); }
    QPushButton* button(const char* name) const {
        return dialog()->findChild<QPushButton*>(QString::fromLatin1(name));
    }

    QWidget window;
    SettingsService settings;
    UpdateNoticeBar* bar = nullptr;
    UpdateController* controller = nullptr;
    QStringList results;
    QStringList checkFailures;
    QStringList commands;
};

int closedPort() {
    QTcpServer probe;
    probe.listen(QHostAddress::LocalHost, 0);
    const int port = probe.serverPort();
    probe.close();
    return port;
}

} // namespace

class TestUpdateController : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
    }

    // A fresh GitBolt.ini and cache for each test, and on Unix the
    // stand-ins: apt-get's check says yes; pkexec "installs" the .deb
    // (its last argument) as the launcher, and fails as apt-get would
    // when it is gone; the launcher leaves "old" in started beside it.
    void init() {
        QSettings(SettingsService().fileName(), QSettings::IniFormat).clear();
        cache_ = std::make_unique<QTemporaryDir>();
        QVERIFY(cache_->isValid());
#if defined(Q_OS_UNIX)
        const QString bin = QDir(cache_->path()).filePath(QStringLiteral("stand-ins"));
        QVERIFY(writeFile(bin + QStringLiteral("/apt-get"), "#!/bin/sh\nexit 0\n", true));
        const QByteArray launcherPath = gitbolt::update::shellQuote(launcher()).toUtf8();
        QVERIFY(writeFile(bin + QStringLiteral("/pkexec"),
                          "#!/bin/sh\nfor last in \"$@\"; do :; done\n[ -f \"$last\" ] || exit 100\ncp \"$last\" "
                              + launcherPath + "\n",
                          true));
        QVERIFY(writeFile(launcher(), "#!/bin/sh\necho old >\"$(dirname \"$0\")/started\"\n", true));
        path_ = std::make_unique<gitbolt::test::ScopedEnv>("PATH", QFile::encodeName(bin) + ':' + qgetenv("PATH"));
#endif
    }

    void cleanup() {
        path_.reset();
    }

    // ---- checking ---------------------------------------------------------

    void manualCheckShowsWhatItFinds_data() {
        QTest::addColumn<QString>("served");
        QTest::addColumn<int>("state");
        QTest::addColumn<QString>("says");
        QTest::newRow("newer") << "0.1.1" << int(State::NotInstallable)
                               << "wasn't installed from a GitBolt release package";
        QTest::newRow("same") << "0.1.0" << int(State::UpToDate) << "GitBolt 0.1.0 is the latest version.";
        QTest::newRow("older") << "0.0.9" << int(State::UpToDate) << "This copy, 0.1.0, is newer.";
        QTest::newRow("error") << "" << int(State::Failed) << "HTTP 500";
    }
    void manualCheckShowsWhatItFinds() {
        QFETCH(QString, served);
        QFETCH(int, state);
        QFETCH(QString, says);
        HttpServer server;
        if (served.isEmpty())
            server.routes.insert(kFeedPath, Served::withStatus(500, "{}"));
        else
            serve(server, served, {{kDebAsset, "new"}});
        // The test binary is no package: Unknown, as a build tree is.
        Harness h(testSetup(server, cache_->path(), gitbolt::update::detectPackage(gitbolt::update::currentProbe())));

        h.controller->checkNow();
        QVERIFY(h.dialog() && h.dialog()->isVisible());
        QCOMPARE(h.dialog()->state(), State::Checking);
        QTRY_COMPARE(int(h.dialog()->state()), state);
        QVERIFY2(h.dialog()->shownText().contains(says), qPrintable(h.dialog()->shownText()));
        QCOMPARE(server.hits(kFeedPath), 1);
        // An answer is saved, a failure isn't; the bar stays out of it.
        QCOMPARE(h.settings.updateLastCheck().isValid(), !served.isEmpty());
        QVERIFY(h.bar->isHidden());
    }

    void tryAgainAsksAgain() {
        HttpServer server;
        server.routes.insert(kFeedPath, Served::withStatus(500, "{}"));
        Harness h(testSetup(server, cache_->path()));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Failed);

        serve(server, QStringLiteral("0.1.0"), {});
        h.button("updateDialog.retry")->click();
        QTRY_COMPARE(h.dialog()->state(), State::UpToDate);
        QCOMPARE(server.hits(kFeedPath), 2);
    }

    // A manual check while the dialog is closed mid-way: what it finds
    // goes to the bar.
    void closingWhileCheckingLeavesTheFindToTheBar() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kDebAsset, "new"}});
        server.routes[kFeedPath].delayMs = 300;
        Harness h(testSetup(server, cache_->path()));
        h.controller->checkNow();
        h.dialog()->reject();
        QTRY_COMPARE(h.bar->isHidden(), false);
        QCOMPARE(h.bar->text(), QStringLiteral("GitBolt 0.1.1 is available."));
        QVERIFY(!h.dialog()->isVisible());
    }

    // ---- the automatic check ------------------------------------------------

    void automaticFindShowsTheBarOnceADay() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kDebAsset, "new"}});
        Harness h(testSetup(server, cache_->path()));

        h.fire();
        QTRY_COMPARE(h.bar->isHidden(), false);
        QCOMPARE(h.bar->text(), QStringLiteral("GitBolt 0.1.1 is available."));
        QVERIFY(!h.dialog());   // no dialog for an automatic find
        QVERIFY(h.settings.updateLastCheck().isValid());

        // Not due again for a day.
        h.fire();
        QTest::qWait(200);
        QCOMPARE(server.hits(kFeedPath), 1);

        // View opens the dialog on what it found, without asking again.
        h.bar->findChild<QPushButton*>(QStringLiteral("updateNotice.view"))->click();
        QVERIFY(h.bar->isHidden());
        QVERIFY(h.dialog() && h.dialog()->isVisible());
        QCOMPARE(h.dialog()->state(), State::NotInstallable);
        QCOMPARE(server.hits(kFeedPath), 1);
    }

    void skippedVersionIsQuietUntilANewerOne() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kDebAsset, "new"}});
        Harness h(testSetup(server, cache_->path()));
        h.settings.setSkippedUpdateVersion(QStringLiteral("0.1.1"));

        h.fire();
        QTRY_COMPARE(server.hits(kFeedPath), 1);
        QTest::qWait(200);
        QVERIFY(h.bar->isHidden());

        // A manual check shows it all the same.
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::NotInstallable);
        h.dialog()->reject();

        // A newer one is news again.
        serve(server, QStringLiteral("0.1.2"), {{QStringLiteral("gitbolt_0.1.2_amd64.deb"), "new"}});
        h.settings.setUpdateLastCheck({});
        h.fire();
        QTRY_COMPARE(h.bar->isHidden(), false);
        QCOMPARE(h.bar->version(), QStringLiteral("0.1.2"));
    }

    void skipThisVersionIsSaved() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kDebAsset, "new"}});
        Harness h(testSetup(server, cache_->path()));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::NotInstallable);
        h.button("updateDialog.skip")->click();
        QCOMPARE(h.settings.skippedUpdateVersion(), QStringLiteral("0.1.1"));
        QVERIFY(!h.dialog()->isVisible());
    }

    void dismissingLastsTheSession() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kDebAsset, "new"}});
        {
            Harness h(testSetup(server, cache_->path()));
            h.fire();
            QTRY_COMPARE(h.bar->isHidden(), false);
            h.bar->findChild<QAbstractButton*>(QStringLiteral("updateNotice.dismiss"))->click();
            QVERIFY(h.bar->isHidden());

            h.settings.setUpdateLastCheck({});
            h.fire();
            QTRY_COMPARE(server.hits(kFeedPath), 2);
            QTest::qWait(200);
            QVERIFY(h.bar->isHidden());
            QVERIFY(h.settings.skippedUpdateVersion().isEmpty());
        }
        // The next session says it again.
        Harness again(testSetup(server, cache_->path()));
        again.settings.setUpdateLastCheck({});
        again.fire();
        QTRY_COMPARE(again.bar->isHidden(), false);
    }

    void switchOffOrOptOutAsksNothingByItself() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kDebAsset, "new"}});
        {
            Harness h(testSetup(server, cache_->path()));
            h.settings.setUpdateAutoCheck(false);
            h.fire();
            QTest::qWait(300);
            QCOMPARE(server.requests().size(), 0);
            // Asked, it asks.
            h.controller->checkNow();
            QTRY_COMPARE(server.hits(kFeedPath), 1);
            h.settings.setUpdateAutoCheck(true);
        }
        // GITBOLT_UPDATE_CHECK=0, the switch on and the check due: the
        // timer, fired, asks nothing either.
        UpdateSetup optedOut = testSetup(server, cache_->path());
        optedOut.envOptOut = true;
        Harness h(optedOut);
        h.settings.setUpdateLastCheck({});
        h.fire();
        QTest::qWait(300);
        QCOMPARE(server.hits(kFeedPath), 1);
    }

    // ---- scheduling ---------------------------------------------------------

    void onlyAnOfficialBuildSchedules() {
        HttpServer server;
        {
            Harness test(testSetup(server, cache_->path()));
            test.controller->start();
            QVERIFY(!test.timer()->isActive());
            QVERIFY(test.timer()->property("fireWhileStopped").toBool());
        }
        UpdateSetup official = testSetup(server, cache_->path());
        official.gate = UpdateGate::Active;
        {
            Harness h(official);
            QVERIFY(!h.timer()->property("fireWhileStopped").toBool());
            h.controller->start();
            QVERIFY(h.timer()->isActive());
            QCOMPARE(h.timer()->interval(), UpdateController::kFirstCheckDelayMs);
            // The first tick, then hourly. Not due: nothing asked.
            h.settings.setUpdateLastCheck(QDateTime::currentDateTimeUtc());
            h.fire();
            QCOMPARE(h.timer()->interval(), UpdateController::kCheckIntervalMs);
            QVERIFY(h.timer()->isActive());
            QTest::qWait(200);
            QCOMPARE(server.requests().size(), 0);
        }
        official.envOptOut = true;
        Harness optedOut(official);
        optedOut.controller->start();
        QVERIFY(!optedOut.timer()->isActive());
    }

    void startClearsOldDownloads() {
        HttpServer server;
        const UpdateSetup setup = testSetup(server, cache_->path());
        const QString updates = setup.config.downloadDir;
        for (const char* version : {"0.0.9", "0.1.0", "0.1.1"})
            QVERIFY(QDir().mkpath(QDir(updates).filePath(QString::fromLatin1(version))));
        Harness h(setup);
        h.controller->start();
        QCOMPARE(entries(updates), QStringList{QStringLiteral("0.1.1")});
    }

    // ---- installing ---------------------------------------------------------

    void cancelLeavesNothingBehind() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kDebAsset, QByteArray(256 * 1024, 'n')}});
        auto& slow = server.routes[QStringLiteral("/download/v0.1.1/%1").arg(kDebAsset).toUtf8()];
        slow.chunkBytes = 4096;
        slow.chunkDelayMs = 40;
        Harness h(testSetup(server, cache_->path(), deb()));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);

        h.button("updateDialog.install")->click();
        QCOMPARE(h.dialog()->state(), State::Downloading);
        // Into the version's folder, as a .part, until Cancel.
        QTRY_VERIFY(h.downloads().contains(QStringLiteral(".") + kDebAsset + QStringLiteral(".part")));
        h.button("updateDialog.cancel")->click();
        QCOMPARE(h.dialog()->state(), State::Available);
        QVERIFY(h.dialog()->isVisible());
        QCOMPARE(h.downloads(), QStringList{});
    }

    void closingTheDialogStopsTheDownload() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kDebAsset, QByteArray(256 * 1024, 'n')}});
        auto& slow = server.routes[QStringLiteral("/download/v0.1.1/%1").arg(kDebAsset).toUtf8()];
        slow.chunkBytes = 4096;
        slow.chunkDelayMs = 40;
        Harness h(testSetup(server, cache_->path(), deb()));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        h.button("updateDialog.install")->click();
        QTRY_VERIFY(h.downloads().size() == 1);
        h.dialog()->reject();
        QVERIFY(!h.downloader()->busy());
        QCOMPARE(h.downloads(), QStringList{});
    }

    // The download finished and was checked, and then the user walked
    // away (closed the dialog) before it was installed: the new deb
    // doesn't stay, and no helper is started.
    void anAbandonedDownloadIsDeleted() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kDebAsset, "the new deb"}});
        Harness h(testSetup(server, cache_->path(), deb()));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);

        // Right after the controller takes the download (it connected
        // first), before the Installer's queued outcome.
        const QString updates = h.controller->setup().config.downloadDir;
        bool abandoned = false;
        connect(h.downloader(), &gitbolt::update::Downloader::finished, this,
                [&](const QString& path) {
                    QCOMPARE(path, QDir(updates).filePath(QStringLiteral("0.1.1/") + kDebAsset));
                    QVERIFY(QFile::exists(path));
                    h.dialog()->reject();
                    abandoned = true;
                });
        h.button("updateDialog.install")->click();
        QTRY_VERIFY(abandoned);
        QTest::qWait(300);   // anything queued has run
        QCOMPARE(h.downloads(), QStringList{});
        QVERIFY(!QFile::exists(QDir(updates).filePath(QStringLiteral("install-helper.sh"))));
        QVERIFY(!QFile::exists(QDir(updates).filePath(QStringLiteral("install.log"))));
    }

    void aBadDownloadFailsCleanlyAndCanBeRetried() {
        HttpServer server;
        // SHA256SUMS.txt says something else.
        serve(server, QStringLiteral("0.1.1"), {{kDebAsset, "the new deb"}}, true,
              QByteArray(64, 'a') + "  " + kDebAsset.toUtf8() + '\n');
        Harness h(testSetup(server, cache_->path(), deb()));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        h.button("updateDialog.install")->click();
        QTRY_COMPARE(h.dialog()->state(), State::Failed);
        QVERIFY2(h.dialog()->shownText().contains(QStringLiteral("Couldn't install GitBolt 0.1.1")),
                 qPrintable(h.dialog()->shownText()));
        QCOMPARE(h.downloads(), QStringList{});

        // Try Again installs again (it doesn't just check).
        const QByteArray assetPath = QStringLiteral("/download/v0.1.1/%1").arg(kDebAsset).toUtf8();
        const int before = server.hits(assetPath);
        h.button("updateDialog.retry")->click();
        QTRY_COMPARE(h.dialog()->state(), State::Failed);
        QCOMPARE(server.hits(assetPath), before + 1);
        QCOMPARE(server.hits(kFeedPath), 1);
    }

    void whatTheReleaseLacksIsSaid_data() {
        QTest::addColumn<bool>("withInstaller");
        QTest::addColumn<bool>("withChecksums");
        QTest::addColumn<QString>("says");
        QTest::newRow("no installer for this system") << false << true
                                                      << "no installer for this system (gitbolt_0.1.1_amd64.deb)";
        QTest::newRow("no checksums") << true << false << "lists no checksums (SHA256SUMS.txt)";
    }
    void whatTheReleaseLacksIsSaid() {
        QFETCH(bool, withInstaller);
        QFETCH(bool, withChecksums);
        QFETCH(QString, says);
        HttpServer server;
        QList<QPair<QString, QByteArray>> assets{{QStringLiteral("GitBolt-0.1.1-Darwin-arm64.dmg"), "dmg"}};
        if (withInstaller)
            assets.append({kDebAsset, "new"});
        serve(server, QStringLiteral("0.1.1"), assets, withChecksums);
        Harness h(testSetup(server, cache_->path(), deb()));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::NotInstallable);
        QVERIFY2(h.dialog()->shownText().contains(says), qPrintable(h.dialog()->shownText()));
    }

    // The deb's reason gives the apt command with the real file name.
    void theDebReasonNamesTheVersion() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{QStringLiteral("gitbolt_0.1.1_amd64.deb"), "deb"}});
        PackageInfo deb{PackageKind::Deb, QStringLiteral("/usr/lib/gitbolt"), false,
                        QStringLiteral("… from a terminal: sudo apt install ./gitbolt_<version>_amd64.deb")};
        Harness h(testSetup(server, cache_->path(), deb));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::NotInstallable);
        QVERIFY2(h.dialog()->shownText().contains(QStringLiteral("sudo apt install ./gitbolt_0.1.1_amd64.deb")),
                 qPrintable(h.dialog()->shownText()));
    }

    // apt-get -s refuses the .deb (an unmet dependency) before GitBolt
    // quits: Failed, with apt's line, and nothing is remembered (apt
    // update may change its answer), so Try Again downloads and asks
    // again. No helper is started.
    void aReleaseThisDebCantInstallIsSaid() {
#if defined(Q_OS_WIN)
        QSKIP("a deb is never installed on Windows");
#else
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kDebAsset, "deb"}});
        QVERIFY(writeFile(QDir(cache_->path()).filePath(QStringLiteral("stand-ins/apt-get")),
                          "#!/bin/sh\necho ' gitbolt : Depends: libc6 (>= 99) but 2.39 is to be installed'\n"
                          "echo 'E: Unable to correct problems, you have held broken packages.'\nexit 100\n",
                          true));
        Harness h(testSetup(server, cache_->path(), deb()));
        int launches = 0;
        h.controller->setLaunchHook([&](gitbolt::update::Installer&, qint64) {
            ++launches;
            return false;
        });
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        h.button("updateDialog.install")->click();
        QTRY_COMPARE(h.dialog()->state(), State::Failed);
        const QString shown = h.dialog()->shownText();
        QVERIFY2(shown.contains(QStringLiteral("apt-get says this system can't install GitBolt 0.1.1: gitbolt : "
                                               "Depends: libc6 (>= 99) but 2.39 is to be installed")),
                 qPrintable(shown));
        QVERIFY(h.settings.incompatibleUpdateVersion().isEmpty());
        QCOMPARE(h.downloads(), QStringList{});
        QCOMPARE(h.results, QStringList{QStringLiteral("failed prepare apt_refused 0.1.0 0.1.1 deb")});
        const QByteArray debPath = QStringLiteral("/download/v0.1.1/%1").arg(kDebAsset).toUtf8();
        QCOMPARE(server.hits(debPath), 1);

        h.button("updateDialog.retry")->click();
        QTRY_COMPARE(server.hits(debPath), 2);
        QTRY_COMPARE(h.dialog()->state(), State::Failed);
        QCOMPARE(launches, 0);
#endif
    }

    // A release Install found this system too old for (macOS's or
    // Windows' minimum, remembered in updates/incompatible*): shown as one
    // this copy can't install, with why, and not downloaded again; with
    // the system changed since, if only its build (Windows 10's feature
    // updates), installable again; and forgotten once a newer one comes out.
    void aReleaseThisSystemIsTooOldForIsntDownloadedAgain_data() {
        QTest::addColumn<QString>("system");
        const QVersionNumber running = gitbolt::update::runningSystem();
        QTest::newRow("this system") << running.toString();
        QTest::newRow("another build since")
            << QVersionNumber(running.majorVersion(), running.minorVersion(), running.microVersion() + 1).toString();
        QTest::newRow("the system changed since") << QStringLiteral("0.0.1");
    }
    void aReleaseThisSystemIsTooOldForIsntDownloadedAgain() {
        QFETCH(QString, system);
        const bool sameSystem = system == gitbolt::update::runningSystem().toString();
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kDebAsset, "new"}});
        Harness h(testSetup(server, cache_->path(), deb()));
        const QString why = QStringLiteral("GitBolt 0.1.1 needs macOS 99.0 or later, and this Mac has macOS 15.6, "
                                           "so it can't be installed here.");
        h.settings.setIncompatibleUpdate(QStringLiteral("0.1.1"), why, system);
        h.controller->checkNow();
        QTRY_VERIFY(h.dialog()->state() != State::Checking);
        if (!sameSystem) {
            QCOMPARE(h.dialog()->state(), State::Available);
            QVERIFY(h.settings.incompatibleUpdateVersion().isEmpty());
            return;
        }
        QCOMPARE(h.dialog()->state(), State::NotInstallable);
        QVERIFY2(h.dialog()->shownText().contains(why), qPrintable(h.dialog()->shownText()));
        QVERIFY(h.button("updateDialog.install")->isHidden());
        QCOMPARE(server.hits(QStringLiteral("/download/v0.1.1/%1").arg(kDebAsset).toUtf8()), 0);
        QCOMPARE(h.settings.incompatibleUpdateVersion(), QStringLiteral("0.1.1"));

        // A newer release is news again: installable, and the refusal
        // is forgotten.
        h.dialog()->reject();
        serve(server, QStringLiteral("0.1.2"), {{QStringLiteral("gitbolt_0.1.2_amd64.deb"), "newer"}});
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        QCOMPARE(headline(h.dialog()), QStringLiteral("GitBolt 0.1.2 is available"));
        QVERIFY(h.settings.incompatibleUpdateVersion().isEmpty());
        QVERIFY(h.settings.incompatibleUpdateReason().isEmpty());
    }

    // macOS, end to end: a disk image whose app needs a newer macOS than
    // this one. Install downloads and attaches it, the Installer refuses
    // it, and the dialog says why, as for a release this copy can't
    // install, with nothing left behind. It is remembered: the next
    // check shows the same without downloading it again.
    void aDiskImageForANewerMacOSIsRemembered() {
#ifndef Q_OS_MACOS
        QSKIP("macOS: hdiutil");
#else
        QTemporaryDir images;
        const QString content = images.filePath(QStringLiteral("content"));
        QVERIFY(gitbolt::test::makeBundle(content + QStringLiteral("/GitBolt.app"), QStringLiteral("0.1.1"),
                                          QStringLiteral("com.gitbolt.app"), {}, QStringLiteral("99.0")));
        const QString dmgName = QStringLiteral("GitBolt-0.1.1-Darwin-arm64.dmg");
        const QString dmgPath = images.filePath(dmgName);
        QVERIFY(gitbolt::test::makeDiskImage(content, dmgPath));
        QFile dmgFile(dmgPath);
        QVERIFY(dmgFile.open(QIODevice::ReadOnly));
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{dmgName, dmgFile.readAll()}});
        QTemporaryDir apps;
        QVERIFY(gitbolt::test::makeBundle(apps.filePath(QStringLiteral("GitBolt.app")), QStringLiteral("0.1.0")));
        PackageInfo bundle{PackageKind::MacBundle, apps.filePath(QStringLiteral("GitBolt.app")), true, {}};
        Harness h(testSetup(server, cache_->path(), bundle));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        h.button("updateDialog.install")->click();
        QTRY_VERIFY_WITH_TIMEOUT(h.dialog()->state() != State::Downloading && h.dialog()->state() != State::Preparing,
                                 30000);
        QCOMPARE(h.dialog()->state(), State::NotInstallable);
        const QString shown = h.dialog()->shownText();
        QVERIFY2(shown.contains(QStringLiteral("GitBolt 0.1.1 needs macOS 99.0 or later, and this Mac has macOS ")),
                 qPrintable(shown));
        QCOMPARE(h.settings.incompatibleUpdateVersion(), QStringLiteral("0.1.1"));
        QCOMPARE(h.settings.incompatibleUpdateSystem(), gitbolt::update::runningSystem().toString());
        QCOMPARE(h.results, QStringList{QStringLiteral("incompatible prepare needs_newer_system 0.1.0 0.1.1 dmg")});
        const QString version = QDir(h.controller->setup().config.downloadDir).filePath(QStringLiteral("0.1.1"));
        QCOMPARE(entries(version), QStringList{});
        QCOMPARE(entries(apps.path()), QStringList{QStringLiteral("GitBolt.app")});

        const QByteArray dmgUrl = QStringLiteral("/download/v0.1.1/%1").arg(dmgName).toUtf8();
        QCOMPARE(server.hits(dmgUrl), 1);
        h.dialog()->reject();
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::NotInstallable);
        QVERIFY2(h.dialog()->shownText().contains(QStringLiteral("needs macOS 99.0 or later")),
                 qPrintable(h.dialog()->shownText()));
        QCOMPARE(server.hits(dmgUrl), 1);
#endif
    }

    // macOS: the download passes its checksum, but isn't a disk image
    // hdiutil attaches. The preparation fails, and the download goes.
    void aDiskImageThatWontAttachIsDeleted() {
#ifndef Q_OS_MACOS
        QSKIP("macOS: hdiutil");
#else
        HttpServer server;
        const QString dmg = QStringLiteral("GitBolt-0.1.1-Darwin-arm64.dmg");
        serve(server, QStringLiteral("0.1.1"), {{dmg, "not a disk image"}});
        QTemporaryDir apps;
        QVERIFY(QDir().mkpath(apps.filePath(QStringLiteral("GitBolt.app/Contents/MacOS"))));
        PackageInfo bundle{PackageKind::MacBundle, apps.filePath(QStringLiteral("GitBolt.app")), true, {}};
        Harness h(testSetup(server, cache_->path(), bundle));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        h.button("updateDialog.install")->click();
        QTRY_COMPARE_WITH_TIMEOUT(h.dialog()->state(), State::Failed, 30000);
        QCOMPARE(h.results, QStringList{QStringLiteral("failed prepare attach_failed 0.1.0 0.1.1 dmg")});
        const QString version = QDir(h.controller->setup().config.downloadDir).filePath(QStringLiteral("0.1.1"));
        QCOMPARE(entries(version), QStringList{});
        QCOMPARE(entries(apps.path()), QStringList{QStringLiteral("GitBolt.app")});
#endif
    }

    // Windows asks UAC while GitBolt runs, and a No (or a refusal of
    // another kind, as a standard user may get at once) comes back to
    // it: GitBolt doesn't quit, and the dialog says nothing was
    // installed and how to install it by hand. After a No, Try Again
    // asks again for the same checked download; closing the dialog
    // deletes it. The launch hook stands in for the prompt, which only
    // Windows with UAC on has.
    void aRefusedPromptKeepsGitBoltRunning_data() {
        QTest::addColumn<bool>("declined");
        QTest::newRow("declined") << true;
        QTest::newRow("failed") << false;
    }
    void aRefusedPromptKeepsGitBoltRunning() {
        QFETCH(bool, declined);
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), kEveryInstaller);
        const QByteArray exePath = "/download/v0.1.1/GitBolt-0.1.1-Windows-AMD64.exe";
        UrlCatcher catcher;
        // A Windows install stages nothing: prepare() checks the name.
        Harness h(testSetup(server, cache_->path(),
                            {PackageKind::WindowsInstall, QDir(cache_->path()).filePath(QStringLiteral("target")),
                             true, {}}));
        const QString byHand = QStringLiteral("To install it yourself, download GitBolt-0.1.1-Windows-AMD64.exe from "
                                              "the release page and run it as an administrator.");
        int quits = 0;
        QList<qint64> launched;
        QString downloaded;
        h.controller->setRestartHooks({[] { return qint64(4321); }, [&] { ++quits; }, 300});
        h.controller->setLaunchHook([&](gitbolt::update::Installer& installer, qint64 pid) {
            launched << pid;
            downloaded = QDir(h.controller->setup().config.downloadDir)
                             .filePath(QStringLiteral("0.1.1/GitBolt-0.1.1-Windows-AMD64.exe"));
            if (declined)
                emit installer.declined(byHand);
            else
                emit installer.failed(QStringLiteral("Windows couldn't start GitBolt's installer as an "
                                                     "administrator: Access is denied. ")
                                      + byHand);
            return false;
        });
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        QVERIFY2(h.dialog()->shownText().contains(
                     QStringLiteral("Windows will ask for permission for Windows PowerShell to install it.")),
                 qPrintable(h.dialog()->shownText()));
        h.button("updateDialog.install")->click();
        QTRY_COMPARE(launched.size(), 1);
        QCOMPARE(launched.first(), qint64(4321));
        QTest::qWait(400);   // past the quit grace: nothing asked to quit
        QCOMPARE(quits, 0);
        QVERIFY(h.dialog()->isVisible());
        const QString shown = h.dialog()->shownText();
        QVERIFY2(shown.contains(byHand), qPrintable(shown));
        QCOMPARE(shownButtons(h.dialog()),
                 (QStringList{QStringLiteral("updateDialog.close"), QStringLiteral("updateDialog.openReleasePage"),
                              QStringLiteral("updateDialog.retry")}));
        h.button("updateDialog.openReleasePage")->click();
        QCOMPARE(catcher.urls,
                 QList<QUrl>{QUrl(QStringLiteral("https://github.com/Inverian/GitBolt/releases/tag/v0.1.1"))});

        if (declined) {
            QCOMPARE(h.dialog()->state(), State::Declined);
            QCOMPARE(h.dialog()->property("updateState").toString(), QStringLiteral("declined"));
            QCOMPARE(headline(h.dialog()), QStringLiteral("GitBolt 0.1.1 wasn't installed"));
            QVERIFY2(shown.contains(QStringLiteral("Windows didn't give permission, so nothing was installed.")),
                     qPrintable(shown));
            // Still checked and ready: Try Again asks Windows again,
            // with no second download.
            QVERIFY(QFile::exists(downloaded));
            h.button("updateDialog.retry")->click();
            QCOMPARE(launched.size(), 2);
            QCOMPARE(server.hits(exePath), 1);
            QCOMPARE(h.dialog()->state(), State::Declined);
            // A check meanwhile leaves it be.
            h.controller->checkNow();
            QCOMPARE(h.dialog()->state(), State::Declined);
            // Closed: the download goes, and still nothing quits.
            h.dialog()->reject();
            QVERIFY(!QFile::exists(downloaded));
        } else {
            QCOMPARE(h.dialog()->state(), State::Failed);
            QCOMPARE(headline(h.dialog()), QStringLiteral("Couldn't install GitBolt 0.1.1"));
            QVERIFY2(shown.contains(QStringLiteral("Access is denied.")), qPrintable(shown));
            QVERIFY(!QFile::exists(downloaded));
            // Try Again starts over: downloaded again, asked again.
            h.button("updateDialog.retry")->click();
            QTRY_COMPARE(launched.size(), 2);
            QCOMPARE(server.hits(exePath), 2);
        }
        QTest::qWait(400);
        QCOMPARE(quits, 0);
    }

    // ---- what the dialog shows is what is acted on ----------------------------

    // The bar offers 0.1.1; a manual check is still out when the user
    // clicks the bar's View and then Install. The check's late answer (a
    // failure, or an older release) must not replace the install screen:
    // its Close would cancel the install.
    void viewDuringAManualCheckKeepsTheInstall_data() {
        QTest::addColumn<bool>("fails");
        QTest::newRow("the check fails") << true;
        QTest::newRow("the check finds nothing newer") << false;
    }
    void viewDuringAManualCheckKeepsTheInstall() {
        QFETCH(bool, fails);
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kDebAsset, QByteArray(64 * 1024, 'n')}});
        // The download stops after a little: no progress() puts the
        // dialog right again.
        server.routes[QStringLiteral("/download/v0.1.1/%1").arg(kDebAsset).toUtf8()].stallAfter = 1024;
        Harness h(testSetup(server, cache_->path(), deb()));

        h.fire();
        QTRY_COMPARE(h.bar->isHidden(), false);
        Served late = fails ? Served::withStatus(500, "{}")
                            : Served::json(releaseJson(server, QStringLiteral("0.0.9"), {}));
        late.delayMs = 700;
        server.routes.insert(kFeedPath, late);

        h.controller->checkNow();
        QCOMPARE(h.dialog()->state(), State::Checking);
        QTRY_COMPARE(server.hits(kFeedPath), 2);
        h.bar->findChild<QPushButton*>(QStringLiteral("updateNotice.view"))->click();
        QCOMPARE(h.dialog()->state(), State::Available);
        h.button("updateDialog.install")->click();
        QCOMPARE(h.dialog()->state(), State::Downloading);

        // The answer comes, and is let be.
        QTRY_VERIFY(!h.checker()->busy());
        QTest::qWait(100);
        QCOMPARE(h.dialog()->state(), State::Downloading);
        QVERIFY2(!h.dialog()->shownText().contains(QStringLiteral("Couldn't check")), qPrintable(h.dialog()->shownText()));
        QVERIFY(h.downloader()->busy());
        QCOMPARE(headline(h.dialog()), QStringLiteral("Downloading GitBolt 0.1.1…"));

        // Try Again isn't there to check again; Cancel goes back to the
        // release, and a later check still answers in the dialog.
        h.button("updateDialog.cancel")->click();
        QCOMPARE(h.dialog()->state(), State::Available);
        QCOMPARE(h.downloads(), QStringList{});
        server.routes.insert(kFeedPath, Served::withStatus(500, "{}"));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Failed);
        QVERIFY(h.dialog()->shownText().contains(QStringLiteral("Couldn't check")));
    }

    // The dialog shows 0.1.1 when the daily check finds 0.1.2: Install
    // and Skip still mean 0.1.1, and the bar offers 0.1.2 once the
    // dialog closes.
    void theDialogsReleaseIsTheOneActedOn() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kDebAsset, QByteArray(256 * 1024, 'n')}});
        const QByteArray oldAsset = QStringLiteral("/download/v0.1.1/%1").arg(kDebAsset).toUtf8();
        server.routes[oldAsset].chunkBytes = 4096;
        server.routes[oldAsset].chunkDelayMs = 40;
        Harness h(testSetup(server, cache_->path(), deb()));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        QCOMPARE(headline(h.dialog()), QStringLiteral("GitBolt 0.1.1 is available"));

        const QString newAsset = QStringLiteral("gitbolt_0.1.2_amd64.deb");
        serve(server, QStringLiteral("0.1.2"), {{newAsset, "newer"}});
        h.settings.setUpdateLastCheck({});
        h.fire();
        QTRY_COMPARE(server.hits(kFeedPath), 2);
        QTRY_VERIFY(!h.checker()->busy());
        QCOMPARE(headline(h.dialog()), QStringLiteral("GitBolt 0.1.1 is available"));
        QVERIFY(h.bar->isHidden());   // the dialog is up

        h.button("updateDialog.install")->click();
        QCOMPARE(headline(h.dialog()), QStringLiteral("Downloading GitBolt 0.1.1…"));
        QTRY_COMPARE(server.hits(oldAsset), 1);
        QCOMPARE(server.hits(QStringLiteral("/download/v0.1.2/%1").arg(newAsset).toUtf8()), 0);
        h.button("updateDialog.cancel")->click();
        QCOMPARE(h.dialog()->state(), State::Available);
        QCOMPARE(headline(h.dialog()), QStringLiteral("GitBolt 0.1.1 is available"));

        h.button("updateDialog.skip")->click();
        QCOMPARE(h.settings.skippedUpdateVersion(), QStringLiteral("0.1.1"));
        QVERIFY(!h.dialog()->isVisible());
        QVERIFY(!h.bar->isHidden());
        QCOMPARE(h.bar->version(), QStringLiteral("0.1.2"));
    }

    // As above, but GitHub then takes 0.1.2 down: the next answer, 0.1.1
    // (the release on screen), says the newer one is gone, and the bar
    // doesn't offer it once the dialog closes. Whether that answer is a
    // manual check's or the daily one's.
    void aWithdrawnReleaseIsntOfferedOnceTheDialogCloses_data() {
        QTest::addColumn<bool>("manual");
        QTest::newRow("Check for Updates") << true;
        QTest::newRow("the daily check") << false;
    }
    void aWithdrawnReleaseIsntOfferedOnceTheDialogCloses() {
        QFETCH(bool, manual);
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kDebAsset, "new"}});
        Harness h(testSetup(server, cache_->path(), deb()));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);

        serve(server, QStringLiteral("0.1.2"), {{QStringLiteral("gitbolt_0.1.2_amd64.deb"), "newer"}});
        h.settings.setUpdateLastCheck({});
        h.fire();
        QTRY_COMPARE(server.hits(kFeedPath), 2);
        QTRY_VERIFY(!h.checker()->busy());

        serve(server, QStringLiteral("0.1.1"), {{kDebAsset, "new"}});
        if (manual) {
            h.controller->checkNow();
        } else {
            h.settings.setUpdateLastCheck({});
            h.fire();
        }
        QTRY_COMPARE(server.hits(kFeedPath), 3);
        QTRY_VERIFY(!h.checker()->busy());
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        QCOMPARE(headline(h.dialog()), QStringLiteral("GitBolt 0.1.1 is available"));

        h.button("updateDialog.later")->click();
        QVERIFY(!h.dialog()->isVisible());
        QTest::qWait(50);
        QVERIFY2(h.bar->isHidden(), qPrintable(h.bar->text()));
    }

    // ---- once the helper runs ------------------------------------------------

    // A real helper for a stand-in deb, waiting on a child process in
    // this process's place; the quit is caught, and never happens.
    void afterTheHelperStartsNothingIsLeftToCancel() {
#if defined(Q_OS_WIN)
        QSKIP("the deb helper is a /bin/sh script");
#else
        HttpServer server;
        // The "new deb": the stand-in pkexec puts it in place as the
        // launcher, which the helper then starts, and it says so.
        const QByteArray script = "#!/bin/sh\necho new >\"$(dirname \"$0\")/started\"\n";
        serve(server, QStringLiteral("0.1.1"), {{kDebAsset, script}});
        QProcess stand;   // GitBolt, for the helper to wait for
        stand.start(QStringLiteral("/bin/sleep"), {QStringLiteral("120")});
        QVERIFY(stand.waitForStarted());
        auto standDown = qScopeGuard([&] {
            stand.kill();
            stand.waitForFinished();
        });
        QString downloaded;
        {
            Harness h(testSetup(server, cache_->path(), deb()));
            int quits = 0;
            h.controller->setRestartHooks({[&] { return stand.processId(); }, [&] { ++quits; }, 300});
            h.controller->checkNow();
            QTRY_COMPARE(h.dialog()->state(), State::Available);
            h.button("updateDialog.install")->click();

            QTRY_COMPARE_WITH_TIMEOUT(h.dialog()->state(), State::Restarting, 15000);
            QCOMPARE(quits, 1);
            QCOMPARE(shownButtons(h.dialog()), QStringList{});
            QCOMPARE(headline(h.dialog()), QStringLiteral("Restarting GitBolt to finish the update…"));
            const QDir downloads(h.controller->setup().config.downloadDir);
            QVERIFY(QFile::exists(downloads.filePath(QStringLiteral("install-helper.sh"))));
            QVERIFY(QFile::exists(downloads.filePath(QStringLiteral("install.log"))));
            downloaded = downloads.filePath(QStringLiteral("0.1.1/") + kDebAsset);
            // Watched until the helper's own wait is over, and a margin.
            auto* deadline = h.window.findChild<QTimer*>(QStringLiteral("updateHelperDeadline"));
            QVERIFY(deadline && deadline->isActive());
            QCOMPARE(deadline->interval(), (gitbolt::update::Installer::kHelperWaitSeconds + 60) * 1000);

            // Still here after the grace time: the quit was refused.
            QTRY_COMPARE(h.dialog()->state(), State::QuitNeeded);
            QCOMPARE(headline(h.dialog()), QStringLiteral("Quit GitBolt to finish installing 0.1.1."));
            QCOMPARE(shownButtons(h.dialog()),
                     (QStringList{QStringLiteral("updateDialog.close"), QStringLiteral("updateDialog.quit")}));
            QVERIFY(h.dialog()->shownText().contains(QStringLiteral("within 10 minutes")));
            h.button("updateDialog.quit")->click();
            QCOMPARE(quits, 2);
            QCOMPARE(h.dialog()->state(), State::Restarting);
            QTRY_COMPARE(h.dialog()->state(), State::QuitNeeded);

            // Nothing starts over: no check, no second install.
            h.controller->checkNow();
            QCOMPARE(h.dialog()->state(), State::QuitNeeded);
            QCOMPARE(server.hits(kFeedPath), 1);

            // Closed, or gone with the window: the download is the
            // helper's, and stays.
            h.dialog()->reject();
        }
        QCOMPARE(contents(downloaded), script);
        QVERIFY(!QFile::exists(started()));

        // "GitBolt" quits: the helper installs the new deb and starts
        // the launcher, now the new one.
        stand.kill();
        stand.waitForFinished();
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(started()), 15000);
        QCOMPARE(contents(started()), QByteArray("new\n"));
        QCOMPARE(contents(launcher()), script);
#endif
    }

    // GitBolt didn't quit (refused, then the user went on working), and
    // the helper's wait ran out: it gave up and changed nothing. The
    // dialog no longer says that quitting installs the update; it says
    // it was abandoned, and checking and installing work again. The
    // deadline here is short (the helper's own wait isn't), so the real
    // helper still waits when it passes: once "GitBolt" does quit, it
    // finds no download any more and changes nothing either.
    void anUpdateTheHelperGaveUpOnIsOver() {
#if defined(Q_OS_WIN)
        QSKIP("the deb helper is a /bin/sh script");
#else
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kDebAsset, "#!/bin/sh\nexit 0\n"}});
        const QByteArray oldLauncher = contents(launcher());
        QProcess stand;
        stand.start(QStringLiteral("/bin/sleep"), {QStringLiteral("120")});
        QVERIFY(stand.waitForStarted());
        auto standDown = qScopeGuard([&] {
            stand.kill();
            stand.waitForFinished();
        });
        Harness h(testSetup(server, cache_->path(), deb()));
        int quits = 0;
        h.controller->setRestartHooks({[&] { return stand.processId(); }, [&] { ++quits; }, 300, 1500});
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        h.button("updateDialog.install")->click();
        QTRY_COMPARE_WITH_TIMEOUT(h.dialog()->state(), State::QuitNeeded, 15000);
        // Closed, as a user who went on working would.
        h.dialog()->reject();
        // What the helper writes as it gives up.
        const QDir downloads(h.controller->setup().config.downloadDir);
        QVERIFY(writeFile(downloads.filePath(QStringLiteral("install-result")),
                          "gave-up 0.1.1 GitBolt didn't quit within 600 seconds\n"));

        QTRY_COMPARE(h.dialog()->state(), State::Failed);
        QVERIFY(h.dialog()->isVisible());
        QCOMPARE(headline(h.dialog()), QStringLiteral("Couldn't install GitBolt 0.1.1"));
        QVERIFY2(h.dialog()->shownText().contains(
                     QStringLiteral("GitBolt didn't quit within 10 minutes of Install and Restart, so the update "
                                    "was abandoned.")),
                 qPrintable(h.dialog()->shownText()));
        QCOMPARE(shownButtons(h.dialog()), (QStringList{QStringLiteral("updateDialog.close"),
                                                         QStringLiteral("updateDialog.openReleasePage"),
                                                         QStringLiteral("updateDialog.retry")}));
        QCOMPARE(quits, 1);
        // Nothing of it is left, and the next start doesn't say it again.
        QCOMPARE(h.downloads(), QStringList{});
        QVERIFY(!QFile::exists(downloads.filePath(QStringLiteral("install-result"))));

        // A check asks GitHub again, and offers the release again.
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        QCOMPARE(server.hits(kFeedPath), 2);
        // So does the daily one.
        h.dialog()->reject();
        h.settings.setUpdateLastCheck({});
        h.fire();
        QTRY_COMPARE(server.hits(kFeedPath), 3);

        // The late helper: "GitBolt" quits, and nothing changes.
        stand.kill();
        stand.waitForFinished();
        QTRY_VERIFY_WITH_TIMEOUT(contents(downloads.filePath(QStringLiteral("install-result"))).startsWith("failed 0.1.1 "),
                                 15000);
        QCOMPARE(contents(launcher()), oldLauncher);
#endif
    }

    // ---- after an install: the helper's install-result -----------------------

    void anEarlierFailureIsShown_data() {
        QTest::addColumn<QByteArray>("result");
        QTest::addColumn<int>("kind");
        QTest::addColumn<QStringList>("says");
        QTest::addColumn<QString>("offerSays");

        QTest::newRow("Windows: the installer failed")
            << QByteArray("failed 0.1.1 the installer exited with code 2\n") << int(PackageKind::WindowsInstall)
            << QStringList{QStringLiteral("Installing GitBolt 0.1.1 failed after GitBolt quit: the installer "
                                          "exited with code 2."),
                           QStringLiteral("download GitBolt-0.1.1-Windows-AMD64.exe from the release page and run "
                                          "it as an administrator.")}
            << "Windows will ask for permission for Windows PowerShell to install it.";
        // PowerShell's way: a BOM, CRLF.
        QTest::newRow("Windows: UAC declined")
            << QByteArray("\xEF\xBB\xBF" "declined 0.1.1 the request for permission was cancelled\r\n")
            << int(PackageKind::WindowsInstall)
            << QStringList{QStringLiteral("needs an administrator's permission, and it wasn't given: the request "
                                          "for permission was cancelled.")}
            << "Windows will ask for permission for Windows PowerShell to install it.";
        QTest::newRow("deb: polkit dismissed") << QByteArray("declined 0.1.1\n") << int(PackageKind::Deb)
                                               << QStringList{QStringLiteral("and it wasn't given."),
                                                              QStringLiteral("sudo apt install ./gitbolt_0.1.1_amd64.deb")}
                                               << "You'll be asked for an administrator's password.";
        QTest::newRow("deb: pkexec couldn't authenticate")
            << QByteArray("failed 0.1.1 pkexec couldn't authenticate (no polkit authentication agent is running, or "
                          "polkit isn't set up to allow it)\n")
            << int(PackageKind::Deb)
            << QStringList{QStringLiteral("\nInstalling GitBolt 0.1.1 failed after GitBolt quit: pkexec couldn't "
                                          "authenticate (no polkit authentication agent is running, or polkit isn't "
                                          "set up to allow it). GitBolt wasn't changed.\n"),
                           QStringLiteral("sudo apt install ./gitbolt_0.1.1_amd64.deb")}
            << "You'll be asked for an administrator's password.";
        QTest::newRow("macOS: rolled back")
            << QByteArray("failed 0.1.1 couldn't move the new copy into place; the old one was put back.\n")
            << int(PackageKind::MacBundle)
            << QStringList{QStringLiteral("the old one was put back."),
                           QStringLiteral("download GitBolt-0.1.1-Darwin-arm64.dmg from the release page, open it, "
                                          "and drag GitBolt into your Applications folder.")}
            << "restarts GitBolt with it.";
        // What the helpers write: the reason, in seconds, isn't repeated.
        QTest::newRow("deb: gave up")
            << QByteArray("gave-up 0.1.1 GitBolt didn't quit within 600 seconds\n") << int(PackageKind::Deb)
            << QStringList{QStringLiteral("\nGitBolt 0.1.1 wasn't installed: GitBolt didn't quit within 10 minutes "
                                          "of Install and Restart. GitBolt wasn't changed.\n"),
                           QStringLiteral("sudo apt install ./gitbolt_0.1.1_amd64.deb")}
            << "You'll be asked for an administrator's password.";
    }
    void anEarlierFailureIsShown() {
        QFETCH(QByteArray, result);
        QFETCH(int, kind);
        QFETCH(QStringList, says);
        QFETCH(QString, offerSays);
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), kEveryInstaller);
        UrlCatcher catcher;
        const PackageKind packageKind = PackageKind(kind);
        UpdateSetup setup = testSetup(server, cache_->path(),
                                      {packageKind, QDir(cache_->path()).filePath(QStringLiteral("target")), true, {}});
        const QDir downloads(setup.config.downloadDir);
        QVERIFY(writeFile(downloads.filePath(QStringLiteral("install-result")), result));
        QByteArray log;
        for (int i = 1; i <= 15; ++i)
            log += "2026-11-07 10:00:" + QByteArray::number(i).rightJustified(2, '0') + " step " + QByteArray::number(i) + '\n';
        QVERIFY(writeFile(downloads.filePath(QStringLiteral("install.log")), log));

        Harness h(setup);
        QSignalSpy status(h.controller, &UpdateController::statusMessage);
        h.controller->start();
        QVERIFY(h.dialog() && h.dialog()->isVisible());
        QCOMPARE(h.dialog()->state(), State::Failed);
        QCOMPARE(headline(h.dialog()), QStringLiteral("Couldn't install GitBolt 0.1.1"));
        const QString shown = h.dialog()->shownText();
        for (const QString& line : says)
            QVERIFY2(shown.contains(line), qPrintable(shown));
        QVERIFY2(shown.contains(QStringLiteral("GitBolt wasn't changed.")), qPrintable(shown));
        QVERIFY2(shown.contains(QDir::toNativeSeparators(downloads.filePath(QStringLiteral("install.log")))),
                 qPrintable(shown));
        // The log's last ten lines, as written.
        auto* notes = h.dialog()->findChild<QPlainTextEdit*>(QStringLiteral("updateDialog.notes"));
        QVERIFY(!notes->isHidden());
        const QStringList lines = notes->toPlainText().split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 10);
        QVERIFY(lines.first().endsWith(QStringLiteral("step 6")));
        QVERIFY(lines.last().endsWith(QStringLiteral("step 15")));
        QCOMPARE(status.count(), 0);
        // Read once: gone, and nothing asked of the network.
        QVERIFY(!QFile::exists(downloads.filePath(QStringLiteral("install-result"))));
        QVERIFY(QFile::exists(downloads.filePath(QStringLiteral("install.log"))));
        QCOMPARE(server.requests().size(), 0);

        // That version's release page.
        h.button("updateDialog.openReleasePage")->click();
        QCOMPARE(catcher.urls, QList<QUrl>{QUrl(QStringLiteral("https://github.com/Inverian/GitBolt/releases/tag/v0.1.1"))});
        // Try Again asks again, and offers it once more, saying what
        // comes (the system's prompt) this time.
        h.button("updateDialog.retry")->click();
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        QCOMPARE(server.hits(kFeedPath), 1);
        QVERIFY2(h.dialog()->shownText().contains(offerSays), qPrintable(h.dialog()->shownText()));
        QVERIFY(notes->toPlainText().contains(QStringLiteral("Fixed.")));

        // The next start, in the same folder, says nothing.
        Harness again(setup);
        again.controller->start();
        QVERIFY(!again.dialog());
    }

    // No log (it was deleted, or never written): the dialog says the rest.
    void anEarlierFailureWithoutALog() {
        HttpServer server;
        UpdateSetup setup = testSetup(server, cache_->path());
        QVERIFY(writeFile(QDir(setup.config.downloadDir).filePath(QStringLiteral("install-result")),
                          "failed 0.1.1 apt-get failed with status 100\n"));
        Harness h(setup);
        h.controller->start();
        QVERIFY(h.dialog());
        QCOMPARE(h.dialog()->state(), State::Failed);
        QVERIFY(h.dialog()->findChild<QPlainTextEdit*>(QStringLiteral("updateDialog.notes"))->isHidden());
        QVERIFY(!h.dialog()->shownText().contains(QStringLiteral("install.log")));
        QVERIFY(h.dialog()->shownText().contains(QStringLiteral("apt-get failed with status 100.")));
    }

    void anEarlierUpdateIsReported_data() {
        QTest::addColumn<QByteArray>("result");
        QTest::addColumn<QString>("says");
        QTest::newRow("this version") << QByteArray("ok 0.1.0 \n") << "GitBolt was updated to 0.1.0.";
        // A test session's relaunched copy goes on being 0.1.0
        // (GITBOLT_TEST_APP_VERSION), whatever it installed.
        QTest::newRow("a newer one") << QByteArray("ok 0.1.9\n") << "GitBolt was updated to 0.1.9.";
        QTest::newRow("stale: an older one") << QByteArray("ok 0.0.9\n") << "";
    }
    void anEarlierUpdateIsReported() {
        QFETCH(QByteArray, result);
        QFETCH(QString, says);
        HttpServer server;
        UpdateSetup setup = testSetup(server, cache_->path());
        const QString path = QDir(setup.config.downloadDir).filePath(QStringLiteral("install-result"));
        QVERIFY(writeFile(path, result));
        Harness h(setup);
        QSignalSpy status(h.controller, &UpdateController::statusMessage);
        h.controller->start();
        QCOMPARE(status.count(), says.isEmpty() ? 0 : 1);
        if (!says.isEmpty())
            QCOMPARE(status.first().first().toString(), says);
        QVERIFY(!h.dialog());
        QVERIFY(!QFile::exists(path));
    }

    void aStaleOrUnreadableResultIsDropped_data() {
        QTest::addColumn<QByteArray>("result");
        // Stale: this version, or an older one, runs now.
        QTest::newRow("failed, this version") << QByteArray("failed 0.1.0 the installer exited with code 2\n");
        QTest::newRow("declined, an older one") << QByteArray("declined 0.0.9\n");
        QTest::newRow("gave up, this version") << QByteArray("gave-up 0.1.0\n");
        // Not an install result.
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("blank") << QByteArray(" \n\n");
        QTest::newRow("a word") << QByteArray("garbage\n");
        QTest::newRow("no version") << QByteArray("failed\n");
        QTest::newRow("a short version") << QByteArray("failed 0.1 reason\n");
        QTest::newRow("a tag, not a version") << QByteArray("failed v0.1.1 reason\n");
        QTest::newRow("an unknown status") << QByteArray("maybe 0.1.1 reason\n");
        QTest::newRow("a status in capitals") << QByteArray("FAILED 0.1.1\n");
        QTest::newRow("two lines") << QByteArray("failed 0.1.1 one\nfailed 0.1.1 two\n");
        QTest::newRow("too long") << QByteArray("failed 0.1.1 ") + QByteArray(5000, 'x') + '\n';
        QTest::newRow("binary") << QByteArray("\x00\x01\x02\xff failed 0.1.1", 17);
    }
    void aStaleOrUnreadableResultIsDropped() {
        QFETCH(QByteArray, result);
        HttpServer server;
        UpdateSetup setup = testSetup(server, cache_->path(), deb());
        const QString path = QDir(setup.config.downloadDir).filePath(QStringLiteral("install-result"));
        QVERIFY(writeFile(path, result));
        Harness h(setup);
        QSignalSpy status(h.controller, &UpdateController::statusMessage);
        h.controller->start();
        QVERIFY(!h.dialog());
        QCOMPARE(status.count(), 0);
        QVERIFY(!QFile::exists(path));
    }

    // ---- outcomes, for usage statistics ------------------------------------------

    // At start(): install-result and the pending-install marker give one
    // outcome; the marker's from only if it is the same attempt's (0.9.x
    // writes none). A marker alone is a helper still at work until its
    // patience is over, then no_result. Stale or malformed: nothing.
    void earlierOutcomesAreReported_data() {
        QTest::addColumn<QByteArray>("result");   // empty: none
        QTest::addColumn<QByteArray>("marker");   // empty: none; <now>, <old>: its time
        QTest::addColumn<int>("kind");
        QTest::addColumn<QString>("reported");    // empty: nothing
        QTest::addColumn<bool>("markerKept");
        const int deb = int(PackageKind::Deb);
        QTest::newRow("ok, with its marker") << QByteArray("ok 0.1.0 \n") << QByteArray("0.0.9 0.1.0 <now>\n") << deb
                                             << "ok install none 0.0.9 0.1.0 deb" << false;
        QTest::newRow("ok, no marker (0.9.x)") << QByteArray("ok 0.1.0\n") << QByteArray() << deb
                                               << "ok install none unknown 0.1.0 deb" << false;
        QTest::newRow("deb: apt-get failed") << QByteArray("failed 0.1.1 apt-get failed with status 100\n")
                                             << QByteArray("0.1.0 0.1.1 <now>") << deb
                                             << "failed install apt_failed 0.1.0 0.1.1 deb" << false;
        QTest::newRow("deb: declined")
            << QByteArray("declined 0.1.1 the administrator password prompt was dismissed\n")
            << QByteArray("0.1.0 0.1.1 <now>") << deb << "declined install permission_declined 0.1.0 0.1.1 deb"
            << false;
        QTest::newRow("gave up") << QByteArray("gave-up 0.1.1 GitBolt didn't quit within 600 seconds\n")
                                 << QByteArray("0.1.0 0.1.1 <now>") << deb
                                 << "gave_up quit quit_timeout 0.1.0 0.1.1 deb" << false;
        QTest::newRow("Windows: the installer failed")
            << QByteArray("failed 0.1.1 the installer exited with code 2\n") << QByteArray("0.1.0 0.1.1 <now>")
            << int(PackageKind::WindowsInstall) << "failed install installer_exit 0.1.0 0.1.1 nsis" << false;
        QTest::newRow("an aged marker alone") << QByteArray() << QByteArray("0.1.0 0.1.1 <old>")
                                              << int(PackageKind::MacBundle)
                                              << "no_result install no_result 0.1.0 0.1.1 dmg" << false;
        QTest::newRow("a young marker alone") << QByteArray() << QByteArray("0.1.0 0.1.1 <now>") << deb << ""
                                              << true;
        QTest::newRow("a marker and an unreadable result") << QByteArray("garbage\n")
                                                           << QByteArray("0.1.0 0.1.1 <now>") << deb
                                                           << "no_result install no_result 0.1.0 0.1.1 deb" << false;
        QTest::newRow("a stale result") << QByteArray("failed 0.1.0 the installer exited with code 2\n")
                                        << QByteArray("0.0.9 0.1.0 <now>") << deb << "" << false;
        QTest::newRow("a marker for another version")
            << QByteArray("failed 0.1.1 apt-get failed with status 100\n") << QByteArray("0.1.0 0.1.2 <now>") << deb
            << "failed install apt_failed unknown 0.1.1 deb" << false;
        QTest::newRow("a malformed marker") << QByteArray() << QByteArray("0.1.0 v0.1.1 <old>") << deb << "" << false;
        QTest::newRow("a marker of two lines") << QByteArray() << QByteArray("0.1.0 0.1.1 1\n0.1.0 0.1.1 1\n") << deb
                                               << "" << false;
    }
    void earlierOutcomesAreReported() {
        QFETCH(QByteArray, result);
        QFETCH(QByteArray, marker);
        QFETCH(int, kind);
        QFETCH(QString, reported);
        QFETCH(bool, markerKept);
        HttpServer server;
        UpdateSetup setup = testSetup(server, cache_->path(),
                                      {PackageKind(kind), QDir(cache_->path()).filePath(QStringLiteral("target")),
                                       true, {}});
        const QDir downloads(setup.config.downloadDir);
        const QString markerPath = downloads.filePath(QLatin1String(UpdateController::kInstallMarkerFile));
        if (!result.isEmpty())
            QVERIFY(writeFile(downloads.filePath(QStringLiteral("install-result")), result));
        if (!marker.isEmpty()) {
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            marker.replace("<now>", QByteArray::number(now));
            marker.replace("<old>", QByteArray::number(now - UpdateController::kResultPatienceSeconds - 60));
            QVERIFY(writeFile(markerPath, marker));
        }
        Harness h(setup);
        h.controller->start();
        QCOMPARE(h.results, reported.isEmpty() ? QStringList{} : QStringList{reported});
        QCOMPARE(QFileInfo::exists(markerPath), markerKept);
        QVERIFY(!QFileInfo::exists(downloads.filePath(QStringLiteral("install-result"))));
    }

    // Once the helper runs, the marker says so for the next start: one
    // that comes while the helper may still work says nothing and keeps
    // it; one past the patience reports no_result and removes it.
    void theHelperLaunchLeavesAMarker() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), kEveryInstaller);
        const UpdateSetup setup = testSetup(
            server, cache_->path(),
            {PackageKind::WindowsInstall, QDir(cache_->path()).filePath(QStringLiteral("target")), true, {}});
        const QString markerPath =
            QDir(setup.config.downloadDir).filePath(QLatin1String(UpdateController::kInstallMarkerFile));
        {
            Harness h(setup);
            int quits = 0;
            h.controller->setRestartHooks({[] { return qint64(4321); }, [&] { ++quits; }, 300});
            h.controller->setLaunchHook([](gitbolt::update::Installer&, qint64) { return true; });
            h.controller->checkNow();
            QTRY_COMPARE(h.dialog()->state(), State::Available);
            const qint64 before = QDateTime::currentSecsSinceEpoch();
            h.button("updateDialog.install")->click();
            QTRY_COMPARE(h.dialog()->state(), State::Restarting);
            const QStringList line = QString::fromUtf8(contents(markerPath)).trimmed().split(u' ');
            QCOMPARE(line.size(), 3);
            QCOMPARE(line.mid(0, 2), (QStringList{QStringLiteral("0.1.0"), QStringLiteral("0.1.1")}));
            QVERIFY(line.at(2).toLongLong() >= before && line.at(2).toLongLong() <= QDateTime::currentSecsSinceEpoch());
            QCOMPARE(h.results, QStringList{});   // nothing to say yet
        }
        {
            Harness again(setup);
            again.controller->start();
            QCOMPARE(again.results, QStringList{});
            QVERIFY(QFileInfo::exists(markerPath));
        }
        const qint64 old = QDateTime::currentSecsSinceEpoch() - UpdateController::kResultPatienceSeconds - 1;
        QVERIFY(writeFile(markerPath, "0.1.0 0.1.1 " + QByteArray::number(old) + '\n'));
        Harness late(setup);
        late.controller->start();
        QCOMPARE(late.results, QStringList{QStringLiteral("no_result install no_result 0.1.0 0.1.1 nsis")});
        QVERIFY(!QFileInfo::exists(markerPath));
    }

    // While GitBolt runs: a failed download, Windows' No or failure (the
    // launch hook standing in for UAC), and a helper that gave up are each
    // reported at once; a Cancel or a closed dialog is no attempt's end.
    void inSessionOutcomesAreReported_data() {
        QTest::addColumn<QString>("how");
        QTest::addColumn<QString>("reported");
        QTest::newRow("a bad checksum") << "bad checksum" << "failed download checksum_mismatch 0.1.0 0.1.1 nsis";
        QTest::newRow("UAC: No") << "declined" << "declined launch permission_declined 0.1.0 0.1.1 nsis";
        QTest::newRow("UAC: refused") << "failed" << "failed launch other 0.1.0 0.1.1 nsis";
        QTest::newRow("the helper gave up") << "gave up" << "gave_up quit quit_timeout 0.1.0 0.1.1 nsis";
        QTest::newRow("Cancel") << "cancel" << "";
        QTest::newRow("closed") << "close" << "";
    }
    void inSessionOutcomesAreReported() {
        QFETCH(QString, how);
        QFETCH(QString, reported);
        HttpServer server;
        const QString exe = QStringLiteral("GitBolt-0.1.1-Windows-AMD64.exe");
        if (how == QLatin1String("bad checksum")) {
            serve(server, QStringLiteral("0.1.1"), {{exe, "exe"}}, true,
                  QByteArray(64, 'a') + "  " + exe.toUtf8() + '\n');
        } else {
            serve(server, QStringLiteral("0.1.1"), {{exe, QByteArray(256 * 1024, 'x')}});
            auto& route = server.routes[QStringLiteral("/download/v0.1.1/%1").arg(exe).toUtf8()];
            if (how == QLatin1String("cancel") || how == QLatin1String("close")) {
                route.chunkBytes = 4096;
                route.chunkDelayMs = 40;
            }
        }
        Harness h(testSetup(server, cache_->path(),
                            {PackageKind::WindowsInstall, QDir(cache_->path()).filePath(QStringLiteral("target")),
                             true, {}}));
        const QString markerPath = QDir(h.controller->setup().config.downloadDir)
                                       .filePath(QLatin1String(UpdateController::kInstallMarkerFile));
        int quits = 0;
        h.controller->setRestartHooks({[] { return qint64(4321); }, [&] { ++quits; }, 300, 1000});
        h.controller->setLaunchHook([&](gitbolt::update::Installer& installer, qint64) {
            if (how == QLatin1String("declined")) {
                emit installer.declined(QStringLiteral("Install it yourself."));
                return false;
            }
            if (how == QLatin1String("failed")) {
                emit installer.failed(QStringLiteral("Access is denied."));
                return false;
            }
            return true;
        });
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        h.button("updateDialog.install")->click();
        if (how == QLatin1String("cancel") || how == QLatin1String("close")) {
            QTRY_VERIFY(h.downloads().size() == 1);
            if (how == QLatin1String("cancel"))
                h.button("updateDialog.cancel")->click();
            else
                h.dialog()->reject();
            QTest::qWait(200);
        } else if (how == QLatin1String("gave up")) {
            QTRY_VERIFY(QFileInfo::exists(markerPath));
            QTRY_COMPARE_WITH_TIMEOUT(h.dialog()->state(), State::Failed, 5000);
            QVERIFY(!QFileInfo::exists(markerPath));
        } else {
            QTRY_VERIFY(!h.results.isEmpty());
        }
        QCOMPARE(h.results, reported.isEmpty() ? QStringList{} : QStringList{reported});
    }

    // Every click on the dialog's and the notice bar's buttons is
    // counted by the button's name, which usage statistics accept.
    void buttonsAreCountedByName() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kDebAsset, QByteArray(256 * 1024, 'n')}});
        auto& slow = server.routes[QStringLiteral("/download/v0.1.1/%1").arg(kDebAsset).toUtf8()];
        slow.chunkBytes = 4096;
        slow.chunkDelayMs = 40;
        Harness h(testSetup(server, cache_->path(), deb()));
        h.fire();
        QTRY_COMPARE(h.bar->isHidden(), false);
        h.bar->findChild<QAbstractButton*>(QStringLiteral("updateNotice.view"))->click();
        QCOMPARE(h.dialog()->state(), State::Available);
        h.button("updateDialog.later")->click();
        h.bar->showVersion(QStringLiteral("0.1.1"));
        h.bar->findChild<QAbstractButton*>(QStringLiteral("updateNotice.dismiss"))->click();

        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        h.button("updateDialog.install")->click();
        h.button("updateDialog.cancel")->click();
        h.button("updateDialog.skip")->click();

        server.routes.insert(kFeedPath, Served::withStatus(500, "{}"));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Failed);
        h.button("updateDialog.retry")->click();
        QTRY_COMPARE(h.dialog()->state(), State::Failed);
        h.button("updateDialog.close")->click();

        const QStringList expected{
            QStringLiteral("updateNotice.view"),    QStringLiteral("updateDialog.later"),
            QStringLiteral("updateNotice.dismiss"), QStringLiteral("updateDialog.install"),
            QStringLiteral("updateDialog.cancel"),  QStringLiteral("updateDialog.skip"),
            QStringLiteral("updateDialog.retry"),   QStringLiteral("updateDialog.close")};
        QCOMPARE(h.commands, expected);
        // Every button either has, the rest included.
        QStringList names;
        for (QAbstractButton* b : h.dialog()->findChildren<QAbstractButton*>())
            names << b->objectName();
        for (QAbstractButton* b : h.bar->findChildren<QAbstractButton*>())
            names << b->objectName();
        QVERIFY(names.size() >= expected.size());
        for (const QString& name : std::as_const(names))
            QCOMPARE(gitbolt::telemetry::commandName(name).value_or(QStringLiteral("(refused)")), name);
    }

    // A check GitHub answered with an error, a rate limit or not a
    // release is reported with its code, manual or automatic; being
    // offline isn't.
    void checkFailuresAreReported_data() {
        QTest::addColumn<int>("status");   // 0: a closed port
        QTest::addColumn<QByteArray>("header");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QString>("reported");
        QTest::newRow("500") << 500 << QByteArray() << QByteArray("{}") << "http_error";
        QTest::newRow("404") << 404 << QByteArray() << QByteArray("{}") << "http_error";
        QTest::newRow("403, no requests left") << 403 << QByteArray("x-ratelimit-remaining") << QByteArray("{}")
                                               << "rate_limited";
        QTest::newRow("429") << 429 << QByteArray() << QByteArray("{}") << "rate_limited";
        QTest::newRow("bad JSON") << 200 << QByteArray() << QByteArray("{\"tag_name\": ") << "bad_response";
        QTest::newRow("offline") << 0 << QByteArray() << QByteArray() << "";
    }
    void checkFailuresAreReported() {
        QFETCH(int, status);
        QFETCH(QByteArray, header);
        QFETCH(QByteArray, body);
        QFETCH(QString, reported);
        HttpServer server;
        Served served = Served::withStatus(status, body);
        if (!header.isEmpty())
            served.headers.append({header, "0"});
        server.routes.insert(kFeedPath, served);
        UpdateSetup setup = testSetup(server, cache_->path());
        if (status == 0)
            setup.config.feedUrl = QUrl(QStringLiteral("http://127.0.0.1:%1/latest").arg(closedPort()));
        Harness h(setup);
        h.controller->checkNow();
        QTRY_COMPARE_WITH_TIMEOUT(h.dialog()->state(), State::Failed, 20000);
        h.dialog()->reject();
        h.fire();
        QTRY_VERIFY_WITH_TIMEOUT(!h.checker()->busy(), 20000);
        QTest::qWait(50);
        const QStringList expected = reported.isEmpty() ? QStringList{} : QStringList{reported, reported};
        QCOMPARE(h.checkFailures, expected);
    }

    // update/Failure.h's names and telemetry's allowlist are one list:
    // every code can be sent, as itself, and no two share a name.
    void everyFailureCodeIsSendable() {
        using gitbolt::update::Failure;
        QSet<QString> names;
        for (int i = int(Failure::Other); i <= int(Failure::None); ++i) {
            const QString name = QString::fromLatin1(gitbolt::update::failureName(Failure(i)));
            QCOMPARE(gitbolt::telemetry::updateReason(name), name);
            QVERIFY2(!names.contains(name), qPrintable(name));
            names.insert(name);
        }
        QCOMPARE(names.size(), int(Failure::None) + 1);
        QCOMPARE(gitbolt::telemetry::updateReason(QStringLiteral("system_too_old")), QStringLiteral("other"));
    }

    // ---- MainWindow -----------------------------------------------------------

    // After an update, the window's status bar says so.
    void anUpdateIsReportedInTheStatusBar() {
        HttpServer server;
        gitbolt::ui::MainWindow window;
        UpdateSetup setup = testSetup(server, cache_->path());
        QVERIFY(writeFile(QDir(setup.config.downloadDir).filePath(QStringLiteral("install-result")), "ok 0.1.0\n"));
        window.setUpdates(setup);
        window.startUpdates();
        QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("GitBolt was updated to 0.1.0."));
        QCOMPARE(window.updateDownloadDir(), setup.config.downloadDir);
        QVERIFY(!window.findChild<UpdateDialog*>());
    }

    // Without the in-app check, both Check for Updates items are greyed
    // out: nothing is asked or opened, and no folder is made.
    void withoutTheCheckTheMenuIsGreyedOut_data() {
        QTest::addColumn<int>("kind");
        QTest::newRow("never set up") << 0;
        QTest::newRow("source build") << 1;
        QTest::newRow("test session without a test feed") << 2;
    }
    void withoutTheCheckTheMenuIsGreyedOut() {
        QFETCH(int, kind);
        UrlCatcher catcher;
        AppMenuWatcher appMenu;
        gitbolt::ui::MainWindow window;
        if (kind > 0) {
            gitbolt::ui::UpdateSetupInputs inputs;
            if (kind == 2)
                inputs.testSession = gitbolt::util::TestSessionKind::Bridge;
            inputs.appVersion = QStringLiteral("0.1.0");
            inputs.cacheDir = cache_->path();
            inputs.tempDir = cache_->path();
            window.setUpdates(gitbolt::ui::setUpUpdates(inputs));
        }
        QCOMPARE(window.updateGate(), kind == 2 ? UpdateGate::TestSession : UpdateGate::CompiledOut);
        window.startUpdates();
        auto* action = window.findChild<QAction*>(QStringLiteral("help.check-for-updates"));
        QVERIFY(action);
        QVERIFY(!action->isEnabled());
#ifdef Q_OS_MACOS
        QAction* appMenuAction = appMenu.named(QStringLiteral("help.check-for-updates-app-menu"));
        QVERIFY(appMenuAction);
        QVERIFY(!appMenuAction->isEnabled());
#endif
        action->trigger();   // ignored while disabled
        QVERIFY(catcher.urls.isEmpty());
        QVERIFY(!window.findChild<UpdateDialog*>());
        QVERIFY(!window.findChild<QTimer*>(QStringLiteral("updateCheckTimer")));
        QVERIFY(!QDir(cache_->path()).exists(QStringLiteral("updates")));
        // No downloads folder of a test session either.
        QCOMPARE(QDir(cache_->path()).entryList({QStringLiteral("gitbolt-updates*")},
                                                 QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
                 QStringList{});
    }

    void withTheCheckTheMenuAsks() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kDebAsset, "new"}});
        UrlCatcher catcher;
        AppMenuWatcher appMenu;
        gitbolt::ui::MainWindow window;
        window.setUpdates(testSetup(server, cache_->path()));
        QCOMPARE(window.updateGate(), UpdateGate::TestFeed);
        QVERIFY(window.findChild<QAction*>(QStringLiteral("help.check-for-updates"))->isEnabled());
#ifdef Q_OS_MACOS
        QAction* appMenuAction = appMenu.named(QStringLiteral("help.check-for-updates-app-menu"));
        QVERIFY(appMenuAction && appMenuAction->isEnabled());
#endif
        // The bar sits above the views, hidden until there's news.
        auto* bar = window.findChild<UpdateNoticeBar*>(QStringLiteral("updateNotice"));
        QVERIFY(bar && bar->isHidden());

        window.findChild<QAction*>(QStringLiteral("help.check-for-updates"))->trigger();
        auto* dialog = window.findChild<UpdateDialog*>();
        QVERIFY(dialog && dialog->isVisible());
        QTRY_COMPARE(dialog->state(), State::NotInstallable);
        QCOMPARE(server.hits(kFeedPath), 1);
        QVERIFY(catcher.urls.isEmpty());
        dialog->reject();
#ifdef Q_OS_MACOS
        // The application menu's item asks too.
        appMenuAction->trigger();
        QTRY_COMPARE(server.hits(kFeedPath), 2);
        QTRY_COMPARE(dialog->state(), State::NotInstallable);
        QVERIFY(catcher.urls.isEmpty());
        dialog->reject();
#endif

        // The daily check's timer, for the bridge.
        auto* timer = window.findChild<QTimer*>(QStringLiteral("updateCheckTimer"));
        QVERIFY(timer && !timer->isActive());
    }

    // macOS: About GitBolt and Check for Updates… reach the application
    // menu (GitBolt's, beside the Apple menu) through a placeholder menu
    // of a parentless menu bar, by their role. Each must have it as it
    // joins the placeholder: one that joins as a plain item (the default
    // role, whose guess from the text "Check for Updates…" is none) is
    // put in the placeholder by Cocoa, and the menu bar then shows the
    // placeholder, empty, as "App", even once the role has moved the
    // item out. About relies on that guess ("About …" is AboutRole).
    void theApplicationMenusItemsJoinWithTheirRoles() {
#ifndef Q_OS_MACOS
        QSKIP("macOS: the application menu");
#else
        AppMenuWatcher watcher;
        {
            gitbolt::ui::MainWindow window;
        }

        QStringList texts;
        for (const AppMenuWatcher::Joined& item : std::as_const(watcher.joined)) {
            texts << item.text;
            const bool guessed = item.role == QAction::TextHeuristicRole && item.text.startsWith(QLatin1String("About "));
            QVERIFY2(guessed || (item.role != QAction::TextHeuristicRole && item.role != QAction::NoRole),
                     qPrintable(item.text));
        }
        QCOMPARE(texts, (QStringList{QStringLiteral("About GitBolt"), QStringLiteral("Check for Updates…")}));
        // Both are named for the bridge, with their final roles; Check for
        // Updates… is greyed out, as the window never had updates on.
        QAction* about = watcher.named(QStringLiteral("app.about"));
        QAction* check = watcher.named(QStringLiteral("help.check-for-updates-app-menu"));
        QVERIFY(about && about->menuRole() == QAction::AboutRole);
        QVERIFY(check && check->menuRole() == QAction::ApplicationSpecificRole);
        QVERIFY(!check->isEnabled());
#endif
    }

private:
    // The deb's layout in the cache folder: usr/lib/gitbolt, and its
    // launcher usr/bin/gitbolt (made by init()), which the helper starts.
    PackageInfo deb() const {
        const QString target = QDir(cache_->path()).filePath(QStringLiteral("usr/lib/gitbolt"));
        QDir().mkpath(target);
        return {PackageKind::Deb, target, true, {}};
    }
    QString launcher() const { return QDir(cache_->path()).filePath(QStringLiteral("usr/bin/gitbolt")); }
    // What the launcher writes when the helper starts it.
    QString started() const { return QDir(cache_->path()).filePath(QStringLiteral("usr/bin/started")); }

    QTemporaryDir settingsDir_;
    std::unique_ptr<QTemporaryDir> cache_;
    std::unique_ptr<gitbolt::test::ScopedEnv> path_;   // the stand-ins first (Unix)
};

QTEST_MAIN(TestUpdateController)
#include "TestUpdateController.moc"
