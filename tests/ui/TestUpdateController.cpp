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
//   - Install and Restart downloads into the AppImage's folder; Cancel,
//     closing the dialog, a failed download and a download abandoned
//     after it finished all leave nothing there but the running AppImage,
//     and never start the helper; on macOS, a disk image that won't
//     attach is deleted too;
//   - MainWindow: without the in-app check (a source build, or a test
//     session with no test feed) Help › Check for Updates opens the
//     releases page; with it, it asks the feed.
//
// Nothing here gets as far as the helper: a prepared install would
// start one for this very process.
//

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVBoxLayout>

#include "../update/HttpServer.h"
#include "conf/SettingsService.h"
#include "dialogs/UpdateDialog.h"
#include "ui/MainWindow.h"
#include "ui/UpdateController.h"
#include "ui/UpdateSetup.h"
#include "update/Downloader.h"
#include "update/Package.h"
#include "widgets/UpdateNoticeBar.h"

using gitbolt::conf::SettingsService;
using gitbolt::dialogs::UpdateDialog;
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
const QString kAppImageAsset = QStringLiteral("GitBolt-0.1.1-x86_64.AppImage");

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

// The window's side of it, without a MainWindow: the controller with a
// window, the window's settings and its notice bar.
struct Harness {
    explicit Harness(UpdateSetup setup) {
        auto* column = new QVBoxLayout(&window);
        bar = new UpdateNoticeBar(&window);
        column->addWidget(bar);
        controller = new UpdateController(std::move(setup), &settings, bar, &window);
    }

    UpdateDialog* dialog() const { return controller->dialog(); }
    QTimer* timer() const { return window.findChild<QTimer*>(QStringLiteral("updateCheckTimer")); }
    gitbolt::update::Downloader* downloader() const { return window.findChild<gitbolt::update::Downloader*>(); }
    // As the bridge's fire-timer does.
    void fire() const { QMetaObject::invokeMethod(timer(), "timeout"); }
    QPushButton* button(const char* name) const {
        return dialog()->findChild<QPushButton*>(QString::fromLatin1(name));
    }

    QWidget window;
    SettingsService settings;
    UpdateNoticeBar* bar = nullptr;
    UpdateController* controller = nullptr;
};

} // namespace

class TestUpdateController : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
    }

    // A fresh GitBolt.ini and cache for each test.
    void init() {
        QSettings(SettingsService().fileName(), QSettings::IniFormat).clear();
        cache_ = std::make_unique<QTemporaryDir>();
        QVERIFY(cache_->isValid());
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
            serve(server, served, {{kAppImageAsset, "new"}});
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
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, "new"}});
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
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, "new"}});
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
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, "new"}});
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
        serve(server, QStringLiteral("0.1.2"), {{QStringLiteral("GitBolt-0.1.2-x86_64.AppImage"), "new"}});
        h.settings.setUpdateLastCheck({});
        h.fire();
        QTRY_COMPARE(h.bar->isHidden(), false);
        QCOMPARE(h.bar->version(), QStringLiteral("0.1.2"));
    }

    void skipThisVersionIsSaved() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, "new"}});
        Harness h(testSetup(server, cache_->path()));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::NotInstallable);
        h.button("updateDialog.skip")->click();
        QCOMPARE(h.settings.skippedUpdateVersion(), QStringLiteral("0.1.1"));
        QVERIFY(!h.dialog()->isVisible());
    }

    void dismissingLastsTheSession() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, "new"}});
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
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, "new"}});
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
        const QString updates = QDir(cache_->path()).filePath(QStringLiteral("gitbolt-updates"));
        for (const char* version : {"0.0.9", "0.1.0", "0.1.1"})
            QVERIFY(QDir().mkpath(QDir(updates).filePath(QString::fromLatin1(version))));
        Harness h(testSetup(server, cache_->path()));
        h.controller->start();
        QCOMPARE(entries(updates), QStringList{QStringLiteral("0.1.1")});
    }

    // ---- installing ---------------------------------------------------------

    void cancelLeavesNothingBehind() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, QByteArray(256 * 1024, 'n')}});
        auto& slow = server.routes[QStringLiteral("/download/v0.1.1/%1").arg(kAppImageAsset).toUtf8()];
        slow.chunkBytes = 4096;
        slow.chunkDelayMs = 40;
        const QString apps = appImageFolder();
        Harness h(testSetup(server, cache_->path(), appImage(apps)));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);

        h.button("updateDialog.install")->click();
        QCOMPARE(h.dialog()->state(), State::Downloading);
        // Into the AppImage's folder, as a .part, until Cancel.
        QTRY_VERIFY(entries(apps).contains(QStringLiteral(".") + kAppImageAsset + QStringLiteral(".part")));
        h.button("updateDialog.cancel")->click();
        QCOMPARE(h.dialog()->state(), State::Available);
        QVERIFY(h.dialog()->isVisible());
        QCOMPARE(entries(apps), QStringList{QStringLiteral("GitBolt.AppImage")});
        QCOMPARE(contents(QDir(apps).filePath(QStringLiteral("GitBolt.AppImage"))), QByteArray("old"));
    }

    void closingTheDialogStopsTheDownload() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, QByteArray(256 * 1024, 'n')}});
        auto& slow = server.routes[QStringLiteral("/download/v0.1.1/%1").arg(kAppImageAsset).toUtf8()];
        slow.chunkBytes = 4096;
        slow.chunkDelayMs = 40;
        const QString apps = appImageFolder();
        Harness h(testSetup(server, cache_->path(), appImage(apps)));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        h.button("updateDialog.install")->click();
        QTRY_VERIFY(entries(apps).size() == 2);
        h.dialog()->reject();
        QVERIFY(!h.downloader()->busy());
        QCOMPARE(entries(apps), QStringList{QStringLiteral("GitBolt.AppImage")});
    }

    // The download finished and was checked, and then the user walked
    // away (closed the dialog) before it was installed: the new AppImage
    // doesn't stay next to the old one, and no helper is started.
    void anAbandonedDownloadIsDeleted() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, "the new AppImage"}});
        const QString apps = appImageFolder();
        Harness h(testSetup(server, cache_->path(), appImage(apps)));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);

        // Right after the controller takes the download (it connected
        // first), before the Installer's queued outcome.
        bool abandoned = false;
        connect(h.downloader(), &gitbolt::update::Downloader::finished, this,
                [&](const QString& path) {
                    QCOMPARE(path, QDir(apps).filePath(kAppImageAsset));
                    QVERIFY(QFile::exists(path));
                    h.dialog()->reject();
                    abandoned = true;
                });
        h.button("updateDialog.install")->click();
        QTRY_VERIFY(abandoned);
        QTest::qWait(300);   // anything queued has run
        QCOMPARE(entries(apps), QStringList{QStringLiteral("GitBolt.AppImage")});
        QCOMPARE(contents(QDir(apps).filePath(QStringLiteral("GitBolt.AppImage"))), QByteArray("old"));
        const QString updates = QDir(cache_->path()).filePath(QStringLiteral("gitbolt-updates"));
        QVERIFY(!QFile::exists(QDir(updates).filePath(QStringLiteral("install-helper.sh"))));
        QVERIFY(!QFile::exists(QDir(updates).filePath(QStringLiteral("install.log"))));
    }

    void aBadDownloadFailsCleanlyAndCanBeRetried() {
        HttpServer server;
        // SHA256SUMS.txt says something else.
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, "the new AppImage"}}, true,
              QByteArray(64, 'a') + "  " + kAppImageAsset.toUtf8() + '\n');
        const QString apps = appImageFolder();
        Harness h(testSetup(server, cache_->path(), appImage(apps)));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        h.button("updateDialog.install")->click();
        QTRY_COMPARE(h.dialog()->state(), State::Failed);
        QVERIFY2(h.dialog()->shownText().contains(QStringLiteral("Couldn't install GitBolt 0.1.1")),
                 qPrintable(h.dialog()->shownText()));
        QCOMPARE(entries(apps), QStringList{QStringLiteral("GitBolt.AppImage")});

        // Try Again installs again (it doesn't just check).
        const QByteArray assetPath = QStringLiteral("/download/v0.1.1/%1").arg(kAppImageAsset).toUtf8();
        const int before = server.hits(assetPath);
        h.button("updateDialog.retry")->click();
        QTRY_COMPARE(h.dialog()->state(), State::Failed);
        QCOMPARE(server.hits(assetPath), before + 1);
        QCOMPARE(server.hits(kFeedPath), 1);
    }

    // The new AppImage's name is the running one's: it would download
    // over itself. Refused before anything is asked for.
    void anAppImageNamedLikeTheNewOneIsRefused() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, "new"}});
        const QString apps = appImageFolder(kAppImageAsset);
        Harness h(testSetup(server, cache_->path(), appImage(apps, kAppImageAsset)));
        h.controller->checkNow();
        QTRY_COMPARE(h.dialog()->state(), State::Available);
        h.button("updateDialog.install")->click();
        QCOMPARE(h.dialog()->state(), State::Failed);
        QVERIFY(h.dialog()->shownText().contains(QStringLiteral("has this one's name")));
        QCOMPARE(server.requests().size(), 1);
        QCOMPARE(contents(QDir(apps).filePath(kAppImageAsset)), QByteArray("old"));
    }

    void whatTheReleaseLacksIsSaid_data() {
        QTest::addColumn<bool>("withInstaller");
        QTest::addColumn<bool>("withChecksums");
        QTest::addColumn<QString>("says");
        QTest::newRow("no installer for this system") << false << true
                                                      << "no installer for this system (GitBolt-0.1.1-x86_64.AppImage)";
        QTest::newRow("no checksums") << true << false << "lists no checksums (SHA256SUMS.txt)";
    }
    void whatTheReleaseLacksIsSaid() {
        QFETCH(bool, withInstaller);
        QFETCH(bool, withChecksums);
        QFETCH(QString, says);
        HttpServer server;
        QList<QPair<QString, QByteArray>> assets{{QStringLiteral("GitBolt-0.1.1-Darwin-arm64.dmg"), "dmg"}};
        if (withInstaller)
            assets.append({kAppImageAsset, "new"});
        serve(server, QStringLiteral("0.1.1"), assets, withChecksums);
        Harness h(testSetup(server, cache_->path(), appImage(appImageFolder())));
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
        const QString version = QDir(cache_->path()).filePath(QStringLiteral("gitbolt-updates/0.1.1"));
        QCOMPARE(entries(version), QStringList{});
        QCOMPARE(entries(apps.path()), QStringList{QStringLiteral("GitBolt.app")});
#endif
    }

    // ---- MainWindow -----------------------------------------------------------

    void withoutTheCheckTheMenuOpensTheReleasesPage_data() {
        QTest::addColumn<bool>("testSession");
        QTest::newRow("source build") << false;
        QTest::newRow("test session without a test feed") << true;
    }
    void withoutTheCheckTheMenuOpensTheReleasesPage() {
        QFETCH(bool, testSession);
        UrlCatcher catcher;
        gitbolt::ui::MainWindow window;
        if (testSession) {
            gitbolt::ui::UpdateSetupInputs inputs;
            inputs.testSession = gitbolt::util::TestSessionKind::Bridge;
            inputs.appVersion = QStringLiteral("0.1.0");
            inputs.cacheDir = cache_->path();
            inputs.tempDir = cache_->path();
            window.setUpdates(gitbolt::ui::setUpUpdates(inputs));
            QCOMPARE(window.updateGate(), UpdateGate::TestSession);
        } else {
            QCOMPARE(window.updateGate(), UpdateGate::CompiledOut);
        }
        window.startUpdates();
        auto* action = window.findChild<QAction*>(QStringLiteral("help.check-for-updates"));
        QVERIFY(action);
        action->trigger();
        QCOMPARE(catcher.urls, QList<QUrl>{QUrl(QStringLiteral("https://github.com/Inverian/GitBolt/releases"))});
        QVERIFY(!window.findChild<UpdateDialog*>());
        QVERIFY(!window.findChild<QTimer*>(QStringLiteral("updateCheckTimer")));
        QVERIFY(!QDir(cache_->path()).exists(QStringLiteral("updates")));
        QVERIFY(!QDir(cache_->path()).exists(QStringLiteral("gitbolt-updates")));
    }

    void withTheCheckTheMenuAsks() {
        HttpServer server;
        serve(server, QStringLiteral("0.1.1"), {{kAppImageAsset, "new"}});
        UrlCatcher catcher;
        gitbolt::ui::MainWindow window;
        window.setUpdates(testSetup(server, cache_->path()));
        QCOMPARE(window.updateGate(), UpdateGate::TestFeed);
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

        // The daily check's timer, for the bridge.
        auto* timer = window.findChild<QTimer*>(QStringLiteral("updateCheckTimer"));
        QVERIFY(timer && !timer->isActive());
    }

private:
    // A folder with a running AppImage stand-in in it, named `name`.
    QString appImageFolder(const QString& name = QStringLiteral("GitBolt.AppImage")) {
        const QString apps = QDir(cache_->path()).filePath(QStringLiteral("apps"));
        QDir().mkpath(apps);
        QFile file(QDir(apps).filePath(name));
        if (file.open(QIODevice::WriteOnly))
            file.write("old");
        return apps;
    }

    static PackageInfo appImage(const QString& folder, const QString& name = QStringLiteral("GitBolt.AppImage")) {
        return {PackageKind::AppImage, QDir(folder).filePath(name), true, {}};
    }

    QTemporaryDir settingsDir_;
    std::unique_ptr<QTemporaryDir> cache_;
};

QTEST_MAIN(TestUpdateController)
#include "TestUpdateController.moc"
